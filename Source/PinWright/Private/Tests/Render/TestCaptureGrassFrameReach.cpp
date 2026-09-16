// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-ortho-capture-renders-no-landscape-grass.
//
// THE DEFECT. An ORTHOGRAPHIC capture rendered ZERO landscape grass over ground the perspective
// capture of the same world span fills with a carpet: orthoWidth 6928 against fov 60 at Z 6000
// returned meanLuminance 0.5161 with none, where the perspective frame returned 0.4796 with a full
// carpet (grass darkens a frame, so the higher number is the one with less of it). The loss was
// TOTAL rather than graded toward the frame edges, and the response was a plain success -- valid,
// non-blank, settled tiles of ground that is actually covered.
//
// DISTANCE CULLING WAS RULED OUT BY MEASUREMENT in the filing session, not by argument: under
// `viewMode: "unlit"` the capture's own survey reported `cullingOriginPushback: 0` and a derived
// r.ViewDistanceScale of 2.29 was live, and the grass was still absent. That is why this is not
// B-ortho-capture-culls-distant-foliage.
//
// THE MECHANISM, attributed against UE 5.8 source. An orthographic editor view can never be a
// landscape-grass camera. `UEditorEngine::UpdateSingleViewportClient` gates BOTH camera feeds on
// `IsPerspective()` -- the streaming-manager registration (Editor/UnrealEd/Private/
// EditorEngine.cpp:2613-2618) and `World->ViewLocationsRenderedLastFrame.Add(...)` (:2628-2634) --
// and the only other writer of that array, `AddStreamingViewInfo` (Runtime/Engine/Private/
// UnrealClient.cpp:1771-1787), runs solely from `UGameViewportClient::Draw`
// (GameViewportClient.cpp:1913), which no editor viewport reaches. `ULandscapeSubsystem::Tick`
// feeds `ALandscapeProxy::UpdateGrass` from exactly those two sources
// (LandscapeSubsystem.cpp:729-757, :891-901) and skips it entirely when neither yields a camera;
// the default `grass.UseStreamingManagerForCameras` is 1 (:65-69), the branch with no `OldCameras`
// fallback. So the grass in an orthographic frame is a leftover of the last PERSPECTIVE view, and
// over ground no perspective view has visited the frame is bare.
//
// A second, downstream way to lose the same pixels, and the reason the reach block exists: grass
// instances are culled against `InstanceEndCullDistance` (LandscapeGrass.cpp:3220, seeded from the
// variety's EndCullDistance, default 10000 cm) measured from `FSceneView::CullingOrigin`
// (SceneView.cpp:844), which for a lit editor orthographic view is NOT the camera. A settled build
// of millions of instances can therefore render as bare ground with `builtForPose: true`,
// `settled: true` and no warning -- the silent-wrong-data failure this ticket is filed for.
//
// WHAT IS ASSERTED HERE, AND WHY IT IS NOT A PIXEL COMPARISON. The ticket's own evidence method is
// a perspective/orthographic luminance A/B, and it needs a landscape carrying an authored grass
// material, which no in-code fixture can build without shipping content. Re-deriving the verdict
// from luminance would also make it depend on a GPU, on lighting and on exposure. The properties
// the fix establishes are exact and checkable without any of that:
//
//     (a) an ORTHOGRAPHIC capture's own pose drives its landscape-grass build, exactly as a
//         perspective one's does -- the two projections report the same grass at the same pose;
//     (b) render.capture_ortho_tiles publishes a grass block at all; and
//     (c) when grass exists and NONE of it can reach the frame, the response says so instead of
//         returning an empty forest as a plain success.
//
// The A/B in (a) is run at the same pose across both projections. On the in-code fixture (a
// landscape with the default WorldGridMaterial, hence no grass types) it compares 0 against 0 and
// only the routing assertions carry; on any map with authored grass it is the ticket's own
// measurement, and it is written to be exactly that.
//
// COUNTERFACTUAL. Remove MeasureGrassFrameReach's call site and (c) loses its warning and the
// `reach` block disappears; remove the SettleGrassForCapturePose call from the ortho tile path and
// (b) fails outright. Neither is reachable on the pre-fix tree, where no orthographic capture path
// ever handed a pose to the grass build and no response carried a field that could contradict an
// empty frame.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Handlers/Render/LandscapeGrassSettle.h"
#include "LandscapeProxy.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    using PinWrightCaptureGrass::FGrassBuildReport;

    // File-unique names throughout. Unity merges the Tests/Render/ translation units, so a helper
    // called anything as generic as `MakeReport` here would redefine a sibling file's
    // (docs/lessons.md).

    // A report shaped like a landscape level whose grass build ran, finished, and produced
    // instances -- the state in which the empty-forest failure is possible and looks clean.
    FGrassBuildReport PinWrightGrassReachMakeSettledReport()
    {
        FGrassBuildReport Report;
        Report.bMeasured = true;
        Report.LandscapeProxies = 1;
        Report.bBuiltForPose = true;
        Report.CameraLocation = FVector(0.0, 0.0, 6000.0);
        Report.ComponentsBefore = 12;
        Report.ComponentsAfter = 12;
        Report.InstancesAfter = 412000;
        // All twelve components were readable, so the 412000 is a total rather than a floor.
        // Added with B-capture-grass-instances-always-zero: a report that leaves this false
        // publishes no `instances` and carries an incomplete-count warning, which would make the
        // out-of-reach and within-reach assertions below read a different response than they mean.
        Report.bInstancesMeasured = true;
        Report.bSettled = true;
        Report.BuildMs = 40.0;
        return Report;
    }

    bool PinWrightGrassReachHasWarning(const TSharedPtr<FJsonObject>& Grass, FString& OutText)
    {
        OutText.Reset();
        return Grass.IsValid() && Grass->TryGetStringField(TEXT("grassWarning"), OutText);
    }

    // TryGet* everywhere, never FJsonObject::GetBoolField: the getter returns a default for a key
    // that is not there, which would let an ABSENT field pass as `false` -- and "the block was
    // never published" is exactly the failure these tests exist to catch.
    const TSharedPtr<FJsonObject>* PinWrightGrassReachSubObject(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (!Object.IsValid() || !Object->TryGetObjectField(FieldName, Sub) || Sub == nullptr ||
            !(*Sub).IsValid())
        {
            Test.AddError(FString::Printf(TEXT("no object field `%s`"), FieldName));
            return nullptr;
        }
        return Sub;
    }

    void PinWrightGrassReachDeleteCaptureFile(const FTestResponseCapture& Capture)
    {
        FString Path;
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetStringField(TEXT("path"), Path))
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }

    void PinWrightGrassReachDeleteDirectory(const FString& Directory)
    {
        if (!Directory.IsEmpty())
        {
            IFileManager::Get().DeleteDirectory(*Directory, false, true);
        }
    }

    // Reads viewport.grass off a capture response, or reports why it could not.
    const TSharedPtr<FJsonObject>* PinWrightGrassReachViewportGrass(FAutomationTestBase& Test,
        const FTestResponseCapture& Capture)
    {
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Viewport =
            PinWrightGrassReachSubObject(Test, Capture.Result, TEXT("viewport"));
        if (!Viewport)
        {
            return nullptr;
        }
        return PinWrightGrassReachSubObject(Test, *Viewport, TEXT("grass"));
    }

    // The capture payload used for both halves of the projection A/B. One pose, one size; only
    // `projectionMode` and its companion parameter differ between the two calls.
    TSharedPtr<FJsonObject> PinWrightGrassReachCapturePayload(const FVector& Pose, bool bOrthographic)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("width"), 128);
        Payload->SetNumberField(TEXT("height"), 128);
        Payload->SetBoolField(TEXT("allowBlank"), true);

        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), Pose.X);
        Location->SetNumberField(TEXT("y"), Pose.Y);
        Location->SetNumberField(TEXT("z"), Pose.Z);
        Payload->SetObjectField(TEXT("location"), Location);

        // Straight down for both. An orthographic capture resolves its viewport TYPE from the
        // rotation, so the two projections have to be aimed the same way or the A/B compares two
        // different pieces of ground.
        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), -90.0);
        Rotation->SetNumberField(TEXT("yaw"), 0.0);
        Rotation->SetNumberField(TEXT("roll"), 0.0);
        Payload->SetObjectField(TEXT("rotation"), Rotation);

        if (bOrthographic)
        {
            Payload->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
            // 6928 cm is the world span an fov-60 frame covers at Z 6000 -- the matched coverage
            // the ticket's own measurement used.
            Payload->SetNumberField(TEXT("orthoWidth"), 6928.0);
        }
        else
        {
            Payload->SetNumberField(TEXT("fov"), 60.0);
        }
        return Payload;
    }
}

