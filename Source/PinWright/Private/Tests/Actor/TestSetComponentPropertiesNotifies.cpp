// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests that actor.set_component_properties runs the ENGINE'S change notification for a
// property it writes by reflection, not merely the field store.
//
// THE DEFECT THESE PIN. The handler wrote every non-special-cased property through
// ApplyJsonValueToProperty - a raw reflection store into the component's memory - and
// committed with MarkRenderStateDirty() + UpdateComponentToWorld() + MarkPackageDirty().
// No FPropertyChangedEvent was ever built, so no class's PostEditChangeProperty override
// ran. For any property whose observable effect lives in that override, the verb reported
// `applied`, read back the requested value, and changed nothing.
//
// Measured on the production map: assigning a WaterBodyRiver's WaterMaterial to None and
// back through this verb left the water Material Instance Dynamic untouched and the frame
// PIXEL-IDENTICAL (mean luma 0.4493 before and after a 2x Scattering change). The same
// assignment through Python set_editor_property - which routes via FPropertyAccessUtil and
// DOES emit the notification - rebuilt the MID (WaterMID_0 -> WaterMID_3) and moved mean
// luma to 0.4743.
//
// HOW THESE DETECT IT WITHOUT THE WATER PLUGIN. Water is one instance of a whole class, so
// the test picks the cheapest member of that class instead: UPrimitiveComponent mirrors
// LDMaxDrawDistance ("Desired Max Draw Distance") into CachedMaxDrawDistance ("Current Max
// Draw Distance"), which is the value the renderer culls on. The mirroring happens ONLY in
// UPrimitiveComponent::PostEditChangeProperty - the LDMaxDrawDistance branch sets
// bCullDistanceInvalidated (PrimitiveComponent.cpp:1552-1555) and the tail calls
// SetCachedMaxDrawDistance (:1628). Nothing else in the engine copies one to the other on a
// property write. So a raw store leaves them diverged, and `CachedMaxDrawDistance ==
// LDMaxDrawDistance` after the verb is an exact, deterministic, RHI-free proxy for "the
// engine's own change path ran". It fails on the pre-fix handler for the right reason.
//
// The probe is set Movable BEFORE the call on purpose: with Static mobility and
// bAllowCullDistanceVolume the engine consults UWorld::UpdateCullDistanceVolumes
// (PrimitiveComponent.cpp:1603-1626), whose answer depends on what volumes the test map
// happens to contain. Movable takes the unconditional `NewCachedMaxDrawDistance =
// LDMaxDrawDistance` branch, so the assertion is about the notification and nothing else.
//
// Actors are placed with FScopedEditorWorldActorGuard and are NOT RF_Transient - transient
// actors are invisible to UEditorActorSubsystem::GetAllLevelActors
// (EditorActorSubsystem.cpp:386), which every actor.* verb walks, so the verb would report
// the probe missing.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightComponentNotifyTest
{
    const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Trailing 'X' so FActorLabelUtilities::SplitActorLabel cannot strip a numeric tail
    // and uniquify the label away from what the test asks for.
    FString MakeProbeLabel()
    {
        return FString::Printf(TEXT("PWNotify_%sX"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    AStaticMeshActor* SpawnMovableCubeProbe(UWorld* World, const FString& Label)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
        if (!World || !Cube)
        {
            return nullptr;
        }
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
        Component->SetMobility(EComponentMobility::Movable);
        Component->SetStaticMesh(Cube);
        Actor->SetActorLabel(Label);
        return Actor;
    }

    TSharedPtr<FJsonObject> MakePayload(const FString& ActorLabel,
                                        const FString& ComponentName,
                                        const FString& PropertyName,
                                        const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetField(PropertyName, Value);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorLabel);
        Payload->SetStringField(TEXT("componentName"), ComponentName);
        Payload->SetObjectField(TEXT("properties"), Properties);
        return Payload;
    }
}

// ============================================================================
// The reproduction, as a test
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetComponentPropertiesRunsEngineNotificationTest,
    "PinWright.actor.set_component_properties.ReflectionWriteRunsEngineNotification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetComponentPropertiesRunsEngineNotificationTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightComponentNotifyTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping ReflectionWriteRunsEngineNotification."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel();
    AStaticMeshActor* Probe = SpawnMovableCubeProbe(World, Label);
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the movable cube probe actor."));
        return false;
    }
    UStaticMeshComponent* Component = Probe->GetStaticMeshComponent();

    // Guards the post-call assertion against a value that was already there: a probe that
    // somehow started at 4321.0f would make the test pass without any notification.
    TestEqual(TEXT("the probe starts with no cull distance"),
        Component->CachedMaxDrawDistance, 0.0f);

    const float Requested = 4321.0f;
    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.set_component_properties is registered"),
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
            MakePayload(Label, Component->GetName(), TEXT("LDMaxDrawDistance"),
                MakeShared<FJsonValueNumber>(Requested)),
            Capture));
    TestTrue(TEXT("the write succeeds"), Capture.bSuccess);
    TestTrue(TEXT("LDMaxDrawDistance is reported applied"),
        JsonStringArrayContains(Capture.Result, TEXT("applied"), TEXT("LDMaxDrawDistance")));

    // The half that always worked.
    TestEqual(TEXT("the field holds the requested value"),
        Component->LDMaxDrawDistance, Requested);

    // The half that did not. Only UPrimitiveComponent::PostEditChangeProperty copies
    // LDMaxDrawDistance into CachedMaxDrawDistance; a raw store leaves this at 0.
    TestEqual(TEXT("the engine re-derived the cull distance the renderer uses"),
        Component->CachedMaxDrawDistance, Requested);

    // The response must say the notification ran, not leave the caller to assume it.
    TestTrue(TEXT("LDMaxDrawDistance is reported notified"),
        JsonStringArrayContains(Capture.Result, TEXT("notified"), TEXT("LDMaxDrawDistance")));

    return true;
}

