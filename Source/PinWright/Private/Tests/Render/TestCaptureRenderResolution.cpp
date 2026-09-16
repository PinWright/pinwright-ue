// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Mesh.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "RHI.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace CaptureRenderResolutionTestLocal
{
    const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    bool IsHostLimitedAcquisitionFailure(const FString& ErrorCode)
    {
        return ErrorCode == ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND ||
            ErrorCode == ErrorCodes::ERR_OPEN_FAILED ||
            ErrorCode == ErrorCodes::ERR_SUBSYSTEM_MISSING ||
            ErrorCode == ErrorCodes::ERR_EDITOR_NOT_AVAILABLE ||
            ErrorCode == ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureRenderResolutionReportedTest,
    "PinWright.render.capture.RenderResolutionReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureRenderResolutionReportedTest::RunTest(const FString& Parameters)
{
    PinWrightRenderCapture::FViewportCaptureOutput Capture;
    Capture.RenderWidth = 1120;
    Capture.RenderHeight = 1400;
    Capture.RenderPrimaryResolutionFraction = 1.0;
    Capture.RenderSecondaryResolutionFraction = 1.0;
    Capture.RenderResolutionFraction = 1.0;
    Capture.RenderScreenPercentage = 100;
    Capture.RenderAntiAliasingMethod = TEXT("FXAA");
    Capture.RenderAntiAliasingMethodValue = 1;
    Capture.bRenderResolutionPinned = true;
    Capture.bRenderUpscaled = false;
    Capture.bTemporalAntiAliasingSuppressed = true;

    const TSharedPtr<FJsonObject> Viewport =
        PinWrightRenderCapture::MakeViewportInfoObject(Capture);
    const TSharedPtr<FJsonObject> Render = Viewport->GetObjectField(TEXT("render"));
    TestEqual(TEXT("the internal raster width is published"),
        Render->GetIntegerField(TEXT("width")), 1120);
    TestEqual(TEXT("the internal raster height is published"),
        Render->GetIntegerField(TEXT("height")), 1400);
    TestEqual(TEXT("the primary resolution fraction is published"),
        Render->GetNumberField(TEXT("primaryResolutionFraction")), 1.0);
    TestEqual(TEXT("the secondary resolution fraction is published"),
        Render->GetNumberField(TEXT("secondaryResolutionFraction")), 1.0);
    TestEqual(TEXT("the combined resolution fraction is published"),
        Render->GetNumberField(TEXT("resolutionFraction")), 1.0);
    TestEqual(TEXT("the editor preview percentage is published"),
        Render->GetIntegerField(TEXT("screenPercentage")), 100);
    TestEqual(TEXT("the effective anti-aliasing method is published"),
        Render->GetStringField(TEXT("antiAliasingMethod")), FString(TEXT("FXAA")));
    TestTrue(TEXT("the native-resolution pin is reported"),
        Render->GetBoolField(TEXT("resolutionPinned")));
    TestFalse(TEXT("native capture is not reported as upscaled"),
        Render->GetBoolField(TEXT("upscaled")));
    TestTrue(TEXT("temporal accumulation suppression is reported"),
        Render->GetBoolField(TEXT("temporalAntiAliasingSuppressed")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureNativeRasterMatchesDecodedPngTest,
    "PinWright.render.capture.NativeRasterMatchesDecodedPng",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureNativeRasterMatchesDecodedPngTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;

    if (!FApp::CanEverRender() || !GDynamicRHI || !FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-rhi"),
            TEXT("The host has no live RHI/Slate renderer, so no preview PNG can be captured."));
        return true;
    }
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor"),
            TEXT("GEditor is unavailable, so the Cube preview cannot be opened."));
        return true;
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr,
        CaptureRenderResolutionTestLocal::CubePath);
    UAssetEditorSubsystem* AssetEditors =
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!Cube || !AssetEditors)
    {
        AddError(TEXT("The Cube fixture or AssetEditorSubsystem is unavailable."));
        return false;
    }
    if (AssetEditors->FindEditorForAsset(Cube, /*bFocusIfOpen=*/false))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cube-editor-already-open"),
            TEXT("The Cube editor was already open, so its private preview-resolution optional "
                 "cannot be assumed unset or safely used for the restoration assertion."));
        return true;
    }

    IConsoleVariable* ModeCVar = IConsoleManager::Get().FindConsoleVariable(
        TEXT("r.Editor.Viewport.ScreenPercentageMode.NonRealTime"));
    IConsoleVariable* PercentageCVar = IConsoleManager::Get().FindConsoleVariable(
        TEXT("r.Editor.Viewport.ScreenPercentage"));
    // The derived default is not the manual percentage alone: FStaticResolutionFractionHeuristic::
    // ResolveResolutionFraction clamps it up to MinRenderingResolution and down to
    // MaxRenderingResolution, both relative to the LIVE viewport's pixel count. On a preview
    // viewport of ~2.2 Mpx the 720p floor alone reports 65 for a requested 61, which reads as a
    // capture defect and is not one. Neutralized for the probe so the percentage reads back
    // exactly; a pixel count is not something this test can choose.
    IConsoleVariable* MinResolutionCVar = IConsoleManager::Get().FindConsoleVariable(
        TEXT("r.Editor.Viewport.MinRenderingResolution"));
    IConsoleVariable* MaxResolutionCVar = IConsoleManager::Get().FindConsoleVariable(
        TEXT("r.Editor.Viewport.MaxRenderingResolution"));
    if (!ModeCVar || !PercentageCVar || !MinResolutionCVar || !MaxResolutionCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("screen-percentage-cvars-missing"),
            TEXT("The editor screen-percentage defaults are unavailable, so unset optional "
                 "state cannot be probed behaviorally."));
        return true;
    }

    const int32 OriginalMode = ModeCVar->GetInt();
    const int32 OriginalPercentage = PercentageCVar->GetInt();
    const EConsoleVariableFlags ModeSetBy = static_cast<EConsoleVariableFlags>(
        ModeCVar->GetFlags() & ECVF_SetByMask);
    const EConsoleVariableFlags PercentageSetBy = static_cast<EConsoleVariableFlags>(
        PercentageCVar->GetFlags() & ECVF_SetByMask);
    const int32 OriginalMinResolution = MinResolutionCVar->GetInt();
    const int32 OriginalMaxResolution = MaxResolutionCVar->GetInt();
    const EConsoleVariableFlags MinResolutionSetBy = static_cast<EConsoleVariableFlags>(
        MinResolutionCVar->GetFlags() & ECVF_SetByMask);
    const EConsoleVariableFlags MaxResolutionSetBy = static_cast<EConsoleVariableFlags>(
        MaxResolutionCVar->GetFlags() & ECVF_SetByMask);
    ModeCVar->Set(0, ModeSetBy);
    PercentageCVar->Set(73, PercentageSetBy);
    // Zero disables each clamp at its `> 0.0f` guard rather than moving it to a different pixel
    // count, so the derived default is exactly the manual percentage on any host.
    MinResolutionCVar->Set(0, MinResolutionSetBy);
    MaxResolutionCVar->Set(0, MaxResolutionSetBy);
    ON_SCOPE_EXIT
    {
        ModeCVar->Set(OriginalMode, ModeSetBy);
        PercentageCVar->Set(OriginalPercentage, PercentageSetBy);
        MinResolutionCVar->Set(OriginalMinResolution, MinResolutionSetBy);
        MaxResolutionCVar->Set(OriginalMaxResolution, MaxResolutionSetBy);
    };

    FAssetEditorViewportAcquisition Acquisition;
    FString ErrorCode;
    FString ErrorMessage;
    FString CapturePath;
    ON_SCOPE_EXIT
    {
        if (!CapturePath.IsEmpty())
        {
            IFileManager::Get().Delete(*CapturePath, /*RequireExists=*/false,
                /*EvenReadOnly=*/true, /*Quiet=*/true);
        }
        Acquisition.ViewportClient = nullptr;
        Acquisition.SceneViewport.Reset();
        Acquisition.ViewportWidget.Reset();
        if (!Acquisition.bWasAlreadyOpen)
        {
            AssetEditors->CloseAllEditorsForAsset(Cube);
        }
    };

    if (!AcquireAssetEditorViewport(Cube,
            PinWrightCaptureSubjectMesh::StaticMeshToolkitNames(), Acquisition,
            ErrorCode, ErrorMessage))
    {
        if (CaptureRenderResolutionTestLocal::IsHostLimitedAcquisitionFailure(ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-preview-viewport"),
                FString::Printf(TEXT("The Cube preview could not be acquired (%s: %s)."),
                    *ErrorCode, *ErrorMessage));
            return true;
        }
        AddError(FString::Printf(TEXT("Unexpected Cube preview acquisition failure (%s: %s)."),
            *ErrorCode, *ErrorMessage));
        return false;
    }

    if (Acquisition.bWasAlreadyOpen)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cube-editor-opened-during-acquire"),
            TEXT("The Cube editor became open before acquisition completed, so this test does "
                 "not own the editor and cannot assume its preview-resolution optional is unset."));
        return true;
    }

    if (!Acquisition.ViewportClient || !Acquisition.SceneViewport.IsValid())
    {
        AddError(TEXT("Cube acquisition succeeded without a usable preview client and viewport."));
        return false;
    }
    FEditorViewportClient& Client = *Acquisition.ViewportClient;
    if (!Client.IsPerspective())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cube-preview-not-perspective"),
            TEXT("The Cube preview is not perspective, so this fixture cannot drive the normal "
                 "preview-resolution path."));
        return true;
    }

    const bool bPreviewingBefore = Client.IsPreviewingScreenPercentage();
    const int32 PreviewPercentageBefore = Client.GetPreviewScreenPercentage();
    if (bPreviewingBefore || PreviewPercentageBefore != 73)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preview-optional-not-unset"),
            FString::Printf(TEXT("A fresh Cube preview did not expose the expected unset state "
                                 "(previewing=%s, derived percentage=%d)."),
                bPreviewingBefore ? TEXT("true") : TEXT("false"), PreviewPercentageBefore));
        return true;
    }

    PinWrightRenderCapture::FViewportCaptureRequest Request;
    Request.Width = 96;
    Request.Height = 80;
    Request.ProjectionMode = TEXT("perspective");
    Request.Location = Client.GetViewLocation();
    Request.Rotation = Client.GetViewRotation();
    Request.bLocationProvided = true;
    Request.bRotationProvided = true;
    Request.Fov = Client.ViewFOV;
    Request.bAllowBlank = true;

    PinWrightRenderCapture::FViewportCaptureOutput Capture;
    if (!PinWrightRenderCapture::CaptureEditorViewportToPng(
            Client, Acquisition.SceneViewport, Request, TEXT("CaptureRenderResolutionTest"),
            TEXT("PinWrightTests"), Capture, ErrorCode, ErrorMessage))
    {
        AddError(FString::Printf(TEXT("Unexpected Cube preview capture failure (%s: %s)."),
            *ErrorCode, *ErrorMessage));
        return false;
    }
    CapturePath = Capture.Path;

    TestEqual(TEXT("capture restores the previewing flag"),
        Client.IsPreviewingScreenPercentage(), bPreviewingBefore);
    TestEqual(TEXT("capture initially restores the derived preview percentage"),
        Client.GetPreviewScreenPercentage(), PreviewPercentageBefore);

    // This distinguishes an unset optional from an explicit copy of its old effective value. The
    // former follows the changed default; the latter remains stuck at 73.
    PercentageCVar->Set(61, PercentageSetBy);
    TestEqual(TEXT("the preview percentage remains unset and follows its default after capture"),
        Client.GetPreviewScreenPercentage(), 61);

    FImage Decoded;
    if (!TestTrue(TEXT("the captured PNG decodes"),
            FImageUtils::LoadImage(*Capture.Path, Decoded)))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Viewport =
        PinWrightRenderCapture::MakeViewportInfoObject(Capture);
    const TSharedPtr<FJsonObject> Render = Viewport->GetObjectField(TEXT("render"));
    TestEqual(TEXT("reported raster width equals the decoded PNG width"),
        Render->GetIntegerField(TEXT("width")), Decoded.SizeX);
    TestEqual(TEXT("reported raster height equals the decoded PNG height"),
        Render->GetIntegerField(TEXT("height")), Decoded.SizeY);
    TestEqual(TEXT("the draw-time primary fraction is native"),
        Render->GetNumberField(TEXT("primaryResolutionFraction")), 1.0);
    TestEqual(TEXT("the draw-time secondary fraction is native"),
        Render->GetNumberField(TEXT("secondaryResolutionFraction")), 1.0);
    TestFalse(TEXT("a native raster is not reported as upscaled"),
        Render->GetBoolField(TEXT("upscaled")));
    return true;
}
