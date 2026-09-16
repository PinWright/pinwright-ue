// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-capture-open-level-pose-params-photograph-stale-grass.
//
// THE DEFECT. `render.capture_open_level {location, rotation}` writes the pose onto the persistent
// FEditorViewportClient and takes it back off on scope exit, inside one RPC. Landscape grass is
// built around CAMERA LOCATIONS collected on the WORLD tick (`ULandscapeSubsystem::Tick` reads
// `World->ViewLocationsRenderedLastFrame`, UE 5.8 LandscapeSubsystem.cpp:733-739, and calls
// `Proxy->UpdateGrass` at :900), and the capture's settle loop pumps only Slate and the renderer.
// So the requested pose never became a grass camera: the frame showed grass built around wherever
// the persistent viewport camera happened to be. Measured live, one map, one variable: the same
// pose captured twice gave meanLuminance 0.6443 / 844 KB with no grass, while `editor.set_camera`
// to that pose (which leaves the camera there long enough for a tick to see it) followed by the
// identical capture gave 0.4796 / 1558 KB with a full carpet. Both responses said success,
// non-blank and settled.
//
// WHAT IS ASSERTED HERE, AND WHY IT IS NOT A PIXEL COMPARISON. The A/B above needs a landscape
// carrying an authored grass material, which no in-code fixture can build without shipping content;
// re-deriving it from luminance would also make the assertion depend on a GPU, on lighting and on
// exposure -- three things that have each already produced a false red in this suite. The property
// the fix actually establishes is narrower and exact:
//
//     the pose these pixels were drawn from IS the camera location the landscape grass was
//     built around, and the response says whether that build finished.
//
// That is checkable against a bare landscape with no grass material at all, and the honesty half
// is a pure function of the report, so it is checked without a level, a viewport or an RHI.
//
// COUNTERFACTUAL. Remove the SettleGrassForCapturePose call from CaptureEditorViewportToPng and
// `viewport.grass.builtForPose` reads false with a warning; remove the whole block and the
// end-to-end test cannot find `viewport.grass` at all. Neither is reachable on the pre-fix tree,
// where no capture path ever handed a pose to the grass build.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Handlers/Render/LandscapeGrassSettle.h"
#include "LandscapeProxy.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    using PinWrightCaptureGrass::FGrassBuildReport;

    // File-unique names throughout. Unity merges the Tests/Render/ translation units, so a helper
    // called anything as generic as `MakeReport` or `DeleteCaptureFile` here would redefine a
    // sibling file's (docs/lessons.md).

    // A report shaped like a landscape level whose grass build ran for the capture pose.
    FGrassBuildReport PinWrightGrassSettleMakeBuiltReport()
    {
        FGrassBuildReport Report;
        Report.bMeasured = true;
        Report.LandscapeProxies = 1;
        Report.bBuiltForPose = true;
        Report.CameraLocation = FVector(1200.0, -3400.0, 560.0);
        Report.ComponentsBefore = 4;
        Report.ComponentsAfter = 4;
        Report.InstancesAfter = 0;
        // All four components were readable, so the zero above is a MEASUREMENT -- which is the
        // whole premise of the bare-ground assertions below. Added with
        // B-capture-grass-instances-always-zero, which separated "counted, and it is zero" from
        // "could not be counted"; a report that leaves this false publishes no `instances` at all.
        Report.bInstancesMeasured = true;
        Report.bSettled = true;
        Report.BuildMs = 12.5;
        return Report;
    }

    bool PinWrightGrassSettleHasWarning(const TSharedPtr<FJsonObject>& Grass)
    {
        FString Unused;
        return Grass.IsValid() && Grass->TryGetStringField(TEXT("grassWarning"), Unused);
    }

    // TryGet* everywhere, never FJsonObject::GetBoolField: the getter returns a default for a key
    // that is not there, which would let an ABSENT field pass as `false` -- and "the block was
    // never published" is exactly the failure these tests exist to catch.
    bool PinWrightGrassSettleReadBool(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        bool Value = false;
        if (!Object.IsValid() || !Object->TryGetBoolField(FieldName, Value))
        {
            Test.AddError(FString::Printf(
                TEXT("the grass block has no boolean field `%s`"), FieldName));
            return false;
        }
        return Value;
    }

    double PinWrightGrassSettleReadNumber(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        double Value = 0.0;
        if (!Object.IsValid() || !Object->TryGetNumberField(FieldName, Value))
        {
            Test.AddError(FString::Printf(
                TEXT("the grass block has no number field `%s`"), FieldName));
            return 0.0;
        }
        return Value;
    }

    void PinWrightGrassSettleDeleteCaptureFile(const FTestResponseCapture& Capture)
    {
        FString Path;
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetStringField(TEXT("path"), Path))
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }
}

