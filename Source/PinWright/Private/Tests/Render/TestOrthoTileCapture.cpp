// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for render.capture_ortho_tiles.
//
// TWO CLASSES OF TEST, kept apart on purpose.
//
// The deterministic ones never render: argument refusals, the burst ceilings, the extent
// expansion, the seam rule and the manifest round trip are all decided before any camera is
// placed, so they run identically on a headless commandlet and on a live editor. They are the
// ones that carry the contract.
//
// The live one drives a real orthographic scene capture and therefore follows the RHI-guard
// pattern of TestAnnotatedCaptureHandlers.cpp: a failure must be TYPED, and the pixel-level
// assertions are skipped. A skipped assertion is the failure mode this project pays for most - a
// test that passes by declining to measure - so the guarded test ASSERTS ON BOTH BRANCHES (the
// level's dirty state is checked whether the capture succeeded or was refused) and logs the
// branch it took with AddInfo so a green run still says which half ran.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Image/ImageOps.h"
#include "Handlers/Render/OrthoTileCaptureUtils.h"
#include "Handlers/Render/TileGridUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
    // The typed, non-crashing exits render.capture_ortho_tiles is allowed to return when no RHI
    // surface or no editor world is available. Anything outside this list is a real defect, so the
    // guard cannot swallow one.
    bool OrthoIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("SCENE_BOUNDS_NOT_MEASURED") ||
            ErrorCode == TEXT("SCENE_CAPTURE_FAILED") ||
            ErrorCode == TEXT("CAPTURE_CAMERA_NOT_APPLIED") ||
            ErrorCode == TEXT("RENDER_TARGET_CREATE_FAILED") ||
            ErrorCode == TEXT("READ_PIXELS_FAILED") ||
            ErrorCode == TEXT("BLANK_CAPTURE") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("WRITE_FAILED");
    }

    TSharedPtr<FJsonObject> OrthoVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // A payload that is valid except for whatever the caller overrides afterwards: a 4000 x 4000
    // cm top-down box at 8 cm/px on 500 px tiles, i.e. exactly 1 x 1 tiles with no expansion.
    TSharedPtr<FJsonObject> OrthoBasePayload()
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetObjectField(TEXT("worldMin"), OrthoVec(-2000.0, -2000.0, 0.0));
        P->SetObjectField(TEXT("worldMax"), OrthoVec(2000.0, 2000.0, 0.0));
        P->SetStringField(TEXT("axes"), TEXT("top_down_x_right_y_down"));
        P->SetNumberField(TEXT("exposure"), 11.0);
        P->SetNumberField(TEXT("cmPerPixel"), 8.0);
        P->SetNumberField(TEXT("tilePixels"), 500.0);
        return P;
    }

    // Dirty-package count over the editor world's levels, mirroring what the handler measures so
    // the test and the verb cannot disagree about what "the level was dirtied" means.
    int32 OrthoCountDirtyLevelPackages()
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return -1;
        }
        TSet<const UPackage*> Seen;
        int32 Dirty = 0;
        if (const UPackage* Outer = World->GetOutermost())
        {
            Seen.Add(Outer);
            Dirty += Outer->IsDirty() ? 1 : 0;
        }
        for (const ULevel* Level : World->GetLevels())
        {
            if (!Level)
            {
                continue;
            }
            const UPackage* Package = Level->GetOutermost();
            if (!Package || Seen.Contains(Package))
            {
                continue;
            }
            Seen.Add(Package);
            Dirty += Package->IsDirty() ? 1 : 0;
        }
        return Dirty;
    }

    void OrthoDeleteDirectory(const FString& Dir)
    {
        if (!Dir.IsEmpty())
        {
            IFileManager::Get().DeleteDirectory(*Dir, false, true);
        }
    }
}

// ============================================================================
// 1. Required arguments - no silent defaults
// ============================================================================

