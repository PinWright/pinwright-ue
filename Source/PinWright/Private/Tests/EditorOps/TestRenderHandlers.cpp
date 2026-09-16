// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Render domain handlers
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMeshActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace
{
    int32 CountEditorWorldActors()
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return -1;
        }

        int32 Count = 0;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            ++Count;
        }
        return Count;
    }

    void DeleteCaptureFileIfPresent(const FTestResponseCapture& Capture)
    {
        FString Path;
        if (Capture.bSuccess && Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("path"), Path))
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }

    // Loads a captured PNG and reports whether it contains more than one distinct
    // color. The empty-render bug produces a solid uniform frame (white plus, at
    // most, a faint single-color gizmo region that the canvas pass overwrites with
    // one value) — so a correctly rendered mesh frame is the only way to exceed a
    // small distinct-color count. Returns false (and leaves OutDistinct at 0) when
    // the file cannot be loaded/decoded so the caller can distinguish "not checked".
    bool CapturedImageHasColorVariation(const FString& PngPath, int32& OutDistinctColors)
    {
        OutDistinctColors = 0;
        FImage Loaded;
        if (!FImageUtils::LoadImage(*PngPath, Loaded))
        {
            return false;
        }

        // ChangeFormat is in-place and a no-op when already BGRA8/sRGB (the usual case
        // for a loaded PNG), so this avoids a second full-image buffer + copy/convert.
        Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
        if (Pixels.Num() == 0)
        {
            return false;
        }

        // Count distinct colors, bailing out early once we have clearly exceeded a
        // blank frame. A solid-white capture yields exactly 1; a rendered shaded
        // mesh yields many (gradients across faces, edges, background falloff).
        TSet<uint32> Distinct;
        Distinct.Reserve(64);
        for (const FColor& C : Pixels)
        {
            Distinct.Add(C.ToPackedARGB());
            if (Distinct.Num() > 16)
            {
                break;
            }
        }
        OutDistinctColors = Distinct.Num();
        return OutDistinctColors > 8;
    }

    // Loads a captured PNG and counts pixels whose alpha is not 255. A screen capture is an
    // opaque frame, so the only correct answer is zero. Returns false (leaving OutCount at 0)
    // when the file cannot be loaded/decoded, so the caller can tell "not checked" from "clean".
    bool CapturedImageNonOpaquePixelCount(const FString& PngPath, int64& OutNonOpaque, int64& OutTotal)
    {
        OutNonOpaque = 0;
        OutTotal = 0;
        FImage Loaded;
        if (!FImageUtils::LoadImage(*PngPath, Loaded))
        {
            return false;
        }

        Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
        if (Pixels.Num() == 0)
        {
            return false;
        }

        OutTotal = Pixels.Num();
        for (const FColor& C : Pixels)
        {
            if (C.A != 255)
            {
                ++OutNonOpaque;
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureImageStatsClassificationTest,
    "PinWright.render.capture_open_level.ImageStatsClassification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureImageStatsClassificationTest::RunTest(const FString& Parameters)
{
    TArray<FColor> BlackPixels;
    BlackPixels.Init(FColor::Black, 64);
    const PinWrightRenderCapture::FCaptureImageStats BlackStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(BlackPixels);
    TestTrue(TEXT("uniform black frame is classified blank"), BlackStats.bBlank);
    TestEqual(TEXT("uniform black frame has zero mean luminance"), BlackStats.MeanLuminance, 0.0);
    TestEqual(TEXT("uniform black frame has zero luminance variance"), BlackStats.LuminanceVariance, 0.0);

    TArray<FColor> LitPixels;
    LitPixels.Init(FColor::Black, 64);
    for (int32 Index = 0; Index < LitPixels.Num(); Index += 2)
    {
        LitPixels[Index] = FColor::White;
    }
    const PinWrightRenderCapture::FCaptureImageStats LitStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(LitPixels);
    TestFalse(TEXT("varied lit frame is not classified blank"), LitStats.bBlank);
    TestTrue(TEXT("varied lit frame has nonzero luminance variance"), LitStats.LuminanceVariance > 0.0);
    return true;
}

// ============================================================================
// render.create_render_target — all params optional; empty payload is valid
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCreateRenderTargetValidParamsNoCrashTest,
    "PinWright.render.create_render_target.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCreateRenderTargetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // All params are optional — empty payload exercises the default code path
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("render.create_render_target handler found and invoked"), InvokeHandler(TEXT("render.create_render_target"), Payload));
    // Empty payload defaults the name to "NewRenderTarget"; the handler leaves the
    // package dirty, which a later save-all would flush to disk — remove it.
    CleanupTestAsset(TEXT("/Game/RenderTargets/NewRenderTarget"));
    return true;
}

// ============================================================================
// render.create_render_target — explicit dimension and name params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCreateRenderTargetWithDimsNoCrashTest,
    "PinWright.render.create_render_target.WithDimsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCreateRenderTargetWithDimsNoCrashTest::RunTest(const FString& Parameters)
{
    // Provide all optional params to exercise the full construction branch
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("MyTestRT"));
    Payload->SetNumberField(TEXT("width"), 512.0);
    Payload->SetNumberField(TEXT("height"), 512.0);
    Payload->SetStringField(TEXT("format"), TEXT("RTF_RGBA8"));
    Payload->SetStringField(TEXT("packagePath"), TEXT("/Game/RenderTargets"));
    TestTrue(TEXT("render.create_render_target with dims handler found and invoked"), InvokeHandler(TEXT("render.create_render_target"), Payload));
    CleanupTestAsset(TEXT("/Game/RenderTargets/MyTestRT"));
    return true;
}

