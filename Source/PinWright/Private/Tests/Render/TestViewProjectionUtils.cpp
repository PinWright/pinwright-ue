// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for PinWrightViewProjection (ViewProjectionUtils.h) — the screen<->world
// deproject/project helper that rebuilds the exact capture view via CalcSceneView.
//
// These are RHI-free math checks: they never read back pixels, they only assert the
// geometry of the rebuilt FSceneView (ray directions, round-trip pixel identity, center
// mapping). Each test frames a known perspective view (camera at origin looking +X, 90 deg
// FOV, 1024x1024) and drives the production util against the active Level Editor viewport.
//
// The util needs a live Level Editor viewport to rebuild the view. When the automation host
// has none (headless CI with no viewport), the util returns a typed NO_ACTIVE_LEVEL_VIEWPORT /
// EDITOR_NOT_AVAILABLE / NO_EDITOR_WORLD error; the tests detect that and AddInfo+skip
// (return true) rather than false-negative.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Render/ViewProjectionUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    using PinWrightRenderCapture::FViewportCaptureRequest;

    // Known perspective view: camera at world origin, FRotator(0,0,0) => forward is +X,
    // 90 deg FOV, square 1024x1024 so the center pixel is (512,512) and horizontal FOV == vertical.
    FViewportCaptureRequest MakeKnownPerspectiveView()
    {
        FViewportCaptureRequest View;
        View.ProjectionMode = TEXT("perspective");
        View.Location = FVector::ZeroVector;
        View.Rotation = FRotator::ZeroRotator;
        View.Fov = 90.0f;
        View.Width = 1024;
        View.Height = 1024;
        return View;
    }

    // True when the util failed because the host has no usable viewport/world (a skip condition),
    // as opposed to a genuine math/build fault (a real failure).
    bool IsNoViewportError(const FString& Err)
    {
        return Err.Contains(TEXT("NO_ACTIVE_LEVEL_VIEWPORT"))
            || Err.Contains(TEXT("EDITOR_NOT_AVAILABLE"))
            || Err.Contains(TEXT("NO_EDITOR_WORLD"))
            || Err.Contains(TEXT("VIEW_BUILD_FAILED"));
    }
}

// ============================================================================
// Center pixel deprojects to camera forward (+X), origin near the camera on-axis.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewProjectionCenterRayForwardTest,
    "PinWright.spatial.view_projection.CenterRayIsForward",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewProjectionCenterRayForwardTest::RunTest(const FString& Parameters)
{
    const FViewportCaptureRequest View = MakeKnownPerspectiveView();

    FVector Origin = FVector::ZeroVector;
    FVector Dir = FVector::ZeroVector;
    FString Err;
    const bool bOk = PinWrightViewProjection::DeprojectScreenToWorld(
        View, View.Width * 0.5f, View.Height * 0.5f, Origin, Dir, Err);

    if (!bOk)
    {
        if (IsNoViewportError(Err))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
                FString::Printf(
                    TEXT("Skipped: no usable Level Editor viewport for the rebuilt view (%s)."), *Err));
            return true;
        }
        TestTrue(*FString::Printf(TEXT("deproject failed unexpectedly: %s"), *Err), false);
        return true;
    }

    // The load-bearing sign check: the center ray must point along the camera's forward (+X).
    TestTrue(TEXT("center ray direction is camera forward (+X)"),
        Dir.Equals(FVector(1.0, 0.0, 0.0), 0.02));

    // The center ray passes through the camera along +X, so its near-plane origin sits on the
    // +X axis (Y,Z ~ 0, X >= 0). Verifies the deproject origin is on-axis, catching a swapped axis.
    TestTrue(TEXT("center ray origin is on the camera forward axis (Y,Z ~ 0)"),
        FMath::Abs(Origin.Y) < 1.0 && FMath::Abs(Origin.Z) < 1.0);
    TestTrue(TEXT("center ray origin is in front of the camera (X >= 0)"),
        Origin.X >= -0.01);

    return true;
}

