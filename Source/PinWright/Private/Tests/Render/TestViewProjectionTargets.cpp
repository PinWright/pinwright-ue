// Copyright (c) 2026 Alexander Penkin. MIT License.

// PinWrightViewProjection::FViewProjectionTarget - rebuilding the capture view from a viewport
// client the CALLER names, instead of always from the active Level Editor one.
//
// WHY A NEW FILE. TestViewProjectionUtils.cpp is the existing floor for the default (level) path
// and has to keep passing untouched; nothing here edits it. Everything here is about the half that
// did not exist: which client the view is built from.
//
// WHAT MAKES THESE ASSERTIONS ABLE TO FAIL. The projection MATHS is a pure function of the request
// - same pose, same size, same FOV - so the projected pixel is identical whichever client built the
// view. Asserting a pixel therefore proves the maths and proves NOTHING about the targeting: a
// session that silently ignored the target and used the level viewport would produce exactly the
// same numbers. The two questions are separated on purpose:
//
//   * the GEOMETRY assertions pin the pixel against arithmetic done in the test (image centre for
//     the on-axis point; (Width/2)*tan(theta)/tan(Fov/2) for the off-axis one; and the
//     FOV-halving ratio tan(45)/tan(22.5) = 2.41421, which is the only one of the three that a
//     dropped `Fov` cannot survive), and
//   * the TARGETING assertions pin WHICH viewport moved: while the session is open the target
//     client must be sitting at the requested pose and the level viewport client must not have
//     moved at all, and after it closes the target must be back where it started.
//
// The target used is a real asset-editor preview viewport, acquired through the production
// resolver (PinWrightCaptureSubject::Resolve on a staticMesh subject) rather than a hand-built
// client, because that is the acquisition the annotated-capture verb actually uses and a fake one
// could not catch a provider that hands back a client without its scene viewport.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Render/ViewProjectionUtils.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "Slate/SceneViewport.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    using PinWrightRenderCapture::FViewportCaptureRequest;

    // The engine unit cube: always present, always has extent, and its editor is the plainest
    // asset editor in the engine.
    const TCHAR* const GProjectionTargetAssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // Camera 1000 cm behind the origin looking down +X, 90 deg FOV, square 1024x1024. The world
    // origin is then dead ahead (image centre), world +Y is screen-right and world +Z is screen-up.
    FViewportCaptureRequest ProjectionTargetMakePerspectiveView(float FovDegrees = 90.0f)
    {
        FViewportCaptureRequest View;
        View.ProjectionMode = TEXT("perspective");
        View.Location = FVector(-1000.0, 0.0, 0.0);
        View.Rotation = FRotator::ZeroRotator;
        View.Fov = FovDegrees;
        View.Width = 1024;
        View.Height = 1024;
        return View;
    }

    // The active Level Editor viewport's client, or null when this host has none. Used only to
    // assert that it did NOT move; every test degrades to reporting that half unmeasured.
    FEditorViewportClient* ProjectionTargetFindLevelViewportClient()
    {
        FLevelEditorModule* LevelEditorModule =
            FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (!LevelEditorModule)
        {
            return nullptr;
        }
        const TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
        if (!ActiveViewport.IsValid())
        {
            return nullptr;
        }
        return &ActiveViewport->GetAssetViewportClient();
    }

    // Resolve the cube's static-mesh preview viewport through the production resolver. Fills
    // OutSkipReason (and returns false) when this host cannot open one, so a caller reports
    // NOT MEASURED rather than a pass.
    bool ProjectionTargetAcquireStaticMeshPreview(PinWrightCaptureSubject::FResolvedSubject& OutSubject,
        FString& OutSkipReason)
    {
        PinWrightCaptureSubject::FSubjectRequest Request;
        Request.Kind = PinWrightCaptureSubject::ESubjectKind::StaticMesh;
        Request.AssetPath = GProjectionTargetAssetPath;
        Request.bProvided = true;
        Request.bKindProvided = true;

        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        FString ErrCode;
        FString ErrMsg;
        if (!PinWrightCaptureSubject::Resolve(Request, OutSubject, TimeSetter, ErrCode, ErrMsg))
        {
            OutSkipReason = FString::Printf(TEXT("%s: %s"), *ErrCode, *ErrMsg);
            return false;
        }
        if (!OutSubject.ViewportClient || !OutSubject.SceneViewport.IsValid())
        {
            OutSkipReason = TEXT("the resolver returned no preview viewport client / scene viewport");
            return false;
        }
        return true;
    }

    // Radial distance from the image centre, in pixels. Radial rather than a signed axis because
    // which screen axis world +Y lands on is an engine convention this file is not testing.
    double ProjectionTargetOffsetFromCentre(const FViewportCaptureRequest& View, float PixelX, float PixelY)
    {
        const double DX = static_cast<double>(PixelX) - static_cast<double>(View.Width) * 0.5;
        const double DY = static_cast<double>(PixelY) - static_cast<double>(View.Height) * 0.5;
        return FMath::Sqrt(DX * DX + DY * DY);
    }
}

