// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the spatial placement RPCs:
//   spatial.place_on_surface - drop an actor onto a surface, pivot-corrected so it rests
//                              flush (restingGap ~ 0) and does not overlap the surface.
//   spatial.place_relative   - place actor B relative to anchor A by AABB arithmetic.
//   spatial.find_clear_placement - SEARCH for a pose a footprint fits in, including over
//                              ISM/HISM scatter, and attribute every rejection to a blocker.
//
// place_on_surface traces against runtime collision, so the editor world must be ticked
// after spawning the floor/actor to flush the just-registered bodies into the scene-query
// structure (same pattern as TestRaycastHandlers). place_relative is pure box math and does
// not need the flush, but is ticked too for parity.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Utils/ActorUtils.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h" // the find_clear_placement scatter fixture
#include "Dispatch/RpcDispatcher.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace
{
    TSharedPtr<FJsonObject> PlaceTestVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    UWorld* PlaceTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Editor worlds don't tick physics on their own; a just-spawned body isn't in the
    // scene-query structure until a world tick flushes it. Tick a couple of frames so an
    // immediate place_on_surface trace can see the floor/actor. Editor worlds don't
    // simulate, so nothing moves.
    void PlaceTestFlushEditorPhysics(UWorld* World)
    {
        if (!World || World->bInTick)
        {
            return;
        }
        for (int32 Iteration = 0; Iteration < 2; ++Iteration)
        {
            World->Tick(LEVELTICK_All, 1.0f / 60.0f);
        }
    }

    // Spawn a 100cm basic Cube at Location via the real actor.spawn handler and return the
    // spawned actor (or nullptr). Label is unique so cleanup targets exactly this actor.
    AActor* PlaceTestSpawnCube(FAutomationTestBase& Test, UWorld* World, const FString& Label,
        const FVector& Location)
    {
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        SpawnPayload->SetStringField(TEXT("actorName"), Label);
        SpawnPayload->SetObjectField(TEXT("location"), PlaceTestVec(Location.X, Location.Y, Location.Z));

        FTestResponseCapture SpawnCapture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        Test.TestTrue(*FString::Printf(TEXT("cube '%s' spawned"), *Label), SpawnCapture.bSuccess);
        return McpActorUtils::FindActorByName(World, Label);
    }

    FString PlaceTestLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PW_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

// (a) dropDown: a cube dropped onto a floor cube rests flush (restingGap ~ 0, not overlapping),
// and its bounds bottom lands on the floor's top face (Z=0).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceOnSurfaceDropDownTest,
    "PinWright.spatial.place_on_surface.DropDownRestsFlush",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlaceOnSurfaceDropDownTest::RunTest(const FString& Parameters)
{
    UWorld* World = PlaceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping place_on_surface dropDown test."));
        return true;
    }

    // Isolated column so no other level geometry sits under the drop.
    const double ColX = 223450.0;
    const double ColY = 167890.0;

    // Floor cube centered at Z=-50 -> top face at Z=0. Drop cube starts above at Z=200.
    const FString FloorLabel = PlaceTestLabel(TEXT("PlaceFloor"));
    const FString DropLabel = PlaceTestLabel(TEXT("PlaceDrop"));
    AActor* Floor = PlaceTestSpawnCube(*this, World, FloorLabel, FVector(ColX, ColY, -50.0));
    AActor* Drop = PlaceTestSpawnCube(*this, World, DropLabel, FVector(ColX, ColY, 200.0));
    ON_SCOPE_EXIT
    {
        if (Floor) { Floor->Destroy(); }
        if (Drop) { Drop->Destroy(); }
    };
    if (!Floor || !Drop)
    {
        return true;
    }

    // Flush both just-registered bodies into the scene-query structure.
    PlaceTestFlushEditorPhysics(World);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), DropLabel);
    Payload->SetBoolField(TEXT("dropDown"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.place_on_surface handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.place_on_surface"), Payload, Capture));
    TestTrue(TEXT("place_on_surface succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bPlaced = false;
        Capture.Result->TryGetBoolField(TEXT("placed"), bPlaced);
        TestTrue(TEXT("placed:true"), bPlaced);

        const TSharedPtr<FJsonObject>* Verification = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("verification"), Verification) && Verification)
        {
            double RestingGap = 100.0;
            (*Verification)->TryGetNumberField(TEXT("restingGap"), RestingGap);
            TestTrue(TEXT("restingGap ~ 0 (flush, not sunk or hovering)"), FMath::Abs(RestingGap) < 1.0);

            bool bOverlapping = true;
            (*Verification)->TryGetBoolField(TEXT("overlapping"), bOverlapping);
            TestFalse(TEXT("placed cube does not overlap the floor"), bOverlapping);
        }
        else
        {
            AddError(TEXT("place_on_surface result missing verification block"));
        }
    }

    // Ground truth: the dropped cube's bounds bottom rests on the floor's top face (Z=0).
    FVector Origin = FVector::ZeroVector;
    FVector Extent = FVector::ZeroVector;
    Drop->GetActorBounds(false, Origin, Extent);
    const double BottomZ = Origin.Z - Extent.Z;
    TestTrue(TEXT("dropped cube bottom rests at Z ~ 0"), FMath::Abs(BottomZ) < 1.0);

    return true;
}

