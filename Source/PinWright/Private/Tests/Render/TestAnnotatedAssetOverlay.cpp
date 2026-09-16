// Copyright (c) 2026 Alexander Penkin. MIT License.

// render.capture_annotated over an ASSET-EDITOR PREVIEW viewport: the frame comes from the asset's
// own preview, and the overlay ink lands on the pixels the arithmetic predicts for THAT frame.
//
// WHY THIS FILE EXISTS. The verb used to refuse every asset kind with UNSUPPORTED_ASSET_EDITOR
// because FViewProjectionSession rebuilt its view from the active Level Editor viewport by
// construction; an overlay painted with it over a preview frame would have been measured for a
// viewport that frame was never drawn in. The session now takes an FViewProjectionTarget and the
// handler hands it the same client the capture rendered through. That is the change under test.
//
// WHY THE ASSERTION IS PIXEL-LEVEL AND NOT RESPONSE-LEVEL. A projection pointed at the WRONG
// viewport still returns a valid PNG, a populated `subject` block and a plausible `framing`
// verdict - every response-shape assertion passes while the axes sit somewhere the frame never
// had. The only assertion that can tell the two apart is "is the ink at the computable
// coordinate", so this file decodes the PNG and looks.
//
// THE ARITHMETIC, worked out here rather than read off the implementation:
//   * No `grid` argument, so the handler's axis length is its 500 cm default
//     (AnnotatedCaptureHandler.cpp, AxisLen).
//   * Camera at (-1000, 0, 0) with rotation (0,0,0) looks down world +X, so the world origin is
//     1000 cm dead ahead and lands on the principal point: (128,128) on a 256x256 image.
//   * A point 500 cm off-axis at 1000 cm depth, at FOV 90, lands (Width/2)*(500/1000)/tan(45 deg)
//     = 128 * 0.5 = 64 px from the centre.
//   * UE's view basis: world +X is view depth, world +Y is view RIGHT and world +Z is view UP
//     (the projection's own normalisation is x = ndc.x/2 + 0.5, y = 1 - ndc.y/2 - 0.5, see
//     FViewProjectionSession::Project). So the +Y axis endpoint is at (192,128) - 64 px RIGHT of
//     centre - and the +Z endpoint at (128,64) - 64 px ABOVE it. The +X axis endpoint is dead
//     ahead and projects onto the centre itself, which is why the red axis is not asserted.
//
// So the green (+Y) segment runs from the centre rightwards and the blue (+Z) segment from the
// centre upwards, both 3 px thick. The test measures the FULL EXTENT of each exact overlay colour
// across the decoded image and compares all four bounds against those coordinates with an explicit
// 3 px tolerance - one pixel for the Bresenham pen's half-width, one for the round-to-int of the
// projected endpoint, one of headroom. Nothing uses a defaulted tolerance.
//
// EVERY capture here is 256x256, matching the sibling subject tests: a capture size that varies
// within an editor session trips FViewport::GetHitProxy's ProxyMap assertion.
//
// LIGHTING IS NOT A PRECONDITION HERE, and that is deliberate rather than an oversight of the
// "an unlit frame is NOT MEASURED" rule: the overlay is synthetic ink composited at alpha 255
// (PinWrightBitmapPaint::CompositeInto takes an exact byte-replace path for an opaque paint), so
// its placement is measurable on a frame of any brightness. The frame's own statistics are
// reported through AddInfo so a dark preview is visible in the log rather than hidden - but no
// assertion here is an assertion about what the SCENE rendered.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/ScopeExit.h"

namespace
{
    const TCHAR* const GAnnotatedAssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // The overlay colours, as LITERALS read off AnnotatedCaptureHandler.cpp's GAxisYColor /
    // GAxisZColor. Deliberately duplicated rather than exported: they are file-local constants in
    // a uniquely-named namespace, and a test that imported them would agree with the handler by
    // construction even after somebody changed both. Restating them here means a silent recolour
    // fails this test, which is the point.
    const FColor GExpectedAxisYColor(40, 200, 40, 255);   // world +Y (green)
    const FColor GExpectedAxisZColor(60, 120, 240, 255);  // world +Z (blue)