// ============================================================================
// The honesty contract, asserted as a pure function of the report.
// ============================================================================

// The one distinction the response could not make before this block existed: an unfinished build
// and genuinely bare ground produce the same picture, and the caller has to act on them in
// opposite directions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassUnfinishedBuildIsNotBareGroundTest,
    "PinWright.render.grass_settle.UnfinishedBuildIsNotBareGround",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassUnfinishedBuildIsNotBareGroundTest::RunTest(const FString& Parameters)
{
    // Bare ground: the build ran for this pose, finished, and produced nothing. That is a
    // measurement, and it must NOT carry a warning -- a warning here would train callers to
    // discount every correct "there is no grass here" answer.
    const FGrassBuildReport BareGround = PinWrightGrassSettleMakeBuiltReport();
    const TSharedPtr<FJsonObject> BareJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(BareGround);
    const bool bBareSettled = PinWrightGrassSettleReadBool(*this, BareJson, TEXT("settled"));
    TestTrue(TEXT("a completed build publishes settled:true"), bBareSettled);
    TestTrue(TEXT("a completed build publishes builtForPose:true"),
        PinWrightGrassSettleReadBool(*this, BareJson, TEXT("builtForPose")));
    TestEqual(TEXT("bare ground reports zero instances rather than omitting the count"),
        PinWrightGrassSettleReadNumber(*this, BareJson, TEXT("instances")), 0.0);
    TestFalse(TEXT("bare ground carries no warning -- it is the honest answer"),
        PinWrightGrassSettleHasWarning(BareJson));

    // Unfinished: the same zero instances, from a build that had not finished. The caller must be
    // able to tell this apart from the case above without inferring it.
    FGrassBuildReport Unfinished = PinWrightGrassSettleMakeBuiltReport();
    Unfinished.PendingComponents = 2;
    Unfinished.PendingTasks = 1;
    Unfinished.bSettled = false;
    const TSharedPtr<FJsonObject> UnfinishedJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(Unfinished);
    const bool bUnfinishedSettled =
        PinWrightGrassSettleReadBool(*this, UnfinishedJson, TEXT("settled"));
    TestFalse(TEXT("an unfinished build publishes settled:false"), bUnfinishedSettled);
    TestTrue(TEXT("an unfinished build is warned about"),
        PinWrightGrassSettleHasWarning(UnfinishedJson));
    TestEqual(TEXT("the pending components are published, not just the verdict"),
        PinWrightGrassSettleReadNumber(*this, UnfinishedJson, TEXT("pendingComponents")), 2.0);
    TestEqual(TEXT("the outstanding async tasks are published too"),
        PinWrightGrassSettleReadNumber(*this, UnfinishedJson, TEXT("pendingTasks")), 1.0);

    // The decisive property: two frames that both show no grass are distinguishable in the
    // response. Asserted on the field a caller would branch on.
    TestTrue(TEXT("bare ground and an unfinished build differ in `settled`"),
        bBareSettled != bUnfinishedSettled);
    return true;
}