// ============================================================================
// 1. A session opened against an explicit target builds the view from THAT client, and the pixels
//    it produces are the ones the arithmetic predicts.
//
// UNABLE TO FAIL IF it asserted only the pixel: the maths is client-independent, so a session that
// ignored the target and used the level viewport would produce identical numbers. The decisive
// assertion is therefore the pair "the preview client is sitting at the requested pose while the
// session is open" and "the level client has not moved", with the geometry asserted alongside so a
// correctly-targeted but wrongly-built view is caught too.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewProjectionExplicitTargetTest,
    "PinWright.spatial.view_projection.ExplicitTargetBuildsTheViewFromThatClient",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewProjectionExplicitTargetTest::RunTest(const FString& Parameters)
{
    PinWrightCaptureSubject::FResolvedSubject Subject;
    FString SkipReason;
    if (!ProjectionTargetAcquireStaticMeshPreview(Subject, SkipReason))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-static-mesh-preview-viewport"), FString::Printf(
            TEXT("%s. ")
            TEXT("NOT MEASURED - nothing below ran."), *SkipReason));
        return true;
    }
    // No explicit release: FResolvedSubject releases in its destructor, at the end of this
    // function - after every assertion below, including the post-session restore checks, which
    // need the preview viewport still alive to read. A second release here would be redundant
    // with the discipline the handler relies on.

    FEditorViewportClient* PreviewClient = Subject.ViewportClient;
    FEditorViewportClient* LevelClient = ProjectionTargetFindLevelViewportClient();
    // The whole point of the test is that these are two different viewports.
    if (!TestTrue(TEXT("the preview client is not the level viewport client"),
            LevelClient == nullptr || LevelClient != PreviewClient))
    {
        return true;
    }

    const FVector PreviewPoseBefore = PreviewClient->GetViewLocation();
    const FRotator PreviewRotationBefore = PreviewClient->GetViewRotation();
    const FVector LevelPoseBefore = LevelClient ? LevelClient->GetViewLocation() : FVector::ZeroVector;
    const FRotator LevelRotationBefore =
        LevelClient ? LevelClient->GetViewRotation() : FRotator::ZeroRotator;

    const FViewportCaptureRequest View = ProjectionTargetMakePerspectiveView();
    const PinWrightViewProjection::FViewProjectionTarget Target(*PreviewClient, Subject.SceneViewport);

    double OnAxisOffset = 0.0;
    double OffAxisOffset = 0.0;
    {
        const PinWrightViewProjection::FViewProjectionSession Session(View, Target);
        if (!Session.IsValid())
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("session-invalid"), FString::Printf(
                TEXT("%s. NOT MEASURED."),
                *Session.GetError()));
            return true;
        }

        // ---- TARGETING: which viewport actually moved. ----
        // ApplyCaptureCamera writes the requested location straight onto the client (after
        // suppressing orbit on the perspective path), so while the session is open the TARGET must
        // be standing at the requested pose.
        TestTrue(FString::Printf(
            TEXT("the TARGET client holds the requested camera location while the session is open ")
            TEXT("(got %s, want %s)"),
            *PreviewClient->GetViewLocation().ToString(), *View.Location.ToString()),
            PreviewClient->GetViewLocation().Equals(View.Location, 1.0));
        TestTrue(FString::Printf(
            TEXT("the TARGET client holds the requested camera rotation while the session is open ")
            TEXT("(got %s, want %s)"),
            *PreviewClient->GetViewRotation().ToString(), *View.Rotation.ToString()),
            PreviewClient->GetViewRotation().Equals(View.Rotation, 0.5f));

        // ...and the level viewport must be untouched. This is the assertion that fails if the
        // target were ignored: the old session mutated the level client on every call.
        if (LevelClient)
        {
            TestTrue(FString::Printf(
                TEXT("the LEVEL viewport client did not move (was %s, now %s)"),
                *LevelPoseBefore.ToString(), *LevelClient->GetViewLocation().ToString()),
                LevelClient->GetViewLocation().Equals(LevelPoseBefore, 0.01));
            TestTrue(FString::Printf(
                TEXT("the LEVEL viewport client did not turn (was %s, now %s)"),
                *LevelRotationBefore.ToString(), *LevelClient->GetViewRotation().ToString()),
                LevelClient->GetViewRotation().Equals(LevelRotationBefore, 0.01f));
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("the ")
                TEXT("'level viewport did not move' half is NOT MEASURED on this host. The ")
                TEXT("'target client moved' half above did run."));
        }

        // ---- GEOMETRY: the pixels the arithmetic predicts, through this client's view. ----
        float PixelX = 0.0f;
        float PixelY = 0.0f;
        bool bBehind = true;
        if (!TestTrue(TEXT("the world origin projects through the targeted view"),
                Session.Project(FVector::ZeroVector, PixelX, PixelY, bBehind)))
        {
            return true;
        }
        TestFalse(TEXT("the world origin is in front of the camera"), bBehind);
        OnAxisOffset = ProjectionTargetOffsetFromCentre(View, PixelX, PixelY);
        // Dead ahead maps to the principal point under every FOV. 12 px of slack on a 1024 image,
        // the same budget the sibling level-viewport test uses for sub-pixel jitter.
        TestTrue(FString::Printf(
            TEXT("the world origin lands on the image centre (got (%.2f,%.2f), %.2f px off centre)"),
            PixelX, PixelY, OnAxisOffset),
            OnAxisOffset <= 12.0);

        // 1000 cm ahead, 200 cm to the side: (Width/2)*(200/1000)/tan(45 deg) = 102.4 px.
        if (!TestTrue(TEXT("the off-axis point projects through the targeted view"),
                Session.Project(FVector(0.0, 200.0, 0.0), PixelX, PixelY, bBehind)))
        {
            return true;
        }
        TestFalse(TEXT("the off-axis point is in front of the camera"), bBehind);
        OffAxisOffset = ProjectionTargetOffsetFromCentre(View, PixelX, PixelY);
        const double ExpectedWide = 512.0 * 0.2 / FMath::Tan(FMath::DegreesToRadians(45.0));
        TestTrue(FString::Printf(
            TEXT("200 cm off-axis at 1000 cm lands %.2f px off centre (want ~%.2f)"),
            OffAxisOffset, ExpectedWide),
            FMath::Abs(OffAxisOffset - ExpectedWide) <= 16.0);
    }

    // ---- RESTORE: the target is put back, exactly as the level path is. ----
    TestTrue(FString::Printf(
        TEXT("the TARGET client's camera location is restored (was %s, now %s)"),
        *PreviewPoseBefore.ToString(), *PreviewClient->GetViewLocation().ToString()),
        PreviewClient->GetViewLocation().Equals(PreviewPoseBefore, 0.01));
    TestTrue(FString::Printf(
        TEXT("the TARGET client's camera rotation is restored (was %s, now %s)"),
        *PreviewRotationBefore.ToString(), *PreviewClient->GetViewRotation().ToString()),
        PreviewClient->GetViewRotation().Equals(PreviewRotationBefore, 0.01f));

    // ---- FOV: the one assertion a client-independent constant cannot fake. ----
    // Halving the field of view must magnify the SAME world point by tan(45)/tan(22.5) = 2.41421.
    // A view built without the request's Fov (or built from a different client's leftover FOV)
    // collapses this ratio to 1.0.
    const FViewportCaptureRequest NarrowView = ProjectionTargetMakePerspectiveView(45.0f);
    double NarrowOffset = 0.0;
    {
        const PinWrightViewProjection::FViewProjectionSession Session(NarrowView, Target);
        if (!TestTrue(TEXT("the narrow-FOV session is valid"), Session.IsValid()))
        {
            return true;
        }
        float PixelX = 0.0f;
        float PixelY = 0.0f;
        bool bBehind = true;
        if (!TestTrue(TEXT("the off-axis point projects at 45 deg too"),
                Session.Project(FVector(0.0, 200.0, 0.0), PixelX, PixelY, bBehind)))
        {
            return true;
        }
        NarrowOffset = ProjectionTargetOffsetFromCentre(NarrowView, PixelX, PixelY);
    }

    const double ExpectedRatio = FMath::Tan(FMath::DegreesToRadians(45.0)) /
        FMath::Tan(FMath::DegreesToRadians(22.5)); // 2.41421
    AddInfo(FString::Printf(
        TEXT("targeted off-axis offsets: fov 90 -> %.2f px, fov 45 -> %.2f px, ratio %.4f."),
        OffAxisOffset, NarrowOffset,
        OffAxisOffset > 0.0 ? NarrowOffset / OffAxisOffset : 0.0));
    TestTrue(FString::Printf(
        TEXT("halving the FOV magnifies the same point by %.4f (got %.4f)"),
        ExpectedRatio, OffAxisOffset > 0.0 ? NarrowOffset / OffAxisOffset : 0.0),
        OffAxisOffset > 0.0 && FMath::Abs(NarrowOffset / OffAxisOffset - ExpectedRatio) <= 0.12);
    return true;
}

