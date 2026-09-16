// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the non-mutating spatial-verification RPCs:
//   spatial.measure_distance / spatial.measure_overlap / spatial.verify_placement.
// measure_* are pure FBox math (no RHI/physics needed). verify_placement.grounded/on
// use the editor down-trace, so those cases tick the editor world after spawning to
// flush the freshly-registered bodies into the scene-query structure (same reason and
// pattern as TestRaycastHandlers.cpp).
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Utils/ActorUtils.h"

#include "Dispatch/RpcDispatcher.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace
{
    TSharedPtr<FJsonObject> MeasureTestVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    UWorld* MeasureTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Tick the editor world a couple of frames so a just-spawned body is flushed into
    // the scene-query acceleration structure before a down-trace runs against it.
    void MeasureTestFlushEditorPhysics(UWorld* World)
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

    FString MeasureTestUniqueLabel(const TCHAR* Stem)
    {
        return FString::Printf(TEXT("PW_%s_%s"), Stem,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Spawns a 100cm engine Cube at the given world location and returns the actor
    // (or nullptr). Asserts the spawn succeeded through the test.
    AActor* MeasureTestSpawnCube(FAutomationTestBase& Test, UWorld* World,
        const FString& Label, double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        SpawnPayload->SetStringField(TEXT("actorName"), Label);
        SpawnPayload->SetObjectField(TEXT("location"), MeasureTestVec(X, Y, Z));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, Capture);
        Test.TestTrue(TEXT("actor.spawn handler registered"), bFound);
        Test.TestTrue(TEXT("cube spawned"), Capture.bSuccess);
        return McpActorUtils::FindActorByName(World, Label);
    }
}

// (a) Two cubes a known distance apart: measure_distance centerDistance matches, and
//     measure_overlap reports them non-overlapping.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeasureDistanceNonOverlapTest,
    "PinWright.spatial.measure_distance.CenterDistanceAndNonOverlap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeasureDistanceNonOverlapTest::RunTest(const FString& Parameters)
{
    UWorld* World = MeasureTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping measure_distance test."));
        return true;
    }

    const double ColX = 200000.0;
    const double ColY = 200000.0;
    const FString LabelA = MeasureTestUniqueLabel(TEXT("MDistA"));
    const FString LabelB = MeasureTestUniqueLabel(TEXT("MDistB"));

    AActor* ActorA = MeasureTestSpawnCube(*this, World, LabelA, ColX, ColY, 0.0);
    AActor* ActorB = MeasureTestSpawnCube(*this, World, LabelB, ColX + 300.0, ColY, 0.0);
    ON_SCOPE_EXIT
    {
        if (ActorA) { ActorA->Destroy(); }
        if (ActorB) { ActorB->Destroy(); }
    };
    if (!ActorA || !ActorB)
    {
        return true;
    }

    // measure_distance: centers are 300cm apart.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("a"), LabelA);
        Payload->SetStringField(TEXT("b"), LabelB);

        FTestResponseCapture Capture;
        TestTrue(TEXT("measure_distance registered"),
            InvokeHandlerWithCapture(TEXT("spatial.measure_distance"), Payload, Capture));
        TestTrue(TEXT("measure_distance succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            double CenterDistance = 0.0;
            TestTrue(TEXT("result carries centerDistance"),
                Capture.Result->TryGetNumberField(TEXT("centerDistance"), CenterDistance));
            TestTrue(TEXT("centerDistance ~= 300cm"), FMath::Abs(CenterDistance - 300.0) < 1.0);

            FString Units;
            TestTrue(TEXT("result echoes units"), Capture.Result->TryGetStringField(TEXT("units"), Units));
            TestEqual(TEXT("units are cm"), Units, FString(TEXT("cm")));
        }
    }

    // measure_overlap: 300cm apart, extents 50+50 -> no overlap.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("a"), LabelA);
        Payload->SetStringField(TEXT("b"), LabelB);

        FTestResponseCapture Capture;
        TestTrue(TEXT("measure_overlap registered"),
            InvokeHandlerWithCapture(TEXT("spatial.measure_overlap"), Payload, Capture));
        TestTrue(TEXT("measure_overlap succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bOverlapping = true;
            Capture.Result->TryGetBoolField(TEXT("overlapping"), bOverlapping);
            TestFalse(TEXT("distant cubes do not overlap"), bOverlapping);
        }
    }

    return true;
}