// ============================================================================
// The honesty contract, asserted as a pure function of the report.
// ============================================================================

// THE test for this ticket. Grass that is built, finished and entirely out of the frame's reach is
// a picture of bare ground that every other field in the response calls a clean success. Before
// this block there was no field that could contradict it, and a reviewer reading the frame
// concluded the vegetation was missing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassOutOfReachIsWarnedTest,
    "PinWright.render.grass_reach.BuiltSettledGrassOutOfReachIsWarned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassOutOfReachIsWarnedTest::RunTest(const FString& Parameters)
{
    FGrassBuildReport OutOfReach = PinWrightGrassReachMakeSettledReport();
    OutOfReach.bReachMeasured = true;
    OutOfReach.bCullingOriginMeasured = true;
    // The lit editor orthographic culling origin: ~2.1e6 cm off the camera, against a 10000 cm
    // grass end-cull. Every instance is out of range, uniformly - which is why the loss is total
    // rather than graded toward the frame edges.
    OutOfReach.CullingOrigin = FVector(0.0, 0.0, 6000.0 + 2097152.0);
    OutOfReach.NearestComponentDistance = 2097152.0;
    OutOfReach.GrassCullDistance = 10000.0;
    OutOfReach.ComponentsInReach = 0;
    OutOfReach.InstancesInReach = 0;

    const TSharedPtr<FJsonObject> Json =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(OutOfReach);

    // The build half still reads clean - that is the point. Nothing here is wrong; the frame is.
    bool bBuilt = false;
    TestTrue(TEXT("the block still reports the build ran for this pose"),
        Json->TryGetBoolField(TEXT("builtForPose"), bBuilt) && bBuilt);
    bool bSettled = false;
    TestTrue(TEXT("the block still reports the build finished"),
        Json->TryGetBoolField(TEXT("settled"), bSettled) && bSettled);

    const TSharedPtr<FJsonObject>* Reach =
        PinWrightGrassReachSubObject(*this, Json, TEXT("reach"));
    if (!Reach)
    {
        return false;
    }
    double InstancesInReach = -1.0;
    TestTrue(TEXT("the reach block reports how many instances can be in the frame"),
        (*Reach)->TryGetNumberField(TEXT("instancesInReach"), InstancesInReach));
    TestEqual(TEXT("none of the built grass can appear in this frame"), InstancesInReach, 0.0);
    double Pushback = -1.0;
    TestTrue(TEXT("the reach block publishes how far the culling origin sits off the camera"),
        (*Reach)->TryGetNumberField(TEXT("cullingOriginPushback"), Pushback));
    TestTrue(TEXT("that pushback is the ~2.1e6 cm an orthographic editor view applies"),
        Pushback > 2.0e6);

    FString Warning;
    TestTrue(TEXT("an empty forest is NOT returned as a plain success"),
        PinWrightGrassReachHasWarning(Json, Warning));
    TestTrue(FString::Printf(TEXT("the warning names culling as the cause, not absence (got: %s)"),
            *Warning),
        Warning.Contains(TEXT("culled")));
    TestTrue(TEXT("the warning says the grass IS built, so the caller does not go fixing content"),
        Warning.Contains(TEXT("BUILT")));
    return true;
}

