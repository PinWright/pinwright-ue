// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the actor.spawn* alias sets declared in
// Handlers/Actor/SpawnParamUtils.h (SpawnLabelKeys / SpawnClassPathKeys / SpawnMeshPathKeys).
//
// The defect these pin is a DECLARED-BUT-NOT-ACCEPTED split, not a missing feature. The
// handler bodies already read the alternate spellings through Ctx.GetStringFirstOf, and the
// param descriptions advertised them ("Accepts class_name and className aliases") — but the
// FParamSpec carried no Aliases, so the dispatcher's UNKNOWN_PARAMS gate
// (RpcDispatcher.cpp ValidateHandlerParams -> AddKnownParamNames) rejected the payload
// BEFORE the body ran. A spelling was documented as accepted and was in fact refused.
//
// Because the gate lives in the dispatcher, the acceptance half MUST route through a real
// FRpcDispatcher: Tests/TestUtils.h's InvokeHandlerWithCapture calls the registered function
// directly and never runs ValidateHandlerParams, so it cannot observe the bug at all.
//
// Two layers of coverage, mirroring TestAssetPathParamAlias / TestDriveWindowSelectorParams:
//  - Declaration: each verb/slot's production FParamSpec carries the canonical Name and the
//    EXACT expected alias set (SpawnAliasSetsAreExact fails loudly if a future edit drops or
//    silently widens one). Includes the header's deliberate constraint that `assetPath` is
//    attached to the classPath slot ONLY, never to meshPath.
//  - Acceptance: dispatch each alias spelling through the real dispatcher and check GROUND
//    TRUTH off the live world — an actor whose GetActorLabel() is the requested label, whose
//    class is AStaticMeshActor, and whose component holds the engine cube. That proves the
//    alias reached the handler, not merely that the response echoed it back.
// Counterfactual: reverting the Aliases wiring makes every acceptance dispatch fail with
// UNKNOWN_PARAMS (no actor spawned, ground truth null) and the declaration checks fail on an
// empty Aliases array.
//
// No content fixture is needed: /Engine/BasicShapes/Cube.Cube ships with every UE install,
// the same always-present-fixture pattern TestActorSpawnMaterialAssignment.cpp uses for
// WorldGridMaterial. Labels are GUID-suffixed so FindActorByName can never resolve ambiguously.
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Utils/ActorUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with the identically-shaped helpers in
// TestActorSpawnMaterialAssignment.cpp (same convention as ParamSpecTestHelpers.h).
namespace SpawnParamAliasTestLocal
{
    // Engine mesh present on every UE install; doubles as the classPath payload (classPath
    // accepts a StaticMesh asset path and auto-picks StaticMeshActor) and the meshPath payload.
    const TCHAR* const AliasProbeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // GUID-suffixed so the label is unique in the level: McpActorUtils::FindActorByName
    // refuses an ambiguous label, which would turn a real pass into a null lookup.
    FString AliasSpawnLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UStaticMesh* AliasProbeMesh()
    {
        return LoadObject<UStaticMesh>(nullptr, AliasProbeMeshPath);
    }

    // One expected slot declaration: the verb, the canonical wire name that must remain
    // FParamSpec.Name, and the exact alias set MakeAliasParamSpec should have produced.
    struct FAliasSlotExpectation
    {
        const TCHAR* Method;
        const TCHAR* Canonical;
        // The declared type token. Path-shaped slots carry `path`/`classref`, which is what
        // makes the dispatcher refuse a doubled slash before the handler loads anything.
        const TCHAR* ExpectedType;
        TArray<FString> Aliases;
    };

    // The full declared contract of SpawnParamUtils across its three wired verbs. Both
    // declaration tests iterate this one table so a new verb/slot is added in a single place.
    const TArray<FAliasSlotExpectation>& ExpectedSpawnAliasSlots()
    {
        static const TArray<FAliasSlotExpectation> Slots = {
            // actor.spawn wires all three slots.
            { TEXT("actor.spawn"), TEXT("classPath"), TEXT("classref"),
              { TEXT("assetPath"), TEXT("class_name"), TEXT("className") } },
            { TEXT("actor.spawn"), TEXT("meshPath"), TEXT("path"),
              { TEXT("mesh_path") } },
            { TEXT("actor.spawn"), TEXT("actorName"), TEXT("string"),
              { TEXT("label"), TEXT("name"), TEXT("actor_name") } },
            // actor.spawn_shape and actor.spawn_from_blueprint wire the label slot only:
            // their "what to spawn" input is `shape` / `blueprintPath`, not a class path.
            { TEXT("actor.spawn_shape"), TEXT("actorName"), TEXT("string"),
              { TEXT("label"), TEXT("name"), TEXT("actor_name") } },
            { TEXT("actor.spawn_from_blueprint"), TEXT("actorName"), TEXT("string"),
              { TEXT("label"), TEXT("name"), TEXT("actor_name") } }
        };
        return Slots;
    }