// (b) Two cubes placed to overlap on X: measure_overlap reports overlapping with a sane
//     positive penetrationDepth on the expected minimum-translation axis.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeasureOverlapPenetrationTest,
    "PinWright.spatial.measure_overlap.OverlappingPenetration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeasureOverlapPenetrationTest::RunTest(const FString& Parameters)
{
    UWorld* World = MeasureTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping measure_overlap test."));
        return true;
    }

    const double ColX = 210000.0;
    const double ColY = 200000.0;
    const FString LabelA = MeasureTestUniqueLabel(TEXT("MOvlA"));
    const FString LabelB = MeasureTestUniqueLabel(TEXT("MOvlB"));

    // 50cm apart on X: |deltaX|=50 < 100 (extentA+extentB) -> overlap on X, full overlap on Y/Z.
    AActor* ActorA = MeasureTestSpawnCube(*this, World, LabelA, ColX, ColY, 0.0);
    AActor* ActorB = MeasureTestSpawnCube(*this, World, LabelB, ColX + 50.0, ColY, 0.0);
    ON_SCOPE_EXIT
    {
        if (ActorA) { ActorA->Destroy(); }
        if (ActorB) { ActorB->Destroy(); }
    };
    if (!ActorA || !ActorB)
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("a"), LabelA);
    Payload->SetStringField(TEXT("b"), LabelB);

    FTestResponseCapture Capture;
    TestTrue(TEXT("measure_overlap registered"),
        InvokeHandlerWithCapture(TEXT("spatial.measure_overlap"), Payload, Capture));
    TestTrue(TEXT("measure_overlap succeeded"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bOverlapping = false;
        Capture.Result->TryGetBoolField(TEXT("overlapping"), bOverlapping);
        TestTrue(TEXT("cubes overlapping"), bOverlapping);

        double PenetrationDepth = 0.0;
        TestTrue(TEXT("result carries penetrationDepth"),
            Capture.Result->TryGetNumberField(TEXT("penetrationDepth"), PenetrationDepth));
        // Min push-out is along X: (50+50) - 50 = 50cm.
        TestTrue(TEXT("penetrationDepth is a sane positive value (~50cm)"),
            PenetrationDepth > 0.0 && FMath::Abs(PenetrationDepth - 50.0) < 1.0);

        FString PenetrationAxis;
        Capture.Result->TryGetStringField(TEXT("penetrationAxis"), PenetrationAxis);
        TestEqual(TEXT("separating axis is x"), PenetrationAxis, FString(TEXT("x")));
    }

    return true;
}