// ============================================================================
// Round-trip: Project(a point along Deproject(px)) ~= px for several off-center pixels.
// View-rect-independent self-consistency check (both directions rebuild the identical view).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewProjectionRoundTripTest,
    "PinWright.spatial.view_projection.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewProjectionRoundTripTest::RunTest(const FString& Parameters)
{
    const FViewportCaptureRequest View = MakeKnownPerspectiveView();

    const TArray<FVector2D> Pixels = {
        FVector2D(512.0, 512.0), // center
        FVector2D(300.0, 400.0),
        FVector2D(700.0, 250.0),
        FVector2D(850.0, 780.0),
    };

    bool bSkipped = false;
    for (const FVector2D& Px : Pixels)
    {
        FVector Origin = FVector::ZeroVector;
        FVector Dir = FVector::ZeroVector;
        FString Err;
        if (!PinWrightViewProjection::DeprojectScreenToWorld(
            View, static_cast<float>(Px.X), static_cast<float>(Px.Y), Origin, Dir, Err))
        {
            if (IsNoViewportError(Err))
            {
                AddInfo(FString::Printf(
                    TEXT("Skipped: no usable Level Editor viewport for the rebuilt view (%s)."), *Err));
                bSkipped = true;
                break;
            }
            TestTrue(*FString::Printf(TEXT("deproject failed unexpectedly: %s"), *Err), false);
            return true;
        }

        // Take a point 1000 units down the ray and project it back; it must land on the source pixel.
        const FVector WorldPoint = Origin + Dir * 1000.0;
        float OutX = 0.0f;
        float OutY = 0.0f;
        bool bBehind = true;
        FString ProjErr;
        const bool bProjected = PinWrightViewProjection::ProjectWorldToScreen(
            View, WorldPoint, OutX, OutY, bBehind, ProjErr);
        TestTrue(*FString::Printf(TEXT("project succeeded for pixel (%.0f,%.0f): %s"),
            Px.X, Px.Y, *ProjErr), bProjected);
        if (!bProjected)
        {
            continue;
        }

        TestFalse(*FString::Printf(TEXT("forward point for pixel (%.0f,%.0f) is in front of camera"),
            Px.X, Px.Y), bBehind);
        // 2px tolerance: DeprojectScreenToWorld truncates the pixel to an integer before mapping,
        // so the round-trip carries up to ~1px of sub-pixel rounding on each axis.
        TestTrue(*FString::Printf(TEXT("round-trip X for pixel (%.0f,%.0f): got %.2f"),
            Px.X, Px.Y, OutX), FMath::Abs(OutX - Px.X) <= 2.0f);
        TestTrue(*FString::Printf(TEXT("round-trip Y for pixel (%.0f,%.0f): got %.2f"),
            Px.X, Px.Y, OutY), FMath::Abs(OutY - Px.Y) <= 2.0f);
    }

    if (bSkipped)
    {
        return true;
    }
    return true;
}

// ============================================================================
// A point straight ahead of the camera projects near the image center.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewProjectionForwardProjectsToCenterTest,
    "PinWright.spatial.view_projection.ForwardProjectsToCenter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewProjectionForwardProjectsToCenterTest::RunTest(const FString& Parameters)
{
    const FViewportCaptureRequest View = MakeKnownPerspectiveView();

    // Camera at origin looking +X: a point straight ahead is on the +X axis.
    const FVector ForwardPoint(1000.0, 0.0, 0.0);
    float OutX = 0.0f;
    float OutY = 0.0f;
    bool bBehind = true;
    FString Err;
    const bool bOk = PinWrightViewProjection::ProjectWorldToScreen(
        View, ForwardPoint, OutX, OutY, bBehind, Err);

    if (!bOk)
    {
        if (IsNoViewportError(Err))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
                FString::Printf(
                    TEXT("Skipped: no usable Level Editor viewport for the rebuilt view (%s)."), *Err));
            return true;
        }
        TestTrue(*FString::Printf(TEXT("project failed unexpectedly: %s"), *Err), false);
        return true;
    }

    TestFalse(TEXT("forward point is in front of the camera"), bBehind);

    // The forward axis lands on the principal point (image center) for a centered projection.
    // 12px slack on a 1024 image absorbs temporal-AA sub-pixel jitter and integer rounding while
    // still catching a flipped-axis or off-by-half-image sign error.
    const float CenterX = View.Width * 0.5f;
    const float CenterY = View.Height * 0.5f;
    TestTrue(*FString::Printf(TEXT("forward point projects near center X (got %.2f, want ~%.1f)"),
        OutX, CenterX), FMath::Abs(OutX - CenterX) <= 12.0f);
    TestTrue(*FString::Printf(TEXT("forward point projects near center Y (got %.2f, want ~%.1f)"),
        OutY, CenterY), FMath::Abs(OutY - CenterY) <= 12.0f);

    return true;
}