    // Dispatches actor.spawn with {<PathKey>: <engine cube>, <LabelKey>: <Label>} through the
    // REAL dispatcher (the UNKNOWN_PARAMS gate lives there, not in the handler body), then
    // asserts ground truth off the live world rather than off the response echo.
    void CheckSpawnAliasPair(FAutomationTestBase& Test, const TCHAR* PathKey,
        const TCHAR* LabelKey, const FString& Label, UStaticMesh* Cube)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(PathKey, AliasProbeMeshPath);
        Params->SetStringField(LabelKey, Label);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.spawn"),
            FString::Printf(TEXT("req-spawn-alias-%s-%s"), PathKey, LabelKey),
            Params, bSuccess, ErrorCode);

        Test.TestNotEqual(*FString::Printf(
                TEXT("actor.spawn does not reject {%s, %s} as UNKNOWN_PARAMS"), PathKey, LabelKey),
            ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
        Test.TestTrue(*FString::Printf(TEXT("actor.spawn succeeds with {%s, %s}"),
            PathKey, LabelKey), bSuccess);

        // GROUND TRUTH: the label alias reached SetActorLabel on a real level actor.
        AActor* Spawned = McpActorUtils::FindActorByName(nullptr, Label);
        if (!Test.TestNotNull(*FString::Printf(
                TEXT("an actor labelled via '%s' exists in the world"), LabelKey), Spawned))
        {
            return;
        }
        Test.TestEqual(*FString::Printf(TEXT("'%s' became the actor label verbatim"), LabelKey),
            Spawned->GetActorLabel(), Label);

        // GROUND TRUTH: the path alias reached the spawn-class resolution, not just the payload.
        AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Spawned);
        Test.TestNotNull(*FString::Printf(TEXT("'%s' resolved to a StaticMeshActor"), PathKey),
            MeshActor);
        if (MeshActor && MeshActor->GetStaticMeshComponent())
        {
            Test.TestTrue(*FString::Printf(TEXT("'%s' bound the engine cube on the component"),
                PathKey), MeshActor->GetStaticMeshComponent()->GetStaticMesh() == Cube);
        }
    }

    // Dispatches Method with a label-slot alias and asserts the spawned actor carries it.
    // Shared by the spawn_shape and spawn_from_blueprint acceptance tests, whose payloads
    // differ only in the non-alias "what to spawn" key.
    void CheckLabelAliasReachesHandler(FAutomationTestBase& Test, const TCHAR* Method,
        const TCHAR* SourceKey, const TCHAR* SourceValue, const TCHAR* LabelKey,
        const FString& Label)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(SourceKey, SourceValue);
        Params->SetStringField(LabelKey, Label);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            FString::Printf(TEXT("req-%s-alias-%s"), Method, LabelKey),
            Params, bSuccess, ErrorCode);

        Test.TestNotEqual(*FString::Printf(TEXT("%s does not reject '%s' as UNKNOWN_PARAMS"),
            Method, LabelKey), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
        Test.TestTrue(*FString::Printf(TEXT("%s succeeds with '%s'"), Method, LabelKey), bSuccess);

        AActor* Spawned = McpActorUtils::FindActorByName(nullptr, Label);
        if (!Test.TestNotNull(*FString::Printf(TEXT("%s labelled an actor via '%s'"),
                Method, LabelKey), Spawned))
        {
            return;
        }
        Test.TestEqual(*FString::Printf(TEXT("%s '%s' became the actor label verbatim"),
            Method, LabelKey), Spawned->GetActorLabel(), Label);
    }
}

// ============================================================================
// 1. Declaration: every wired verb/slot registers the canonical name plus every alias
//    SpawnParamUtils promises. Without these the dispatcher's known-param set omits the
//    spelling and the payload is rejected before the handler body reads it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnVerbsDeclareSpawnParamAliasesTest,
    "PinWright.actor.aliases.SpawnVerbsDeclareSpawnParamAliases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnVerbsDeclareSpawnParamAliasesTest::RunTest(const FString& Parameters)
{
    using SpawnParamAliasTestLocal::FAliasSlotExpectation;

    for (const FAliasSlotExpectation& Slot : SpawnParamAliasTestLocal::ExpectedSpawnAliasSlots())
    {
        const FParamSpec* Spec = ParamSpecTestHelpers::FindParamSpec(Slot.Method, Slot.Canonical);
        if (!TestNotNull(*FString::Printf(TEXT("%s declares the '%s' slot"),
                Slot.Method, Slot.Canonical), Spec))
        {
            continue;
        }

        TestEqual(*FString::Printf(TEXT("%s '%s' keeps its canonical name"),
            Slot.Method, Slot.Canonical), Spec->Name, FString(Slot.Canonical));
        TestEqual(*FString::Printf(TEXT("%s '%s' declares type '%s'"),
            Slot.Method, Slot.Canonical, Slot.ExpectedType), Spec->Type, FString(Slot.ExpectedType));
        TestFalse(*FString::Printf(TEXT("%s '%s' is optional"),
            Slot.Method, Slot.Canonical), Spec->bRequired);

        for (const FString& Alias : Slot.Aliases)
        {
            TestTrue(*FString::Printf(TEXT("%s '%s' accepts the '%s' alias"),
                Slot.Method, Slot.Canonical, *Alias), Spec->Aliases.Contains(Alias));
        }
    }
    return true;
}