// (c) A cube resting ~1cm above a floor: verify_placement {grounded:{maxGap:5}} passes,
//     {on:floor} passes, and {noOverlapWith:[floor]} passes (bodies do not intersect).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyPlacementGroundedTest,
    "PinWright.spatial.verify_placement.GroundedRestingCube",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyPlacementGroundedTest::RunTest(const FString& Parameters)
{
    UWorld* World = MeasureTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping verify_placement grounded test."));
        return true;
    }

    const double ColX = 220000.0;
    const double ColY = 200000.0;
    const FString FloorLabel = MeasureTestUniqueLabel(TEXT("VpFloor"));
    const FString CubeLabel = MeasureTestUniqueLabel(TEXT("VpRest"));

    // Floor cube: 100cm centered at Z=0 -> top face Z=50.
    AActor* Floor = MeasureTestSpawnCube(*this, World, FloorLabel, ColX, ColY, 0.0);
    // Resting cube: center Z=101 -> bottom face Z=51, so a 1cm gap above the floor top.
    AActor* Cube = MeasureTestSpawnCube(*this, World, CubeLabel, ColX, ColY, 101.0);
    ON_SCOPE_EXIT
    {
        if (Cube) { Cube->Destroy(); }
        if (Floor) { Floor->Destroy(); }
    };
    if (!Floor || !Cube)
    {
        return true;
    }

    // Flush both bodies into the scene-query structure before the down-trace.
    MeasureTestFlushEditorPhysics(World);

    // grounded {maxGap:5}: 1cm gap is within tolerance -> pass.
    {
        TSharedPtr<FJsonObject> Grounded = MakeShared<FJsonObject>();
        Grounded->SetNumberField(TEXT("maxGap"), 5.0);
        TSharedPtr<FJsonObject> Expect = MakeShared<FJsonObject>();
        Expect->SetObjectField(TEXT("grounded"), Grounded);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actor"), CubeLabel);
        Payload->SetObjectField(TEXT("expect"), Expect);

        FTestResponseCapture Capture;
        TestTrue(TEXT("verify_placement registered"),
            InvokeHandlerWithCapture(TEXT("spatial.verify_placement"), Payload, Capture));
        TestTrue(TEXT("verify_placement succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bPass = false;
            Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
            TestTrue(TEXT("resting cube is grounded"), bPass);

            TSharedPtr<FJsonObject> GroundedCheck = JsonArrayFindObjectByStringField(
                Capture.Result, TEXT("checks"), TEXT("name"), TEXT("grounded"));
            TestTrue(TEXT("grounded check present"), GroundedCheck.IsValid());
            if (GroundedCheck.IsValid())
            {
                double Gap = -1.0;
                TestTrue(TEXT("grounded check reports gap"),
                    GroundedCheck->TryGetNumberField(TEXT("gap"), Gap));
                TestTrue(TEXT("measured gap ~= 1cm"), FMath::Abs(Gap - 1.0) < 1.0);
            }
        }
    }

    // on: floor -> the surface directly below is the floor actor -> pass.
    {
        TSharedPtr<FJsonObject> Expect = MakeShared<FJsonObject>();
        Expect->SetStringField(TEXT("on"), FloorLabel);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actor"), CubeLabel);
        Payload->SetObjectField(TEXT("expect"), Expect);

        FTestResponseCapture Capture;
        TestTrue(TEXT("verify_placement registered (on)"),
            InvokeHandlerWithCapture(TEXT("spatial.verify_placement"), Payload, Capture));
        TestTrue(TEXT("verify_placement succeeded (on)"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bPass = false;
            Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
            TestTrue(TEXT("cube rests on the floor"), bPass);
        }
    }

    // noOverlapWith [floor]: bodies are 1cm apart, so no overlap -> pass.
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        Names.Add(MakeShared<FJsonValueString>(FloorLabel));
        TSharedPtr<FJsonObject> Expect = MakeShared<FJsonObject>();
        Expect->SetArrayField(TEXT("noOverlapWith"), Names);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actor"), CubeLabel);
        Payload->SetObjectField(TEXT("expect"), Expect);

        FTestResponseCapture Capture;
        TestTrue(TEXT("verify_placement registered (noOverlapWith)"),
            InvokeHandlerWithCapture(TEXT("spatial.verify_placement"), Payload, Capture));
        TestTrue(TEXT("verify_placement succeeded (noOverlapWith)"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bPass = false;
            Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
            TestTrue(TEXT("resting cube does not overlap the floor"), bPass);
        }
    }

    return true;
}

// (d) A cube floating far above the floor: verify_placement {grounded:{maxGap:5}} fails,
//     and the measured (too-large) gap is present in the grounded check detail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVerifyPlacementFloatingTest,
    "PinWright.spatial.verify_placement.FloatingCubeFailsGrounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVerifyPlacementFloatingTest::RunTest(const FString& Parameters)
{
    UWorld* World = MeasureTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping verify_placement floating test."));
        return true;
    }

    const double ColX = 230000.0;
    const double ColY = 200000.0;
    const FString FloorLabel = MeasureTestUniqueLabel(TEXT("VpFloorF"));
    const FString CubeLabel = MeasureTestUniqueLabel(TEXT("VpFloat"));

    // Floor top at Z=50. Floating cube center Z=200 -> bottom Z=150 -> 100cm gap.
    AActor* Floor = MeasureTestSpawnCube(*this, World, FloorLabel, ColX, ColY, 0.0);
    AActor* Cube = MeasureTestSpawnCube(*this, World, CubeLabel, ColX, ColY, 200.0);
    ON_SCOPE_EXIT
    {
        if (Cube) { Cube->Destroy(); }
        if (Floor) { Floor->Destroy(); }
    };
    if (!Floor || !Cube)
    {
        return true;
    }

    MeasureTestFlushEditorPhysics(World);

    TSharedPtr<FJsonObject> Grounded = MakeShared<FJsonObject>();
    Grounded->SetNumberField(TEXT("maxGap"), 5.0);
    TSharedPtr<FJsonObject> Expect = MakeShared<FJsonObject>();
    Expect->SetObjectField(TEXT("grounded"), Grounded);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor"), CubeLabel);
    Payload->SetObjectField(TEXT("expect"), Expect);

    FTestResponseCapture Capture;
    TestTrue(TEXT("verify_placement registered"),
        InvokeHandlerWithCapture(TEXT("spatial.verify_placement"), Payload, Capture));
    TestTrue(TEXT("verify_placement succeeded (call, not check)"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bPass = true;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestFalse(TEXT("floating cube is not grounded"), bPass);

        TSharedPtr<FJsonObject> GroundedCheck = JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("checks"), TEXT("name"), TEXT("grounded"));
        TestTrue(TEXT("grounded check present"), GroundedCheck.IsValid());
        if (GroundedCheck.IsValid())
        {
            bool bCheckPass = true;
            GroundedCheck->TryGetBoolField(TEXT("pass"), bCheckPass);
            TestFalse(TEXT("grounded check failed"), bCheckPass);

            double Gap = -1.0;
            TestTrue(TEXT("grounded check still reports the measured gap"),
                GroundedCheck->TryGetNumberField(TEXT("gap"), Gap));
            TestTrue(TEXT("measured gap ~= 100cm (exceeds maxGap)"),
                FMath::Abs(Gap - 100.0) < 2.0);
        }
    }

    return true;
}