// ============================================================================
// Top-down orthographic: the requested rotation reaches the view matrix, and orthoWidth is world cm.
//
// Camera 1000 cm above the origin looking straight down, framing 1000 cm across a 1024 px image =>
// exactly 1.024 px per cm. A world point 300 cm along +X and one 300 cm along +Y must therefore land
// 307.2 px from the centre, each on a DIFFERENT screen axis. That magnitude check is the unit test
// for orthoWidth (the old ortho-zoom meaning would frame ~14 m and put both points ~22 px out).
//
// The direction checks encode the engine's fixed in-plane orientation for LVT_OrthoXY, which is
// version-dependent (EditorViewportClient.cpp, the LVT_OrthoXY ViewRotationMatrix):
//   5.6+   screen right = world -Y, screen up = world -X  -> +X goes DOWN,  +Y goes LEFT
//   <= 5.5 screen right = world +X, screen up = world -Y  -> +X goes RIGHT, +Y goes DOWN
// GOrthoViewportTypes in PreviewViewportCaptureUtils.cpp carries the same split, and the two must
// agree. If this test fails on a supported host the matrices moved again: both need a new row.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewProjectionOrthoTopDownOrientationTest,
    "PinWright.spatial.view_projection.OrthoTopDownOrientation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewProjectionOrthoTopDownOrientationTest::RunTest(const FString& Parameters)
{
    FViewportCaptureRequest View;
    View.ProjectionMode = TEXT("orthographic");
    View.Location = FVector(0.0, 0.0, 1000.0);
    View.Rotation = FRotator(-90.0f, 0.0f, 0.0f);
    View.OrthoWidth = 1000.0f; // world centimetres
    View.Width = 1024;
    View.Height = 1024;

    const float CenterX = View.Width * 0.5f;
    const float CenterY = View.Height * 0.5f;
    const float PixelsPerCm = View.Width / View.OrthoWidth;
    const float ExpectedOffset = 300.0f * PixelsPerCm; // 307.2 px

    float PlusXPixelX = 0.0f;
    float PlusXPixelY = 0.0f;
    float PlusYPixelX = 0.0f;
    float PlusYPixelY = 0.0f;
    bool bBehind = true;
    FString Err;

    if (!PinWrightViewProjection::ProjectWorldToScreen(
        View, FVector(300.0, 0.0, 0.0), PlusXPixelX, PlusXPixelY, bBehind, Err))
    {
        if (IsNoViewportError(Err))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
                FString::Printf(
                    TEXT("Skipped: no usable Level Editor viewport for the rebuilt view (%s)."), *Err));
            return true;
        }
        TestTrue(*FString::Printf(TEXT("orthographic project failed unexpectedly: %s"), *Err), false);
        return true;
    }
    if (!PinWrightViewProjection::ProjectWorldToScreen(
        View, FVector(0.0, 300.0, 0.0), PlusYPixelX, PlusYPixelY, bBehind, Err))
    {
        TestTrue(*FString::Printf(TEXT("orthographic project failed unexpectedly: %s"), *Err), false);
        return true;
    }

    // Where 300 cm along each world axis must land, in pixels (+X right, +Y down), under this
    // engine's LVT_OrthoXY basis - see the header comment for the 5.6 change. Both the SCALE (the
    // orthoWidth unit contract: 300 cm on a 1000 cm frame is 307.2 px) and the ORIENTATION (which
    // screen axis, and which way along it) are in these two vectors, so a zoom-meaning regression
    // and an engine basis rotation are both caught, and the failure message names the pixel.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    const FVector2D PlusXScreenDir(0.0, 1.0);    // world +X goes DOWN the image
    const FVector2D PlusYScreenDir(-1.0, 0.0);   // world +Y goes LEFT