// ============================================================================
// 2. Regression guard: the alias sets are EXACT. A future edit that drops one spelling (the
//    original defect) or quietly widens a slot fails here rather than silently changing which
//    payloads the dispatcher admits. Also pins the header's deliberate single-slot rule for
//    `assetPath`: aliasing it onto meshPath as well would let one wire key populate two
//    mutually-informing slots in the same request.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnAliasSetsAreExactTest,
    "PinWright.actor.aliases.SpawnAliasSetsAreExact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnAliasSetsAreExactTest::RunTest(const FString& Parameters)
{
    using SpawnParamAliasTestLocal::FAliasSlotExpectation;

    for (const FAliasSlotExpectation& Slot : SpawnParamAliasTestLocal::ExpectedSpawnAliasSlots())
    {
        const FParamSpec* Spec = ParamSpecTestHelpers::FindParamSpec(Slot.Method, Slot.Canonical);
        if (!TestNotNull(*FString::Printf(TEXT("%s declares the '%s' slot"),
                Slot.Method, Slot.Canonical), Spec))
        {
            continue;
        }

        TestEqual(*FString::Printf(TEXT("%s '%s' declares exactly %d aliases"),
            Slot.Method, Slot.Canonical, Slot.Aliases.Num()),
            Spec->Aliases.Num(), Slot.Aliases.Num());

        // Named individually so a widened set reports WHICH spelling appeared.
        for (const FString& Declared : Spec->Aliases)
        {
            TestTrue(*FString::Printf(TEXT("%s '%s' alias '%s' is part of the declared contract"),
                Slot.Method, Slot.Canonical, *Declared), Slot.Aliases.Contains(Declared));
        }

        // The canonical name must never be duplicated into its own alias list: the dispatcher
        // unions Name + Aliases, and a duplicate would mask a canonical rename.
        TestFalse(*FString::Printf(TEXT("%s '%s' does not repeat itself in Aliases"),
            Slot.Method, Slot.Canonical), Spec->Aliases.Contains(FString(Slot.Canonical)));
    }

    // assetPath belongs to the classPath slot only (SpawnParamUtils.h header note).
    if (const FParamSpec* MeshSpec =
            ParamSpecTestHelpers::FindParamSpec(TEXT("actor.spawn"), TEXT("meshPath")))
    {
        TestFalse(TEXT("actor.spawn meshPath does not also claim assetPath"),
            MeshSpec->Aliases.Contains(FString(TEXT("assetPath"))));
    }

    // The label-only verbs must not have acquired the class/mesh slots as a side effect.
    TestNull(TEXT("actor.spawn_shape declares no classPath slot"),
        ParamSpecTestHelpers::FindParamSpec(TEXT("actor.spawn_shape"), TEXT("classPath")));
    TestNull(TEXT("actor.spawn_from_blueprint declares no classPath slot"),
        ParamSpecTestHelpers::FindParamSpec(TEXT("actor.spawn_from_blueprint"), TEXT("classPath")));
    return true;
}

// ============================================================================
// 3. Acceptance (actor.spawn): the surface-dominant `assetPath` spelling plus the
//    actor.set_label-style `label` spelling resolve end to end. Ground truth is the live
//    actor — its label, its class, and the mesh bound on its component — so the assertion
//    cannot be satisfied by a response echo.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnAcceptsAssetPathAndLabelOnWireTest,
    "PinWright.actor.aliases.SpawnAcceptsAssetPathAndLabelOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnAcceptsAssetPathAndLabelOnWireTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn alias acceptance test."));
        return true;
    }
    UStaticMesh* Cube = SpawnParamAliasTestLocal::AliasProbeMesh();
    if (!Cube)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/BasicShapes/Cube.Cube unavailable; skipping alias acceptance test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    SpawnParamAliasTestLocal::CheckSpawnAliasPair(*this, TEXT("assetPath"), TEXT("label"),
        SpawnParamAliasTestLocal::AliasSpawnLabel(TEXT("PW_SpawnAliasAssetPath")), Cube);
    return true;
}

