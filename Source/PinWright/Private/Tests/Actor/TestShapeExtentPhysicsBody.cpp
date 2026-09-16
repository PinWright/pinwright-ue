// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests that actor.set_component_properties leaves a shape component's LIVE PHYSICS BODY
// describing the extent it just wrote - not only the field, the bounds and the renderer.
//
// THE DEFECT THESE PIN. The handler stored the extent by reflection, fired
// UShapeComponent::PostEditChangeProperty, and committed with MarkRenderStateDirty() +
// UpdateComponentToWorld(). Every one of those moves a representation that is NOT the one
// scene queries read. Measured on a TriggerBox: BoxExtent 40 -> 400 returned
// {"applied":["BoxExtent"],"notified":["BoxExtent"]}, actor.get_bounding_box returned 400,
// a fixed-pose capture showed the wireframe grow from 46 px to filling a 768 px frame - and
// a raycast at the new face came back BYTE-IDENTICAL to the pre-write baseline. Shrinking
// was worse: the body stayed LARGER than the drawing, so a ray 150 cm outside the visible
// box still blocked - an invisible collider standing in empty space.
//
// Both instruments a careful caller reaches for - the picture and the bounding box - confirm
// the write. The one that would catch it is the one nobody runs because the others said yes.
// That is what these tests run.
//
// HOW THEY MEASURE IT. UPrimitiveComponent::OverlapComponent forwards straight to
// FBodyInstance::OverlapTest (PrimitiveComponent.cpp:3937-3946), so it asks the live Chaos
// shapes and nothing else - no collision channel, no profile, no world scene state to make
// the result host-dependent. A 1 cm probe sphere placed OUTSIDE the old extent and INSIDE
// the new one therefore answers exactly one question: which extent is the body built from.
// On the pre-fix handler the grow case reports "no overlap" and fails for the right reason.
//
// AND WHY IT IS NOT ONLY A RAYCAST PROBLEM. spatial.find_clear_placement measures occupancy
// through SpatialTraceUtils::ProbeFootprintOccupancy, which is UWorld::OverlapMultiByChannel
// over the same bodies - so a stale body makes that verb silently answer for the OLD
// footprint. The second test drives that helper directly rather than asserting it by proxy.
//
// Probe actors are placed with FScopedEditorWorldActorGuard and are NOT RF_Transient:
// transient actors are invisible to UEditorActorSubsystem::GetAllLevelActors
// (EditorActorSubsystem.cpp:386), which every actor.* verb walks, so the verb under test
// would report the probe missing.

#include "Misc/AutomationTest.h"

#include "Handlers/Spatial/SpatialTraceUtils.h"

#include "CollisionShape.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightShapeExtentTest
{
    // Far enough above the open map that nothing else is in the probed volume. Only the
    // footprint-occupancy test can see other actors at all (OverlapComponent asks one body),
    // and that one also filters to the probe actor - this is belt and braces.
    const FVector ProbeOrigin(0.0, 0.0, 10000.0);

    UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Trailing 'X' so FActorLabelUtilities::SplitActorLabel cannot strip a numeric tail and
    // uniquify the label away from what the test asks for - same reason as
    // MakeLabelResolutionLabel in TestActorLabelResolution.cpp.
    FString MakeProbeLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%sX"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A level actor whose ROOT is a freshly registered shape component. The size is set
    // through the ENGINE SETTER before RegisterComponent, so the probe starts with field and
    // body in agreement: a baseline assertion that passed because both halves were wrong
    // would prove nothing.
    template <typename ShapeType, typename SizeSetter>
    AActor* SpawnShapeProbe(UWorld* World, const FString& Label, const TCHAR* ComponentName,
                            SizeSetter&& ApplyBaselineSize, ShapeType*& OutShape)
    {
        OutShape = nullptr;
        if (!World)
        {
            return nullptr;
        }
        AActor* Actor = World->SpawnActor<AActor>(
            AActor::StaticClass(), ProbeOrigin, FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        ShapeType* Shape = NewObject<ShapeType>(Actor, ComponentName, RF_Transactional);
        if (!Shape)
        {
            return nullptr;
        }
        Actor->SetRootComponent(Shape);
        Actor->AddInstanceComponent(Shape);
        Shape->OnComponentCreated();
        ApplyBaselineSize(Shape);
        Shape->RegisterComponent();
        Actor->SetActorLabel(Label);
        OutShape = Shape;
        return Actor;
    }

    // "Is the live body wide enough to reach this point?" - the whole measurement. Asks the
    // component's own FBodyInstance, so nothing about the level, the channel setup or the
    // collision profile can change the answer.
    bool BodyReaches(const UPrimitiveComponent* Shape, const FVector& WorldOffset)
    {
        return Shape && Shape->OverlapComponent(Shape->GetComponentLocation() + WorldOffset,
                                                FQuat::Identity,
                                                FCollisionShape::MakeSphere(1.0f));
    }

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

    TSharedPtr<FJsonValue> MakeVectorValue(double Uniform)
    {
        TSharedPtr<FJsonObject> Vector = MakeShared<FJsonObject>();
        Vector->SetNumberField(TEXT("X"), Uniform);
        Vector->SetNumberField(TEXT("Y"), Uniform);
        Vector->SetNumberField(TEXT("Z"), Uniform);
        return MakeShared<FJsonValueObject>(Vector);
    }

    bool ResponseListsIn(const FTestResponseCapture& Capture, const TCHAR* ArrayField,
                         const FString& PropertyName)
    {
        if (!Capture.Result.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!Capture.Result->TryGetArrayField(ArrayField, Entries) || !Entries)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Entry : *Entries)
        {
            if (Entry.IsValid() && Entry->AsString() == PropertyName)
            {
                return true;
            }
        }
        return false;
    }

    // Runs the verb and reports whether it succeeded, so each test states the write once.
    bool WriteProperty(const FString& Label, const FString& ComponentName,
                       const FString& PropertyName, const TSharedPtr<FJsonValue>& Value,
                       FTestResponseCapture& OutCapture)
    {
        return InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
                   MakeSetPropertiesPayload(Label, ComponentName, PropertyName, Value), OutCapture)
            && OutCapture.bSuccess;
    }
}