// ============================================================================
// 2. Omitting the target still resolves the active Level Editor viewport, and the pixel is the one
//    an explicit level target produces.
//
// The regression half: every pre-existing caller (spatial.raycast_screen, spatial.place_on_surface,
// the level path of render.capture_annotated) passes no target, and this is what pins that the
// default did not quietly change meaning.
//
// UNABLE TO FAIL IF it compared a defaulted call against another defaulted call - that is a
// tautology. It compares the DEFAULT against an EXPLICIT target naming the level client, so the
// two agree only if the default really resolves that client.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewProjectionDefaultTargetIsTheLevelViewportTest,
    "PinWright.spatial.view_projection.OmittedTargetStillMeansTheLevelViewport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewProjectionDefaultTargetIsTheLevelViewportTest::RunTest(const FString& Parameters)
{
    FLevelEditorModule* LevelEditorModule =
        FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
    const TSharedPtr<IAssetViewport> ActiveViewport =
        LevelEditorModule ? LevelEditorModule->GetFirstActiveViewport() : nullptr;
    if (!ActiveViewport.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("there is no default ")
            TEXT("target to compare against. NOT MEASURED."));
        return true;
    }
    FEditorViewportClient& LevelClient = ActiveViewport->GetAssetViewportClient();
    const TSharedPtr<FSceneViewport> LevelSceneViewport = ActiveViewport->GetSharedActiveViewport();
    if (!LevelSceneViewport.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-scene-viewport"),
            TEXT("the level viewport has no scene viewport, so nothing below was measured"));
        return true;
    }

    const FViewportCaptureRequest View = ProjectionTargetMakePerspectiveView();
    const FVector Probe(0.0, 200.0, 150.0);

    float DefaultX = 0.0f;
    float DefaultY = 0.0f;
    bool bDefaultBehind = true;
    FString DefaultErr;
    const bool bDefaultOk = PinWrightViewProjection::ProjectWorldToScreen(
        View, Probe, DefaultX, DefaultY, bDefaultBehind, DefaultErr);
    if (!bDefaultOk)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("default-projection-failed"), FString::Printf(
            TEXT("%s. NOT MEASURED."),
            *DefaultErr));
        return true;
    }

    float TargetedX = 0.0f;
    float TargetedY = 0.0f;
    bool bTargetedBehind = true;
    FString TargetedErr;
    const bool bTargetedOk = PinWrightViewProjection::ProjectWorldToScreen(
        View, Probe, TargetedX, TargetedY, bTargetedBehind, TargetedErr,
        PinWrightViewProjection::FViewProjectionTarget(LevelClient, LevelSceneViewport));
    if (!TestTrue(FString::Printf(TEXT("the explicit level target projects: %s"), *TargetedErr),
            bTargetedOk))
    {
        return true;
    }

    // Same client, same request => the same pixel. THE TOLERANCE IS SPELLED OUT (0.01 px) rather
    // than left to TestEqual's default: the default for a float overload is KINDA_SMALL_NUMBER,
    // which is an assertion whose strictness is invisible at the call site, and the whole point
    // here is that the two calls must agree to far better than a pixel. 0.01 px absorbs nothing
    // real - two views built from the same client and the same request are bit-identical - while
    // being far below the tens of pixels a different view matrix would move the point.
    TestEqual(TEXT("the defaulted projection and the explicit level target agree in X"),
        TargetedX, DefaultX, 0.01f);
    TestEqual(TEXT("the defaulted projection and the explicit level target agree in Y"),
        TargetedY, DefaultY, 0.01f);
    TestTrue(TEXT("the defaulted projection and the explicit level target agree on behind-camera"),
        bTargetedBehind == bDefaultBehind);
    return true;
}