// ============================================================================
// 4. Acceptance (actor.spawn): the snake_case and casing-variant spellings the handler body
//    has always read — class_name / className / mesh_path — plus the remaining label
//    spellings name / actor_name. These are the exact keys that were documented as accepted
//    and returned UNKNOWN_PARAMS.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnAcceptsSnakeCaseSpellingsOnWireTest,
    "PinWright.actor.aliases.SpawnAcceptsSnakeCaseSpellingsOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnAcceptsSnakeCaseSpellingsOnWireTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn snake_case alias test."));
        return true;
    }
    UStaticMesh* Cube = SpawnParamAliasTestLocal::AliasProbeMesh();
    if (!Cube)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/BasicShapes/Cube.Cube unavailable; skipping snake_case alias test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // class_name + name: the ticket's headline pair (classPath alias x lighting/environment
    // spawn-verb label spelling).
    SpawnParamAliasTestLocal::CheckSpawnAliasPair(*this, TEXT("class_name"), TEXT("name"),
        SpawnParamAliasTestLocal::AliasSpawnLabel(TEXT("PW_SpawnAliasClassName")), Cube);

    // mesh_path + actor_name: the explicit mesh slot's snake_case alias x the snake_case
    // variant of the canonical label key.
    SpawnParamAliasTestLocal::CheckSpawnAliasPair(*this, TEXT("mesh_path"), TEXT("actor_name"),
        SpawnParamAliasTestLocal::AliasSpawnLabel(TEXT("PW_SpawnAliasMeshPath")), Cube);

    // className + the canonical actorName: covers the last classPath spelling and proves the
    // canonical key still works alongside the new aliases.
    SpawnParamAliasTestLocal::CheckSpawnAliasPair(*this, TEXT("className"), TEXT("actorName"),
        SpawnParamAliasTestLocal::AliasSpawnLabel(TEXT("PW_SpawnAliasClassNameCamel")), Cube);
    return true;
}

// ============================================================================
// 5. Acceptance (actor.spawn_shape): the label slot's `name` and `actor_name` spellings reach
//    SetActorLabel. spawn_shape wires the label slot only, so its "what to spawn" key stays
//    the required `shape` enum.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnShapeAcceptsLabelAliasesOnWireTest,
    "PinWright.actor.aliases.SpawnShapeAcceptsLabelAliasesOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnShapeAcceptsLabelAliasesOnWireTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn_shape alias test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    SpawnParamAliasTestLocal::CheckLabelAliasReachesHandler(*this, TEXT("actor.spawn_shape"),
        TEXT("shape"), TEXT("CUBE"), TEXT("name"),
        SpawnParamAliasTestLocal::AliasSpawnLabel(TEXT("PW_ShapeAliasName")));

    SpawnParamAliasTestLocal::CheckLabelAliasReachesHandler(*this, TEXT("actor.spawn_shape"),
        TEXT("shape"), TEXT("CUBE"), TEXT("actor_name"),
        SpawnParamAliasTestLocal::AliasSpawnLabel(TEXT("PW_ShapeAliasActorName")));
    return true;
}

// ============================================================================
// 6. Acceptance (actor.spawn_from_blueprint): the label slot's `label` and `name` spellings
//    reach SetActorLabel.
//
//    No Blueprint fixture is loaded. The verb's own documented resolution chain ends in
//    ResolveClassByName(blueprintPath) (SpawnHandler.cpp), which resolves a bare engine class
//    name via /Script/Engine.<Name> — so `StaticMeshActor` spawns deterministically on any
//    host and the test measures the label alias rather than host content. If that fallback is
//    ever removed the dispatch stops succeeding and this test fails pointing at the comment,
//    which is the correct signal for a resolution-chain change.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnFromBlueprintAcceptsLabelAliasesOnWireTest,
    "PinWright.actor.aliases.SpawnFromBlueprintAcceptsLabelAliasesOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnFromBlueprintAcceptsLabelAliasesOnWireTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn_from_blueprint alias test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    SpawnParamAliasTestLocal::CheckLabelAliasReachesHandler(*this,
        TEXT("actor.spawn_from_blueprint"), TEXT("blueprintPath"), TEXT("StaticMeshActor"),
        TEXT("label"), SpawnParamAliasTestLocal::AliasSpawnLabel(TEXT("PW_BpAliasLabel")));

    SpawnParamAliasTestLocal::CheckLabelAliasReachesHandler(*this,
        TEXT("actor.spawn_from_blueprint"), TEXT("blueprintPath"), TEXT("StaticMeshActor"),
        TEXT("name"), SpawnParamAliasTestLocal::AliasSpawnLabel(TEXT("PW_BpAliasName")));
    return true;
}