#else
    const FVector2D PlusXScreenDir(1.0, 0.0);    // world +X goes RIGHT
    const FVector2D PlusYScreenDir(0.0, 1.0);    // world +Y goes DOWN
#endif
    const float ExpectedPlusXPixelX = CenterX + static_cast<float>(PlusXScreenDir.X) * ExpectedOffset;
    const float ExpectedPlusXPixelY = CenterY + static_cast<float>(PlusXScreenDir.Y) * ExpectedOffset;
    const float ExpectedPlusYPixelX = CenterX + static_cast<float>(PlusYScreenDir.X) * ExpectedOffset;
    const float ExpectedPlusYPixelY = CenterY + static_cast<float>(PlusYScreenDir.Y) * ExpectedOffset;

    // 8 px of slack absorbs sub-pixel rounding without admitting a different zoom meaning.
    TestTrue(*FString::Printf(TEXT("+X 300cm lands at pixel X %.1f (want ~%.1f)"),
        PlusXPixelX, ExpectedPlusXPixelX),
        FMath::Abs(PlusXPixelX - ExpectedPlusXPixelX) <= 8.0f);
    TestTrue(*FString::Printf(TEXT("+X 300cm lands at pixel Y %.1f (want ~%.1f)"),
        PlusXPixelY, ExpectedPlusXPixelY),
        FMath::Abs(PlusXPixelY - ExpectedPlusXPixelY) <= 8.0f);
    TestTrue(*FString::Printf(TEXT("+Y 300cm lands at pixel X %.1f (want ~%.1f)"),
        PlusYPixelX, ExpectedPlusYPixelX),
        FMath::Abs(PlusYPixelX - ExpectedPlusYPixelX) <= 8.0f);
    TestTrue(*FString::Printf(TEXT("+Y 300cm lands at pixel Y %.1f (want ~%.1f)"),
        PlusYPixelY, ExpectedPlusYPixelY),
        FMath::Abs(PlusYPixelY - ExpectedPlusYPixelY) <= 8.0f);

    return true;
}