// (b) place_relative on_top_of: B's bounds bottom lands on A's top face and B does not overlap A.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceRelativeOnTopOfTest,
    "PinWright.spatial.place_relative.OnTopOf",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlaceRelativeOnTopOfTest::RunTest(const FString& Parameters)
{
    UWorld* World = PlaceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping place_relative test."));
        return true;
    }

    const double ColX = 323450.0;
    const double ColY = 267890.0;

    // Anchor A centered at Z=0 (top face Z=50); B starts far above.
    const FString LabelA = PlaceTestLabel(TEXT("PlaceAnchorA"));
    const FString LabelB = PlaceTestLabel(TEXT("PlaceMoverB"));
    AActor* ActorA = PlaceTestSpawnCube(*this, World, LabelA, FVector(ColX, ColY, 0.0));
    AActor* ActorB = PlaceTestSpawnCube(*this, World, LabelB, FVector(ColX, ColY, 500.0));
    ON_SCOPE_EXIT
    {
        if (ActorA) { ActorA->Destroy(); }
        if (ActorB) { ActorB->Destroy(); }
    };
    if (!ActorA || !ActorB)
    {
        return true;
    }

    PlaceTestFlushEditorPhysics(World);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor"), LabelB);
    Payload->SetStringField(TEXT("anchor"), LabelA);
    Payload->SetStringField(TEXT("relation"), TEXT("on_top_of"));
    Payload->SetNumberField(TEXT("gap"), 0.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.place_relative handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.place_relative"), Payload, Capture));
    TestTrue(TEXT("place_relative succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bPlaced = false;
        Capture.Result->TryGetBoolField(TEXT("placed"), bPlaced);
        TestTrue(TEXT("placed:true"), bPlaced);

        const TSharedPtr<FJsonObject>* Verification = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("verification"), Verification) && Verification)
        {
            double EdgeGap = 100.0;
            (*Verification)->TryGetNumberField(TEXT("edgeGap"), EdgeGap);
            TestTrue(TEXT("edgeGap ~ 0 (B rests on A)"), FMath::Abs(EdgeGap) < 1.0);

            bool bOverlapping = true;
            (*Verification)->TryGetBoolField(TEXT("overlapping"), bOverlapping);
            TestFalse(TEXT("B does not overlap A"), bOverlapping);
        }
        else
        {
            AddError(TEXT("place_relative result missing verification block"));
        }
    }

    // Ground truth: B.min.Z == A.max.Z (=50).
    FVector OriginA, ExtentA, OriginB, ExtentB;
    ActorA->GetActorBounds(false, OriginA, ExtentA);
    ActorB->GetActorBounds(false, OriginB, ExtentB);
    const double AMaxZ = OriginA.Z + ExtentA.Z;
    const double BMinZ = OriginB.Z - ExtentB.Z;
    TestTrue(TEXT("B.min.Z ~ A.max.Z"), FMath::Abs(BMinZ - AMaxZ) < 1.0);

    return true;
}