// ============================================================================
// The reproduction, as a test
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShapeExtentUpdatesPhysicsBodyTest,
    "PinWright.actor.set_component_properties.BoxExtentUpdatesThePhysicsBody",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShapeExtentUpdatesPhysicsBodyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightShapeExtentTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the probe cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel(TEXT("PWShapeExtent"));
    UBoxComponent* Box = nullptr;
    AActor* Probe = SpawnShapeProbe<UBoxComponent>(World, Label, TEXT("PWShapeExtentProbeBox"),
        [](UBoxComponent* Component) { Component->SetBoxExtent(FVector(40.0)); }, Box);
    if (!Probe || !Box)
    {
        AddError(TEXT("Failed to spawn the box probe actor."));
        return false;
    }
    if (!Box->IsPhysicsStateCreated())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shape-physics-state-not-created"),
            TEXT("The registered UBoxComponent has no physics state, so there is no live body to measure."));
        return true;
    }

    // 200 cm out: outside the 40 baseline, inside the 400 the test is about to ask for.
    const FVector Outside40Inside400(200.0, 0.0, 0.0);
    TestFalse(TEXT("the baseline body does not reach 200 cm"), BodyReaches(Box, Outside40Inside400));

    FTestResponseCapture Grow;
    TestTrue(TEXT("the grow write succeeds"),
        WriteProperty(Label, Box->GetName(), TEXT("BoxExtent"), MakeVectorValue(400.0), Grow));
    TestTrue(TEXT("BoxExtent is reported applied"),
        ResponseListsIn(Grow, TEXT("applied"), TEXT("BoxExtent")));
    // Routed to the typed setter, which is a superset of the notification - so it reports as
    // applied and NOT as notified, exactly like StaticMesh and SkinnedAsset.
    TestFalse(TEXT("BoxExtent is not reported notified"),
        ResponseListsIn(Grow, TEXT("notified"), TEXT("BoxExtent")));

    // The half that was never broken: the field, and everything a reader derives from it.
    TestEqual(TEXT("the component stores the requested extent"),
        Box->GetUnscaledBoxExtent(), FVector(400.0));

    // THE REGRESSION. Pre-fix this is false: the Chaos shapes were still the 40 box.
    TestTrue(TEXT("the physics body reaches 200 cm after growing to 400"),
        BodyReaches(Box, Outside40Inside400));

    // The worse direction. A body left at 400 while the drawing says 50 is an invisible
    // collider in empty space - the failure no capture and no bounding box can show.
    FTestResponseCapture Shrink;
    TestTrue(TEXT("the shrink write succeeds"),
        WriteProperty(Label, Box->GetName(), TEXT("BoxExtent"), MakeVectorValue(50.0), Shrink));
    TestEqual(TEXT("the component stores the shrunken extent"),
        Box->GetUnscaledBoxExtent(), FVector(50.0));
    TestFalse(TEXT("the physics body no longer reaches 200 cm after shrinking to 50"),
        BodyReaches(Box, Outside40Inside400));

    return true;
}

// ============================================================================
// The verb that would have measured the stale body
// ============================================================================