// ============================================================================
// 3. A target whose scene viewport is missing is a TYPED refusal, not a view built at the wrong
//    size and not a crash.
//
// The view rect CalcSceneView reads comes from the scene viewport's size, which SetFixedViewportSize
// is applied to. Without it there is no way to build a view at the captured dimensions, so every
// projected pixel would sit on a different grid from the PNG - plausible numbers, wrong frame.
//
// UNABLE TO FAIL IF it asserted only "the session is invalid": a null client would also be invalid.
// It asserts the code name in the error, and that the client it was handed was left alone.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewProjectionTargetWithoutSceneViewportTest,
    "PinWright.spatial.view_projection.TargetWithoutASceneViewportIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewProjectionTargetWithoutSceneViewportTest::RunTest(const FString& Parameters)
{
    PinWrightCaptureSubject::FResolvedSubject Subject;
    FString SkipReason;
    if (!ProjectionTargetAcquireStaticMeshPreview(Subject, SkipReason))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-static-mesh-preview-viewport"), FString::Printf(
            TEXT("%s. ")
            TEXT("NOT MEASURED."), *SkipReason));
        return true;
    }
    // No explicit release: FResolvedSubject releases in its destructor, at the end of this
    // function - after every assertion below, including the post-session restore checks, which
    // need the preview viewport still alive to read. A second release here would be redundant
    // with the discipline the handler relies on.

    FEditorViewportClient* PreviewClient = Subject.ViewportClient;
    const FVector PoseBefore = PreviewClient->GetViewLocation();

    // A real client, deliberately paired with no scene viewport.
    PinWrightViewProjection::FViewProjectionTarget Broken;
    Broken.Client = PreviewClient;
    TestTrue(TEXT("a client without a scene viewport still counts as an explicit target"),
        Broken.IsExplicit());

    const PinWrightViewProjection::FViewProjectionSession Session(
        ProjectionTargetMakePerspectiveView(), Broken);
    TestFalse(TEXT("a target with no scene viewport does not produce a view"), Session.IsValid());
    TestTrue(FString::Printf(TEXT("the refusal is typed PREVIEW_VIEWPORT_NOT_FOUND (got '%s')"),
        *Session.GetError()),
        Session.GetError().Contains(TEXT("PREVIEW_VIEWPORT_NOT_FOUND")));

    float PixelX = 1.0f;
    float PixelY = 1.0f;
    bool bBehind = false;
    TestFalse(TEXT("projecting through a refused session fails rather than inventing a pixel"),
        Session.Project(FVector::ZeroVector, PixelX, PixelY, bBehind));

    // Nothing was applied, so nothing needed restoring - and a refusal that had mutated the client
    // on its way out would be worse than the crash it replaced.
    TestTrue(FString::Printf(TEXT("the refused target's client was not moved (was %s, now %s)"),
        *PoseBefore.ToString(), *PreviewClient->GetViewLocation().ToString()),
        PreviewClient->GetViewLocation().Equals(PoseBefore, 0.01));
    return true;
}