// The other direction, so the warning cannot be hardcoded: grass inside the frame's reach is a
// working capture and must say nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassWithinReachIsNotWarnedTest,
    "PinWright.render.grass_reach.GrassWithinReachIsNotWarned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassWithinReachIsNotWarnedTest::RunTest(const FString& Parameters)
{
    FGrassBuildReport InReach = PinWrightGrassReachMakeSettledReport();
    InReach.bReachMeasured = true;
    InReach.bCullingOriginMeasured = true;
    InReach.CullingOrigin = InReach.CameraLocation;
    InReach.NearestComponentDistance = 6000.0;
    InReach.GrassCullDistance = 10000.0;
    InReach.ComponentsInReach = 12;
    InReach.InstancesInReach = InReach.InstancesAfter;

    const TSharedPtr<FJsonObject> Json = PinWrightCaptureGrass::MakeGrassBuildInfoObject(InReach);
    FString Warning;
    TestFalse(FString::Printf(TEXT("a frame whose grass is in range carries no warning (got: %s)"),
            *Warning),
        PinWrightGrassReachHasWarning(Json, Warning));

    const TSharedPtr<FJsonObject>* Reach =
        PinWrightGrassReachSubObject(*this, Json, TEXT("reach"));
    if (!Reach)
    {
        return false;
    }
    double InstancesInReach = 0.0;
    TestTrue(TEXT("the reach block is published on a healthy frame too"),
        (*Reach)->TryGetNumberField(TEXT("instancesInReach"), InstancesInReach));
    TestEqual(TEXT("every built instance can appear in this frame"),
        InstancesInReach, static_cast<double>(InReach.InstancesAfter));

    // A build that has NOT finished keeps its own warning: the unfinished-build case and the
    // out-of-reach case are different readings of one picture and must not swallow each other.
    FGrassBuildReport Unfinished = InReach;
    Unfinished.bSettled = false;
    Unfinished.PendingComponents = 3;
    const TSharedPtr<FJsonObject> UnfinishedJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(Unfinished);
    FString UnfinishedWarning;
    TestTrue(TEXT("an unfinished build still warns, in reach or not"),
        PinWrightGrassReachHasWarning(UnfinishedJson, UnfinishedWarning));
    TestFalse(TEXT("and it is the unfinished-build warning, not the culling one"),
        UnfinishedWarning.Contains(TEXT("culled")));
    return true;
}