// spatial.find_clear_placement decides whether a pose is free by calling
// SpatialTraceUtils::ProbeFootprintOccupancy, which is UWorld::OverlapMultiByChannel against
// the same live bodies. A stale body therefore does not merely mis-report a raycast: it makes
// a placement search answer for the OLD footprint - reporting clear space that is occupied, or
// an obstacle that is no longer there. This drives that helper rather than inferring it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShapeExtentReachesFootprintOccupancyTest,
    "PinWright.actor.set_component_properties.ShapeExtentReachesFootprintOccupancy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShapeExtentReachesFootprintOccupancyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightShapeExtentTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the probe cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel(TEXT("PWShapeFootprint"));
    UBoxComponent* Box = nullptr;
    AActor* Probe = SpawnShapeProbe<UBoxComponent>(World, Label, TEXT("PWShapeFootprintProbeBox"),
        [](UBoxComponent* Component) { Component->SetBoxExtent(FVector(40.0)); }, Box);
    if (!Probe || !Box)
    {
        AddError(TEXT("Failed to spawn the box probe actor."));
        return false;
    }
    if (!Box->IsPhysicsStateCreated())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shape-physics-state-not-created"),
            TEXT("The registered UBoxComponent has no physics state, so there is no live body to measure."));
        return true;
    }

    // A small footprint 200 cm off the probe centre, filtered to the probe actor by pointer
    // identity so nothing else in the open map can decide the verdict.
    SpatialTraceUtils::FSpatialFootprintProbe Footprint;
    Footprint.Center = Box->GetComponentLocation() + FVector(200.0, 0.0, 0.0);
    Footprint.HalfExtent = FVector(10.0);
    Footprint.Filter.OnlyActors.Add(Probe);

    TArray<SpatialTraceUtils::FSpatialOccupant> Occupants;
    TestFalse(TEXT("the baseline 40 box does not occupy the probed footprint"),
        SpatialTraceUtils::ProbeFootprintOccupancy(World, Footprint, /*InflateXYCm*/ 0.0, Occupants));

    FTestResponseCapture Grow;
    TestTrue(TEXT("the grow write succeeds"),
        WriteProperty(Label, Box->GetName(), TEXT("BoxExtent"), MakeVectorValue(400.0), Grow));

    // THE REGRESSION, at the verb that consumes it. Pre-fix the overlap resolves against the
    // 40 box and find_clear_placement calls occupied space clear.
    TestTrue(TEXT("the grown box occupies the probed footprint"),
        SpatialTraceUtils::ProbeFootprintOccupancy(World, Footprint, /*InflateXYCm*/ 0.0, Occupants));
    TestTrue(TEXT("the occupant is the probe's own box component"),
        Occupants.Num() == 1 && Occupants[0].Component.Get() == Box);

    return true;
}

// ============================================================================
// The rest of the class: sphere and capsule
// ============================================================================