// (e) A non-existent actor name is a typed ACTOR_NOT_FOUND, not a fake success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeasureMissingActorTest,
    "PinWright.spatial.measure_distance.MissingActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeasureMissingActorTest::RunTest(const FString& Parameters)
{
    const FString MissingA = MeasureTestUniqueLabel(TEXT("NoSuchA"));
    const FString MissingB = MeasureTestUniqueLabel(TEXT("NoSuchB"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("a"), MissingA);
    Payload->SetStringField(TEXT("b"), MissingB);

    FTestResponseCapture Capture;
    TestTrue(TEXT("measure_distance registered"),
        InvokeHandlerWithCapture(TEXT("spatial.measure_distance"), Payload, Capture));
    TestFalse(TEXT("missing actor is an error"), Capture.bSuccess);
    TestEqual(TEXT("missing actor -> ACTOR_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));

    return true;
}

// (f) An unknown argument is rejected by the dispatcher's auto-validation with
//     UNKNOWN_PARAMS. Only the real dispatcher path validates params, so drive it via a
//     wired dispatcher fixture (the direct-invoke bypass skips validation).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeasureUnknownArgRejectedTest,
    "PinWright.spatial.measure_distance.UnknownArgRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeasureUnknownArgRejectedTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("a"), TEXT("SomeActor"));
    Params->SetStringField(TEXT("b"), TEXT("OtherActor"));
    Params->SetStringField(TEXT("bogusParam"), TEXT("nope"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("spatial.measure_distance"),
        TEXT("req-measure-unknown"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown param is rejected"), bSuccess);
    TestEqual(TEXT("unknown param -> UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));

    return true;
}