// ============================================================================
// render.attach_render_target_to_volume — empty payload triggers actor-not-found
// error path; handler must not crash
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAttachRenderTargetToVolumeValidParamsNoCrashTest,
    "PinWright.render.attach_render_target_to_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAttachRenderTargetToVolumeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Empty volumePath causes SendError("ACTOR_NOT_FOUND") — a stable, non-crashing exit
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("render.attach_render_target_to_volume handler found and invoked"), InvokeHandler(TEXT("render.attach_render_target_to_volume"), Payload));
    return true;
}

// ============================================================================
// render.attach_render_target_to_volume — missing materialPath/parameterName
// exercises the INVALID_ARGUMENT error branch after volume/RT lookups fail first
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAttachRenderTargetToVolumeMissingMaterialNoCrashTest,
    "PinWright.render.attach_render_target_to_volume.MissingMaterialNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAttachRenderTargetToVolumeMissingMaterialNoCrashTest::RunTest(const FString& Parameters)
{
    // volumePath and targetPath resolve to nothing → ACTOR_NOT_FOUND before material check
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumePath"), TEXT("/Game/NonExistentVolume"));
    Payload->SetStringField(TEXT("targetPath"), TEXT("/Game/NonExistentRT"));
    TestTrue(TEXT("render.attach_render_target_to_volume with missing material found and invoked"), InvokeHandler(TEXT("render.attach_render_target_to_volume"), Payload));
    return true;
}

// ============================================================================
// render.nanite_rebuild_mesh — assetPath provided but points to a non-existent
// asset; handler must return ASSET_NOT_FOUND without crashing
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderNaniteRebuildMeshValidParamsNoCrashTest,
    "PinWright.render.nanite_rebuild_mesh.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderNaniteRebuildMeshValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // assetPath is present but the mesh does not exist in the test environment;
    // the handler emits ASSET_NOT_FOUND and returns gracefully
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NonExistentMesh.NonExistentMesh"));
    TestTrue(TEXT("render.nanite_rebuild_mesh (non-existent asset) handler found and invoked"), InvokeHandler(TEXT("render.nanite_rebuild_mesh"), Payload));
    return true;
}

// ============================================================================
// render.lumen_update_scene — no params required; handler must not crash
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderLumenUpdateSceneValidParamsNoCrashTest,
    "PinWright.render.lumen_update_scene.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderLumenUpdateSceneValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // RPC_NO_PARAMS — empty payload is the canonical invocation
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("render.lumen_update_scene handler found and invoked"), InvokeHandler(TEXT("render.lumen_update_scene"), Payload));
    return true;
}