// Same defect, same fix, three more properties. Each engine setter ends in the same
// bPhysicsStateCreated -> BodyInstance.UpdateBodyScale(..., true) tail, so a fix that covered
// only UBoxComponent would leave the identical hole one class over.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSphereAndCapsuleExtentsUpdatePhysicsBodyTest,
    "PinWright.actor.set_component_properties.SphereAndCapsuleExtentsUpdateThePhysicsBody",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSphereAndCapsuleExtentsUpdatePhysicsBodyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightShapeExtentTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the probes cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;

    // ---- USphereComponent::SphereRadius ----
    {
        const FString Label = MakeProbeLabel(TEXT("PWShapeSphere"));
        USphereComponent* Sphere = nullptr;
        AActor* Probe = SpawnShapeProbe<USphereComponent>(World, Label, TEXT("PWShapeProbeSphere"),
            [](USphereComponent* Component) { Component->SetSphereRadius(40.0f); }, Sphere);
        if (!Probe || !Sphere)
        {
            AddError(TEXT("Failed to spawn the sphere probe actor."));
            return false;
        }
        if (!Sphere->IsPhysicsStateCreated())
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("shape-physics-state-not-created"),
                TEXT("The registered USphereComponent has no physics state, so there is no live body to measure."));
            return true;
        }

        const FVector Outside40Inside400(200.0, 0.0, 0.0);
        TestFalse(TEXT("the baseline sphere body does not reach 200 cm"),
            BodyReaches(Sphere, Outside40Inside400));

        FTestResponseCapture Capture;
        TestTrue(TEXT("the sphere radius write succeeds"),
            WriteProperty(Label, Sphere->GetName(), TEXT("SphereRadius"),
                MakeShared<FJsonValueNumber>(400.0), Capture));
        TestTrue(TEXT("SphereRadius is reported applied"),
            ResponseListsIn(Capture, TEXT("applied"), TEXT("SphereRadius")));
        TestEqual(TEXT("the component stores the requested radius"),
            Sphere->GetUnscaledSphereRadius(), 400.0f);
        TestTrue(TEXT("the sphere physics body reaches 200 cm after growing to 400"),
            BodyReaches(Sphere, Outside40Inside400));
    }

    // ---- UCapsuleComponent::CapsuleHalfHeight / CapsuleRadius ----
    {
        const FString Label = MakeProbeLabel(TEXT("PWShapeCapsule"));
        UCapsuleComponent* Capsule = nullptr;
        AActor* Probe = SpawnShapeProbe<UCapsuleComponent>(World, Label, TEXT("PWShapeProbeCapsule"),
            [](UCapsuleComponent* Component) { Component->SetCapsuleSize(20.0f, 40.0f); }, Capsule);
        if (!Probe || !Capsule)
        {
            AddError(TEXT("Failed to spawn the capsule probe actor."));
            return false;
        }
        if (!Capsule->IsPhysicsStateCreated())
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("shape-physics-state-not-created"),
                TEXT("The registered UCapsuleComponent has no physics state, so there is no live body to measure."));
            return true;
        }

        const FVector AboveBaselineHalfHeight(0.0, 0.0, 200.0);
        const FVector OutsideBaselineRadius(150.0, 0.0, 0.0);
        TestFalse(TEXT("the baseline capsule body does not reach 200 cm up"),
            BodyReaches(Capsule, AboveBaselineHalfHeight));

        FTestResponseCapture HalfHeight;
        TestTrue(TEXT("the capsule half-height write succeeds"),
            WriteProperty(Label, Capsule->GetName(), TEXT("CapsuleHalfHeight"),
                MakeShared<FJsonValueNumber>(400.0), HalfHeight));
        TestEqual(TEXT("the component stores the requested half-height"),
            Capsule->GetUnscaledCapsuleHalfHeight(), 400.0f);
        TestTrue(TEXT("the capsule physics body reaches 200 cm up after growing to 400"),
            BodyReaches(Capsule, AboveBaselineHalfHeight));

        TestFalse(TEXT("the capsule body does not yet reach 150 cm sideways"),
            BodyReaches(Capsule, OutsideBaselineRadius));

        FTestResponseCapture Radius;
        TestTrue(TEXT("the capsule radius write succeeds"),
            WriteProperty(Label, Capsule->GetName(), TEXT("CapsuleRadius"),
                MakeShared<FJsonValueNumber>(200.0), Radius));
        TestEqual(TEXT("the component stores the requested radius"),
            Capsule->GetUnscaledCapsuleRadius(), 200.0f);
        TestTrue(TEXT("the capsule physics body reaches 150 cm sideways after growing to 200"),
            BodyReaches(Capsule, OutsideBaselineRadius));
    }

    return true;
}

// ============================================================================
// The routing boundary
// ============================================================================

// Two halves of one contract: the size properties must be intercepted, and NOTHING else on a
// shape component may be. A predicate that widened to "any property on a UShapeComponent"
// would strip the change notification off every other field the class owns.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShapeExtentRoutingBoundaryTest,
    "PinWright.actor.set_component_properties.ShapeExtentRoutingBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FShapeExtentRoutingBoundaryTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightShapeExtentTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the probe cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel(TEXT("PWShapeRoute"));
    UBoxComponent* Box = nullptr;
    AActor* Probe = SpawnShapeProbe<UBoxComponent>(World, Label, TEXT("PWShapeRouteProbeBox"),
        [](UBoxComponent* Component) { Component->SetBoxExtent(FVector(40.0)); }, Box);
    if (!Probe || !Box)
    {
        AddError(TEXT("Failed to spawn the box probe actor."));
        return false;
    }

    // An ordinary property on the very same component must fall through to the generic
    // reflection + notification path, so it appears in `notified`.
    FTestResponseCapture Capture;
    TestTrue(TEXT("the ordinary property write succeeds"),
        WriteProperty(Label, Box->GetName(), TEXT("bDrawOnlyIfSelected"),
            MakeShared<FJsonValueBoolean>(true), Capture));
    TestTrue(TEXT("an ordinary shape property is reported applied"),
        ResponseListsIn(Capture, TEXT("applied"), TEXT("bDrawOnlyIfSelected")));
    TestTrue(TEXT("an ordinary shape property still runs the engine notification"),
        ResponseListsIn(Capture, TEXT("notified"), TEXT("bDrawOnlyIfSelected")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