// `instancesInReach: 0` is exactly the reading an unmeasured reach would be mistaken for, so the
// block is omitted rather than zeroed. docs/rpc-design.md section 4.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassUnmeasuredReachPublishesNoZerosTest,
    "PinWright.render.grass_reach.UnmeasuredReachPublishesNoZeros",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassUnmeasuredReachPublishesNoZerosTest::RunTest(const FString& Parameters)
{
    const FGrassBuildReport NotMeasured = PinWrightGrassReachMakeSettledReport();
    TestFalse(TEXT("the fixture leaves the reach unmeasured"), NotMeasured.bReachMeasured);

    const TSharedPtr<FJsonObject> Json =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(NotMeasured);
    const TSharedPtr<FJsonObject>* Reach = nullptr;
    TestFalse(TEXT("no reach block is invented for a measurement nobody took"),
        Json->TryGetObjectField(TEXT("reach"), Reach) && Reach != nullptr);
    FString Warning;
    TestFalse(TEXT("and no culling warning is raised on the strength of an absent measurement"),
        PinWrightGrassReachHasWarning(Json, Warning));

    // A world with no landscape at all is a different fact, and it is the one case where a
    // measured zero is the whole answer.
    FGrassBuildReport NoTerrain;
    NoTerrain.bMeasured = true;
    NoTerrain.bSettled = true;
    const TSharedPtr<FJsonObject> NoTerrainJson =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(NoTerrain);
    TestFalse(TEXT("a level with no terrain publishes no reach block either"),
        NoTerrainJson->TryGetObjectField(TEXT("reach"), Reach) && Reach != nullptr);
    return true;
}