// The pre-fix state of every level capture, and the state a transaction still produces: the pose
// never reached the grass build. It must be visible in the response rather than silent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassPoseThatNeverDroveTheBuildIsWarnedTest,
    "PinWright.render.grass_settle.PoseThatNeverDroveTheBuildIsWarned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassPoseThatNeverDroveTheBuildIsWarnedTest::RunTest(const FString& Parameters)
{
    FGrassBuildReport NotBuilt = PinWrightGrassSettleMakeBuiltReport();
    NotBuilt.bBuiltForPose = false;
    NotBuilt.bSettled = false;
    const TSharedPtr<FJsonObject> NotBuiltJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(NotBuilt);
    TestFalse(TEXT("a pose that never reached the build reports builtForPose:false"),
        PinWrightGrassSettleReadBool(*this, NotBuiltJson, TEXT("builtForPose")));
    TestTrue(TEXT("and it is warned about"), PinWrightGrassSettleHasWarning(NotBuiltJson));
    FString Warning;
    NotBuiltJson->TryGetStringField(TEXT("grassWarning"), Warning);
    TestTrue(TEXT("the warning says the grass belongs to a different camera"),
        Warning.Contains(TEXT("different camera")));

    // The transaction case has a different remedy, so it must not share the generic wording.
    FGrassBuildReport Blocked = NotBuilt;
    Blocked.bBlockedByTransaction = true;
    const TSharedPtr<FJsonObject> BlockedJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(Blocked);
    FString BlockedWarning;
    BlockedJson->TryGetStringField(TEXT("grassWarning"), BlockedWarning);
    TestTrue(TEXT("a transaction-blocked build names the transaction"),
        BlockedWarning.Contains(TEXT("transaction")));
    return true;
}

// `measured` separates "looked and there is nothing" from "never looked". Without it the zeros
// below would be an absence of grass this capture never checked for.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassUnmeasuredPublishesNoZerosTest,
    "PinWright.render.grass_settle.UnmeasuredPublishesNoZeros",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassUnmeasuredPublishesNoZerosTest::RunTest(const FString& Parameters)
{
    const FGrassBuildReport Unmeasured;
    const TSharedPtr<FJsonObject> Json =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(Unmeasured);
    TestFalse(TEXT("an unmeasured report says so"),
        PinWrightGrassSettleReadBool(*this, Json, TEXT("measured")));
    double Ignored = 0.0;
    TestFalse(TEXT("no instance count is invented for a report that never ran"),
        Json->TryGetNumberField(TEXT("instances"), Ignored));
    TestFalse(TEXT("no landscape count is invented either"),
        Json->TryGetNumberField(TEXT("landscapes"), Ignored));

    // A measured world with no terrain is a different fact and does publish its zero.
    FGrassBuildReport NoTerrain;
    NoTerrain.bMeasured = true;
    NoTerrain.bSettled = true;
    const TSharedPtr<FJsonObject> NoTerrainJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(NoTerrain);
    TestEqual(TEXT("a measured world with no landscape publishes landscapes:0"),
        PinWrightGrassSettleReadNumber(*this, NoTerrainJson, TEXT("landscapes")), 0.0);
    TestFalse(TEXT("and carries no warning"), PinWrightGrassSettleHasWarning(NoTerrainJson));
    return true;
}

// ============================================================================
// The behaviour: a capture pose reaches the grass build and the build completes.
// ============================================================================

