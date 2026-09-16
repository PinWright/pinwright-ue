// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the shadow-copy property class: component UPROPERTYs whose value the engine
// mirrors into a PRIVATE cached copy and ensures on when the two diverge.
//
// THE DEFECT THESE PIN. actor.set_component_properties wrote every property through
// ApplyJsonValueToProperty - a raw reflection store - and then committed with
// MarkRenderStateDirty() + UpdateComponentToWorld(). For UStaticMeshComponent::StaticMesh
// that store leaves KnownStaticMesh pointing at the OLD mesh, so the very next bounds
// update raises, on the production map:
//
//   Ensure condition failed: KnownStaticMesh == StaticMesh   [StaticMeshComponent.cpp:744]
//   StaticMesh property overwritten for component HierarchicalInstancedStaticMeshComponent
//   ...PersistentLevel.Actor_6.HISM_Foliage_Conifer without a call to
//   NotifyIfStaticMeshChanged().
//
// with UnrealEditor-PinWright.dll!AutoHandler_332_ (ComponentHandler.cpp) two frames below
// the ensure. The component's render, streaming and physics state then describes a
// different mesh from the one the package serializes.
//
// HOW THEY DETECT IT WITHOUT DEPENDING ON THE ENSURE. The divergence is not directly
// observable: KnownStaticMesh is private, and every public reader
// (UStaticMeshComponent::GetStaticMesh, StaticMeshComponent.h:455-460) REPAIRS it on the
// way past, so a test cannot see the stale state twice. What is observable is the
// notification itself: UStaticMeshComponent::OnStaticMeshChanged() (StaticMeshComponent.h:946)
// is broadcast by SetStaticMesh and by PostEditChangeProperty, and by nothing else. A raw
// reflection store broadcasts zero times. Counting that delegate is therefore an exact,
// deterministic proxy for "the engine's own change path ran", and it fails on the
// pre-fix code for the right reason rather than by catching a log line.
//
// Actors are placed with FScopedEditorWorldActorGuard, non-transient (RF_Transient actors
// are invisible to UEditorActorSubsystem::GetAllLevelActors, EditorActorSubsystem.cpp:386,
// so every actor.* verb would report the probe missing).

#include "Misc/AutomationTest.h"

#include "Utils/ComponentAssetPropertyWrite.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
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

namespace PinWrightComponentAssetWriteTest
{
    const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
    const TCHAR* const SpherePath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
    // A real asset that is NOT a UStaticMesh, for the wrong-class case.
    const TCHAR* const MaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

    UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Trailing 'X' so FActorLabelUtilities::SplitActorLabel cannot strip a numeric tail
    // and uniquify the label away from what the test asks for - same reason as
    // MakeLabelResolutionLabel in TestActorLabelResolution.cpp.
    FString MakeProbeLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%sX"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    AStaticMeshActor* SpawnCubeProbe(UWorld* World, const FString& Label)
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
        Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
        Actor->SetActorLabel(Label);
        return Actor;
    }

    // A level actor whose ROOT is a HISM carrying InstanceCount instances - the exact
    // shape the production ensure was raised on (a foliage HISM with thousands of
    // instances), minus the scale.
    AActor* SpawnHismProbe(UWorld* World, const FString& Label, int32 InstanceCount,
                           UHierarchicalInstancedStaticMeshComponent*& OutHism)
    {
        OutHism = nullptr;
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
        if (!World || !Cube)
        {
            return nullptr;
        }
        AActor* Actor = World->SpawnActor<AActor>(
            AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        UHierarchicalInstancedStaticMeshComponent* Hism =
            NewObject<UHierarchicalInstancedStaticMeshComponent>(
                Actor, TEXT("HISM_AssetWriteProbe"), RF_Transactional);
        if (!Hism)
        {
            return nullptr;
        }
        Actor->SetRootComponent(Hism);
        Actor->AddInstanceComponent(Hism);
        Hism->OnComponentCreated();
        Hism->SetStaticMesh(Cube);
        Hism->RegisterComponent();
        for (int32 Index = 0; Index < InstanceCount; ++Index)
        {
            Hism->AddInstance(FTransform(FVector(Index * 200.0, 0.0, 0.0)));
        }
        Actor->SetActorLabel(Label);
        OutHism = Hism;
        return Actor;
    }

    // Counts UStaticMeshComponent::OnStaticMeshChanged broadcasts for one component and
    // unsubscribes on scope exit, so a failing test cannot leave a dangling handle on a
    // component the world guard is about to destroy.
    class FStaticMeshChangeCounter
    {
    public:
        explicit FStaticMeshChangeCounter(UStaticMeshComponent* InComponent)
            : Component(InComponent)
        {
            if (Component)
            {
                Handle = Component->OnStaticMeshChanged().AddLambda(
                    [this](UStaticMeshComponent*) { ++Count; });
            }
        }

        ~FStaticMeshChangeCounter()
        {
            if (Component && Handle.IsValid())
            {
                Component->OnStaticMeshChanged().Remove(Handle);
            }
        }

        int32 Get() const { return Count; }

    private:
        UStaticMeshComponent* Component = nullptr;
        FDelegateHandle Handle;
        int32 Count = 0;
    };

    TSharedPtr<FJsonObject> MakeSetPropertiesPayload(const FString& ActorLabel,
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

    bool ResponseListsApplied(const FTestResponseCapture& Capture, const FString& PropertyName)
    {
        if (!Capture.Result.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Applied = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("applied"), Applied) || !Applied)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Entry : *Applied)
        {
            if (Entry.IsValid() && Entry->AsString() == PropertyName)
            {
                return true;
            }
        }
        return false;
    }

    bool ResponseWarnsAbout(const FTestResponseCapture& Capture, const FString& Needle)
    {
        if (!Capture.Result.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Entry : *Warnings)
        {
            if (Entry.IsValid() && Entry->AsString().Contains(Needle))
            {
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// The reproduction, as a test
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComponentStaticMeshWriteNotifiesEngineTest,
    "PinWright.actor.set_component_properties.StaticMeshRunsEngineNotification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComponentStaticMeshWriteNotifiesEngineTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightComponentAssetWriteTest;

    UWorld* World = EditorWorld();
    UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, SpherePath);
    if (!World || !Sphere)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No editor world or engine Sphere asset; skipping StaticMeshRunsEngineNotification."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel(TEXT("PWAssetWrite"));
    AStaticMeshActor* Probe = SpawnCubeProbe(World, Label);
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the static-mesh probe actor."));
        return false;
    }
    UStaticMeshComponent* Component = Probe->GetStaticMeshComponent();

    FStaticMeshChangeCounter Counter(Component);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.set_component_properties is registered"),
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
            MakeSetPropertiesPayload(Label, Component->GetName(), TEXT("StaticMesh"),
                MakeShared<FJsonValueString>(SpherePath)),
            Capture));
    TestTrue(TEXT("the write succeeds"), Capture.bSuccess);
    TestTrue(TEXT("StaticMesh is reported applied"),
        ResponseListsApplied(Capture, TEXT("StaticMesh")));

    // Engine state, not the verb's return value.
    TestTrue(TEXT("the component now holds the requested mesh"),
        Component->GetStaticMesh() == Sphere);

    // THE REGRESSION. A raw reflection store broadcasts nothing; only the engine's own
    // setter (or PostEditChangeProperty) does. Exactly one broadcast means the
    // notification ran once and was not double-applied.
    TestEqual(TEXT("the engine's static-mesh change notification fired exactly once"),
        Counter.Get(), 1);

    return true;
}