// ============================================================================
// The behaviour: an ORTHOGRAPHIC capture builds its own grass.
// ============================================================================

// The ticket's own comparison, encoded. Builds a landscape IN-CODE through the production
// landscape.create handler, then captures the SAME pose twice -- once perspective at fov 60, once
// orthographic at the matched orthoWidth 6928 -- and requires the two responses to agree about the
// grass. On the pre-fix tree the orthographic half could not agree with anything: an orthographic
// editor view is excluded from the grass camera set by projection type (see the file header), so
// nothing ever built grass for it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassOrthographicPoseDrivesBuildTest,
    "PinWright.render.grass_reach.OrthographicCapturePoseDrivesTheGrassBuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassOrthographicPoseDrivesBuildTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        // Environment precondition, not the fixture. The fixture is built below and its absence
        // IS a failure.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-unavailable"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so no landscape could be "
                 "spawned and no orthographic capture could be taken."));
        return true;
    }

    TestTrue(TEXT("landscape.create handler registered"),
        IsHandlerRegistered(TEXT("landscape.create")));

    // Destroys any actor spawned during the test and restores the level dirty flag on scope exit,
    // so this test leaves the open map exactly as it found it.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString LandscapeLabel = FString::Printf(TEXT("PW_GrassReach_%s"),
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
        TestTrue(TEXT("landscape.create succeeded (fixture built)"),
            CreateCapture->bWasCalled && CreateCapture->bSuccess);
        if (!CreateCapture->bWasCalled || !CreateCapture->bSuccess)
        {
            return false;
        }
    }

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

    // The pose the ticket measured at: straight down from Z 6000 over the fixture.
    const FVector Pose(0.0, 0.0, 6000.0);

    FTestResponseCapture PerspectiveCapture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"),
            PinWrightGrassReachCapturePayload(Pose, /*bOrthographic=*/false), PerspectiveCapture));
    FTestResponseCapture OrthographicCapture;
    InvokeHandlerWithCapture(TEXT("render.capture_open_level"),
        PinWrightGrassReachCapturePayload(Pose, /*bOrthographic=*/true), OrthographicCapture);
    ON_SCOPE_EXIT
    {
        PinWrightGrassReachDeleteCaptureFile(PerspectiveCapture);
        PinWrightGrassReachDeleteCaptureFile(OrthographicCapture);
    };

    if (!OrthographicCapture.bSuccess || !OrthographicCapture.Result.IsValid())
    {
        // No live Level Editor viewport on this host (or another process holds the GPU). Visible
        // downstream rather than counted as a pass.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            FString::Printf(
                TEXT("the orthographic render.capture_open_level returned %s (code=%s, message=%s), "
                     "so its viewport.grass block could not be read."),
                OrthographicCapture.bSuccess ? TEXT("no result") : TEXT("an error"),
                *OrthographicCapture.ErrorCode, *OrthographicCapture.Message));
        return true;
    }

    // The frame really is orthographic - otherwise the assertions below are about a perspective
    // capture wearing the wrong label, and the defect would be invisible to them.
    FString ProjectionMode;
    TestTrue(TEXT("the response echoes the projection it rendered"),
        OrthographicCapture.Result->TryGetStringField(TEXT("projectionMode"), ProjectionMode));
    TestEqual(TEXT("the orthographic half really rendered orthographically"),
        ProjectionMode, FString(TEXT("orthographic")));

    const TSharedPtr<FJsonObject>* OrthoGrass =
        PinWrightGrassReachViewportGrass(*this, OrthographicCapture);
    if (!OrthoGrass)
    {
        AddError(TEXT("an orthographic capture published no viewport.grass block, so nothing in "
                      "the response can distinguish bare ground from grass that was never built "
                      "for this view"));
        return false;
    }

    bool bMeasured = false;
    TestTrue(TEXT("the orthographic capture measured its grass"),
        (*OrthoGrass)->TryGetBoolField(TEXT("measured"), bMeasured) && bMeasured);
    double Landscapes = 0.0;
    TestTrue(TEXT("the block reports how many landscapes it surveyed"),
        (*OrthoGrass)->TryGetNumberField(TEXT("landscapes"), Landscapes));
    TestTrue(TEXT("the fixture landscape was surveyed by the orthographic capture"),
        Landscapes > 0.0);

    // THE assertion this file exists for. An orthographic editor view is structurally excluded
    // from the engine's grass camera set, so nothing on the pre-fix tree could make this true.
    bool bBuiltForPose = false;
    TestTrue(TEXT("the ORTHOGRAPHIC capture's own pose drove its landscape-grass build"),
        (*OrthoGrass)->TryGetBoolField(TEXT("builtForPose"), bBuiltForPose) && bBuiltForPose);

    const TSharedPtr<FJsonObject>* GrassCamera =
        PinWrightGrassReachSubObject(*this, *OrthoGrass, TEXT("cameraLocation"));
    if (GrassCamera)
    {
        double Z = 0.0;
        (*GrassCamera)->TryGetNumberField(TEXT("z"), Z);
        TestTrue(FString::Printf(
                TEXT("the grass was built around the orthographic capture's own eye (z %.1f)"), Z),
            FMath::IsNearlyEqual(Z, Pose.Z, 1.0));
    }

    // ---- the ticket's A/B: the same ground, two projections, the same grass ----
    //
    // On a map with authored grass this is the 0.4796-vs-0.5161 comparison the ticket was filed
    // from, read off counts instead of luminance. On this in-code fixture (default
    // WorldGridMaterial, so no grass types) both sides are zero and only the routing assertions
    // above carry - which is stated rather than hidden, because a comparison that can only ever
    // pass is not evidence.
    const TSharedPtr<FJsonObject>* PerspectiveGrass =
        PinWrightGrassReachViewportGrass(*this, PerspectiveCapture);
    if (PerspectiveGrass)
    {
        double PerspectiveInstances = -1.0;
        double OrthoInstances = -2.0;
        const bool bBothReported =
            (*PerspectiveGrass)->TryGetNumberField(TEXT("instances"), PerspectiveInstances) &&
            (*OrthoGrass)->TryGetNumberField(TEXT("instances"), OrthoInstances);
        TestTrue(TEXT("both projections report an instance count"), bBothReported);
        if (bBothReported)
        {
            TestEqual(FString::Printf(
                    TEXT("the orthographic frame has the same grass as the perspective frame at "
                         "the same pose (perspective %.0f, orthographic %.0f)"),
                    PerspectiveInstances, OrthoInstances),
                OrthoInstances, PerspectiveInstances);
            if (PerspectiveInstances <= 0.0)
            {
                AddInfo(TEXT("BRANCH: the open level's landscape carries no grass types, so the "
                             "projection A/B compared zero against zero. The routing assertions "
                             "above are the ones that ran."));
            }
        }
    }
    return true;
}

