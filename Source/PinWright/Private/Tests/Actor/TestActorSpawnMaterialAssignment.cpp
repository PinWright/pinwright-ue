// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for material-at-spawn on actor.spawn / actor.spawn_shape /
// actor.spawn_batch.
//
// Before this feature no spawn verb could set a material: every blockout workflow had to
// either duplicate a mesh asset per colour and bake with static_mesh.set_material, or spawn
// first and issue a second per-actor actor.set_component_properties {OverrideMaterials:[...]}
// call. The fix adds `materialPath` (slot 0, mirroring blueprint.scs.add_component) and
// `materialPaths` (index i -> slot i) to all three verbs, plus a batch-level default and a
// per-transform override on actor.spawn_batch.
//
// GROUND TRUTH: these tests do not settle for the response echo. They read
// UMeshComponent::GetMaterial(0) back off the live spawned component, which is the
// serialized OverrideMaterials UPROPERTY that persists into the level package - the thing
// the ticket actually asks for. Reverting the handler change leaves slot 0 on the mesh
// default and the ground-truth assertions fail.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Guid.h"
#include "Utils/ActorUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Engine material that is present on every UE install - the same fixture
    // TestEnvironmentHandlers.cpp and the asset-dump tests lean on.
    const TCHAR* const SpawnMatProbeMaterial =
        TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // Uniquely named so a Unity merge cannot ODR-clash it with sibling test helpers.
    FString SpawnMatLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UMaterialInterface* SpawnMatLoadProbe()
    {
        return LoadObject<UMaterialInterface>(nullptr, SpawnMatProbeMaterial);
    }

    // Slot-0 material read straight off the live component, not off the response.
    UMaterialInterface* SpawnMatReadSlot0(AActor* Actor)
    {
        if (!Actor)
        {
            return nullptr;
        }
        UMeshComponent* MeshComp = Cast<UMeshComponent>(Actor->GetRootComponent());
        if (!MeshComp)
        {
            MeshComp = Actor->FindComponentByClass<UMeshComponent>();
        }
        return MeshComp ? MeshComp->GetMaterial(0) : nullptr;
    }

    TSharedPtr<FJsonValue> SpawnMatTransformEntry(const FVector& Location, const FString& Name,
                                                  const FString& MaterialPath)
    {
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), Location.X);
        Loc->SetNumberField(TEXT("y"), Location.Y);
        Loc->SetNumberField(TEXT("z"), Location.Z);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetObjectField(TEXT("location"), Loc);
        if (!Name.IsEmpty())
        {
            Entry->SetStringField(TEXT("name"), Name);
        }
        if (!MaterialPath.IsEmpty())
        {
            Entry->SetStringField(TEXT("materialPath"), MaterialPath);
        }
        return MakeShared<FJsonValueObject>(Entry);
    }
}

// ============================================================================
// Discoverability: all three verbs must REGISTER the new slots, or the dispatcher's
// unknown-param check rejects a caller that uses them (RpcDispatcher.cpp
// ValidateHandlerParams -> UNKNOWN_PARAMS).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnMaterialParamsRegisteredTest,
    "PinWright.actor.spawn.MaterialParamsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnMaterialParamsRegisteredTest::RunTest(const FString& Parameters)
{
    const TCHAR* const Methods[] = {
        TEXT("actor.spawn"), TEXT("actor.spawn_shape"), TEXT("actor.spawn_batch") };

    for (const TCHAR* Method : Methods)
    {
        const FParamSpec* PathSpec = GetRegisteredParamSpec(Method, TEXT("materialPath"));
        TestNotNull(*FString::Printf(TEXT("%s registers materialPath"), Method), PathSpec);
        if (PathSpec)
        {
            TestTrue(*FString::Printf(TEXT("%s materialPath is optional"), Method),
                !PathSpec->bRequired);
            TestTrue(*FString::Printf(TEXT("%s materialPath accepts the snake_case alias"), Method),
                PathSpec->Aliases.Contains(FString(TEXT("material_path"))));
        }

        const FParamSpec* PathsSpec = GetRegisteredParamSpec(Method, TEXT("materialPaths"));
        TestNotNull(*FString::Printf(TEXT("%s registers materialPaths"), Method), PathsSpec);
        if (PathsSpec)
        {
            TestTrue(*FString::Printf(TEXT("%s materialPaths is optional"), Method),
                !PathsSpec->bRequired);
            TestEqual(*FString::Printf(TEXT("%s materialPaths is an array param"), Method),
                PathsSpec->Type, FString(TEXT("array")));
        }
    }
    return true;
}