// ============================================================================
// The production shape: a HISM with instances
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComponentHismStaticMeshWriteKeepsInstancesTest,
    "PinWright.actor.set_component_properties.HismStaticMeshKeepsInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComponentHismStaticMeshWriteKeepsInstancesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightComponentAssetWriteTest;

    UWorld* World = EditorWorld();
    UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, SpherePath);
    if (!World || !Sphere)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No editor world or engine Sphere asset; skipping HismStaticMeshKeepsInstances."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel(TEXT("PWAssetWriteHism"));
    UHierarchicalInstancedStaticMeshComponent* Hism = nullptr;
    AActor* Probe = SpawnHismProbe(World, Label, /*InstanceCount*/ 4, Hism);
    if (!Probe || !Hism)
    {
        AddError(TEXT("Failed to spawn the HISM probe actor."));
        return false;
    }
    const int32 InstancesBefore = Hism->GetInstanceCount();
    TestEqual(TEXT("the probe starts with four instances"), InstancesBefore, 4);

    FStaticMeshChangeCounter Counter(Hism);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.set_component_properties is registered"),
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
            MakeSetPropertiesPayload(Label, Hism->GetName(), TEXT("StaticMesh"),
                MakeShared<FJsonValueString>(SpherePath)),
            Capture));
    TestTrue(TEXT("the write succeeds"), Capture.bSuccess);

    TestTrue(TEXT("the HISM now holds the requested mesh"), Hism->GetStaticMesh() == Sphere);
    TestEqual(TEXT("the engine's static-mesh change notification fired exactly once"),
        Counter.Get(), 1);
    // A mesh swap must not cost the caller its instances - the failure mode that would
    // make this verb unusable on foliage even after the ensure was silenced.
    TestEqual(TEXT("every instance survives the mesh swap"),
        Hism->GetInstanceCount(), InstancesBefore);

    return true;
}

// ============================================================================
// Refusals - the write must not half-land
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComponentStaticMeshWriteRefusalsTest,
    "PinWright.actor.set_component_properties.StaticMeshRefusalsLeaveMeshIntact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComponentStaticMeshWriteRefusalsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightComponentAssetWriteTest;

    UWorld* World = EditorWorld();
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
    if (!World || !Cube)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No editor world or engine Cube asset; skipping StaticMeshRefusalsLeaveMeshIntact."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel(TEXT("PWAssetWriteBad"));
    AStaticMeshActor* Probe = SpawnCubeProbe(World, Label);
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the static-mesh probe actor."));
        return false;
    }
    UStaticMeshComponent* Component = Probe->GetStaticMeshComponent();

    // (1) A path that loads nothing. The importer's own error text is preserved, so this
    //     reads identically to any other failed property write.
    {
        FStaticMeshChangeCounter Counter(Component);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
            MakeSetPropertiesPayload(Label, Component->GetName(), TEXT("StaticMesh"),
                MakeShared<FJsonValueString>(TEXT("/Game/PinWrightProbe/NoSuchMesh.NoSuchMesh"))),
            Capture);
        TestTrue(TEXT("an unloadable mesh path warns"),
            ResponseWarnsAbout(Capture, TEXT("StaticMesh")));
        TestFalse(TEXT("an unloadable mesh path is not reported applied"),
            ResponseListsApplied(Capture, TEXT("StaticMesh")));
        TestTrue(TEXT("the mesh is untouched by a failed write"),
            Component->GetStaticMesh() == Cube);
        TestEqual(TEXT("no change notification fires for a failed write"), Counter.Get(), 0);
    }

    // (2) A real asset of the wrong class. The reflection store performs no class check
    //     of its own, so without the guard in ApplyComponentAssetProperty this either
    //     stored a UMaterial in a UStaticMesh slot or silently CLEARED the mesh. Neither
    //     is what the caller asked for, so both must read as a refusal.
    {
        FStaticMeshChangeCounter Counter(Component);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
            MakeSetPropertiesPayload(Label, Component->GetName(), TEXT("StaticMesh"),
                MakeShared<FJsonValueString>(MaterialPath)),
            Capture);
        TestFalse(TEXT("a wrong-class asset is not reported applied"),
            ResponseListsApplied(Capture, TEXT("StaticMesh")));
        TestTrue(TEXT("a wrong-class asset is not silently swallowed"),
            ResponseWarnsAbout(Capture, TEXT("StaticMesh")));
        TestTrue(TEXT("the mesh is not cleared by a wrong-class value"),
            Component->GetStaticMesh() == Cube);
        TestEqual(TEXT("no change notification fires for a wrong-class value"), Counter.Get(), 0);
    }

    return true;
}

// ============================================================================
// The response has to admit what it did not do
// ============================================================================