// ============================================================================
// render.lumen_update_scene — regression for B-lumen-update-scene-silent-success.
//
// The handler used to run GEngine->Exec(World, "r.Lumen.Scene.Recapture"), a
// console command that does not exist in UE 5.x (the real Lumen cvars are under
// r.LumenScene.*). It discarded the Exec bool and hardcoded executed:true, so the
// RPC reported a successful Lumen recapture that never ran. The fix runs the real
// one-shot lever r.LumenScene.SurfaceCache.Reset 1, honors the Exec bool, echoes
// the actual command, and emits EXEC_FAILED when the engine rejects it.
//
// Counterfactual: reverting to the bogus command makes the echoed `command`
// field a name the engine rejects, so running that same string through
// system.console_command returns EXEC_FAILED, failing the cross-check below.
// (The cross-check is the authoritative discriminator: it catches any
// unrecognized command name, not just the one specific old string.)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderLumenUpdateSceneRunsRealCommandTest,
    "PinWright.render.lumen_update_scene.RunsRealCommand",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderLumenUpdateSceneRunsRealCommandTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world; the handler's recapture path is unreachable in this run."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("render.lumen_update_scene handler found"),
        InvokeHandlerWithCapture(TEXT("render.lumen_update_scene"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    // With a world present, the real cvar is recognized → success. The old code
    // would also report success here (it hardcoded it), so success alone is not the
    // discriminator — the system.console_command cross-check below is.
    TestTrue(TEXT("lumen_update_scene reports success when an editor world exists"), Capture.bSuccess);

    // Assert the preconditions unconditionally: an absent result object or an
    // empty/missing command field is itself the silent-success regression this
    // test guards against, so it must fail the test rather than skip the checks.
    TestTrue(TEXT("success payload carries a result object"), Capture.Result.IsValid());
    FString ReportedCommand;
    if (Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("command"), ReportedCommand);
    }
    TestFalse(TEXT("handler echoed a command"), ReportedCommand.IsEmpty());

    // Cross-check (the authoritative discriminator): the command the handler claims
    // to have run must be one the engine actually recognizes. system.console_command
    // honors the Exec bool and emits EXEC_FAILED for unrecognized commands, so running
    // the echoed command string through it must succeed. This fails if the handler
    // regresses to the bogus r.Lumen.Scene.Recapture name (or any unrecognized name).
    // Run unconditionally on the now-guaranteed-non-empty command; if the precondition
    // above failed, this exercises the empty-command rejection path and still fails.
    //
    // force:true is required and does NOT weaken this assertion. r.LumenScene.SurfaceCache.Reset
    // carries ECVF_Scalability (LumenSceneRendering.cpp:90, flags :93), so
    // system.console_command now refuses a set of it with SCALABILITY_CVAR_USE_TYPED_VERB before
    // reaching Exec (board B-console-member-cvar-pin-freezes-scalability). Without the flag this
    // cross-check would report the guard's refusal rather than the engine's verdict on the name,
    // which is the exact opposite of what it exists to measure. It costs the host nothing extra:
    // render.lumen_update_scene ran the identical line through GEngine->Exec a few statements
    // above, so the CVar is already at ECVF_SetByConsole either way — and the engine clears this
    // particular one-shot lever back to 0 on the next frame.
    TSharedPtr<FJsonObject> ConsolePayload = MakeShared<FJsonObject>();
    ConsolePayload->SetStringField(TEXT("command"), ReportedCommand);
    ConsolePayload->SetBoolField(TEXT("force"), true);
    FTestResponseCapture ConsoleCapture;
    TestTrue(TEXT("system.console_command handler found"),
        InvokeHandlerWithCapture(TEXT("system.console_command"), ConsolePayload, ConsoleCapture));
    TestNotEqual(TEXT("the scalability guard did not swallow the cross-check"),
        ConsoleCapture.ErrorCode, FString(TEXT("SCALABILITY_CVAR_USE_TYPED_VERB")));
    TestTrue(TEXT("the echoed command is recognized by the engine (no EXEC_FAILED)"),
        ConsoleCapture.bSuccess);
    return true;
}