// `axes` has no default that is right for both documented top-down poses, so an omitted value must
// be a refusal rather than a pick. Counterfactual: default `axes` to either preset and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesAxesRequiredTest,
    "PinWright.render.capture_ortho_tiles.AxesRequired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesAxesRequiredTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = OrthoBasePayload();
    Payload->RemoveField(TEXT("axes"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_ortho_tiles handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("a missing axes mapping is refused"), Capture.bSuccess);
    TestEqual(TEXT("missing axes error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

// `exposure` is required on THIS verb although it is optional on every other capture verb: a scene
// capture keeps no persistent view state, so an unpinned burst exposes each tile to its own
// content and the mosaic steps at every seam. Counterfactual: make it optional and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesExposureRequiredTest,
    "PinWright.render.capture_ortho_tiles.ExposureRequired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesExposureRequiredTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = OrthoBasePayload();
    Payload->RemoveField(TEXT("exposure"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_ortho_tiles handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("an omitted exposure is refused"), Capture.bSuccess);
    TestEqual(TEXT("missing exposure error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

// Exactly one of cmPerPixel / (cols + rows). Neither leaves the subdivision undefined; both would
// let a stale cmPerPixel silently win over a corrected cols/rows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesGridSelectorIsExclusiveTest,
    "PinWright.render.capture_ortho_tiles.GridSelectorIsExclusive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesGridSelectorIsExclusiveTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> Payload = OrthoBasePayload();
        Payload->RemoveField(TEXT("cmPerPixel"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found (neither)"),
            InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
        TestFalse(TEXT("neither cmPerPixel nor cols/rows is refused"), Capture.bSuccess);
        TestEqual(TEXT("neither error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }
    {
        TSharedPtr<FJsonObject> Payload = OrthoBasePayload();
        Payload->SetNumberField(TEXT("cols"), 2.0);
        Payload->SetNumberField(TEXT("rows"), 2.0);
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found (both)"),
            InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
        TestFalse(TEXT("cmPerPixel together with cols/rows is refused"), Capture.bSuccess);
        TestEqual(TEXT("both error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }
    return true;
}

// Every wire parameter must be declared in RPC_PARAMS or the dispatcher rejects the call before
// the handler body ever runs. Routed through the real dispatcher because that gate is upstream of
// the handler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesUnknownParamRejectedTest,
    "PinWright.render.capture_ortho_tiles.UnknownParamRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesUnknownParamRejectedTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = OrthoBasePayload();
    Params->SetStringField(TEXT("bogusUnknownArg"), TEXT("x"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.capture_ortho_tiles"),
        TEXT("req-ortho-unknown"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown param rejected"), bSuccess);
    TestEqual(TEXT("unknown param error is UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

// The mirror of the above: a payload naming every declared parameter must NOT be rejected as
// UNKNOWN_PARAMS. It may still fail with a typed capture failure when there is no RHI surface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesKnownParamsAcceptedTest,
    "PinWright.render.capture_ortho_tiles.KnownParamsAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesKnownParamsAcceptedTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FString Prefix = FString::Printf(TEXT("pw_ortho_known_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
    TSharedPtr<FJsonObject> Params = OrthoBasePayload();
    Params->RemoveField(TEXT("cmPerPixel"));
    Params->SetNumberField(TEXT("cols"), 1.0);
    Params->SetNumberField(TEXT("rows"), 1.0);
    Params->SetNumberField(TEXT("tilePixels"), 64.0);
    Params->SetNumberField(TEXT("depthCm"), 0.0);
    Params->SetNumberField(TEXT("cameraDepthCm"), 500000.0);
    Params->SetNumberField(TEXT("cameraClearanceCm"), 12345.0);
    Params->SetStringField(TEXT("namePrefix"), Prefix);
    Params->SetBoolField(TEXT("overwrite"), true);
    Params->SetBoolField(TEXT("allowBlank"), true);

    bool bSuccess = true;
    TSharedPtr<FJsonObject> ResultObj;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.capture_ortho_tiles"),
        TEXT("req-ortho-known"), Params, bSuccess, ResultObj, ErrorCode);

    // The decisive assertion: a fully-declared payload is never rejected as UNKNOWN_PARAMS.
    TestNotEqual(TEXT("declared params are not rejected as UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    if (!bSuccess)
    {
        TestTrue(FString::Printf(TEXT("any failure is a typed capture failure, not a param error (got %s)"), *ErrorCode),
            OrthoIsTypedCaptureFailure(ErrorCode));
    }
    else if (ResultObj.IsValid())
    {
        FString OutputDir;
        if (ResultObj->TryGetStringField(TEXT("outputDir"), OutputDir))
        {
            OrthoDeleteDirectory(OutputDir);
        }
    }
    return true;
}

// ============================================================================
// 2. The burst ceilings - refused BEFORE any render
// ============================================================================

// A tile count past PinWrightOrthoTiles::MaxTilesPerCall is refused with a typed error and no
// files. The alternative is a synchronous burst that outlives the transport's response timeout
// with nobody able to observe or cancel it.
//
// Counterfactual: remove the ceiling check and this fails (the call either succeeds or fails with
// a capture error instead of TILE_BUDGET_EXCEEDED).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesTileCountCeilingRefusedTest,
    "PinWright.render.capture_ortho_tiles.TileCountCeilingRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesTileCountCeilingRefusedTest::RunTest(const FString& Parameters)
{
    // 100 x 100 tiles of 64 px at 1 cm/px = 10000 tiles, well past the 64-tile ceiling but only
    // 41 MP, so the TILE-COUNT ceiling is the one that has to bind.
    TSharedPtr<FJsonObject> Payload = OrthoBasePayload();
    Payload->SetObjectField(TEXT("worldMin"), OrthoVec(0.0, 0.0, 0.0));
    Payload->SetObjectField(TEXT("worldMax"), OrthoVec(6400.0, 6400.0, 0.0));
    Payload->SetNumberField(TEXT("cmPerPixel"), 1.0);
    Payload->SetNumberField(TEXT("tilePixels"), 64.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_ortho_tiles handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("a 10000-tile burst is refused"), Capture.bSuccess);
    TestEqual(TEXT("tile-count ceiling error"), Capture.ErrorCode, FString(TEXT("TILE_BUDGET_EXCEEDED")));

    if (Capture.Result.IsValid())
    {
        double Tiles = 0.0;
        TestTrue(TEXT("the refusal reports the tile count it refused"),
            Capture.Result->TryGetNumberField(TEXT("tilesRequested"), Tiles));
        TestTrue(TEXT("the refused tile count is past the ceiling"),
            Tiles > static_cast<double>(PinWrightOrthoTiles::MaxTilesPerCall));
        double MaxTiles = 0.0;
        TestTrue(TEXT("the refusal names the ceiling"),
            Capture.Result->TryGetNumberField(TEXT("maxTilesPerCall"), MaxTiles));
        TestEqual(TEXT("the reported ceiling is the code's own"),
            static_cast<int32>(MaxTiles), PinWrightOrthoTiles::MaxTilesPerCall);
    }
    else
    {
        AddError(TEXT("the tile-budget refusal carried no structured payload, so a caller cannot see what bound"));
    }
    return true;
}

// The other half of the ceiling: a grid INSIDE the tile count but past the total-pixel budget.
// Cost tracks pixels, not tiles - 91-96% of per-tile time is encode plus readback - so a
// tile-count-only ceiling would let 64 tiles at 8192 px through as a two-minute-plus burst.
//
// Counterfactual: drop MaxTotalPixelsPerCall and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesPixelCeilingRefusedTest,
    "PinWright.render.capture_ortho_tiles.PixelCeilingRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesPixelCeilingRefusedTest::RunTest(const FString& Parameters)
{
    // 8 x 8 = 64 tiles (exactly at the tile ceiling, so it cannot be what binds) of 8192 px =
    // 4295 MP, sixteen times the pixel budget.
    TSharedPtr<FJsonObject> Payload = OrthoBasePayload();
    Payload->SetObjectField(TEXT("worldMin"), OrthoVec(0.0, 0.0, 0.0));
    Payload->SetObjectField(TEXT("worldMax"), OrthoVec(65536.0, 65536.0, 0.0));
    Payload->SetNumberField(TEXT("cmPerPixel"), 1.0);
    Payload->SetNumberField(TEXT("tilePixels"), 8192.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_ortho_tiles handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("a 4295-megapixel burst is refused"), Capture.bSuccess);
    TestEqual(TEXT("pixel ceiling error"), Capture.ErrorCode, FString(TEXT("TILE_BUDGET_EXCEEDED")));

    if (Capture.Result.IsValid())
    {
        double Tiles = 0.0;
        Capture.Result->TryGetNumberField(TEXT("tilesRequested"), Tiles);
        // The decisive part: the TILE count is within its ceiling, so only the pixel budget can
        // have refused this. Without that, the test would pass on the wrong mechanism.
        TestTrue(TEXT("the tile count alone is within the tile ceiling"),
            Tiles <= static_cast<double>(PinWrightOrthoTiles::MaxTilesPerCall));
        double TotalPixels = 0.0;
        TestTrue(TEXT("the refusal reports the pixel total"),
            Capture.Result->TryGetNumberField(TEXT("totalPixels"), TotalPixels));
        TestTrue(TEXT("the refused pixel total is past the budget"),
            TotalPixels > static_cast<double>(PinWrightOrthoTiles::MaxTotalPixelsPerCall));
    }
    else
    {
        AddError(TEXT("the pixel-budget refusal carried no structured payload"));
    }
    return true;
}

// ============================================================================
// 3. The georeference contract
// ============================================================================

// The camera pose is DERIVED from the requested axis mapping and must classify back to it. This is
// the one place the whole feature can silently mirror or transpose itself, so both directions are
// pinned for every preset.
//
// Counterfactual: flip the screen-up negation in CameraForwardForAxisMapping and this fails on
// every preset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesCameraRotationRoundTripsTest,
    "PinWright.render.capture_ortho_tiles.CameraRotationRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesCameraRotationRoundTripsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightTileGrid;

    TArray<FString> PresetNames;
    TArray<FScreenAxisMapping> PresetMappings;
    PresetNames.Add(TEXT("top_down_x_up_y_right"));   PresetMappings.Add(TopDownXUpYRight());
    PresetNames.Add(TEXT("top_down_x_right_y_down")); PresetMappings.Add(TopDownXRightYDown());
    PresetNames.Add(TEXT("front_y_right_z_up"));      PresetMappings.Add(FrontYRightZUp());
    PresetNames.Add(TEXT("side_x_right_z_up"));       PresetMappings.Add(SideXRightZUp());

    for (int32 Index = 0; Index < PresetNames.Num(); ++Index)
    {
        const FString& Name = PresetNames[Index];
        const FScreenAxisMapping& Expected = PresetMappings[Index];

        FRotator Rotation = FRotator::ZeroRotator;
        FString Err;
        if (!TryMakeCameraRotationForAxisMapping(Expected, Rotation, Err))
        {
            AddError(FString::Printf(TEXT("no camera rotation for preset %s: %s"), *Name, *Err));
            continue;
        }

        FScreenAxisMapping Classified;
        FString ClassifyErr;
        if (!TryMakeAxisMappingFromEffectiveRotation(Rotation, Classified, ClassifyErr))
        {
            AddError(FString::Printf(TEXT("preset %s produced rotation (P=%.3f Y=%.3f R=%.3f) that classifies to nothing: %s"),
                *Name, Rotation.Pitch, Rotation.Yaw, Rotation.Roll, *ClassifyErr));
            continue;
        }
        TestTrue(FString::Printf(TEXT("preset %s round-trips through the rotation"), *Name),
            Classified == Expected);

        // The forward direction must run along the depth axis, because the camera-depth placement
        // divides the world into "in front" and "clipped by the near-plane-0 projection" on its
        // sign. A forward that is not on the depth axis makes that placement meaningless.
        const FVector Forward = CameraForwardForAxisMapping(Expected);
        TestTrue(FString::Printf(TEXT("preset %s forward is a unit vector"), *Name),
            FMath::IsNearlyEqual(Forward.Size(), 1.0, 1.0e-6));
        TestTrue(FString::Printf(TEXT("preset %s forward runs along the depth axis %s"),
                *Name, AxisName(Expected.DepthAxis())),
            FMath::IsNearlyEqual(FMath::Abs(GetAxisValue(Forward, Expected.DepthAxis())), 1.0, 1.0e-6));
    }

    // The pinned empirical case, from docs/wiki-src/level-review.framing-math.md: pitch -90 /
    // yaw 0 renders +X UP and +Y RIGHT. A table that drifted off the measurement would still
    // round-trip against itself, so the measurement is asserted separately.
    FRotator TopDown = FRotator::ZeroRotator;
    FString TopDownErr;
    if (TryMakeCameraRotationForAxisMapping(TopDownXUpYRight(), TopDown, TopDownErr))
    {
        TestTrue(TEXT("+X up / +Y right is the measured pitch -90, yaw 0 pose"),
            FMath::IsNearlyEqual(TopDown.Pitch, -90.0, 1.0e-3) &&
            FMath::IsNearlyEqual(FRotator::NormalizeAxis(TopDown.Yaw), 0.0, 1.0e-3));
    }
    else
    {
        AddError(FString::Printf(TEXT("top_down_x_up_y_right has no camera rotation: %s"), *TopDownErr));
    }
    return true;
}

// The manifest's georeference must parse back through PinWrightImage::ParseGeoreference - the
// exact function image.tile / image.annotate / image.compare use - and reproduce the same grid,
// both for the whole mosaic and for a single tile at that tile's own resolution.
//
// This is the round trip the whole feature exists for, so it is asserted against the READER's
// parser rather than against a restatement of the writer's own maths.
//
// Counterfactual: change the wire spelling on either side (a key rename, a swapped worldMin/Max,
// a cols/rows transposition) and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesManifestGeoreferenceRoundTripsTest,
    "PinWright.render.capture_ortho_tiles.ManifestGeoreferenceRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesManifestGeoreferenceRoundTripsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;
    using namespace PinWrightTileGrid;

    const FScreenAxisMapping Mapping = TopDownXRightYDown();
    const FWorldExtent2D Requested = MakeWorldExtent(-3000.0, -2000.0, 3000.0, 2000.0);
    constexpr int32 TilePixels = 256;

    FTileGridPlan Plan;
    FString Err;
    if (!PlanTileGrid(Requested, Mapping, TilePixels, TilePixels, /*cm/px*/ 4.0, /*MaxTiles*/ 0, Plan, Err))
    {
        AddError(FString::Printf(TEXT("PlanTileGrid refused the fixture: %s"), *Err));
        return false;
    }

    // Build the georeference exactly as the handler does: the COVERED extent's corners, with the
    // depth axis at the measurement plane.
    FGeoreference Written;
    Written.Grid = Plan.Grid;
    Written.DepthCm = 0.0;
    SetAxisValue(Written.WorldMin, Mapping.ScreenXAxis, Plan.Grid.World.Min.X);
    SetAxisValue(Written.WorldMin, Mapping.ScreenYAxis, Plan.Grid.World.Min.Y);
    SetAxisValue(Written.WorldMin, Mapping.DepthAxis(), 0.0);
    SetAxisValue(Written.WorldMax, Mapping.ScreenXAxis, Plan.Grid.World.Max.X);
    SetAxisValue(Written.WorldMax, Mapping.ScreenYAxis, Plan.Grid.World.Max.Y);
    SetAxisValue(Written.WorldMax, Mapping.DepthAxis(), 0.0);

    const TSharedPtr<FJsonObject> Wire = SerializeGeoreference(Written);
    TestTrue(TEXT("the serialized georeference is an object"), Wire.IsValid());

    // (a) the whole-mosaic read, at the resolution a mosaic of these tiles would have.
    FGeoreference ReadBack;
    FString ErrCode;
    FString ErrMsg;
    const bool bMosaicParsed = ParseGeoreference(Wire, Plan.Grid.ImageWidth(), Plan.Grid.ImageHeight(),
        /*bImageIsSingleTile=*/false, ReadBack, ErrCode, ErrMsg);
    TestTrue(FString::Printf(TEXT("the reader parses the manifest georeference (%s: %s)"), *ErrCode, *ErrMsg),
        bMosaicParsed);
    if (bMosaicParsed)
    {
        FString Why;
        const bool bAgree = GeoreferencesAgree(Written, ReadBack, Why);
        TestTrue(FString::Printf(TEXT("the parsed georeference describes the same ground (%s)"), *Why), bAgree);
        TestEqual(TEXT("cols survive the round trip"), ReadBack.Grid.Cols, Plan.Grid.Cols);
        TestEqual(TEXT("rows survive the round trip"), ReadBack.Grid.Rows, Plan.Grid.Rows);
        TestEqual(TEXT("the reader derives the same tile pixel width"),
            ReadBack.Grid.TilePixelWidth, TilePixels);
        TestEqual(TEXT("the reader derives the same tile pixel height"),
            ReadBack.Grid.TilePixelHeight, TilePixels);
    }

    // (b) the per-tile read: one tile's standalone georeference, parsed against one tile image.
    // This is the shape image.annotate consumes, and it must land on the same ground the parent
    // grid assigns that tile.
    const FTileIndex Tile(Plan.Grid.Cols - 1, Plan.Grid.Rows - 1);
    FWorldExtent2D TileExtent;
    if (TileWorldExtent(Plan.Grid, Tile, TileExtent))
    {
        FGeoreference TileGeo;
        TileGeo.DepthCm = 0.0;
        SetAxisValue(TileGeo.WorldMin, Mapping.ScreenXAxis, TileExtent.Min.X);
        SetAxisValue(TileGeo.WorldMin, Mapping.ScreenYAxis, TileExtent.Min.Y);
        SetAxisValue(TileGeo.WorldMax, Mapping.ScreenXAxis, TileExtent.Max.X);
        SetAxisValue(TileGeo.WorldMax, Mapping.ScreenYAxis, TileExtent.Max.Y);
        FString TileGridErr;
        if (MakeTileGrid(TileExtent, Mapping, 1, 1, TilePixels, TilePixels, TileGeo.Grid, TileGridErr))
        {
            FGeoreference TileReadBack;
            const bool bTileParsed = ParseGeoreference(SerializeGeoreference(TileGeo),
                TilePixels, TilePixels, /*bImageIsSingleTile=*/true, TileReadBack, ErrCode, ErrMsg);
            TestTrue(FString::Printf(TEXT("the reader parses a per-tile georeference (%s: %s)"), *ErrCode, *ErrMsg),
                bTileParsed);
            if (bTileParsed)
            {
                // The decisive cross-check: the tile's own georeference maps its own centre pixel
                // to the same world point the PARENT grid maps that mosaic pixel to. Two different
                // grids, one answer.
                const FVector2D MosaicPixel = TilePixelToPixel(Plan.Grid, Tile,
                    FVector2D(TilePixels * 0.5, TilePixels * 0.5));
                const FVector FromParent = PixelToWorld(Plan.Grid, MosaicPixel, 0.0);
                const FVector FromTile = PixelToWorld(TileReadBack.Grid,
                    FVector2D(TilePixels * 0.5, TilePixels * 0.5), 0.0);
                TestTrue(FString::Printf(TEXT("tile and parent agree on the same point (%s vs %s)"),
                        *FromParent.ToString(), *FromTile.ToString()),
                    FromParent.Equals(FromTile, 1.0e-4));
            }
        }
        else
        {
            AddError(FString::Printf(TEXT("could not build a 1x1 grid for the tile: %s"), *TileGridErr));
        }
    }
    else
    {
        AddError(TEXT("TileWorldExtent refused the last tile of its own grid"));
    }
    return true;
}

// The seam rule is TileGridUtils' to define, and this verb's tiling must be the same one. Asserted
// AGAINST the util rather than restated: for every interior seam, the mosaic pixel exactly on the
// seam belongs to the tile on its right / below at PixelInTile 0, and the tile world extents the
// manifest publishes are TileWorldExtent's own answers, abutting with no gap and no overlap.
//
// Counterfactual: change the seam convention in TileGridUtils (or hand-roll a second one here) and
// this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesSeamRuleMatchesTileGridTest,
    "PinWright.render.capture_ortho_tiles.SeamRuleMatchesTileGrid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesSeamRuleMatchesTileGridTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightTileGrid;

    const FScreenAxisMapping Mapping = TopDownXRightYDown();
    constexpr int32 TilePixels = 128;
    FTileGrid Grid;
    FString Err;
    if (!MakeTileGrid(MakeWorldExtent(0.0, 0.0, 3000.0, 2000.0), Mapping, 3, 2,
        TilePixels, TilePixels, Grid, Err))
    {
        AddError(FString::Printf(TEXT("MakeTileGrid refused the fixture: %s"), *Err));
        return false;
    }

    for (int32 Col = 1; Col < Grid.Cols; ++Col)
    {
        const FVector2D Seam(static_cast<double>(Col * TilePixels), 0.5);
        FTilePixel Split;
        if (!PixelToTilePixel(Grid, Seam, Split))
        {
            AddError(FString::Printf(TEXT("column seam at x=%.1f falls outside the image"), Seam.X));
            continue;
        }
        TestEqual(FString::Printf(TEXT("column seam %d belongs to the tile on its RIGHT"), Col),
            Split.Tile.Col, Col);
        TestEqual(FString::Printf(TEXT("column seam %d sits at PixelInTile 0"), Col),
            Split.PixelInTile.X, 0.0);
    }
    for (int32 Row = 1; Row < Grid.Rows; ++Row)
    {
        const FVector2D Seam(0.5, static_cast<double>(Row * TilePixels));
        FTilePixel Split;
        if (!PixelToTilePixel(Grid, Seam, Split))
        {
            AddError(FString::Printf(TEXT("row seam at y=%.1f falls outside the image"), Seam.Y));
            continue;
        }
        TestEqual(FString::Printf(TEXT("row seam %d belongs to the tile BELOW"), Row),
            Split.Tile.Row, Row);
        TestEqual(FString::Printf(TEXT("row seam %d sits at PixelInTile 0"), Row),
            Split.PixelInTile.Y, 0.0);
    }

    // The world side of the same rule: neighbouring tile extents abut exactly. A gap or an overlap
    // here is a georeference that disagrees with its own mosaic by a tile-fraction.
    for (int32 Row = 0; Row < Grid.Rows; ++Row)
    {
        for (int32 Col = 0; Col + 1 < Grid.Cols; ++Col)
        {
            FWorldExtent2D Left;
            FWorldExtent2D Right;
            if (TileWorldExtent(Grid, FTileIndex(Col, Row), Left) &&
                TileWorldExtent(Grid, FTileIndex(Col + 1, Row), Right))
            {
                TestTrue(FString::Printf(TEXT("tiles (%d,%d) and (%d,%d) abut across the seam"),
                        Col, Row, Col + 1, Row),
                    FMath::IsNearlyEqual(Left.Max.X, Right.Min.X, 1.0e-6));
            }
        }
    }

    // And the capture plan the verb renders from is centred on that same extent, so the pixels and
    // the georeference cannot disagree about where the tile is.
    for (int32 Row = 0; Row < Grid.Rows; ++Row)
    {
        for (int32 Col = 0; Col < Grid.Cols; ++Col)
        {
            FTileCapturePlan Plan;
            FString PlanErr;
            if (!ComputeTileCapturePlan(Grid, FTileIndex(Col, Row), /*Depth*/ 12345.0, Plan, PlanErr))
            {
                AddError(FString::Printf(TEXT("ComputeTileCapturePlan refused tile (%d,%d): %s"),
                    Col, Row, *PlanErr));
                continue;
            }
            FWorldExtent2D Extent;
            TileWorldExtent(Grid, FTileIndex(Col, Row), Extent);
            TestTrue(FString::Printf(TEXT("tile (%d,%d) camera sits at its extent centre on screen X"), Col, Row),
                FMath::IsNearlyEqual(GetAxisValue(Plan.CameraLocation, Mapping.ScreenXAxis),
                    Extent.Centre().X, 1.0e-6));
            TestTrue(FString::Printf(TEXT("tile (%d,%d) camera sits at its extent centre on screen Y"), Col, Row),
                FMath::IsNearlyEqual(GetAxisValue(Plan.CameraLocation, Mapping.ScreenYAxis),
                    Extent.Centre().Y, 1.0e-6));
            TestTrue(FString::Printf(TEXT("tile (%d,%d) camera sits at the requested depth"), Col, Row),
                FMath::IsNearlyEqual(GetAxisValue(Plan.CameraLocation, Mapping.DepthAxis()), 12345.0, 1.0e-6));
            TestTrue(FString::Printf(TEXT("tile (%d,%d) orthoWidth is its own world span"), Col, Row),
                FMath::IsNearlyEqual(static_cast<double>(Plan.OrthoWidth), Extent.SpanAcross(), 1.0e-3));
        }
    }
    return true;
}

// PlanTileGrid EXPANDS, never crops - so a caller whose extent does not divide into whole tiles
// gets a larger area than they asked for, and must be told. The verb reports `extentExpanded`, and
// this pins both directions: expanded when it does not divide, NOT expanded when it does.
//
// Counterfactual: hardcode extentExpanded either way and one half of this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesExtentExpansionReportedTest,
    "PinWright.render.capture_ortho_tiles.ExtentExpansionReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesExtentExpansionReportedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightTileGrid;

    const FScreenAxisMapping Mapping = TopDownXRightYDown();
    constexpr int32 TilePixels = 100;
    constexpr double CmPerPixel = 10.0; // a whole tile covers 1000 cm

    // (a) 2500 x 1000 cm does NOT divide by 1000 across: 3 columns are needed and the extent grows.
    {
        const FWorldExtent2D Requested = MakeWorldExtent(0.0, 0.0, 2500.0, 1000.0);
        FTileGridPlan Plan;
        FString Err;
        if (!PlanTileGrid(Requested, Mapping, TilePixels, TilePixels, CmPerPixel, 0, Plan, Err))
        {
            AddError(FString::Printf(TEXT("PlanTileGrid refused the non-dividing fixture: %s"), *Err));
            return false;
        }
        TestTrue(TEXT("a non-dividing extent is reported as expanded"), Plan.bExtentExpanded);
        TestEqual(TEXT("three columns cover 2500 cm at 1000 cm per tile"), Plan.Grid.Cols, 3);
        TestEqual(TEXT("one row covers 1000 cm"), Plan.Grid.Rows, 1);
        TestTrue(TEXT("the covered span is strictly larger than the requested one"),
            Plan.Grid.World.SpanAcross() > Requested.SpanAcross());
        // Expansion is symmetric about the requested centre - not anchored to a corner, which
        // would move every coordinate the caller already computed against their own box.
        TestTrue(TEXT("the expansion is centred on the requested centre"),
            FMath::IsNearlyEqual(Plan.Grid.World.Centre().X, Requested.Centre().X, 1.0e-6) &&
            FMath::IsNearlyEqual(Plan.Grid.World.Centre().Y, Requested.Centre().Y, 1.0e-6));
        // The scale is held exactly: the planner absorbs the remainder by growing the area, never
        // by coarsening the resolution the caller asked for.
        TestTrue(TEXT("the achieved scale is exactly the requested one"),
            FMath::IsNearlyEqual(Plan.WorldUnitsPerPixel, CmPerPixel, 1.0e-9));
    }

    // (b) 3000 x 1000 cm divides exactly: no expansion, and the covered extent is the requested
    // one to the last centimetre.
    {
        const FWorldExtent2D Requested = MakeWorldExtent(0.0, 0.0, 3000.0, 1000.0);
        FTileGridPlan Plan;
        FString Err;
        if (!PlanTileGrid(Requested, Mapping, TilePixels, TilePixels, CmPerPixel, 0, Plan, Err))
        {
            AddError(FString::Printf(TEXT("PlanTileGrid refused the dividing fixture: %s"), *Err));
            return false;
        }
        TestFalse(TEXT("an exactly-dividing extent is NOT reported as expanded"), Plan.bExtentExpanded);
        TestEqual(TEXT("three columns"), Plan.Grid.Cols, 3);
        TestTrue(TEXT("the covered extent equals the requested one"),
            FMath::IsNearlyEqual(Plan.Grid.World.Min.X, Requested.Min.X, 1.0e-6) &&
            FMath::IsNearlyEqual(Plan.Grid.World.Max.X, Requested.Max.X, 1.0e-6) &&
            FMath::IsNearlyEqual(Plan.Grid.World.Min.Y, Requested.Min.Y, 1.0e-6) &&
            FMath::IsNearlyEqual(Plan.Grid.World.Max.Y, Requested.Max.Y, 1.0e-6));
    }
    return true;
}

// The exposure pin's arithmetic, without a renderer: the physical-camera fields this verb writes
// must evaluate, through the engine's own EV100 formula, to exactly the EV100 asked for. This is
// what makes `exposure.pinned` a measurement rather than an echo.
//
// Counterfactual: change PinFstop or PinIso without changing the shutter solve and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesExposureSolveIsExactTest,
    "PinWright.render.capture_ortho_tiles.ExposureSolveIsExact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesExposureSolveIsExactTest::RunTest(const FString& Parameters)
{
    const float Ev100Values[] = { -30.0f, -12.5f, -1.0f, 0.0f, 4.0f, 11.0f, 17.3f, 30.0f };
    for (float Ev100 : Ev100Values)
    {
        float Fstop = 0.0f;
        float Shutter = 0.0f;
        float Iso = 0.0f;
        PinWrightOrthoTiles::SolvePhysicalCameraForEv100(Ev100, Fstop, Shutter, Iso);
        const float RoundTrip = PinWrightOrthoTiles::PhysicalCameraEv100(Fstop, Shutter, Iso);
        TestTrue(FString::Printf(
                TEXT("EV100 %.4f solves to f/%.3f, 1/%g s, ISO %.1f and reads back as %.6f"),
                Ev100, Fstop, Shutter, Iso, RoundTrip),
            FMath::IsNearlyEqual(RoundTrip, Ev100, 1.0e-3f));
        TestTrue(TEXT("the solved ISO is at or above the engine's runtime floor of 1"), Iso >= 1.0f);
        TestTrue(TEXT("the solved shutter speed is strictly positive"), Shutter > 0.0f);
    }
    return true;
}

// ============================================================================
// 4. The live capture - RHI-guarded, but assertive on BOTH branches
// ============================================================================

// The capture must not dirty the level. The component is ownerless, transient and outered to the
// transient package rather than hung off a spawned actor, so no level package should change state
// - and this measures that rather than trusting the mechanism.
//
// THE GUARD DOES NOT SKIP THE ASSERTION. Whether the render succeeded or was refused for want of
// an RHI surface, the dirty-package count is compared before and after, because a verb that
// dirties the level while failing is exactly as bad as one that dirties it while succeeding. Only
// the pixel-level assertions are branch-dependent, and the branch taken is logged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoTilesDoesNotDirtyLevelTest,
    "PinWright.render.capture_ortho_tiles.DoesNotDirtyLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoTilesDoesNotDirtyLevelTest::RunTest(const FString& Parameters)
{
    const int32 DirtyBefore = OrthoCountDirtyLevelPackages();
    if (DirtyBefore < 0)
    {
        AddError(TEXT("no editor world, so the level-dirty contract could not be measured at all"));
        return false;
    }

    const FString Prefix = FString::Printf(TEXT("pw_ortho_dirty_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
    TSharedPtr<FJsonObject> Payload = OrthoBasePayload();
    Payload->RemoveField(TEXT("cmPerPixel"));
    Payload->SetNumberField(TEXT("cols"), 2.0);
    Payload->SetNumberField(TEXT("rows"), 2.0);
    Payload->SetNumberField(TEXT("tilePixels"), 64.0);
    Payload->SetStringField(TEXT("namePrefix"), Prefix);
    Payload->SetBoolField(TEXT("overwrite"), true);
    // Blank frames are expected headless and are not what this test is about.
    Payload->SetBoolField(TEXT("allowBlank"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_ortho_tiles handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    const int32 DirtyAfter = OrthoCountDirtyLevelPackages();

    // Asserted on BOTH branches - this is the whole point of the test.
    TestEqual(TEXT("the capture dirtied no additional level package"), DirtyAfter, DirtyBefore);

    FString OutputDir;
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        AddInfo(TEXT("BRANCH: the orthographic capture RAN - pixel-side assertions are live."));
        Capture.Result->TryGetStringField(TEXT("outputDir"), OutputDir);

        // The verb's own measurement of the same fact must agree with the test's.
        const TSharedPtr<FJsonObject>* LevelObj = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("level"), LevelObj) && LevelObj && (*LevelObj).IsValid())
        {
            bool bDirtied = true;
            TestTrue(TEXT("the response reports whether it dirtied the level"),
                (*LevelObj)->TryGetBoolField(TEXT("dirtiedByCapture"), bDirtied));
            TestFalse(TEXT("the response agrees that the level was not dirtied"), bDirtied);
        }
        else
        {
            AddError(TEXT("a successful capture published no `level` block, so the caller cannot see the dirty verdict"));
        }

        double TilesWritten = 0.0;
        TestTrue(TEXT("the response reports how many tiles it wrote"),
            Capture.Result->TryGetNumberField(TEXT("tilesWritten"), TilesWritten));
        TestEqual(TEXT("a 2x2 grid writes four tiles"), static_cast<int32>(TilesWritten), 4);

        FString ManifestPath;
        if (Capture.Result->TryGetStringField(TEXT("manifest"), ManifestPath))
        {
            TestTrue(TEXT("the manifest exists on disk"), IFileManager::Get().FileExists(*ManifestPath));

            // The reader's own loader must find the georeference in it, including a per-tile one.
            // This is the manifest -> image.tile handshake, exercised through the production
            // function image.tile calls rather than a test-local JSON walk.
            TSharedPtr<FJsonObject> LoadedRoot;
            FString ErrCode;
            FString ErrMsg;
            const bool bRootLoaded = PinWrightImage::LoadGeoreferenceObjectFromFile(ManifestPath,
                PinWrightTileGrid::FTileIndex(), LoadedRoot, ErrCode, ErrMsg);
            TestTrue(FString::Printf(TEXT("the image.* loader reads the manifest georeference (%s: %s)"),
                *ErrCode, *ErrMsg), bRootLoaded);
            TSharedPtr<FJsonObject> LoadedTile;
            const bool bTileLoaded = PinWrightImage::LoadGeoreferenceObjectFromFile(ManifestPath,
                PinWrightTileGrid::FTileIndex(1, 1), LoadedTile, ErrCode, ErrMsg);
            TestTrue(FString::Printf(TEXT("the image.* loader reads tile (1,1) georeference (%s: %s)"),
                *ErrCode, *ErrMsg), bTileLoaded);
        }
        else
        {
            AddError(TEXT("a successful capture published no manifest path"));
        }
    }
    else
    {
        AddInfo(FString::Printf(
            TEXT("BRANCH: the orthographic capture was REFUSED with %s - pixel-side assertions are SKIPPED, "
                 "but the level-dirty assertion above still ran. Message: %s"),
            *Capture.ErrorCode, *Capture.Message));
        TestTrue(FString::Printf(TEXT("the refusal is a typed capture failure (got %s)"), *Capture.ErrorCode),
            OrthoIsTypedCaptureFailure(Capture.ErrorCode));
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("outputDir"), OutputDir);
        }
    }

    OrthoDeleteDirectory(OutputDir);
    return true;
}