    // The typed, non-crashing exits the verb may return when this host cannot open or render a
    // static-mesh preview; the resolver's own set is read off CaptureSubject.cpp's OutErrCode
    // assignments. A LOCAL copy of the list the sibling files keep, for the same reason they keep
    // their own: the frozen regression floor must not be edited to export a helper.
    // INVALID_ARGUMENT is deliberately NOT in the list - it is what a malformed payload returns,
    // and a payload defect must fail this file rather than skip it.
    bool AnnotatedAssetIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            ErrorCode == TEXT("UNSUPPORTED_ASSET_EDITOR") ||
            ErrorCode == TEXT("SUBSYSTEM_MISSING") ||
            ErrorCode == TEXT("OPEN_FAILED") ||
            ErrorCode == TEXT("ASSET_NOT_FOUND") ||
            ErrorCode == TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION") ||
            ErrorCode == TEXT("DECODE_FAILED") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED");
    }

    // The extent of one exact overlay colour across the whole image, so the painted axis can be
    // measured rather than merely spotted.
    struct FAnnotatedAssetColorExtent
    {
        int32 Count = 0;
        int32 MinX = MAX_int32;
        int32 MaxX = MIN_int32;
        int32 MinY = MAX_int32;
        int32 MaxY = MIN_int32;

        FString Describe() const
        {
            return Count == 0
                ? FString(TEXT("no pixels of this colour"))
                : FString::Printf(TEXT("%d px, x in [%d,%d], y in [%d,%d]"),
                    Count, MinX, MaxX, MinY, MaxY);
        }
    };

    // RGB-exact. Alpha is ignored: the capture stamps an opaque alpha over the whole frame, so it
    // carries no information here, and the overlay composites at alpha 255 (an opaque FPaint takes
    // PinWrightBitmapPaint's exact byte-replace path), so the painted bytes survive the lossless
    // PNG round trip unchanged.
    FAnnotatedAssetColorExtent AnnotatedAssetMeasureColorExtent(const TArrayView64<FColor>& Pixels, int32 W, int32 H,
        const FColor& Wanted)
    {
        FAnnotatedAssetColorExtent Out;
        for (int32 Y = 0; Y < H; ++Y)
        {
            for (int32 X = 0; X < W; ++X)
            {
                const FColor& Px = Pixels[static_cast<int64>(Y) * W + X];
                if (Px.R == Wanted.R && Px.G == Wanted.G && Px.B == Wanted.B)
                {
                    ++Out.Count;
                    Out.MinX = FMath::Min(Out.MinX, X);
                    Out.MaxX = FMath::Max(Out.MaxX, X);
                    Out.MinY = FMath::Min(Out.MinY, Y);
                    Out.MaxY = FMath::Max(Out.MaxY, Y);
                }
            }
        }
        return Out;
    }

    void AnnotatedAssetDeleteFileIfPresent(const FString& Path)
    {
        if (!Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }

    TSharedPtr<FJsonObject> AnnotatedAssetMakeVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    TSharedPtr<FJsonObject> AnnotatedAssetMakeRot(double Pitch, double Yaw, double Roll)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("pitch"), Pitch);
        Obj->SetNumberField(TEXT("yaw"), Yaw);
        Obj->SetNumberField(TEXT("roll"), Roll);
        return Obj;
    }
}