// ============================================================================
// Perspective FOV reaches the projection, at the right MAGNITUDE.
//
// The three perspective tests above are all FOV-blind, and that is not an oversight that a
// tighter tolerance would fix — none of them can see the field of view at all:
//   * CenterRayIsForward and ForwardProjectsToCenter only exercise the optical axis, which maps
//     to the principal point under every FOV.
//   * RoundTrip deprojects and reprojects through the SAME rebuilt FSceneView, so any error in
//     the shared projection matrix cancels exactly — the round trip returns the source pixel
//     whether the FOV is 90, 50, or whatever the viewport happened to be left on.
// Delete `ViewportClient.FOVAngle = Request.Fov; ViewportClient.ViewFOV = Request.Fov;`
// (PreviewViewportCaptureUtils.cpp, ApplyCaptureCamera) and all three still pass, while every
// off-axis pixel in every perspective capture, raycast and label anchor lands in the wrong place.
//
// This test measures the one thing that carries the FOV: the off-axis scale. For a square view,
// a point θ off the optical axis projects (Width/2)·tanθ/tan(FOV/2) pixels from the centre, so
// halving the FOV must magnify the SAME world point by tan(45°)/tan(22.5°) = 2.41421.
//
// The RATIO is the load-bearing assertion because it is convention-free: it holds whichever
// screen axis world +Y maps to, whichever sign that axis runs in, and whether the engine treats
// FOVAngle as horizontal or vertical (a square viewport makes the two identical). A dropped or
// ignored `Fov` collapses the ratio to 1.0. The absolute offsets are asserted too, more loosely,
// so a plausible-ratio-but-wrong-scale error still has something to trip over.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewProjectionPerspectiveFovScaleTest,
    "PinWright.spatial.view_projection.PerspectiveFovSetsOffAxisScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewProjectionPerspectiveFovScaleTest::RunTest(const FString& Parameters)
{
    // 1000 cm ahead and 200 cm to the side: 11.3 deg off-axis, comfortably inside the frame at
    // both fields of view below, so neither measurement is taken near a clipped edge.
    const FVector OffAxisPoint(1000.0, 200.0, 0.0);

    const auto OffsetFromCentreAtFov = [this, &OffAxisPoint](float FovDegrees, double& OutOffset) -> bool
    {
        FViewportCaptureRequest View = MakeKnownPerspectiveView();
        View.Fov = FovDegrees;

        float OutX = 0.0f;
        float OutY = 0.0f;
        bool bBehind = true;
        FString Err;
        if (!PinWrightViewProjection::ProjectWorldToScreen(View, OffAxisPoint, OutX, OutY, bBehind, Err))
        {
            if (IsNoViewportError(Err))
            {
                AddInfo(FString::Printf(
                    TEXT("Skipped: no usable Level Editor viewport for the rebuilt view (%s)."), *Err));
                return false;
            }
            TestTrue(*FString::Printf(TEXT("project at fov %.0f failed unexpectedly: %s"),
                FovDegrees, *Err), false);
            return false;
        }
        TestFalse(*FString::Printf(TEXT("the off-axis point is in front of the camera at fov %.0f"),
            FovDegrees), bBehind);

        // Radial distance rather than a signed axis: which screen axis world +Y lands on, and in
        // which direction, is the engine's convention and is not what this test is about.
        const double DX = static_cast<double>(OutX) - static_cast<double>(View.Width) * 0.5;
        const double DY = static_cast<double>(OutY) - static_cast<double>(View.Height) * 0.5;
        OutOffset = FMath::Sqrt(DX * DX + DY * DY);
        return true;
    };

    double WideOffset = 0.0;
    double NarrowOffset = 0.0;
    if (!OffsetFromCentreAtFov(90.0f, WideOffset) || !OffsetFromCentreAtFov(45.0f, NarrowOffset))
    {
        return true;
    }

    // (Width/2)·(200/1000)/tan(FOV/2), Width 1024.
    const double ExpectedWide = 512.0 * 0.2 / FMath::Tan(FMath::DegreesToRadians(45.0));   // 102.4 px
    const double ExpectedNarrow = 512.0 * 0.2 / FMath::Tan(FMath::DegreesToRadians(22.5)); // 247.2 px
    const double ExpectedRatio = ExpectedNarrow / ExpectedWide;                            // 2.41421

    AddInfo(FString::Printf(
        TEXT("off-axis offset: fov 90 -> %.2f px (want ~%.2f), fov 45 -> %.2f px (want ~%.2f)."),
        WideOffset, ExpectedWide, NarrowOffset, ExpectedNarrow));

    // The decisive one. An ignored `Fov` makes both captures identical and this lands on 1.0.
    TestTrue(*FString::Printf(
        TEXT("halving the FOV magnifies the same point by tan(45)/tan(22.5) (got %.4f, want %.4f)"),
        WideOffset > 0.0 ? NarrowOffset / WideOffset : 0.0, ExpectedRatio),
        WideOffset > 0.0 && FMath::Abs(NarrowOffset / WideOffset - ExpectedRatio) <= 0.12);

    // Absolute scale, loosely: 16 px of slack absorbs the integer/sub-pixel rounding the sibling
    // tests already budget for, and is far under the ~2.4x error a wrong FOV produces.
    TestTrue(*FString::Printf(TEXT("fov 90 puts the point %.2f px off centre (want ~%.2f)"),
        WideOffset, ExpectedWide), FMath::Abs(WideOffset - ExpectedWide) <= 16.0);
    TestTrue(*FString::Printf(TEXT("fov 45 puts the point %.2f px off centre (want ~%.2f)"),
        NarrowOffset, ExpectedNarrow), FMath::Abs(NarrowOffset - ExpectedNarrow) <= 16.0);

    return true;
}
