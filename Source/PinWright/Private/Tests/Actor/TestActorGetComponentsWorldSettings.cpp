// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-get-components-cannot-resolve-worldsettings.
// UEditorActorSubsystem::GetAllLevelActors excludes the live editor world's
// AWorldSettings actor, even though actor.find_by_class can return its path.
// This test drives actor.get_components through the handler harness with that
// exact path and verifies the component row it returns.
#include "Misc/AutomationTest.h"
#include "Utils/ActorUtils.h"

#include "Components/DecalComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/BlockingVolume.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameStateBase.h"
#include "Misc/ScopeExit.h"
#include "Misc/Guid.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ActorGetComponentsWorldSettingsTest
{
    // Compare one of actor.get_components' serialized transform objects against
    // the live component. Rotation uses the same X/Y/Z slots as pitch/yaw/roll.
    bool AssertTransformField(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Row, const TCHAR* FieldName,
        const TCHAR* XKey, const TCHAR* YKey, const TCHAR* ZKey,
        const FVector& Expected)
    {
        const TSharedPtr<FJsonObject>* Value = nullptr;
        if (!Test.TestTrue(
                *FString::Printf(TEXT("component row contains %s"), FieldName),
                Row->TryGetObjectField(FieldName, Value) && Value && (*Value).IsValid()))
        {
            return false;
        }

        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        const bool bHasNumbers = (*Value)->TryGetNumberField(XKey, X)
            && (*Value)->TryGetNumberField(YKey, Y)
            && (*Value)->TryGetNumberField(ZKey, Z);
        if (!Test.TestTrue(
                *FString::Printf(TEXT("component row %s has numeric values"), FieldName),
                bHasNumbers))
        {
            return false;
        }

        return Test.TestTrue(
            *FString::Printf(TEXT("component row %s matches the live component"), FieldName),
            FMath::IsNearlyEqual(X, static_cast<double>(Expected.X), 0.01)
                && FMath::IsNearlyEqual(Y, static_cast<double>(Expected.Y), 0.01)
                && FMath::IsNearlyEqual(Z, static_cast<double>(Expected.Z), 0.01));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetComponentsWorldSettingsObjectPathTest,
    "PinWright.actor.get_components.WorldSettingsObjectPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetComponentsWorldSettingsObjectPathTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping the WorldSettings actor.get_components test."));
        return true;
    }

    // actor.get_components intentionally uses the resolver's null-world
    // PIE-first policy. A PIE WorldSettings actor has the same object path but
    // is not the editor actor carrying this test component, so an active PIE
    // session is a host limitation for this editor-world round-trip test.
    if (GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-world-active"),
            TEXT("A PIE world is active; skipping the editor-world WorldSettings round-trip test."));
        return true;
    }

    AWorldSettings* WorldSettings =
        World->GetWorldSettings(/*bCheckStreamingPersistent=*/false, /*bChecked=*/false);
    if (!WorldSettings)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-world-settings"),
            TEXT("The editor world has no WorldSettings actor; skipping the component round-trip test."));
        return true;
    }

    UPackage* LevelPackage = World->PersistentLevel
        ? World->PersistentLevel->GetOutermost()
        : nullptr;
    const bool bLevelWasDirty = LevelPackage && LevelPackage->IsDirty();

    UDecalComponent* Probe = NewObject<UDecalComponent>(
        WorldSettings,
        MakeUniqueObjectName(WorldSettings, UDecalComponent::StaticClass(),
            TEXT("PinWrightWorldSettingsDecal")),
        RF_Transient);
    if (!TestNotNull(TEXT("transient WorldSettings decal component created"), Probe))
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (IsValid(Probe))
        {
            Probe->DestroyComponent();
        }
        if (LevelPackage)
        {
            LevelPackage->SetDirtyFlag(bLevelWasDirty);
        }
    };

    WorldSettings->AddInstanceComponent(Probe);
    Probe->OnComponentCreated();

    const FVector RequestedLocation(11.0, 22.0, 33.0);
    const FRotator RequestedRotation(10.0, 20.0, 30.0);
    const FVector RequestedScale(1.25, 0.75, 2.0);
    Probe->SetRelativeLocation(RequestedLocation);
    Probe->SetRelativeRotation(RequestedRotation);
    Probe->SetRelativeScale3D(RequestedScale);
    Probe->RegisterComponent();

    if (!TestTrue(TEXT("WorldSettings decal component registered"),
            Probe->IsRegistered()))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), WorldSettings->GetPathName());
    Payload->SetStringField(TEXT("componentClass"),
        UDecalComponent::StaticClass()->GetPathName());

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("actor.get_components handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.get_components"), Payload, Capture)))
    {
        return false;
    }

    TestTrue(TEXT("actor.get_components dispatched a response"), Capture.bWasCalled);
    TestTrue(TEXT("the exact WorldSettings object path resolves"), Capture.bSuccess);
    if (!Capture.bWasCalled || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
    if (!TestTrue(TEXT("response contains a components array"),
            Capture.Result->TryGetArrayField(TEXT("components"), Components)
                && Components))
    {
        return false;
    }

    double Count = 0.0;
    TestTrue(TEXT("response contains a positive component count"),
        Capture.Result->TryGetNumberField(TEXT("count"), Count) && Count >= 1.0);

    TSharedPtr<FJsonObject> ComponentRow;
    const FString ProbePath = Probe->GetPathName();
    for (const TSharedPtr<FJsonValue>& Value : *Components)
    {
        if (!Value.IsValid())
        {
            continue;
        }
        const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
        FString CandidatePath;
        if (Candidate.IsValid()
            && Candidate->TryGetStringField(TEXT("path"), CandidatePath)
            && CandidatePath == ProbePath)
        {
            ComponentRow = Candidate;
            break;
        }
    }

    if (!TestTrue(TEXT("response lists the WorldSettings decal component by path"),
            ComponentRow.IsValid()))
    {
        return false;
    }

    FString ComponentName;
    FString ComponentClass;
    TestTrue(TEXT("component row contains the component name"),
        ComponentRow->TryGetStringField(TEXT("name"), ComponentName));
    TestEqual(TEXT("component row reports the live component name"),
        ComponentName, Probe->GetName());
    TestTrue(TEXT("component row contains the component class"),
        ComponentRow->TryGetStringField(TEXT("class"), ComponentClass));
    TestEqual(TEXT("component row reports the live component class"),
        ComponentClass, UDecalComponent::StaticClass()->GetPathName());

    ActorGetComponentsWorldSettingsTest::AssertTransformField(*this, ComponentRow,
        TEXT("relativeLocation"), TEXT("x"), TEXT("y"), TEXT("z"), RequestedLocation);
    ActorGetComponentsWorldSettingsTest::AssertTransformField(*this, ComponentRow,
        TEXT("relativeRotation"), TEXT("pitch"), TEXT("yaw"), TEXT("roll"),
        FVector(RequestedRotation.Pitch, RequestedRotation.Yaw, RequestedRotation.Roll));
    ActorGetComponentsWorldSettingsTest::AssertTransformField(*this, ComponentRow,
        TEXT("relativeScale"), TEXT("x"), TEXT("y"), TEXT("z"), RequestedScale);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorResolverExactPathLiveActorClassesTest,
    "PinWright.actor_utils.ResolveActor.ExactPathLiveActorClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorResolverExactPathLiveActorClassesTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-required"),
            TEXT("The exact-path actor resolver test requires an editor world outside PIE."));
        return true;
    }

    UWorld* OriginalEditorWorld = GEditor->GetEditorWorldContext().World();
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString WorldName = FString::Printf(TEXT("PinWrightActorResolver_%s"), *Stamp);
    UPackage* Package = CreatePackage(*FString::Printf(
        TEXT("/Temp/PinWrightTests/%s"), *WorldName));
    UWorld* World = Package
        ? UWorld::CreateWorld(EWorldType::Editor, false, FName(*WorldName), Package, true)
        : nullptr;
    FScopedTransientWorldGuard WorldGuard(World);
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Could not create the transient resolver world."));
        return true;
    }

    const FString OtherWorldName = FString::Printf(TEXT("PinWrightActorResolverOther_%s"), *Stamp);
    UPackage* OtherPackage = CreatePackage(*FString::Printf(
        TEXT("/Temp/PinWrightTests/%s"), *OtherWorldName));
    UWorld* OtherWorld = OtherPackage
        ? UWorld::CreateWorld(EWorldType::Editor, false, FName(*OtherWorldName), OtherPackage, true)
        : nullptr;
    FScopedTransientWorldGuard OtherWorldGuard(OtherWorld);
    if (!OtherWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Could not create the second transient resolver world."));
        return true;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transient;
    ABlockingVolume* Brush = World->SpawnActor<ABlockingVolume>(
        ABlockingVolume::StaticClass(), FTransform::Identity, SpawnParameters);
    AGameStateBase* Info = World->SpawnActor<AGameStateBase>(
        AGameStateBase::StaticClass(), FTransform::Identity, SpawnParameters);
    ABlockingVolume* WrongWorldBrush = OtherWorld->SpawnActor<ABlockingVolume>(
        ABlockingVolume::StaticClass(), FTransform::Identity, SpawnParameters);
    if (!Brush || !Info || !WrongWorldBrush)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Could not spawn the brush and concrete AInfo-derived resolver fixtures."));
        return true;
    }

    GEditor->GetEditorWorldContext().SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        GEditor->GetEditorWorldContext().SetCurrentWorld(OriginalEditorWorld);
    };

    const McpActorUtils::FActorResolution BrushResolution =
        McpActorUtils::ResolveActor(nullptr, Brush->GetPathName());
    TestTrue(TEXT("an exact path resolves a transient ABrush-derived actor"),
        BrushResolution.IsResolved() && BrushResolution.Actor == Brush);

    const McpActorUtils::FActorResolution InfoResolution =
        McpActorUtils::ResolveActor(nullptr, Info->GetPathName());
    TestTrue(TEXT("an exact path resolves a transient concrete AInfo-derived actor"),
        InfoResolution.IsResolved() && InfoResolution.Actor == Info);

    const McpActorUtils::FActorResolution WrongWorldResolution =
        McpActorUtils::ResolveActor(nullptr, WrongWorldBrush->GetPathName());
    TestFalse(TEXT("an exact path from another world is refused"),
        WrongWorldResolution.IsResolved());

    return true;
}

#endif