// ============================================================================
// actor.spawn: materialPath binds slot 0 on the spawned mesh component, and the
// assignment is readable back off the live component (OverrideMaterials).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnMaterialPathAssignsSlotZeroTest,
    "PinWright.actor.spawn.MaterialPathAssignsSlotZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnMaterialPathAssignsSlotZeroTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn material test."));
        return true;
    }
    UMaterialInterface* Probe = SpawnMatLoadProbe();
    if (!Probe)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("WorldGridMaterial unavailable; skipping actor.spawn material test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = SpawnMatLabel(TEXT("PW_SpawnMat"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetStringField(TEXT("materialPath"), SpawnMatProbeMaterial);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
    TestTrue(TEXT("actor.spawn with materialPath succeeds"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bApplied = false;
        TestTrue(TEXT("response carries material_applied"),
            Capture.Result->TryGetBoolField(TEXT("material_applied"), bApplied));
        TestTrue(TEXT("material_applied is true"), bApplied);

        double Slots = 0.0;
        TestTrue(TEXT("response carries materialSlotsApplied"),
            Capture.Result->TryGetNumberField(TEXT("materialSlotsApplied"), Slots));
        TestEqual(TEXT("exactly one slot was applied"), (int32)Slots, 1);

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        TestFalse(TEXT("a clean apply carries no warnings"),
            Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings));
    }

    // GROUND TRUTH: slot 0 of the live component is the probe material.
    AActor* Spawned = McpActorUtils::FindActorByName(nullptr, Label);
    TestNotNull(TEXT("spawned actor found by label"), Spawned);
    TestTrue(TEXT("component slot 0 holds the requested material"),
        SpawnMatReadSlot0(Spawned) == Probe);
    return true;
}

// ============================================================================
// actor.spawn: an unloadable materialPath is a typed MATERIAL_NOT_FOUND and NOTHING is
// spawned - the pre-flight contract. A spawn-and-warn implementation would leave an orphan
// actor and pass a naive success check, so this asserts the world is unchanged.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnMaterialNotFoundIsPreflightErrorTest,
    "PinWright.actor.spawn.MaterialNotFoundIsPreflightError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnMaterialNotFoundIsPreflightErrorTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping material-not-found test."));
        return true;
    }

    bSuppressLogErrors = true;
    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = SpawnMatLabel(TEXT("PW_SpawnMatMissing"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetStringField(TEXT("materialPath"),
        TEXT("/Game/PinWrightTests/DoesNotExist/M_NoSuchMaterial"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
    TestFalse(TEXT("an unloadable materialPath is not a success"), Capture.bSuccess);
    TestEqual(TEXT("unloadable materialPath yields MATERIAL_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("MATERIAL_NOT_FOUND")));

    // Pre-flight, not spawn-and-warn: no orphan actor was left behind.
    TestNull(TEXT("no actor was spawned when the material failed to resolve"),
        McpActorUtils::FindActorByName(nullptr, Label));
    return true;
}

// ============================================================================
// actor.spawn_shape: materialPath works on the BasicShapes wrapper, and a materialPaths
// entry past the single Cube slot warns instead of failing the call.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnShapeMaterialAndOverflowWarnsTest,
    "PinWright.actor.spawn_shape.MaterialAndSlotOverflowWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnShapeMaterialAndOverflowWarnsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping spawn_shape material test."));
        return true;
    }
    UMaterialInterface* Probe = SpawnMatLoadProbe();
    if (!Probe)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("WorldGridMaterial unavailable; skipping spawn_shape material test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = SpawnMatLabel(TEXT("PW_ShapeMat"));

    // Slot 0 valid, slot 1 past the Cube's single slot -> applied + warned, still a success.
    TArray<TSharedPtr<FJsonValue>> Paths;
    Paths.Add(MakeShared<FJsonValueString>(FString(SpawnMatProbeMaterial)));
    Paths.Add(MakeShared<FJsonValueString>(FString(SpawnMatProbeMaterial)));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetArrayField(TEXT("materialPaths"), Paths);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_shape is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), Payload, Capture));
    TestTrue(TEXT("slot overflow does not fail the spawn"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double Slots = 0.0;
        Capture.Result->TryGetNumberField(TEXT("materialSlotsApplied"), Slots);
        TestEqual(TEXT("only the in-range slot was applied"), (int32)Slots, 1);

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        TestTrue(TEXT("the out-of-range slot is reported in warnings"),
            Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings &&
            Warnings->Num() == 1);
    }

    AActor* Spawned = McpActorUtils::FindActorByName(nullptr, Label);
    TestNotNull(TEXT("shape actor found by label"), Spawned);
    TestTrue(TEXT("shape component slot 0 holds the requested material"),
        SpawnMatReadSlot0(Spawned) == Probe);
    return true;
}