// (c) An unknown relation is a typed INVALID_ARGUMENT error (not a fake success).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceRelativeUnknownRelationTest,
    "PinWright.spatial.place_relative.UnknownRelation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlaceRelativeUnknownRelationTest::RunTest(const FString& Parameters)
{
    UWorld* World = PlaceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping unknown-relation test."));
        return true;
    }

    const FString LabelA = PlaceTestLabel(TEXT("PlaceRelErrA"));
    const FString LabelB = PlaceTestLabel(TEXT("PlaceRelErrB"));
    AActor* ActorA = PlaceTestSpawnCube(*this, World, LabelA, FVector(423450.0, 367890.0, 0.0));
    AActor* ActorB = PlaceTestSpawnCube(*this, World, LabelB, FVector(423450.0, 367890.0, 400.0));
    ON_SCOPE_EXIT
    {
        if (ActorA) { ActorA->Destroy(); }
        if (ActorB) { ActorB->Destroy(); }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor"), LabelB);
    Payload->SetStringField(TEXT("anchor"), LabelA);
    Payload->SetStringField(TEXT("relation"), TEXT("diagonally_askew"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.place_relative"), Payload, Capture));
    TestFalse(TEXT("unknown relation is an error"), Capture.bSuccess);
    TestEqual(TEXT("unknown relation -> INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    return true;
}

// (d) Missing anchor (place_relative) and missing actor target (place_on_surface) are typed errors.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceMissingArgsTest,
    "PinWright.spatial.place.MissingArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlaceMissingArgsTest::RunTest(const FString& Parameters)
{
    // place_relative with actor but no anchor -> MISSING_REQUIRED_PARAM.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actor"), TEXT("SomeActor"));
        Payload->SetStringField(TEXT("relation"), TEXT("on_top_of"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("place_relative handler registered"),
            InvokeHandlerWithCapture(TEXT("spatial.place_relative"), Payload, Capture));
        TestFalse(TEXT("missing anchor is an error"), Capture.bSuccess);
        TestEqual(TEXT("missing anchor -> MISSING_REQUIRED_PARAM"),
            Capture.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    }

    // place_on_surface with no actor identity at all -> typed INVALID_ARGUMENT (from RequireActorName).
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("place_on_surface handler registered"),
            InvokeHandlerWithCapture(TEXT("spatial.place_on_surface"), MakeShared<FJsonObject>(), Capture));
        TestFalse(TEXT("missing actorName is an error"), Capture.bSuccess);
        TestEqual(TEXT("missing actorName -> INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    return true;
}

// (e) An unknown argument is rejected by the dispatcher's auto-validation with UNKNOWN_PARAMS.
// This path runs only through the real dispatcher, so drive it via a wired dispatcher fixture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceRelativeUnknownArgRejectedTest,
    "PinWright.spatial.place_relative.UnknownArgRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlaceRelativeUnknownArgRejectedTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), TEXT("SomeActor"));
    Params->SetStringField(TEXT("anchor"), TEXT("SomeAnchor"));
    Params->SetStringField(TEXT("relation"), TEXT("on_top_of"));
    Params->SetStringField(TEXT("bogusParam"), TEXT("nope"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("spatial.place_relative"),
        TEXT("req-place-unknown"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown param is rejected"), bSuccess);
    TestEqual(TEXT("unknown param -> UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));

    return true;
}

// ================= spatial.find_clear_placement =================
//
// The occupancy SEARCH verb. What these three tests are actually guarding is that it does not
// ship a plausible wrong answer: (f) that a returned pose really is clear of the obstacle and
// its reported clearance is the measured gap rather than a constant, (g) that an ISM/HISM
// scatter is seen AND attributed down to the instance - the half no actor-name occupancy input
// can express - and (h) that a malformed or over-budget request is refused rather than answered
// with a partial sweep.
namespace
{
    // An actor whose ROOT is a HISM carrying InstanceCount cube instances strung along X, with
    // collision stated explicitly rather than inherited from a default: the whole point of the
    // fixture is that the instances are query-visible, so leaving that to a component default
    // would make a failure ambiguous between "the verb is blind" and "the fixture has no bodies".
    //
    // Built directly because no RPC in this plugin makes one. Spawned at the origin and moved
    // afterwards: SetRootComponent on a just-spawned actor re-seats the actor transform, so a
    // spawn-time location would not survive it.
    AActor* FindClearTestSpawnScatterHolder(FAutomationTestBase& Test, UWorld* World,
                                            const FString& Label, const FVector& Location,
                                            int32 InstanceCount, double SpacingCm)
    {
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
            NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, TEXT("HISM_FindClearScatter"),
                RF_Transactional);
        Holder->SetRootComponent(Hism);
        Holder->AddInstanceComponent(Hism);
        Hism->OnComponentCreated();
        Hism->SetStaticMesh(Cube);
        Hism->SetCollisionProfileName(TEXT("BlockAll"));
        Hism->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Hism->RegisterComponent();
        for (int32 Index = 0; Index < InstanceCount; ++Index)
        {
            const double OffsetX = (static_cast<double>(Index) - 0.5 * (InstanceCount - 1)) * SpacingCm;
            Hism->AddInstance(FTransform(FVector(OffsetX, 0.0, 0.0)));
        }
        Holder->SetActorLocation(Location);
        Holder->SetActorLabel(Label);
        return Holder;
    }

    // {min,max} region object in the shape the verb takes.
    TSharedPtr<FJsonObject> FindClearTestBoxRegion(const FVector& Min, const FVector& Max)
    {
        TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
        Region->SetObjectField(TEXT("min"), PlaceTestVec(Min.X, Min.Y, Min.Z));
        Region->SetObjectField(TEXT("max"), PlaceTestVec(Max.X, Max.Y, Max.Z));
        return Region;
    }

    TSharedPtr<FJsonObject> FindClearTestFootprint(double Width, double Depth, double Height)
    {
        TSharedPtr<FJsonObject> Footprint = MakeShared<FJsonObject>();
        Footprint->SetNumberField(TEXT("width"), Width);
        Footprint->SetNumberField(TEXT("depth"), Depth);
        Footprint->SetNumberField(TEXT("height"), Height);
        return Footprint;
    }
}

// (f) A single obstacle in an otherwise empty region: the poses that come back are genuinely
// clear of it (checked against the fixture's own geometry, not against the verb's own report),
// the nearest one's clearance is the MEASURED 50 cm gap rather than a constant, and the
// rejection is attributed to the obstacle by name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindClearPlacementGapBesideObstacleTest,
    "PinWright.spatial.find_clear_placement.FindsTheGapBesideAnObstacle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFindClearPlacementGapBesideObstacleTest::RunTest(const FString& Parameters)
{
    UWorld* World = PlaceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping find_clear_placement obstacle test."));
        return true;
    }

    // Isolated column so no other level geometry lands inside the searched region.
    const double CenterX = 523450.0;
    const double CenterY = 467890.0;
    const double CenterZ = 1200.0;

    const FString ObstacleLabel = PlaceTestLabel(TEXT("FindClearBlocker"));
    AActor* Obstacle = PlaceTestSpawnCube(*this, World, ObstacleLabel,
        FVector(CenterX, CenterY, CenterZ));
    ON_SCOPE_EXIT
    {
        if (Obstacle) { Obstacle->Destroy(); }
    };
    if (!Obstacle)
    {
        return true;
    }

    // Flush the just-registered body into the scene-query structure; an overlap cannot see it
    // before the editor world ticks.
    PlaceTestFlushEditorPhysics(World);

    // A 100 cm footprint on a 150 cm grid over +/-300 cm: 5 x 5 columns, one yaw. Only the
    // column coincident with the 100 cm obstacle can overlap it; its neighbours clear it by
    // exactly 50 cm, which is what the measured clearance has to reproduce.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("footprint"), FindClearTestFootprint(100.0, 100.0, 100.0));
    Payload->SetObjectField(TEXT("region"), FindClearTestBoxRegion(
        FVector(CenterX - 300.0, CenterY - 300.0, CenterZ - 50.0),
        FVector(CenterX + 300.0, CenterY + 300.0, CenterZ + 50.0)));
    Payload->SetNumberField(TEXT("step"), 150.0);
    Payload->SetNumberField(TEXT("yawStep"), 0.0);
    Payload->SetBoolField(TEXT("seatOnGround"), false);
    Payload->SetStringField(TEXT("rank"), TEXT("distance"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.find_clear_placement handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.find_clear_placement"), Payload, Capture));
    TestTrue(TEXT("find_clear_placement succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    double Evaluated = 0.0;
    Capture.Result->TryGetNumberField(TEXT("evaluated"), Evaluated);
    TestEqual(TEXT("5 x 5 columns x 1 yaw were evaluated"), static_cast<int32>(Evaluated), 25);

    double Found = 0.0;
    Capture.Result->TryGetNumberField(TEXT("found"), Found);
    TestTrue(TEXT("the 24 columns clear of the obstacle were found"),
        static_cast<int32>(Found) >= 20);

    const TSharedPtr<FJsonObject>* Rejected = nullptr;
    if (Capture.Result->TryGetObjectField(TEXT("rejected"), Rejected) && Rejected)
    {
        double Total = -1.0;
        double Occupied = 0.0;
        (*Rejected)->TryGetNumberField(TEXT("total"), Total);
        (*Rejected)->TryGetNumberField(TEXT("occupied"), Occupied);
        // found + rejected == evaluated is the accounting invariant: a pose that was neither
        // returned nor accounted for is a pose the search silently dropped.
        TestEqual(TEXT("found + rejected.total == evaluated"),
            static_cast<int32>(Found + Total), static_cast<int32>(Evaluated));
        TestTrue(TEXT("the coincident column was rejected as occupied"),
            static_cast<int32>(Occupied) >= 1);
    }
    else
    {
        AddError(TEXT("find_clear_placement result missing the rejected block"));
    }

    // The binder: what actually blocked the rejected pose, by name.
    const TArray<TSharedPtr<FJsonValue>>* Binders = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("binders"), Binders) && Binders && Binders->Num() > 0)
    {
        const TSharedPtr<FJsonObject> Binder = (*Binders)[0]->AsObject();
        if (Binder.IsValid())
        {
            FString BinderActor;
            Binder->TryGetStringField(TEXT("actor"), BinderActor);
            TestEqual(TEXT("the binder names the obstacle actor"), BinderActor, ObstacleLabel);
            double BlockedPoses = 0.0;
            Binder->TryGetNumberField(TEXT("blockedPoses"), BlockedPoses);
            TestTrue(TEXT("the binder is credited with the poses it killed"),
                static_cast<int32>(BlockedPoses) >= 1);
        }
        else
        {
            AddError(TEXT("binders[0] is not an object"));
        }
    }
    else
    {
        AddError(TEXT("find_clear_placement reported no binder for a rejected pose"));
    }

    // Every returned pose is checked against the FIXTURE's geometry, not against the verb's own
    // report: a 100 cm footprint clears a 100 cm obstacle only when their centres are at least
    // 100 cm apart on one horizontal axis.
    const TArray<TSharedPtr<FJsonValue>>* Poses = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("poses"), Poses) && Poses && Poses->Num() > 0)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Poses)
        {
            const TSharedPtr<FJsonObject> Pose = Value->AsObject();
            const TSharedPtr<FJsonObject>* Location = nullptr;
            if (!Pose.IsValid() || !Pose->TryGetObjectField(TEXT("location"), Location) || !Location)
            {
                AddError(TEXT("a returned pose has no location"));
                continue;
            }
            double PoseX = 0.0;
            double PoseY = 0.0;
            (*Location)->TryGetNumberField(TEXT("x"), PoseX);
            (*Location)->TryGetNumberField(TEXT("y"), PoseY);
            const double SeparationCm =
                FMath::Max(FMath::Abs(PoseX - CenterX), FMath::Abs(PoseY - CenterY));
            TestTrue(TEXT("a returned pose does not overlap the obstacle"),
                SeparationCm >= 100.0 - 0.01);
        }

        // rank:"distance" puts a nearest neighbour first, and its real gap to the obstacle is
        // 50 cm. The reported clearance is a bisected LOWER bound at 200/64 = 3.125 cm
        // resolution, so it must land just under 50 - never at 0, and never above the real gap.
        const TSharedPtr<FJsonObject> Nearest = (*Poses)[0]->AsObject();
        if (Nearest.IsValid())
        {
            double ClearanceCm = -1.0;
            Nearest->TryGetNumberField(TEXT("clearanceCm"), ClearanceCm);
            TestTrue(TEXT("the nearest pose's measured clearance is just under the real 50 cm gap"),
                ClearanceCm > 40.0 && ClearanceCm <= 50.01);

            bool bCapped = true;
            Nearest->TryGetBoolField(TEXT("clearanceCapped"), bCapped);
            TestFalse(TEXT("that clearance was measured, not capped at the probe ceiling"), bCapped);

            const TSharedPtr<FJsonObject>* ClearanceBinder = nullptr;
            if (Nearest->TryGetObjectField(TEXT("clearanceBinder"), ClearanceBinder) && ClearanceBinder)
            {
                FString BinderActor;
                (*ClearanceBinder)->TryGetStringField(TEXT("actor"), BinderActor);
                TestEqual(TEXT("an accepted pose still names what bounds its clearance"),
                    BinderActor, ObstacleLabel);
            }
            else
            {
                AddError(TEXT("an accepted pose reported no clearanceBinder"));
            }
        }
    }
    else
    {
        AddError(TEXT("find_clear_placement returned no poses"));
    }

    return true;
}

// (g) The case the ticket exists for: instanced scatter. A HISM's occupancy is invisible to
// every name-based input in this namespace - the holder's AABB spans the whole scatter - so the
// verb has to see the INSTANCES and say which one is in the way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindClearPlacementSeesInstancedScatterTest,
    "PinWright.spatial.find_clear_placement.SeesInstancedScatter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFindClearPlacementSeesInstancedScatterTest::RunTest(const FString& Parameters)
{
    UWorld* World = PlaceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping find_clear_placement scatter test."));
        return true;
    }

    const double CenterX = 623450.0;
    const double CenterY = 567890.0;
    const double CenterZ = 2400.0;

    // Five 100 cm instances 300 cm apart, i.e. 200 cm of open space between neighbours.
    const FString HolderLabel = PlaceTestLabel(TEXT("FindClearScatter"));
    AActor* Holder = FindClearTestSpawnScatterHolder(*this, World, HolderLabel,
        FVector(CenterX, CenterY, CenterZ), /*InstanceCount=*/5, /*SpacingCm=*/300.0);
    ON_SCOPE_EXIT
    {
        if (Holder) { Holder->Destroy(); }
    };
    if (!Holder)
    {
        return true;
    }

    PlaceTestFlushEditorPhysics(World);

    // A 50 cm footprint on a 100 cm grid across the scatter. The 25 cm of slack on every side is
    // deliberate: no column's footprint ever merely TOUCHES an instance, so the occupied count
    // is decided by containment rather than by the physics backend's touching convention.
    // Columns land on X = -700, -600, ... , +700 relative to the holder; the five instances sit
    // at -600, -300, 0, +300, +600, so exactly five columns are inside an instance.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("footprint"), FindClearTestFootprint(50.0, 50.0, 50.0));
    Payload->SetObjectField(TEXT("region"), FindClearTestBoxRegion(
        FVector(CenterX - 700.0, CenterY - 1.0, CenterZ - 50.0),
        FVector(CenterX + 700.0, CenterY + 1.0, CenterZ + 50.0)));
    Payload->SetNumberField(TEXT("step"), 100.0);
    Payload->SetNumberField(TEXT("yawStep"), 0.0);
    Payload->SetBoolField(TEXT("seatOnGround"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.find_clear_placement handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.find_clear_placement"), Payload, Capture));
    TestTrue(TEXT("find_clear_placement succeeded over a scatter"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    double Evaluated = 0.0;
    Capture.Result->TryGetNumberField(TEXT("evaluated"), Evaluated);
    TestEqual(TEXT("15 columns x 1 yaw were evaluated"), static_cast<int32>(Evaluated), 15);

    double Found = 0.0;
    Capture.Result->TryGetNumberField(TEXT("found"), Found);
    TestTrue(TEXT("the gaps between instances were found"), static_cast<int32>(Found) >= 1);

    const TSharedPtr<FJsonObject>* Rejected = nullptr;
    if (Capture.Result->TryGetObjectField(TEXT("rejected"), Rejected) && Rejected)
    {
        double Occupied = 0.0;
        (*Rejected)->TryGetNumberField(TEXT("occupied"), Occupied);
        // Each of the five instances must at minimum block the column it stands in. Asserted as
        // a floor rather than an equality because the count above five would depend on the
        // backend's touching convention, which this fixture deliberately does not exercise.
        TestTrue(TEXT("every instance blocked the column it stands in"),
            static_cast<int32>(Occupied) >= 5);
    }
    else
    {
        AddError(TEXT("find_clear_placement result missing the rejected block"));
    }

    // The load-bearing assertion of this whole ticket: the blocker is reported as an INSTANCED
    // component of the holder, with an instance index - not merely as the holder actor, whose
    // own bounds span all five instances and therefore identify none of them.
    bool bFoundInstancedBinder = false;
    const TArray<TSharedPtr<FJsonValue>>* Binders = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("binders"), Binders) && Binders)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Binders)
        {
            const TSharedPtr<FJsonObject> Binder = Value->AsObject();
            if (!Binder.IsValid())
            {
                continue;
            }
            bool bInstanced = false;
            Binder->TryGetBoolField(TEXT("instanced"), bInstanced);
            if (!bInstanced)
            {
                continue;
            }
            bFoundInstancedBinder = true;

            FString BinderActor;
            Binder->TryGetStringField(TEXT("actor"), BinderActor);
            TestEqual(TEXT("the instanced binder names the scatter holder"),
                BinderActor, HolderLabel);

            FString ComponentClass;
            Binder->TryGetStringField(TEXT("componentClass"), ComponentClass);
            TestTrue(TEXT("the binder names an instanced-static-mesh component"),
                ComponentClass.Contains(TEXT("InstancedStaticMesh")));

            const TArray<TSharedPtr<FJsonValue>>* InstanceIndices = nullptr;
            if (Binder->TryGetArrayField(TEXT("instanceIndices"), InstanceIndices) && InstanceIndices)
            {
                TestTrue(TEXT("the binder names at least one blocking INSTANCE"),
                    InstanceIndices->Num() >= 1);
            }
            else
            {
                AddError(TEXT("an instanced binder carried no instanceIndices"));
            }

            double InstanceCount = 0.0;
            Binder->TryGetNumberField(TEXT("instanceCount"), InstanceCount);
            TestEqual(TEXT("the binder reports the scatter's instance count"),
                static_cast<int32>(InstanceCount), 5);
            break;
        }
    }
    TestTrue(TEXT("the scatter was attributed as an instanced blocker, not just as an actor"),
        bFoundInstancedBinder);

    return true;
}

// (h) Malformed and over-budget requests are refused with typed errors, and an over-budget grid
// is refused rather than partially searched - a partial sweep would report "no room" for a
// region it never looked at.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindClearPlacementTypedRefusalsTest,
    "PinWright.spatial.find_clear_placement.TypedRefusals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFindClearPlacementTypedRefusalsTest::RunTest(const FString& Parameters)
{
    UWorld* World = PlaceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping find_clear_placement refusal test."));
        return true;
    }

    // No footprint and no assetPath -> INVALID_PARAMS.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), FindClearTestBoxRegion(
            FVector(0.0, 0.0, 0.0), FVector(100.0, 100.0, 100.0)));

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered"),
            InvokeHandlerWithCapture(TEXT("spatial.find_clear_placement"), Payload, Capture));
        TestFalse(TEXT("a search with no footprint is an error"), Capture.bSuccess);
        TestEqual(TEXT("no footprint -> INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    // A footprint with no region. Driven through the REAL dispatcher, because that is where the
    // required-param contract lives: InvokeHandlerWithCapture calls the handler body directly and
    // would only exercise RequireObject's own INVALID_PARAMS fallback, never proving `region` is
    // declared required in RPC_PARAMS.
    {
        bSuppressLogWarnings = true;

        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("footprint"), FindClearTestFootprint(100.0, 100.0, 100.0));

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("spatial.find_clear_placement"),
            TEXT("req-find-clear-no-region"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("a search with no region is an error"), bSuccess);
        TestEqual(TEXT("no region -> MISSING_REQUIRED_PARAM"),
            ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    }

    // A grid over the pose budget is REFUSED, not truncated.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("footprint"), FindClearTestFootprint(100.0, 100.0, 100.0));
        Payload->SetObjectField(TEXT("region"), FindClearTestBoxRegion(
            FVector(0.0, 0.0, 0.0), FVector(10000.0, 10000.0, 100.0)));
        Payload->SetNumberField(TEXT("step"), 10.0);
        Payload->SetNumberField(TEXT("yawStep"), 0.0);
        Payload->SetNumberField(TEXT("maxPoses"), 100.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered"),
            InvokeHandlerWithCapture(TEXT("spatial.find_clear_placement"), Payload, Capture));
        TestFalse(TEXT("an over-budget grid is an error, not a partial sweep"), Capture.bSuccess);
        TestEqual(TEXT("over-budget grid -> INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    return true;
}