// ============================================================================
// The engine-setter paths must NOT be double-notified
// ============================================================================
//
// Mobility, SimulatePhysics, StaticMesh and SkinnedAsset are routed through the engine's
// own typed setters, which are supersets of the notification (docs/rpc-design.md 5c). If
// one of those ever started appearing in `notified` it would mean the raw reflection path
// had swallowed it - the exact regression Utils/ComponentAssetPropertyWrite.h exists to
// prevent - so this asserts the absence rather than trusting the branch order.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetComponentPropertiesSetterPathNotNotifiedTest,
    "PinWright.actor.set_component_properties.EngineSetterPathIsNotNotified",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetComponentPropertiesSetterPathNotNotifiedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightComponentNotifyTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping EngineSetterPathIsNotNotified."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel();
    AStaticMeshActor* Probe = SpawnMovableCubeProbe(World, Label);
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the movable cube probe actor."));
        return false;
    }
    UStaticMeshComponent* Component = Probe->GetStaticMeshComponent();
    Component->SetMobility(EComponentMobility::Static);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.set_component_properties is registered"),
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
            MakePayload(Label, Component->GetName(), TEXT("Mobility"),
                MakeShared<FJsonValueString>(TEXT("Movable"))),
            Capture));
    TestTrue(TEXT("the write succeeds"), Capture.bSuccess);
    TestTrue(TEXT("Mobility is reported applied"),
        JsonStringArrayContains(Capture.Result, TEXT("applied"), TEXT("Mobility")));
    TestEqual(TEXT("the engine setter moved the component"),
        static_cast<int32>(Component->Mobility.GetValue()),
        static_cast<int32>(EComponentMobility::Movable));
    TestFalse(TEXT("Mobility is NOT reported notified - the setter is the superset"),
        JsonStringArrayContains(Capture.Result, TEXT("notified"), TEXT("Mobility")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
