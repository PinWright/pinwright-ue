// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests that a collision write through actor.set_component_properties reaches the objects
// that actually decide collision for an ISM/HISM - the PER-INSTANCE FBodyInstances - and
// that the verb reports what those bodies measure rather than what the template says.
//
// THE DEFECT THESE PIN. A UInstancedStaticMeshComponent's own inherited BodyInstance owns no
// shapes: it is a template copied into each instance body once, at creation
// (InstancedMeshComponentBodies.cpp:105), and never consulted again. Setting ECC_Pawn ->
// Ignore on 12 HISM/foliage-ISM components of /Game/Maps/PW_VegetationTest (5527 instances)
// returned OK 12/12 with a REAL read-back of ECR_Ignore off
// GetCollisionResponseToChannel(ECC_Pawn) and a profile that flipped BlockAllDynamic ->
// Custom - and a pawn-profile capsule sweep taken immediately afterwards still named 7 of
// those 12 as blocking hits. Board ticket B-collision-write-skips-instance-bodies.
//
// WHY THEY READ THE BODIES DIRECTLY. The usual defence - write it, then read it back - is
// the one that fails hardest here, because the read walks to the same memory the write did
// and that memory is the half that was right. Asserting through the component, through the
// RPC response, or through the profile name would all pass on the broken build. So these
// tests walk UInstancedStaticMeshComponent::GetInstanceBodies() and ask each FBodyInstance
// for its own collision state.
//
// The fixture is built directly because no RPC in this plugin makes an ISM, and its collision
// is stated explicitly rather than inherited from a component default: the point is that the
// instance bodies start out BLOCKING, so a failure cannot be ambiguous between "the write is
// blind" and "the fixture had nothing to change".
//
// Probe actors are placed with FScopedEditorWorldActorGuard and are NOT RF_Transient:
// transient actors are invisible to UEditorActorSubsystem::GetAllLevelActors, which every
// actor.* verb walks, so the verb under test would report the probe missing.