// ============================================================================
// render.capture_asset_preview — registration and validation
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAssetPreviewMissingAssetPathTest,
    "PinWright.render.capture_asset_preview.MissingAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAssetPreviewMissingAssetPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_asset_preview handler found"), InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("missing assetPath fails"), Capture.bSuccess);
    TestEqual(TEXT("missing assetPath error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAssetPreviewInvalidDimensionsTest,
    "PinWright.render.capture_asset_preview.InvalidDimensions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAssetPreviewInvalidDimensionsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Payload->SetNumberField(TEXT("width"), 0);
    Payload->SetNumberField(TEXT("height"), 128);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_asset_preview handler found"), InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("invalid dimensions fail"), Capture.bSuccess);
    TestEqual(TEXT("invalid dimensions error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAssetPreviewMissingAssetTest,
    "PinWright.render.capture_asset_preview.MissingAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAssetPreviewMissingAssetTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NoSuchMesh.NoSuchMesh"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_asset_preview handler found"), InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("missing asset fails"), Capture.bSuccess);
    TestEqual(TEXT("missing asset error"), Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAssetPreviewKnownMeshNoActorSpawnTest,
    "PinWright.render.capture_asset_preview.KnownMeshNoActorSpawn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAssetPreviewKnownMeshNoActorSpawnTest::RunTest(const FString& Parameters)
{
    const int32 ActorCountBefore = CountEditorWorldActors();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 128);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), -300);
    Location->SetNumberField(TEXT("y"), 0);
    Location->SetNumberField(TEXT("z"), 120);
    Payload->SetObjectField(TEXT("location"), Location);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -15);
    Rotation->SetNumberField(TEXT("yaw"), 0);
    Rotation->SetNumberField(TEXT("roll"), 0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);
    Payload->SetNumberField(TEXT("fov"), 50);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_asset_preview handler found"), InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        DeleteCaptureFileIfPresent(Capture);
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                if (UObject* Cube = LoadObject<UObject>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
                {
                    AssetEditorSubsystem->CloseAllEditorsForAsset(Cube);
                }
            }
        }
    };

    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    if (Capture.bSuccess)
    {
        TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
        if (Capture.Result.IsValid())
        {
            TestEqual(TEXT("capture width"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("width"))), 256);
            TestEqual(TEXT("capture height"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("height"))), 128);
            TestEqual(TEXT("capture source"), Capture.Result->GetStringField(TEXT("captureSource")), FString(TEXT("staticMeshEditorPreview")));
        }
    }
    else
    {
        const bool bTypedPreviewFailure =
            Capture.ErrorCode == TEXT("OPEN_FAILED") ||
            Capture.ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            Capture.ErrorCode == TEXT("CAPTURE_FAILED");
        TestTrue(TEXT("failure is typed when no preview viewport can be captured"), bTypedPreviewFailure);
    }

    const int32 ActorCountAfter = CountEditorWorldActors();
    if (ActorCountBefore >= 0 && ActorCountAfter >= 0)
    {
        TestEqual(TEXT("asset preview capture does not spawn level actors"), ActorCountAfter, ActorCountBefore);
    }
    return true;
}

// ============================================================================
// render.capture_asset_preview — regression for B-capture-asset-preview-renders-empty.
//
// The Static Mesh editor's preview FEditorViewportClient is NOT realtime by default
// (FEditorViewportClient::IsRealtime() returns false absent a realtime override /
// RealTimeUntilFrameNumber), so the manually driven offscreen Draw() in the capture
// path composited only the canvas/HUD pass (the world-axis gizmo) into the slate
// readback target — the 3D scene render never landed. The result was a clean success
// with a valid PNG whose pixels were a solid uniform frame: silent success-with-no-effect.
//
// The fix pushes a temporary realtime override (AddRealtimeOverride + RequestRealTimeFrames)
// on the preview viewport client for the duration of the capture. This test captures the
// engine Cube and asserts the resulting PNG has real color variation. A blank/uniform
// frame (the reverted behavior) has ~1 distinct color and fails CapturedImageHasColorVariation.
//
// Counterfactual: reverting the realtime override leaves the scene pass un-composited, so
// the captured PNG is a solid frame, CapturedImageHasColorVariation returns false, and the
// "rendered frame is not blank" assertion fails. Headless runs with no real scene render
// fail with a typed CAPTURE_FAILED/PREVIEW_VIEWPORT_NOT_FOUND/OPEN_FAILED and skip the
// pixel assertion (no false negative) — the same live-viewport guard the sibling render
// tests use.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAssetPreviewRendersNonBlankTest,
    "PinWright.render.capture_asset_preview.RendersNonBlank",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAssetPreviewRendersNonBlankTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), -300);
    Location->SetNumberField(TEXT("y"), 0);
    Location->SetNumberField(TEXT("z"), 120);
    Payload->SetObjectField(TEXT("location"), Location);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -15);
    Rotation->SetNumberField(TEXT("yaw"), 0);
    Rotation->SetNumberField(TEXT("roll"), 0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);
    Payload->SetNumberField(TEXT("fov"), 50);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_asset_preview handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        DeleteCaptureFileIfPresent(Capture);
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                if (UObject* Cube = LoadObject<UObject>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
                {
                    AssetEditorSubsystem->CloseAllEditorsForAsset(Cube);
                }
            }
        }
    };

    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess)
    {
        // Headless / no-RHI-scene-render run: the capture path could not produce a real
        // frame. Require a TYPED capture/preview failure (never a crash or wrong code),
        // then skip the pixel assertion so the test does not false-negative.
        const bool bTypedPreviewFailure =
            Capture.ErrorCode == TEXT("OPEN_FAILED") ||
            Capture.ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            Capture.ErrorCode == TEXT("CAPTURE_FAILED");
        TestTrue(TEXT("failure is typed when no preview viewport can be captured"), bTypedPreviewFailure);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            TEXT("Skipped pixel check: preview viewport capture unavailable in this run."));
        return true;
    }

    TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return false;
    }

    FString Path;
    TestTrue(TEXT("success result carries a path"), Capture.Result->TryGetStringField(TEXT("path"), Path));
    TestTrue(TEXT("captured PNG exists on disk"), !Path.IsEmpty() && IFileManager::Get().FileExists(*Path));
    if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
    {
        return false;
    }

    // The core regression assertion: the rendered Cube frame must NOT be a uniform
    // (blank/white) image. The pre-fix code composited only the canvas pass, yielding a
    // solid frame (~1 distinct color); the realtime-override fix lands the scene render.
    int32 DistinctColors = 0;
    const bool bHasVariation = CapturedImageHasColorVariation(Path, DistinctColors);
    TestTrue(FString::Printf(TEXT("captured mesh frame is not blank (distinct colors=%d)"), DistinctColors), bHasVariation);

    return true;
}