// ============================================================================
// actor.spawn_batch: batch-level default applies to every actor, and a per-entry
// materialPath overrides it for just that placement.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnBatchMaterialDefaultAndOverrideTest,
    "PinWright.actor.spawn_batch.MaterialDefaultAndPerEntryOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnBatchMaterialDefaultAndOverrideTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping spawn_batch material test."));
        return true;
    }
    UMaterialInterface* Probe = SpawnMatLoadProbe();
    if (!Probe)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("WorldGridMaterial unavailable; skipping spawn_batch material test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString LabelA = SpawnMatLabel(TEXT("PW_BatchMatA"));
    const FString LabelB = SpawnMatLabel(TEXT("PW_BatchMatB"));

    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(SpawnMatTransformEntry(FVector(0, 0, 0), LabelA, FString()));
    Transforms.Add(SpawnMatTransformEntry(FVector(300, 0, 0), LabelB, SpawnMatProbeMaterial));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    Payload->SetStringField(TEXT("materialPath"), SpawnMatProbeMaterial);
    Payload->SetArrayField(TEXT("transforms"), Transforms);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_batch is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_batch"), Payload, Capture));
    TestTrue(TEXT("actor.spawn_batch with materials succeeds"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double Applied = 0.0;
        Capture.Result->TryGetNumberField(TEXT("materialSlotsApplied"), Applied);
        TestEqual(TEXT("two slots applied across the batch"), (int32)Applied, 2);

        double WithMaterial = 0.0;
        Capture.Result->TryGetNumberField(TEXT("actorsWithMaterial"), WithMaterial);
        TestEqual(TEXT("both actors received a material"), (int32)WithMaterial, 2);
    }

    // GROUND TRUTH on both: the inherited default and the per-entry override.
    TestTrue(TEXT("batch-default actor slot 0 holds the material"),
        SpawnMatReadSlot0(McpActorUtils::FindActorByName(nullptr, LabelA)) == Probe);
    TestTrue(TEXT("per-entry override actor slot 0 holds the material"),
        SpawnMatReadSlot0(McpActorUtils::FindActorByName(nullptr, LabelB)) == Probe);
    return true;
}

// ============================================================================
// actor.spawn_batch: a bad per-entry material path aborts the WHOLE call before anything
// spawns. This is the anti-half-batch guarantee - the verb already reports malformed
// entries as skips, and a mid-loop material failure would be a second failure mode.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnBatchBadEntryMaterialSpawnsNothingTest,
    "PinWright.actor.spawn_batch.BadEntryMaterialSpawnsNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnBatchBadEntryMaterialSpawnsNothingTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping spawn_batch abort test."));
        return true;
    }

    bSuppressLogErrors = true;
    FScopedEditorWorldActorGuard WorldGuard;
    const FString LabelA = SpawnMatLabel(TEXT("PW_BatchAbortA"));
    const FString LabelB = SpawnMatLabel(TEXT("PW_BatchAbortB"));

    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(SpawnMatTransformEntry(FVector(0, 0, 0), LabelA, FString()));
    Transforms.Add(SpawnMatTransformEntry(FVector(300, 0, 0), LabelB,
        TEXT("/Game/PinWrightTests/DoesNotExist/M_NoSuchMaterial")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    Payload->SetArrayField(TEXT("transforms"), Transforms);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_batch is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_batch"), Payload, Capture));
    TestFalse(TEXT("a bad per-entry material fails the call"), Capture.bSuccess);
    TestEqual(TEXT("bad per-entry material yields MATERIAL_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("MATERIAL_NOT_FOUND")));
    TestTrue(TEXT("the error names the offending transforms entry"),
        Capture.Message.Contains(TEXT("transforms[1]")));

    // No half-spawned batch: the good entry that precedes the bad one must not exist.
    TestNull(TEXT("no actor from the preceding good entry was spawned"),
        McpActorUtils::FindActorByName(nullptr, LabelA));
    TestNull(TEXT("no actor from the bad entry was spawned"),
        McpActorUtils::FindActorByName(nullptr, LabelB));
    return true;
}

// ============================================================================
// Backward compatibility: a spawn with NO material argument must carry none of the new
// response fields, so existing callers see a byte-identical result shape.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnWithoutMaterialKeepsLegacyShapeTest,
    "PinWright.actor.spawn.WithoutMaterialKeepsLegacyResponseShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnWithoutMaterialKeepsLegacyShapeTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping legacy-shape test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = SpawnMatLabel(TEXT("PW_SpawnNoMat"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Payload->SetStringField(TEXT("actorName"), Label);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
    TestTrue(TEXT("actor.spawn without a material still succeeds"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bApplied = false;
        TestFalse(TEXT("no material_applied field when no material was requested"),
            Capture.Result->TryGetBoolField(TEXT("material_applied"), bApplied));
        double Slots = 0.0;
        TestFalse(TEXT("no materialSlotsApplied field when no material was requested"),
            Capture.Result->TryGetNumberField(TEXT("materialSlotsApplied"), Slots));
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        TestFalse(TEXT("no warnings field when no material was requested"),
            Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings));
    }
    return true;
}