#include "Misc/AutomationTest.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/Guid.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightInstanceBodyCollisionTest
{
    // Far above the open map so nothing else shares the volume. Nothing here traces the
    // world, but a scatter dropped on top of other fixtures is a nuisance for whatever runs
    // next in the same editor session.
    const FVector ScatterOrigin(0.0, 0.0, 12000.0);

    constexpr int32 InstanceCount = 4;

    UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Trailing 'X' so FActorLabelUtilities::SplitActorLabel cannot strip a numeric tail and
    // uniquify the label away from what the test asks for.
    FString MakeScatterLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%sX"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // An actor whose ROOT is a HISM carrying InstanceCount cube instances. Instances are
    // added AFTER RegisterComponent so each one gets a live per-instance body through
    // FInstancedMeshComponentBodies::Insert (InstancedMeshComponentBodies.cpp:221) - a
    // component registered with instances already present would batch them the same way, but
    // this order is the one actor.add_component + the scatter recipe produce.
    AActor* SpawnScatterProbe(FAutomationTestBase& Test, UWorld* World, const FString& Label,
                              UHierarchicalInstancedStaticMeshComponent*& OutHism)
    {
        OutHism = nullptr;
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!World || !Cube)
        {
            Test.AddError(TEXT("engine cube mesh unavailable for the scatter fixture"));
            return nullptr;
        }

        AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector,
                                                   FRotator::ZeroRotator);
        if (!Holder)
        {
            Test.AddError(TEXT("scatter holder actor did not spawn"));
            return nullptr;
        }

        UHierarchicalInstancedStaticMeshComponent* Hism =
            NewObject<UHierarchicalInstancedStaticMeshComponent>(
                Holder, TEXT("PWInstanceBodyCollisionScatter"), RF_Transactional);
        Holder->SetRootComponent(Hism);
        Holder->AddInstanceComponent(Hism);
        Hism->OnComponentCreated();
        Hism->SetStaticMesh(Cube);
        Hism->SetCollisionProfileName(TEXT("BlockAll"));
        Hism->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Hism->RegisterComponent();
        for (int32 Index = 0; Index < InstanceCount; ++Index)
        {
            Hism->AddInstance(FTransform(FVector(Index * 200.0, 0.0, 0.0)));
        }
        Holder->SetActorLocation(ScatterOrigin);
        Holder->SetActorLabel(Label);
        OutHism = Hism;
        return Holder;
    }

    // Valid per-instance bodies, or an empty array. This is the measurement surface for every
    // assertion below; nothing here asks the component.
    TArray<FBodyInstance*> LiveInstanceBodies(const UInstancedStaticMeshComponent* Hism)
    {
        TArray<FBodyInstance*> Live;
        if (!Hism)
        {
            return Live;
        }
        // UE 5.8 moved the per-instance body array behind GetInstanceBodies(); older engines
        // only expose the public member.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        const TArray<FBodyInstance*>& InstanceBodies = Hism->GetInstanceBodies();
#else
        const TArray<FBodyInstance*>& InstanceBodies = Hism->InstanceBodies;
#endif
        for (FBodyInstance* Body : InstanceBodies)
        {
            if (Body)
            {
                Live.Add(Body);
            }
        }
        return Live;
    }

    // How many of the per-instance bodies report the given response on the given channel.
    int32 CountBodiesRespondingWith(const TArray<FBodyInstance*>& Bodies,
                                    ECollisionChannel Channel, ECollisionResponse Response)
    {
        int32 Matches = 0;
        for (const FBodyInstance* Body : Bodies)
        {
            if (Body->GetResponseToChannel(Channel) == Response)
            {
                ++Matches;
            }
        }
        return Matches;
    }

    int32 CountBodiesWithProfile(const TArray<FBodyInstance*>& Bodies, const FName& Profile)
    {
        int32 Matches = 0;
        for (const FBodyInstance* Body : Bodies)
        {
            if (Body->GetCollisionProfileName() == Profile)
            {
                ++Matches;
            }
        }
        return Matches;
    }

    // GetCollisionEnabled(false) skips the owner-actor override deliberately: what is being
    // measured is the value the propagation put on the body, not the actor-level flag the
    // engine applies on top of it.
    int32 CountBodiesWithCollisionEnabled(const TArray<FBodyInstance*>& Bodies,
                                          ECollisionEnabled::Type Enabled)
    {
        int32 Matches = 0;
        for (const FBodyInstance* Body : Bodies)
        {
            if (Body->GetCollisionEnabled(false) == Enabled)
            {
                ++Matches;
            }
        }
        return Matches;
    }

    TSharedPtr<FJsonObject> MakeBodyInstancePayload(const FString& ActorLabel,
                                                    const FString& ComponentName,
                                                    const TSharedPtr<FJsonObject>& BodyInstanceFields)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("BodyInstance"), BodyInstanceFields);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorLabel);
        Payload->SetStringField(TEXT("componentName"), ComponentName);
        Payload->SetObjectField(TEXT("properties"), Properties);
        return Payload;
    }

    // {"CollisionResponses": {"ResponseToChannels": {"<Channel>": "<Response>"}}} - the shape
    // the importer's FStructProperty recursion reaches, one named channel at a time.
    TSharedPtr<FJsonObject> MakeChannelResponseFields(const TCHAR* ChannelName,
                                                      const TCHAR* ResponseName)
    {
        TSharedPtr<FJsonObject> Channels = MakeShared<FJsonObject>();
        Channels->SetStringField(ChannelName, ResponseName);

        TSharedPtr<FJsonObject> Responses = MakeShared<FJsonObject>();
        Responses->SetObjectField(TEXT("ResponseToChannels"), Channels);

        TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
        Fields->SetObjectField(TEXT("CollisionResponses"), Responses);
        return Fields;
    }
}

// ============================================================================
// The reproduction, as a test
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceBodiesCarryCollisionResponseTest,
    "PinWright.actor.set_component_properties.InstanceBodiesCarryCollisionResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInstanceBodiesCarryCollisionResponseTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightInstanceBodyCollisionTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the scatter cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeScatterLabel(TEXT("PWInstanceBodyResponse"));
    UHierarchicalInstancedStaticMeshComponent* Hism = nullptr;
    AActor* Probe = SpawnScatterProbe(*this, World, Label, Hism);
    if (!Probe || !Hism)
    {
        return false;
    }

    TArray<FBodyInstance*> Bodies = LiveInstanceBodies(Hism);
    if (Bodies.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-instance-bodies"),
            TEXT("The registered HISM created no per-instance bodies, so there is nothing the ")
            TEXT("physics scene would consult and nothing to measure."));
        return true;
    }

    // The baseline the field pass started from: every instance body BLOCKS the pawn channel.
    TestEqual(TEXT("every per-instance body blocks ECC_Pawn before the write"),
        CountBodiesRespondingWith(Bodies, ECC_Pawn, ECR_Block), Bodies.Num());

    FTestResponseCapture Capture;
    TestTrue(TEXT("the collision response write succeeds"),
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
            MakeBodyInstancePayload(Label, Hism->GetName(),
                MakeChannelResponseFields(TEXT("Pawn"), TEXT("ECR_Ignore"))),
            Capture) && Capture.bSuccess);

    // The corroboration that made the original defect look like a success. This passed on the
    // broken build too - it reads the template, which is the half that was always right - and
    // is asserted here only so a future change that breaks the template write is not mistaken
    // for the instance-body defect.
    TestEqual(TEXT("the component template reports ECR_Ignore on ECC_Pawn"),
        Hism->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);

    // THE REGRESSION. Pre-fix the per-instance bodies still carry the response they were
    // COPIED at creation, so this is 0 of 4 and a pawn-profile sweep still hits the scatter.
    Bodies = LiveInstanceBodies(Hism);
    TestEqual(TEXT("every per-instance body ignores ECC_Pawn after the write"),
        CountBodiesRespondingWith(Bodies, ECC_Pawn, ECR_Ignore), Bodies.Num());

    // Untouched channels must not be collateral damage: the propagation copies the whole
    // template, so a bug that overwrote the container wholesale would show up here.
    TestEqual(TEXT("every per-instance body still blocks ECC_Visibility"),
        CountBodiesRespondingWith(Bodies, ECC_Visibility, ECR_Block), Bodies.Num());

    return true;
}