// ============================================================================
// render.capture_open_level — captured PNG must be fully opaque
//
// The editor back buffer carries alpha 0 over every SCENE pixel; only Slate-composited
// overlays (the world-axis gizmo, the ortho scale bar) land with alpha 255. Encoding that
// raw alpha produced a PNG that was ~99.97% transparent, so whether the capture "worked"
// depended entirely on the consumer: one that composites alpha showed a blank frame with
// nothing but the overlays, while one that re-encodes without an alpha channel showed the
// identical file rendering correctly. That split ran along file size, which is why it was
// misdiagnosed for a day as "horizontal orthographic renders no scene geometry" — the
// edge-on ortho shots are small enough to survive as PNG while the perspective and
// top-down shots are large enough to be re-encoded on the way to a viewer. The geometry
// was in the RGB planes the whole time (B-horizontal-orthographic-views-render-no-geometry).
//
// The fix stamps every capture opaque through PinWrightScreenshotUtils::ForceOpaqueAlpha.
//
// Counterfactual: remove the ForceOpaqueAlpha call at PreviewViewportCaptureUtils.cpp's
// capture path — reintroducing the exact regression — and this test fails, because the
// decoded PNG is then almost entirely alpha 0. The sibling unit test
// Tests/Render/TestCaptureOpaqueAlpha.cpp does NOT catch that: it calls the helper itself,
// so it passes with the production call site deleted. This test is the one wired to the
// handler. Headless runs that cannot produce a frame fail with a typed error and skip the
// pixel assertion, matching the sibling render tests.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelOpaqueAlphaTest,
    "PinWright.render.capture_open_level.CapturedPngIsOpaque",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelOpaqueAlphaTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    Payload->SetBoolField(TEXT("allowBlank"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    ON_SCOPE_EXIT { DeleteCaptureFileIfPresent(Capture); };

    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess)
    {
        // Headless / no-viewport run: skip the pixel assertion but still require a typed
        // failure so a crash or a wrong error code cannot pass silently.
        const bool bTypedCaptureFailure =
            Capture.ErrorCode == TEXT("CAPTURE_FAILED") ||
            Capture.ErrorCode == TEXT("NO_VIEWPORT") ||
            Capture.ErrorCode == TEXT("VIEWPORT_NOT_FOUND");
        TestTrue(TEXT("failure is typed when no level viewport can be captured"), bTypedCaptureFailure);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            TEXT("Skipped alpha check: level viewport capture unavailable in this run."));
        return true;
    }

    TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return false;
    }

    FString Path;
    TestTrue(TEXT("success result carries a path"), Capture.Result->TryGetStringField(TEXT("path"), Path));
    TestTrue(TEXT("captured PNG exists on disk"), !Path.IsEmpty() && IFileManager::Get().FileExists(*Path));
    if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
    {
        return false;
    }

    // The core regression assertion: not one pixel of a screen capture may be non-opaque.
    int64 NonOpaque = 0;
    int64 Total = 0;
    const bool bDecoded = CapturedImageNonOpaquePixelCount(Path, NonOpaque, Total);
    TestTrue(TEXT("captured PNG decodes"), bDecoded);
    if (bDecoded)
    {
        TestEqual(
            FString::Printf(TEXT("captured PNG has no non-opaque pixels (%lld of %lld were non-opaque)"),
                NonOpaque, Total),
            NonOpaque, static_cast<int64>(0));
    }

    return true;
}