// ============================================================================
// 1. A static-mesh subject is captured from its own preview viewport, and the overlay ink lands on
//    the computed pixels of THAT frame.
//
// UNABLE TO FAIL IF it asserted only that the response carried an overlay echo, a `subject` block
// or a non-blank PNG: a session pointed at the wrong viewport produces all three. It measures the
// painted extent of each exact overlay colour and compares all four bounds against coordinates this
// test computes from the FOV, with an explicit 3 px tolerance - and it refuses to make that
// comparison unless the frame's MEASURED camera pose is the one the arithmetic was done for
// (viewport.aim.applied), because a mis-aimed frame invalidates every expected coordinate.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnnotatedAssetOverlayGeometryTest,
    "PinWright.render.capture_annotated.AssetOverlayLandsOnTheComputedPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnnotatedAssetOverlayGeometryTest::RunTest(const FString& Parameters)
{
    constexpr int32 ImageSize = 256;
    constexpr int32 Centre = ImageSize / 2;                  // 128
    // (Width/2) * (500 cm / 1000 cm) / tan(FOV/2 = 45 deg) = 128 * 0.5 = 64 px.
    constexpr int32 AxisEndOffset = 64;

    TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
    Subject->SetStringField(TEXT("kind"), TEXT("staticMesh"));
    Subject->SetStringField(TEXT("path"), GAnnotatedAssetPath);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), ImageSize);
    Payload->SetNumberField(TEXT("height"), ImageSize);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    Payload->SetNumberField(TEXT("fov"), 90.0);
    Payload->SetObjectField(TEXT("location"), AnnotatedAssetMakeVec(-1000.0, 0.0, 0.0));
    Payload->SetObjectField(TEXT("rotation"), AnnotatedAssetMakeRot(0.0, 0.0, 0.0));
    Payload->SetBoolField(TEXT("axes"), true);
    // Labels off: a glyph plate near an axis endpoint would add ink this test does not model.
    Payload->SetBoolField(TEXT("labels"), false);
    Payload->SetObjectField(TEXT("subject"), Subject);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess)
    {
        // The one failure that would mean the lift did not happen at all. Matched on the MESSAGE
        // rather than the code, because UNSUPPORTED_ASSET_EDITOR is also what the resolver
        // legitimately returns when a host's toolkit is not one this plugin will cast through
        // (CaptureSubject.cpp) - only the verb's own blanket refusal said "cannot annotate".
        TestFalse(FString::Printf(
            TEXT("the verb no longer refuses an asset subject outright (got: %s)"), *Capture.Message),
            Capture.Message.Contains(TEXT("cannot annotate")));
        TestTrue(TEXT("an asset subject is a declared, accepted argument"),
            Capture.ErrorCode != TEXT("UNKNOWN_PARAMS"));
        TestTrue(FString::Printf(TEXT("capture failure is typed (got '%s': %s)"),
            *Capture.ErrorCode, *Capture.Message),
            AnnotatedAssetIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-preview-capture"),
            FString::Printf(TEXT("the capture returned %s, and the overlay geometry needs a "
                                 "rendered frame"), *Capture.ErrorCode));
        return true;
    }
    if (!TestTrue(TEXT("success result exists"), Capture.Result.IsValid()))
    {
        return true;
    }

    FString CapturedPath;
    Capture.Result->TryGetStringField(TEXT("path"), CapturedPath);
    ON_SCOPE_EXIT { AnnotatedAssetDeleteFileIfPresent(CapturedPath); };

    // ---- the frame came from the ASSET editor's preview, not the level viewport ----
    FString CaptureSource;
    Capture.Result->TryGetStringField(TEXT("captureSource"), CaptureSource);
    TestEqual(TEXT("captureSource names the static mesh editor preview"),
        CaptureSource, FString(TEXT("staticMeshEditorPreviewAnnotated")));
    TestFalse(TEXT("no levelPath on an asset capture - these pixels are not of a level"),
        Capture.Result->HasField(TEXT("levelPath")));

    const TSharedPtr<FJsonObject>* SubjectObj = nullptr;
    if (TestTrue(TEXT("a resolved asset subject publishes the subject block"),
            Capture.Result->TryGetObjectField(TEXT("subject"), SubjectObj) &&
            SubjectObj && (*SubjectObj).IsValid()))
    {
        FString Kind;
        (*SubjectObj)->TryGetStringField(TEXT("kind"), Kind);
        TestEqual(TEXT("subject.kind is staticMesh"), Kind, FString(TEXT("staticMesh")));

        // The window-state pair, and the reason the verb releases its subject BEFORE building the
        // response: a subject still holding its asset editor can only ever publish
        // assetEditorClosed:false, which is a prediction wearing a measurement's name.
        bool bWasAlreadyOpen = true;
        bool bClosed = false;
        const bool bHasWasOpen =
            (*SubjectObj)->TryGetBoolField(TEXT("assetEditorWasAlreadyOpen"), bWasAlreadyOpen);
        const bool bHasClosed = (*SubjectObj)->TryGetBoolField(TEXT("assetEditorClosed"), bClosed);
        TestTrue(TEXT("subject carries assetEditorWasAlreadyOpen"), bHasWasOpen);
        TestTrue(TEXT("subject carries assetEditorClosed"), bHasClosed);
        if (bHasWasOpen && bHasClosed && !bWasAlreadyOpen)
        {
            // This call opened the window, and `closeAfterCapture` defaults to closing what the
            // call opened. The close now runs from a core-ticker pass rather than on the capture's
            // own stack - destroying a toolkit there is an access violation - so the honest outcome
            // is either measured-closed OR queued-and-gone-next-tick. Both are acceptable; what is
            // NOT is neither, which means the window leaked. Guarded on !bWasAlreadyOpen because an
            // editor the caller already had open is deliberately left alone by the same default.
            bool bCloseDeferred = false;
            (*SubjectObj)->TryGetBoolField(TEXT("assetEditorCloseDeferred"), bCloseDeferred);
            TestTrue(TEXT("an asset editor this call opened is reported CLOSED or DEFERRED - ")
                TEXT("measured after the release, never predicted before it, never leaked"),
                bClosed || bCloseDeferred);
        }
        else if (bWasAlreadyOpen)
        {
            AddInfo(TEXT("the cube's editor was already open when this test ran (a sibling test ")
                TEXT("left it), so the close verdict is deliberately not asserted."));
        }
    }
    const TSharedPtr<FJsonObject>* FramingObj = nullptr;
    if (TestTrue(TEXT("a resolved asset subject publishes the framing block"),
            Capture.Result->TryGetObjectField(TEXT("framing"), FramingObj) &&
            FramingObj && (*FramingObj).IsValid()))
    {
        bool bEvaluated = false;
        (*FramingObj)->TryGetBoolField(TEXT("evaluated"), bEvaluated);
        // The cube has extent, so the asset-bounds framing check has something to measure. A block
        // that always reported evaluated:false would satisfy a bare presence check.
        TestTrue(TEXT("framing was evaluated against the asset's own bounds"), bEvaluated);
    }

    // ---- the pose the arithmetic below was done for ----
    const TSharedPtr<FJsonObject>* ViewportObj = nullptr;
    bool bAimApplied = false;
    if (Capture.Result->TryGetObjectField(TEXT("viewport"), ViewportObj) &&
        ViewportObj && (*ViewportObj).IsValid())
    {
        const TSharedPtr<FJsonObject>* AimObj = nullptr;
        if ((*ViewportObj)->TryGetObjectField(TEXT("aim"), AimObj) && AimObj && (*AimObj).IsValid())
        {
            (*AimObj)->TryGetBoolField(TEXT("applied"), bAimApplied);
        }
    }
    // A preview viewport runs its orbit camera by default and ApplyCaptureCamera suppresses it on
    // the perspective path precisely so the requested pose reaches the pixels. If that failed, the
    // frame is of somewhere else entirely and the expected coordinates below are meaningless - so
    // it is a FAILURE, not a skip: a mis-aimed preview capture is the exact defect this wave exists
    // to close.
    TestTrue(TEXT("the requested camera aim reached the preview frame (viewport.aim.applied)"),
        bAimApplied);
    if (!bAimApplied)
    {
        FString Measured;
        const TSharedPtr<FJsonObject>* LocObj = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("cameraLocation"), LocObj) && LocObj)
        {
            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;
            (*LocObj)->TryGetNumberField(TEXT("x"), X);
            (*LocObj)->TryGetNumberField(TEXT("y"), Y);
            (*LocObj)->TryGetNumberField(TEXT("z"), Z);
            Measured = FString::Printf(TEXT("(%.1f, %.1f, %.1f)"), X, Y, Z);
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("frame-not-at-requested-location"),
            FString::Printf(
                TEXT("the overlay-coordinate assertions were NOT MEASURED: the frame was drawn from ")
                TEXT("%s, not the requested (-1000, 0, 0)."), *Measured));
        return true;
    }

    // ---- decode and look ----
    if (CapturedPath.IsEmpty() || !IFileManager::Get().FileExists(*CapturedPath))
    {
        AddError(TEXT("the verb reported success but wrote no PNG to inspect"));
        return true;
    }
    FImage Loaded;
    if (!FImageUtils::LoadImage(*CapturedPath, Loaded))
    {
        AddError(FString::Printf(TEXT("the annotated PNG at %s could not be decoded"), *CapturedPath));
        return true;
    }
    Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
    if (!TestTrue(TEXT("the annotated PNG decoded to pixels"),
            Loaded.SizeX == ImageSize && Loaded.SizeY == ImageSize &&
            Pixels.Num() >= static_cast<int64>(ImageSize) * ImageSize))
    {
        return true;
    }

    // What the frame itself looked like, reported rather than asserted (see the file header).
    const PinWrightRenderCapture::FCaptureImageStats FrameStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(
            TConstArrayView<FColor>(Pixels.GetData(), static_cast<int32>(Pixels.Num())));
    AddInfo(FString::Printf(
        TEXT("preview frame: mean %.4f, min %.4f, max %.4f, lit fraction %.4f."),
        FrameStats.MeanLuminance, FrameStats.MinLuminance, FrameStats.MaxLuminance,
        FrameStats.LitPixelFraction));

    // ---- THE DECISIVE ASSERTIONS ----
    //
    // The whole painted extent of each axis colour is measured and compared against the coordinates
    // computed at the top of this function. TOLERANCE IS 3 PX, EXPLICITLY, on a 256 px image: the
    // Bresenham pen is 3 px wide (so a correct line covers +/-1 px around its ideal path) and the
    // projected endpoint is rounded to an integer (+/-1 px), leaving 1 px of headroom and nothing
    // more. Nothing here uses a defaulted tolerance.
    //
    // WHAT 3 PX CATCHES. The endpoint sits 64 px from the centre, so the projected off-axis scale
    // is pinned to about 5 %. A view rebuilt with the viewport's own field of view instead of the
    // request's 90 deg moves that endpoint by tens of pixels (a 53.4 deg preview default puts it at
    // 127 px); a view rebuilt at the viewport's NATIVE size rather than the requested 256 scales
    // the offset with the width and pushes the endpoint clean off the frame; a mirrored or swapped
    // basis moves it 128 px; a camera that did not go to (-1000,0,0) changes the depth and with it
    // the scale. All of those produce a perfectly plausible PNG and are exactly what a wrongly
    // targeted projection looks like from the response alone.
    //
    // WHAT IT CANNOT CATCH, stated so nobody reads more into it than it proves: the projection
    // MATHS is a pure function of the request, so these coordinates would be identical if the
    // session had been built from the level viewport instead of this preview one. Which client the
    // session actually moves is pinned separately, in
    // PinWright.spatial.view_projection.ExplicitTargetBuildsTheViewFromThatClient.
    constexpr int32 PixelTolerance = 3;
    const int32 ExpectedGreenEndX = Centre + AxisEndOffset;  // 192
    const int32 ExpectedBlueEndY = Centre - AxisEndOffset;   // 64

    const FAnnotatedAssetColorExtent Green = AnnotatedAssetMeasureColorExtent(Pixels, ImageSize, ImageSize, GExpectedAxisYColor);
    const FAnnotatedAssetColorExtent Blue = AnnotatedAssetMeasureColorExtent(Pixels, ImageSize, ImageSize, GExpectedAxisZColor);
    AddInfo(FString::Printf(TEXT("+Y axis ink: %s (want x in [%d,%d], y ~ %d). "),
        *Green.Describe(), Centre, ExpectedGreenEndX, Centre));
    AddInfo(FString::Printf(TEXT("+Z axis ink: %s (want y in [%d,%d], x ~ %d)."),
        *Blue.Describe(), ExpectedBlueEndY, Centre, Centre));

    // Precondition: the axes were painted at all. Without this the extent comparisons below are
    // vacuous - MAX_int32 bounds satisfy nothing, but a reader deserves the direct statement.
    if (!TestTrue(TEXT("the +Y axis was painted (green overlay ink is present)"), Green.Count > 0) ||
        !TestTrue(TEXT("the +Z axis was painted (blue overlay ink is present)"), Blue.Count > 0))
    {
        AddError(TEXT("no axis ink at all. Either the overlay pass drew nothing - which is what a ")
            TEXT("session that could not build a view over the preview client does - or it drew ")
            TEXT("entirely off-frame, which is what a view rebuilt at the wrong size does."));
        return true;
    }

    // +Y runs from the image centre RIGHTWARDS to the computed endpoint, along the centre row.
    TestTrue(FString::Printf(
        TEXT("the +Y axis starts at the image centre column (min x %d, want %d +/- %d)"),
        Green.MinX, Centre, PixelTolerance),
        FMath::Abs(Green.MinX - Centre) <= PixelTolerance);
    TestTrue(FString::Printf(
        TEXT("the +Y axis ends where 500 cm off-axis at 1000 cm depth and FOV 90 puts it ")
        TEXT("(max x %d, want %d +/- %d)"),
        Green.MaxX, ExpectedGreenEndX, PixelTolerance),
        FMath::Abs(Green.MaxX - ExpectedGreenEndX) <= PixelTolerance);
    TestTrue(FString::Printf(
        TEXT("the +Y axis stays on the image centre row (y in [%d,%d], want %d +/- %d)"),
        Green.MinY, Green.MaxY, Centre, PixelTolerance),
        FMath::Abs(Green.MinY - Centre) <= PixelTolerance &&
        FMath::Abs(Green.MaxY - Centre) <= PixelTolerance);

    // +Z runs from the image centre UPWARDS, along the centre column.
    TestTrue(FString::Printf(
        TEXT("the +Z axis reaches up to the computed endpoint (min y %d, want %d +/- %d)"),
        Blue.MinY, ExpectedBlueEndY, PixelTolerance),
        FMath::Abs(Blue.MinY - ExpectedBlueEndY) <= PixelTolerance);
    TestTrue(FString::Printf(
        TEXT("the +Z axis starts at the image centre row (max y %d, want %d +/- %d)"),
        Blue.MaxY, Centre, PixelTolerance),
        FMath::Abs(Blue.MaxY - Centre) <= PixelTolerance);
    TestTrue(FString::Printf(
        TEXT("the +Z axis stays on the image centre column (x in [%d,%d], want %d +/- %d)"),
        Blue.MinX, Blue.MaxX, Centre, PixelTolerance),
        FMath::Abs(Blue.MinX - Centre) <= PixelTolerance &&
        FMath::Abs(Blue.MaxX - Centre) <= PixelTolerance);

    // Diagnostics, not extra assertions: name the failure mode rather than leaving the next reader
    // to work out which of several it was.
    if (Green.MaxX < Centre - PixelTolerance)
    {
        AddError(TEXT("the +Y axis was painted entirely LEFT of the image centre. That is a ")
            TEXT("mirrored screen basis, not a missing overlay: UE 5.8 puts world +Y to view right ")
            TEXT("(x = ndc.x/2 + 0.5 in FViewProjectionSession::Project). Re-check the basis before ")
            TEXT("changing this test."));
    }
    if (Blue.MinY > Centre + PixelTolerance)
    {
        AddError(TEXT("the +Z axis was painted entirely BELOW the image centre. UE 5.8 puts world ")
            TEXT("+Z to view up (y = 1 - ndc.y/2 - 0.5). Re-check the basis before changing this ")
            TEXT("test."));
    }
    return true;
}