// The verb this ticket names as the casualty: render.capture_ortho_tiles is the board's own
// prescribed workaround for wide-frame foliage loss, and for landscape grass it produced an empty
// frame - silently, with valid non-blank tiles. It renders through a transient
// USceneCaptureComponent2D, which is neither a perspective editor viewport nor a game viewport, so
// it can never be a grass camera at all.
//
// RHI-guarded, and asserts on BOTH branches: the block's presence is a response-shape contract
// that does not need pixels, and a burst that is refused must still not claim a grass measurement
// it did not take.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureGrassOrthoTilesPublishGrassTest,
    "PinWright.render.grass_reach.OrthoTileBurstPublishesGrass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureGrassOrthoTilesPublishGrassTest::RunTest(const FString& Parameters)
{
    const FString Prefix = FString::Printf(TEXT("pw_grass_reach_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Short));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    const auto MakeVec = [](double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> V = MakeShared<FJsonObject>();
        V->SetNumberField(TEXT("x"), X);
        V->SetNumberField(TEXT("y"), Y);
        V->SetNumberField(TEXT("z"), Z);
        return V;
    };
    Payload->SetObjectField(TEXT("worldMin"), MakeVec(-2000.0, -2000.0, 0.0));
    Payload->SetObjectField(TEXT("worldMax"), MakeVec(2000.0, 2000.0, 0.0));
    Payload->SetStringField(TEXT("axes"), TEXT("top_down_x_right_y_down"));
    Payload->SetNumberField(TEXT("exposure"), 11.0);
    Payload->SetNumberField(TEXT("cols"), 2.0);
    Payload->SetNumberField(TEXT("rows"), 2.0);
    Payload->SetNumberField(TEXT("tilePixels"), 64.0);
    Payload->SetStringField(TEXT("namePrefix"), Prefix);
    Payload->SetBoolField(TEXT("overwrite"), true);
    Payload->SetBoolField(TEXT("allowBlank"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_ortho_tiles handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    FString OutputDir;
    if (Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("outputDir"), OutputDir);
    }
    ON_SCOPE_EXIT
    {
        PinWrightGrassReachDeleteDirectory(OutputDir);
    };

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddInfo(FString::Printf(
            TEXT("BRANCH: the tile burst was REFUSED with %s - the grass-block assertions are "
                 "SKIPPED. Message: %s"),
            *Capture.ErrorCode, *Capture.Message));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("ortho-tile-capture-unavailable"),
            FString::Printf(TEXT("render.capture_ortho_tiles returned %s, so its grass block could "
                                 "not be read off a real burst."),
                *Capture.ErrorCode));
        return true;
    }

    const TSharedPtr<FJsonObject>* Grass =
        PinWrightGrassReachSubObject(*this, Capture.Result, TEXT("grass"));
    if (!Grass)
    {
        AddError(TEXT("a tile burst published no grass block, so a caller reviewing vegetation off "
                      "these tiles has nothing that could contradict an empty frame"));
        return false;
    }
    bool bMeasured = false;
    TestTrue(TEXT("the burst measured its landscape grass"),
        (*Grass)->TryGetBoolField(TEXT("measured"), bMeasured));
    TestTrue(TEXT("and it measured it on a real world"), bMeasured);

    double Landscapes = -1.0;
    TestTrue(TEXT("the burst reports how many landscapes it surveyed"),
        (*Grass)->TryGetNumberField(TEXT("landscapes"), Landscapes));
    if (Landscapes > 0.0)
    {
        // Only meaningful with terrain in the burst; on a level with none these two would be
        // numbers about nothing and are deliberately not published.
        bool bBuiltForPose = false;
        TestTrue(TEXT("each tile's own camera drove its grass build"),
            (*Grass)->TryGetBoolField(TEXT("builtForPose"), bBuiltForPose) && bBuiltForPose);
        double BuildMsTotal = -1.0;
        TestTrue(TEXT("the burst reports what the grass builds cost across every tile"),
            (*Grass)->TryGetNumberField(TEXT("buildMsTotal"), BuildMsTotal));
        TestTrue(TEXT("that cost is a real measurement, not a negative placeholder"),
            BuildMsTotal >= 0.0);
        bool bAllTilesSettled = false;
        TestTrue(TEXT("the burst reports whether EVERY tile's build finished, not just the last"),
            (*Grass)->TryGetBoolField(TEXT("allTilesSettled"), bAllTilesSettled));
    }
    else
    {
        AddInfo(TEXT("BRANCH: the open level has no landscape, so the burst-level grass numbers "
                     "are correctly absent. The block's presence and `measured` still ran."));
    }
    return true;
}