// ============================================================================
// render.capture_open_level — registration and validation
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelInvalidDimensionsTest,
    "PinWright.render.capture_open_level.InvalidDimensions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelInvalidDimensionsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), -1);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"), InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("invalid dimensions fail"), Capture.bSuccess);
    TestEqual(TEXT("invalid dimensions error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelInvalidProjectionModeTest,
    "PinWright.render.capture_open_level.InvalidProjectionMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelInvalidProjectionModeTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("projectionMode"), TEXT("fisheye"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"), InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("invalid projection mode fails"), Capture.bSuccess);
    TestEqual(TEXT("invalid projection mode error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelArbitraryOrthoRotationTest,
    "PinWright.render.capture_open_level.ArbitraryOrthoRotation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelArbitraryOrthoRotationTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), 13);
    Rotation->SetNumberField(TEXT("yaw"), 27);
    Rotation->SetNumberField(TEXT("roll"), 0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"), InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("arbitrary orthographic rotation fails or no viewport is available"), Capture.bSuccess);
    const bool bExpectedError =
        Capture.ErrorCode == TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION") ||
        Capture.ErrorCode == TEXT("NO_EDITOR_WORLD") ||
        Capture.ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT");
    TestTrue(TEXT("arbitrary orthographic rotation has typed failure"), bExpectedError);
    return true;
}

// A straight-down orthographic pose (pitch -90) maps onto the engine's LVT_OrthoXY top view, so it
// must NEVER be rejected as an unsupported orthographic rotation. Guards the regression this fix
// removed: the old guard accepted only rotation (0,0,0), making top-down layout shots impossible.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelTopDownOrthoAcceptedTest,
    "PinWright.render.capture_open_level.TopDownOrthoAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelTopDownOrthoAcceptedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
    Payload->SetNumberField(TEXT("orthoWidth"), 5000);
    Payload->SetBoolField(TEXT("allowBlank"), true);
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), 0);
    Location->SetNumberField(TEXT("y"), 0);
    Location->SetNumberField(TEXT("z"), 5000);
    Payload->SetObjectField(TEXT("location"), Location);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -90);
    Rotation->SetNumberField(TEXT("yaw"), 0);
    Rotation->SetNumberField(TEXT("roll"), 0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"), InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        DeleteCaptureFileIfPresent(Capture);
    };

    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    // The decisive assertion: whatever else happens (headless hosts have no viewport), a top-down
    // orthographic pose is never an unsupported-rotation failure.
    TestNotEqual(TEXT("top-down orthographic is not rejected as an unsupported rotation"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION")));

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        TestEqual(TEXT("orthographic capture reports the resolved view"),
            Capture.Result->GetStringField(TEXT("orthoView")), FString(TEXT("top")));
        // The engine fixes the in-plane orientation of its top view, so the effective pose is
        // pitch -90 / yaw 180 even though yaw 0 was requested.
        const TSharedPtr<FJsonObject>* RotObj = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("cameraRotation"), RotObj) && RotObj)
        {
            double Pitch = 0.0;
            (*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
            TestTrue(TEXT("effective orthographic pose looks straight down"),
                FMath::Abs(Pitch + 90.0) < 0.5);
        }
    }
    else if (!Capture.bSuccess)
    {
        const bool bTypedViewportFailure =
            Capture.ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            Capture.ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            Capture.ErrorCode == TEXT("CAPTURE_FAILED");
        TestTrue(TEXT("failure is typed when no level viewport can be captured"), bTypedViewportFailure);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelPerspectiveNoActorSpawnTest,
    "PinWright.render.capture_open_level.PerspectiveNoActorSpawn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelPerspectiveNoActorSpawnTest::RunTest(const FString& Parameters)
{
    const int32 ActorCountBefore = CountEditorWorldActors();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 128);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), 0);
    Location->SetNumberField(TEXT("y"), 0);
    Location->SetNumberField(TEXT("z"), 300);
    Payload->SetObjectField(TEXT("location"), Location);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -20);
    Rotation->SetNumberField(TEXT("yaw"), 0);
    Rotation->SetNumberField(TEXT("roll"), 0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);
    Payload->SetNumberField(TEXT("fov"), 60);
    Payload->SetBoolField(TEXT("allowBlank"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"), InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        DeleteCaptureFileIfPresent(Capture);
    };

    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    if (Capture.bSuccess)
    {
        TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
        if (Capture.Result.IsValid())
        {
            TestEqual(TEXT("capture width"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("width"))), 256);
            TestEqual(TEXT("capture height"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("height"))), 128);
            TestEqual(TEXT("capture source"), Capture.Result->GetStringField(TEXT("captureSource")), FString(TEXT("levelEditorViewport")));
        }
    }
    else
    {
        const bool bTypedViewportFailure =
            Capture.ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            Capture.ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            Capture.ErrorCode == TEXT("CAPTURE_FAILED");
        TestTrue(TEXT("failure is typed when no level viewport can be captured"), bTypedViewportFailure);
    }

    const int32 ActorCountAfter = CountEditorWorldActors();
    if (ActorCountBefore >= 0 && ActorCountAfter >= 0)
    {
        TestEqual(TEXT("open level capture does not spawn level actors"), ActorCountAfter, ActorCountBefore);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelPopulatedLitVarianceTest,
    "PinWright.render.capture_open_level.PopulatedLitSceneHasVariance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelPopulatedLitVarianceTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped populated-lit capture: no active editor world."));
        return true;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags |= RF_Transient;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AStaticMeshActor* MeshActor = World->SpawnActor<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), FTransform(FVector::ZeroVector), SpawnParameters);
    ADirectionalLight* LightActor = World->SpawnActor<ADirectionalLight>(
        ADirectionalLight::StaticClass(), FTransform(FRotator(-45.0f, 25.0f, 0.0f)), SpawnParameters);
    ON_SCOPE_EXIT
    {
        if (MeshActor) MeshActor->Destroy();
        if (LightActor) LightActor->Destroy();
    };

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!MeshActor || !LightActor || !Cube)
    {
        AddError(TEXT("Could not create populated lit capture fixture."));
        return false;
    }
    MeshActor->GetStaticMeshComponent()->SetStaticMesh(Cube);
    MeshActor->SetActorScale3D(FVector(2.0f));
    LightActor->GetLightComponent()->SetIntensity(8.0f);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), -500);
    Location->SetNumberField(TEXT("y"), 0);
    Location->SetNumberField(TEXT("z"), 150);
    Payload->SetObjectField(TEXT("location"), Location);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -16.7);
    Rotation->SetNumberField(TEXT("yaw"), 0);
    Rotation->SetNumberField(TEXT("roll"), 0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    ON_SCOPE_EXIT { DeleteCaptureFileIfPresent(Capture); };
    if (!Capture.bSuccess)
    {
        const bool bUnavailable =
            Capture.ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            Capture.ErrorCode == TEXT("VIEWPORT_WORLD_MISMATCH") ||
            Capture.ErrorCode == TEXT("CAPTURE_FAILED");
        TestTrue(TEXT("unavailable viewport fails with a typed diagnostic"), bUnavailable);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped variance assertion: level viewport unavailable in this run."));
        return true;
    }

    TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid()) return false;
    TestFalse(TEXT("populated lit capture is not blank"), Capture.Result->GetBoolField(TEXT("blank")));
    const TSharedPtr<FJsonObject>* ImageStats = nullptr;
    TestTrue(TEXT("capture reports imageStats"),
        Capture.Result->TryGetObjectField(TEXT("imageStats"), ImageStats) && ImageStats);
    if (ImageStats && *ImageStats)
    {
        TestTrue(TEXT("populated lit capture has nonzero luminance variance"),
            (*ImageStats)->GetNumberField(TEXT("luminanceVariance")) > 0.0);
    }
    const TSharedPtr<FJsonObject>* Viewport = nullptr;
    TestTrue(TEXT("capture reports viewport state"),
        Capture.Result->TryGetObjectField(TEXT("viewport"), Viewport) && Viewport);
    TestTrue(TEXT("capture reports matching active and viewport worlds"),
        Capture.Result->GetBoolField(TEXT("worldMatches")));
    return true;
}