// ============================================================================
// 2. The two LEVEL-ONLY overlays are a typed refusal against an asset subject, and the refusal
//    writes no PNG.
//
// `bounds` resolves names through McpActorUtils::FindActorByName(EditorWorld, ...) and
// `actorLabels` walks TActorIterator(EditorWorld, ...). An asset-editor preview is an FPreviewScene
// of registered components, so both would return the level's answer for a frame of something else -
// or, if pointed at the preview world, nothing at all. Either way the caller reads "the level is
// empty" instead of "you asked the wrong world", which is why this is answered rather than drawn.
//
// UNABLE TO FAIL IF it asserted only that the call failed: a bad asset path fails too. It asserts
// the code, that the message names BOTH the offending argument and the refused kind, and that no
// path came back.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnnotatedAssetLevelOverlaysRefusedTest,
    "PinWright.render.capture_annotated.LevelOverlaysAreRefusedOnAnAssetSubject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnnotatedAssetLevelOverlaysRefusedTest::RunTest(const FString& Parameters)
{
    const auto MakeAssetSubject = []()
    {
        TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
        Subject->SetStringField(TEXT("kind"), TEXT("staticMesh"));
        Subject->SetStringField(TEXT("path"), GAnnotatedAssetPath);
        return Subject;
    };

    // ---- bounds ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("width"), 256.0);
        Payload->SetNumberField(TEXT("height"), 256.0);
        Payload->SetBoolField(TEXT("axes"), false);
        Payload->SetObjectField(TEXT("subject"), MakeAssetSubject());
        TArray<TSharedPtr<FJsonValue>> Bounds;
        Bounds.Add(MakeShared<FJsonValueString>(TEXT("PW_NoSuchActor_ZZZ")));
        Payload->SetArrayField(TEXT("bounds"), Bounds);

        FTestResponseCapture Capture;
        TestTrue(TEXT("render.capture_annotated handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
        TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

        TestFalse(TEXT("bounds + an asset subject is refused"), Capture.bSuccess);
        TestEqual(TEXT("the refusal is INVALID_ARGUMENT, not a new error code"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(FString::Printf(TEXT("the message names the offending argument (got: %s)"),
            *Capture.Message), Capture.Message.Contains(TEXT("bounds")));
        TestTrue(FString::Printf(TEXT("the message names the refused kind (got: %s)"),
            *Capture.Message), Capture.Message.Contains(TEXT("staticMesh")));
        // Raised in the argument-validation pass, so nothing is opened and nothing is rendered.
        TestFalse(TEXT("a refused call reports no path"),
            Capture.Result.IsValid() && Capture.Result->HasField(TEXT("path")));
    }

    // ---- actorLabels ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("width"), 256.0);
        Payload->SetNumberField(TEXT("height"), 256.0);
        Payload->SetBoolField(TEXT("axes"), false);
        Payload->SetObjectField(TEXT("subject"), MakeAssetSubject());
        Payload->SetBoolField(TEXT("actorLabels"), true);

        FTestResponseCapture Capture;
        TestTrue(TEXT("render.capture_annotated handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
        TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

        TestFalse(TEXT("actorLabels + an asset subject is refused"), Capture.bSuccess);
        TestEqual(TEXT("the refusal is INVALID_ARGUMENT, not a new error code"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(FString::Printf(TEXT("the message names the offending argument (got: %s)"),
            *Capture.Message), Capture.Message.Contains(TEXT("actorLabels")));
        TestTrue(FString::Printf(TEXT("the message names the refused kind (got: %s)"),
            *Capture.Message), Capture.Message.Contains(TEXT("staticMesh")));
        TestFalse(TEXT("a refused call reports no path"),
            Capture.Result.IsValid() && Capture.Result->HasField(TEXT("path")));
    }

    // ---- the same two overlays are UNCHANGED without an asset subject ----
    // The control that keeps the refusal from being a blanket ban: the level path must still accept
    // both. Asserted on the error code alone, because whether a capture succeeds depends on the
    // host having a viewport and this half is about the ARGUMENT gate.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("width"), 256.0);
        Payload->SetNumberField(TEXT("height"), 256.0);
        Payload->SetBoolField(TEXT("axes"), false);
        Payload->SetBoolField(TEXT("actorLabels"), true);
        TArray<TSharedPtr<FJsonValue>> Bounds;
        Bounds.Add(MakeShared<FJsonValueString>(TEXT("PW_NoSuchActor_ZZZ")));
        Payload->SetArrayField(TEXT("bounds"), Bounds);

        FTestResponseCapture Capture;
        TestTrue(TEXT("render.capture_annotated handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
        FString CapturedPath;
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("path"), CapturedPath);
            // Both overlays still ran: `bounds` echoes one entry per requested name and
            // `actorLabels` returns its map. Neither block exists unless the argument was honoured.
            TestTrue(TEXT("the actorLabels map is still returned on the level path"),
                Capture.Result->HasField(TEXT("actorLabels")));
            const TSharedPtr<FJsonObject>* OverlaysObj = nullptr;
            if (Capture.Result->TryGetObjectField(TEXT("overlays"), OverlaysObj) &&
                OverlaysObj && (*OverlaysObj).IsValid())
            {
                TestTrue(TEXT("the bounds overlay echo is still returned on the level path"),
                    (*OverlaysObj)->HasField(TEXT("bounds")));
            }
        }
        else
        {
            TestNotEqual(TEXT("bounds + actorLabels are not refused without an asset subject"),
                Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
            TestTrue(FString::Printf(TEXT("capture failure is typed (got '%s')"), *Capture.ErrorCode),
                AnnotatedAssetIsTypedCaptureFailure(Capture.ErrorCode));
        }
        AnnotatedAssetDeleteFileIfPresent(CapturedPath);
    }
    return true;
}