// ============================================================================
// The read-back half: profile + CollisionEnabled, and what the response publishes
// ============================================================================

// A CollisionProfileName write has a second failure on top of the instance-body one: the raw
// reflection store writes the NAME without running FBodyInstance::LoadProfileData, so the
// profile's responses and CollisionEnabled are never applied and the component reports a
// profile it does not implement. This drives the profile through the engine setter and then
// asks the instance bodies what they ended up with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceBodiesFollowCollisionProfileTest,
    "PinWright.actor.set_component_properties.InstanceBodiesFollowCollisionProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInstanceBodiesFollowCollisionProfileTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightInstanceBodyCollisionTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the scatter cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeScatterLabel(TEXT("PWInstanceBodyProfile"));
    UHierarchicalInstancedStaticMeshComponent* Hism = nullptr;
    AActor* Probe = SpawnScatterProbe(*this, World, Label, Hism);
    if (!Probe || !Hism)
    {
        return false;
    }

    TArray<FBodyInstance*> Bodies = LiveInstanceBodies(Hism);
    if (Bodies.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-instance-bodies"),
            TEXT("The registered HISM created no per-instance bodies, so there is nothing the ")
            TEXT("physics scene would consult and nothing to measure."));
        return true;
    }
    const int32 BodyCount = Bodies.Num();

    TestEqual(TEXT("every per-instance body starts on the BlockAll profile"),
        CountBodiesWithProfile(Bodies, TEXT("BlockAll")), BodyCount);

    TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
    Fields->SetStringField(TEXT("CollisionProfileName"), TEXT("OverlapAll"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("the collision profile write succeeds"),
        InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
            MakeBodyInstancePayload(Label, Hism->GetName(), Fields), Capture)
        && Capture.bSuccess);

    Bodies = LiveInstanceBodies(Hism);
    // THE REGRESSION, both halves. Pre-fix the template holds the NAME with none of the
    // profile's settings loaded, and the instance bodies hold neither.
    TestEqual(TEXT("every per-instance body carries the OverlapAll profile"),
        CountBodiesWithProfile(Bodies, TEXT("OverlapAll")), BodyCount);
    TestEqual(TEXT("every per-instance body overlaps ECC_Pawn"),
        CountBodiesRespondingWith(Bodies, ECC_Pawn, ECR_Overlap), BodyCount);
    TestEqual(TEXT("every per-instance body is QueryOnly, as the profile specifies"),
        CountBodiesWithCollisionEnabled(Bodies, ECollisionEnabled::QueryOnly), BodyCount);

    // The response must publish what the BODIES measure, not what the template says, so a
    // write that fails to propagate is visible to a caller who never opens the editor.
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("the verb returned no result payload"));
        return false;
    }
    const TSharedPtr<FJsonObject>* InstanceBodies = nullptr;
    if (!Capture.Result->TryGetObjectField(TEXT("instanceBodies"), InstanceBodies) ||
        !InstanceBodies || !InstanceBodies->IsValid())
    {
        AddError(TEXT("the response carries no instanceBodies block for a collision write on a HISM"));
        return false;
    }
    int32 ReportedCount = 0;
    (*InstanceBodies)->TryGetNumberField(TEXT("count"), ReportedCount);
    TestEqual(TEXT("the response counts the per-instance bodies it measured"),
        ReportedCount, BodyCount);

    const TSharedPtr<FJsonObject>* Measured = nullptr;
    if (!(*InstanceBodies)->TryGetObjectField(TEXT("measured"), Measured) || !Measured ||
        !Measured->IsValid())
    {
        AddError(TEXT("the response omits the measured block although the bodies were inspectable"));
        return false;
    }
    FString MeasuredProfile;
    TestTrue(TEXT("the measured block names CollisionProfileName"),
        (*Measured)->TryGetStringField(TEXT("CollisionProfileName"), MeasuredProfile));
    TestEqual(TEXT("the measured profile is the one the instance bodies carry"),
        MeasuredProfile, FString(TEXT("OverlapAll")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