// A second defect found in the same handler: PropertyWarnings was collected and then
// dropped on the floor. actor.set_component_properties' own registered summary says
// "Per-property failures returned in 'warnings'", and actor.add_component emits exactly
// that array - but the set verb built the response with `applied` only. An unknown or
// unconvertible property therefore came back as an unqualified success with a shorter
// `applied` list, which a caller cannot distinguish from having asked for less.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComponentSetPropertiesReportsFailuresTest,
    "PinWright.actor.set_component_properties.PerPropertyFailuresAreReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComponentSetPropertiesReportsFailuresTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightComponentAssetWriteTest;

    UWorld* World = EditorWorld();
    if (!World || !LoadObject<UStaticMesh>(nullptr, CubePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No editor world or engine Cube asset; skipping PerPropertyFailuresAreReported."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel(TEXT("PWSetPropsWarn"));
    AStaticMeshActor* Probe = SpawnCubeProbe(World, Label);
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the static-mesh probe actor."));
        return false;
    }
    UStaticMeshComponent* Component = Probe->GetStaticMeshComponent();

    // One property that exists and one that does not, in a single call: the partial
    // outcome is exactly the case the dropped array made unreportable.
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetBoolField(TEXT("CastShadow"), false);
    Properties->SetNumberField(TEXT("PinWrightNoSuchProperty"), 1);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetStringField(TEXT("componentName"), Component->GetName());
    Payload->SetObjectField(TEXT("properties"), Properties);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.set_component_properties is registered"),
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"), Payload, Capture));
    TestTrue(TEXT("a partial batch still succeeds"), Capture.bSuccess);
    TestTrue(TEXT("the property that exists is reported applied"),
        ResponseListsApplied(Capture, TEXT("CastShadow")));
    TestTrue(TEXT("the property that does not exist is reported in warnings"),
        ResponseWarnsAbout(Capture, TEXT("PinWrightNoSuchProperty")));
    TestFalse(TEXT("the unknown property is not reported applied"),
        ResponseListsApplied(Capture, TEXT("PinWrightNoSuchProperty")));

    return true;
}

// ============================================================================
// The routing boundary
// ============================================================================

// Two halves of one contract: the shadow-copy properties must be intercepted, and
// NOTHING else may be. A predicate that widened to every FObjectProperty would send
// unrelated component references down a setter that does not exist for them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComponentAssetWriteRoutingBoundaryTest,
    "PinWright.actor.set_component_properties.AssetWriteRoutingBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComponentAssetWriteRoutingBoundaryTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightComponentAssetWriteTest;

    UWorld* World = EditorWorld();
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
    if (!World || !Cube)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No editor world or engine Cube asset; skipping AssetWriteRoutingBoundary."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel(TEXT("PWAssetWriteRoute"));
    AStaticMeshActor* Probe = SpawnCubeProbe(World, Label);
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the static-mesh probe actor."));
        return false;
    }
    UStaticMeshComponent* Component = Probe->GetStaticMeshComponent();

    FProperty* MeshProperty = Component->GetClass()->FindPropertyByName(
        UStaticMeshComponent::GetMemberNameChecked_StaticMesh());
    TestNotNull(TEXT("StaticMesh resolves as a property on the component class"), MeshProperty);

    FProperty* CastShadowProperty = Component->GetClass()->FindPropertyByName(TEXT("CastShadow"));
    TestNotNull(TEXT("CastShadow resolves as a property on the component class"), CastShadowProperty);

    FString Error;
    if (CastShadowProperty)
    {
        // An ordinary property must fall through untouched, so the generic
        // ApplyJsonValueToProperty path stays the default for ~everything.
        TestTrue(TEXT("a non-shadow property is left to the generic write path"),
            PinWright::ApplyComponentAssetProperty(Component, CastShadowProperty,
                MakeShared<FJsonValueBoolean>(false), Error)
                == PinWright::EComponentAssetWrite::NotApplicable);
    }

    if (MeshProperty)
    {
        // The documented clear sentinels must clear, and must do so through the setter
        // (one broadcast), not by a raw store.
        FStaticMeshChangeCounter Counter(Component);
        TestTrue(TEXT("'None' clears the mesh through the engine setter"),
            PinWright::ApplyComponentAssetProperty(Component, MeshProperty,
                MakeShared<FJsonValueString>(TEXT("None")), Error)
                == PinWright::EComponentAssetWrite::Applied);
        TestNull(TEXT("the component holds no mesh after the clear"),
            Component->GetStaticMesh().Get());
        TestEqual(TEXT("the clear notified the engine exactly once"), Counter.Get(), 1);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