// Builds its fixture IN-CODE through the production landscape.create handler -- a landscape with
// the default WorldGridMaterial and therefore no grass types, which is exactly enough to assert
// the routing property (the requested pose became the grass camera, and the build finished)
// without shipping a grass material. Then drives a real render.capture_open_level at that pose and
// asserts the response's own grass block agrees with the pose the pixels were drawn from.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassRequestedPoseDrivesTheBuildTest,
    "PinWright.render.grass_settle.RequestedPoseDrivesTheGrassBuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassRequestedPoseDrivesTheBuildTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        // Environment precondition, not the fixture. The fixture is built below and its absence
        // IS a failure.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-unavailable"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so no landscape could be "
                 "spawned and no capture could be taken."));
        return true;
    }

    TestTrue(TEXT("landscape.create handler registered"),
        IsHandlerRegistered(TEXT("landscape.create")));

    // Destroys any actor spawned during the test and restores the level dirty flag on scope exit,
    // so this test leaves the open map exactly as it found it.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString LandscapeLabel = FString::Printf(TEXT("PW_GrassSettle_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), LandscapeLabel);
        CreatePayload->SetNumberField(TEXT("componentsX"), 1);
        CreatePayload->SetNumberField(TEXT("componentsY"), 1);
        CreatePayload->SetNumberField(TEXT("quadsPerComponent"), 63);
        CreatePayload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        TSharedRef<FTestResponseCapture> CreateCapture = MakeShared<FTestResponseCapture>();
        const bool bCreateFound = InvokeHandlerWithSharedCapture(
            TEXT("landscape.create"), CreatePayload, CreateCapture);
        TestTrue(TEXT("landscape.create invoked"), bCreateFound);
        if (!bCreateFound)
        {
            return false;
        }
        PumpUntilCaptured(*CreateCapture, /*TimeoutSeconds=*/30.0);
        TestTrue(TEXT("landscape.create responded"), CreateCapture->bWasCalled);
        TestTrue(TEXT("landscape.create succeeded (fixture built)"), CreateCapture->bSuccess);
        if (!CreateCapture->bWasCalled || !CreateCapture->bSuccess)
        {
            return false;
        }
    }

    // The landscape has to actually be in the world, or the routing assertion below would pass
    // vacuously on the no-terrain branch.
    int32 ProxiesInWorld = 0;
    for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
    {
        ++ProxiesInWorld;
    }
    TestTrue(TEXT("the world holds at least one landscape proxy after create"), ProxiesInWorld > 0);
    if (ProxiesInWorld == 0)
    {
        return false;
    }

    // ---- the routing property, asserted directly on the function the capture calls ----
    const FVector Pose(2100.0, -900.0, 1750.0);
    const FGrassBuildReport Report =
        PinWrightCaptureGrass::SettleGrassForCapturePose(World, Pose);
    TestTrue(TEXT("the grass build was measured on a world that has a landscape subsystem"),
        Report.bMeasured);
    TestEqual(TEXT("every landscape proxy in the world was surveyed"),
        Report.LandscapeProxies, ProxiesInWorld);
    TestFalse(TEXT("no editor transaction was open, so the build was free to run"),
        Report.bBlockedByTransaction);
    // THE assertion this file exists for: the pose passed in is the camera the grass was built
    // around. Nothing on the pre-fix tree could make this true.
    TestTrue(TEXT("the requested pose drove the grass build"), Report.bBuiltForPose);
    TestTrue(TEXT("the grass was built around the requested pose, not around another camera"),
        Report.CameraLocation.Equals(Pose, 0.01));
    // Synchronous by construction (bForceSync waits the async builders out), so a pending count
    // here means the build did not complete inside the call it promised to complete in.
    TestTrue(TEXT("the forced build left nothing pending"), Report.bSettled);
    TestEqual(TEXT("no grass component is left flagged pending"), Report.PendingComponents, 0);
    TestEqual(TEXT("no async grass task is left outstanding"), Report.PendingTasks, 0);

    // ---- end to end: the capture publishes the block, for the pose its own pixels show ----
    TSharedPtr<FJsonObject> CapturePayload = MakeShared<FJsonObject>();
    CapturePayload->SetNumberField(TEXT("width"), 128);
    CapturePayload->SetNumberField(TEXT("height"), 128);
    CapturePayload->SetBoolField(TEXT("allowBlank"), true);
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), Pose.X);
    Location->SetNumberField(TEXT("y"), Pose.Y);
    Location->SetNumberField(TEXT("z"), Pose.Z);
    CapturePayload->SetObjectField(TEXT("location"), Location);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -30.0);
    Rotation->SetNumberField(TEXT("yaw"), 45.0);
    Rotation->SetNumberField(TEXT("roll"), 0.0);
    CapturePayload->SetObjectField(TEXT("rotation"), Rotation);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), CapturePayload, Capture));
    ON_SCOPE_EXIT
    {
        PinWrightGrassSettleDeleteCaptureFile(Capture);
    };

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        // No live Level Editor viewport on this host (or another process holds the GPU). The
        // routing assertions above already ran; only the response-shape half is stepped over, and
        // that has to be visible downstream rather than counted as a pass.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            FString::Printf(TEXT("render.capture_open_level returned %s (code=%s, message=%s), so "
                                 "the viewport.grass block could not be read off a real capture."),
                Capture.bSuccess ? TEXT("no result") : TEXT("an error"),
                *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    const TSharedPtr<FJsonObject>* Viewport = nullptr;
    TestTrue(TEXT("a successful capture reports its viewport block"),
        Capture.Result->TryGetObjectField(TEXT("viewport"), Viewport) && Viewport != nullptr);
    if (!Viewport || !Viewport->IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* Grass = nullptr;
    TestTrue(TEXT("every capture publishes viewport.grass"),
        (*Viewport)->TryGetObjectField(TEXT("grass"), Grass) && Grass != nullptr);
    if (!Grass || !Grass->IsValid())
    {
        return false;
    }

    TestTrue(TEXT("the grass build was measured for this capture"),
        PinWrightGrassSettleReadBool(*this, *Grass, TEXT("measured")));
    double Landscapes = 0.0;
    TestTrue(TEXT("the block reports how many landscapes it surveyed"),
        (*Grass)->TryGetNumberField(TEXT("landscapes"), Landscapes));
    TestTrue(TEXT("the fixture landscape was surveyed by the capture"), Landscapes > 0.0);
    TestTrue(TEXT("the capture's own pose drove its grass build"),
        PinWrightGrassSettleReadBool(*this, *Grass, TEXT("builtForPose")));
    TestFalse(TEXT("the capture's grass build carries no unfinished-build warning"),
        PinWrightGrassSettleHasWarning(*Grass));

    // The pose the grass was built around is the pose the PIXELS were drawn from -- read off the
    // response's own measured cameraLocation, not off the request, because an orbit-mode viewport
    // can put the eye somewhere else and the grass must follow the eye that rendered.
    const TSharedPtr<FJsonObject>* GrassCamera = nullptr;
    const TSharedPtr<FJsonObject>* DrawnCamera = nullptr;
    if ((*Grass)->TryGetObjectField(TEXT("cameraLocation"), GrassCamera) && GrassCamera &&
        Capture.Result->TryGetObjectField(TEXT("cameraLocation"), DrawnCamera) && DrawnCamera)
    {
        const auto ReadVector = [](const TSharedPtr<FJsonObject>& Object)
        {
            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;
            Object->TryGetNumberField(TEXT("x"), X);
            Object->TryGetNumberField(TEXT("y"), Y);
            Object->TryGetNumberField(TEXT("z"), Z);
            return FVector(X, Y, Z);
        };
        const FVector GrassAt = ReadVector(*GrassCamera);
        const FVector DrawnAt = ReadVector(*DrawnCamera);
        TestTrue(FString::Printf(
                TEXT("the grass was built around the pose the pixels show (grass %s vs drawn %s)"),
                *GrassAt.ToString(), *DrawnAt.ToString()),
            GrassAt.Equals(DrawnAt, 0.01));
    }
    else
    {
        AddError(TEXT("the capture response is missing cameraLocation on the grass block or at "
                      "top level, so the two poses cannot be compared"));
    }
    return true;
}
