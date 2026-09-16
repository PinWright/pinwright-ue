// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/ScreenshotUtils.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Scene.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/UserInterfaceSettings.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PinWrightSubsystem.h" // LogPinWrightSubsystem
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "RHI.h" // GIsRHIInitialized, guarding the shared readback flush
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "Slate/SceneViewport.h"
#include "Slate/SGameLayerManager.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UObject/StrongObjectPtr.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWidget.h"
#include "Widgets/SViewport.h"

namespace PinWrightScreenshotUtils
{
namespace
{
    // The stamp a GENERATED screenshot name carries.
    //
    // WHY IT IS NOT JUST A TIMESTAMP. It used to be `%Y%m%d_%H%M%S` alone - one-second
    // resolution and nothing else - while a viewport capture on this machine takes about 60 ms.
    // Two captures issued back to back therefore composed the SAME path, the second
    // FFileHelper::SaveArrayToFile overwrote the first, and BOTH responses returned success with a
    // `path` field: one of them naming a file that by then held the other's pixels. Nothing failed,
    // nothing warned, and every downstream comparison of "shot A vs shot B" silently compared one
    // file with itself. That is how PinWright.render.capture_asset_preview.
    // PinnedCapturesReproduceWithinTolerance measured a same-EV100 difference of 0.000 for weeks -
    // and how, on the one run whose three captures happened to STRADDLE a second boundary, it
    // finally reported 89.134 for both its same-pin and its cross-EV measure: identical because
    // two of the three files were the same file.
    //
    // Milliseconds alone would not close it - two captures can land in one millisecond, and the
    // wall clock can step backwards under NTP or a DST change - so a process-lifetime sequence is
    // appended as well. The sequence is what makes the guarantee unconditional.
    FString GeneratedNameStamp()
    {
        static FThreadSafeCounter Sequence;
        const FDateTime Now = FDateTime::Now();
        return FString::Printf(TEXT("%s_%03d_%04d"),
            *Now.ToString(TEXT("%Y%m%d_%H%M%S")),
            Now.GetMillisecond(),
            Sequence.Increment() % 10000);
    }

    bool ExposureFieldsMatch(
        const FPostProcessSettings& Actual, const FPostProcessSettings& Expected)
    {
        return Actual.bOverride_AutoExposureMethod == Expected.bOverride_AutoExposureMethod
            && Actual.AutoExposureMethod == Expected.AutoExposureMethod
            && Actual.bOverride_AutoExposureApplyPhysicalCameraExposure ==
                Expected.bOverride_AutoExposureApplyPhysicalCameraExposure
            && Actual.AutoExposureApplyPhysicalCameraExposure ==
                Expected.AutoExposureApplyPhysicalCameraExposure
            && Actual.bOverride_CameraISO == Expected.bOverride_CameraISO
            && FMath::IsNearlyEqual(Actual.CameraISO, Expected.CameraISO)
            && Actual.bOverride_CameraShutterSpeed == Expected.bOverride_CameraShutterSpeed
            && FMath::IsNearlyEqual(Actual.CameraShutterSpeed, Expected.CameraShutterSpeed)
            && Actual.bOverride_DepthOfFieldFstop == Expected.bOverride_DepthOfFieldFstop
            && FMath::IsNearlyEqual(Actual.DepthOfFieldFstop, Expected.DepthOfFieldFstop)
            && Actual.bOverride_AutoExposureBias == Expected.bOverride_AutoExposureBias
            && FMath::IsNearlyEqual(Actual.AutoExposureBias, Expected.AutoExposureBias)
            && Actual.bOverride_AutoExposureBiasCurve == Expected.bOverride_AutoExposureBiasCurve
            && Actual.AutoExposureBiasCurve == Expected.AutoExposureBiasCurve;
    }

    class FGameViewportExposureViewExtension final : public FSceneViewExtensionBase
    {
    public:
        FGameViewportExposureViewExtension(const FAutoRegister& AutoRegister,
            FViewport* InViewport, float InEv100)
            : FSceneViewExtensionBase(AutoRegister)
            , Viewport(InViewport)
            , Ev100(InEv100)
        {
        }

        virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override
        {
            if (InViewFamily.RenderTarget == Viewport)
            {
                bObserved = false;
                bAllApplied = true;
                bFamilyApplied = false;
                ViewCount = 0;
                // This is the renderer's authoritative fixed-exposure path. Unlike selecting
                // AEM_Manual alone, it cannot be replaced by r.EyeAdaptation.MethodOverride.
                InViewFamily.ExposureSettings.bFixed = true;
                InViewFamily.ExposureSettings.FixedEV100 = Ev100;
                bFamilyApplied = InViewFamily.ExposureSettings.bFixed
                    && FMath::IsNearlyEqual(InViewFamily.ExposureSettings.FixedEV100, Ev100);
            }
        }

        virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override
        {
            if (InViewFamily.RenderTarget != Viewport)
            {
                return;
            }

            FPostProcessSettings Expected = InView.FinalPostProcessSettings;
            ApplyFixedEv100ToPostProcessSettings(Expected, Ev100);
            ApplyFixedEv100ToPostProcessSettings(InView.FinalPostProcessSettings, Ev100);
            bAllApplied &= ExposureFieldsMatch(InView.FinalPostProcessSettings, Expected);
            bObserved = true;
            ++ViewCount;
        }

        // Pure virtual through UE 5.5 (ISceneViewExtension gained empty defaults in 5.6). The
        // fixed exposure is applied in SetupViewFamily/SetupView; nothing is done at submit time.
        virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

        virtual int32 GetPriority() const override
        {
            // Extensions execute from highest to lowest priority. Run last so camera modifiers,
            // stereo devices, and every other extension have already supplied their final blend.
            return MIN_int32;
        }

        bool WasApplied() const
        {
            return bObserved && bAllApplied && bFamilyApplied && ViewCount > 0;
        }
        int32 GetViewCount() const { return ViewCount; }

    protected:
        virtual bool IsActiveThisFrame_Internal(
            const FSceneViewExtensionContext& Context) const override
        {
            return Context.Viewport == Viewport;
        }

    private:
        FViewport* Viewport = nullptr;
        float Ev100 = 0.0f;
        bool bObserved = false;
        bool bAllApplied = true;
        bool bFamilyApplied = false;
        int32 ViewCount = 0;
    };

    class FScopedGameViewportSize
    {
    public:
        FScopedGameViewportSize(FSceneViewport* InViewport, FIntPoint DesiredSize)
            : Viewport(InViewport)
            , bRequested(DesiredSize != FIntPoint::ZeroValue)
        {
            if (!Viewport)
            {
                return;
            }
            OriginalSize = Viewport->GetSizeXY();
            bWasFixed = Viewport->HasFixedSize();
            if (!bRequested)
            {
                bApplied = true;
                return;
            }
            if (OriginalSize.X <= 0 || OriginalSize.Y <= 0)
            {
                return;
            }
            if (DesiredSize != OriginalSize)
            {
                Viewport->SetFixedViewportSize(DesiredSize.X, DesiredSize.Y);
                bChanged = true;
            }
            bApplied = Viewport->GetSizeXY() == DesiredSize;
        }

        ~FScopedGameViewportSize()
        {
            Restore();
        }

        bool WasApplied() const { return bApplied; }
        FIntPoint GetOriginalSize() const { return OriginalSize; }
        bool WasOriginallyFixed() const { return bWasFixed; }

        bool Restore()
        {
            if (bRestored)
            {
                return bRestoreSucceeded;
            }
            bRestored = true;
            if (!bRequested)
            {
                bRestoreSucceeded = true;
                return true;
            }
            if (!Viewport)
            {
                bRestoreSucceeded = false;
                return false;
            }
            if (bChanged)
            {
                Viewport->SetFixedViewportSize(OriginalSize.X, OriginalSize.Y);
                if (!bWasFixed)
                {
                    Viewport->SetFixedViewportSize(0, 0);
                }
            }
            bRestoreSucceeded = Viewport->GetSizeXY() == OriginalSize
                && Viewport->HasFixedSize() == bWasFixed;
            return bRestoreSucceeded;
        }

    private:
        FSceneViewport* Viewport = nullptr;
        FIntPoint OriginalSize = FIntPoint::ZeroValue;
        bool bRequested = false;
        bool bWasFixed = false;
        bool bChanged = false;
        bool bApplied = false;
        bool bRestored = false;
        bool bRestoreSucceeded = false;
    };
}

    FString MakeScreenshotFilename(FString RequestedFilename, const FString& DefaultPrefix,
        bool* bOutGenerated)
    {
        if (bOutGenerated)
        {
            *bOutGenerated = false;
        }
        const FString EffectivePrefix = DefaultPrefix.IsEmpty() ? TEXT("Screenshot") : DefaultPrefix;
        if (RequestedFilename.IsEmpty())
        {
            RequestedFilename = FString::Printf(TEXT("%s_%s"),
                *EffectivePrefix, *GeneratedNameStamp());
            if (bOutGenerated)
            {
                *bOutGenerated = true;
            }
        }

        RequestedFilename = FPaths::GetCleanFilename(RequestedFilename);
        if (RequestedFilename.Contains(TEXT("..")) ||
            RequestedFilename.Contains(TEXT("/")) ||
            RequestedFilename.Contains(TEXT("\\")))
        {
            RequestedFilename = FString::Printf(TEXT("%s_%s"),
                *EffectivePrefix, *GeneratedNameStamp());
            if (bOutGenerated)
            {
                *bOutGenerated = true;
            }
        }

        if (!RequestedFilename.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase))
        {
            RequestedFilename += TEXT(".png");
        }

        return RequestedFilename;
    }

    FString MakeScreenshotOutputPath(const FString& RequestedFilename, const FString& DefaultPrefix,
        const FString& Subdirectory, FString& OutFilename)
    {
        FString ScreenshotDir = FPaths::ProjectSavedDir() / TEXT("Screenshots");
        if (!Subdirectory.IsEmpty())
        {
            ScreenshotDir /= Subdirectory;
        }

        IFileManager::Get().MakeDirectory(*ScreenshotDir, true);
        bool bGenerated = false;
        OutFilename = MakeScreenshotFilename(RequestedFilename, DefaultPrefix, &bGenerated);
        FString FullPath = ScreenshotDir / OutFilename;
        // A GENERATED name must never land on a file that already exists. The stamp already makes
        // it unique within this process; this closes the two cases the stamp cannot see - a file
        // left by an earlier editor session, and a second editor running against the same
        // checkout (docs/multi-checkout-setup.md). A CALLER-SUPPLIED name is deliberately left
        // alone and still overwrites; the ortho tile grid's `<prefix>_r<row>c<col>.png` depends on
        // that determinism. Disposable pose-list captures instead use generated or GUID names.
        for (int32 Attempt = 0; bGenerated && Attempt < 64
            && IFileManager::Get().FileExists(*FullPath); ++Attempt)
        {
            OutFilename = MakeScreenshotFilename(FString(), DefaultPrefix);
            FullPath = ScreenshotDir / OutFilename;
        }
        return FullPath;
    }

    FString MakeUiScreenshotPath(const FString& RequestedPath, const FString& RequestedFilename,
        FString& OutFilename)
    {
        FString ScreenshotPath = RequestedPath;
        if (ScreenshotPath.IsEmpty())
        {
            ScreenshotPath = FPaths::ProjectSavedDir() / TEXT("Screenshots/WindowsEditor");
        }

        // Filename routes through MakeScreenshotFilename (.png guard + traversal strip,
        // contract in ScreenshotUtils.h); the target directory is the separate RequestedPath.
        OutFilename = MakeScreenshotFilename(RequestedFilename, TEXT(""));

        FString FullPath = FPaths::Combine(ScreenshotPath, OutFilename);
        FPaths::MakeStandardFilename(FullPath);
        return FullPath;
    }

    void ForceOpaqueAlpha(TArray<FColor>& Bitmap)
    {
        for (FColor& Pixel : Bitmap)
        {
            Pixel.A = 0xFF;
        }
    }

    bool IsBlankReadback(const TArray<FColor>& Bitmap)
    {
        // Contract in ScreenshotUtils.h. An empty bitmap is "nothing read", not "blank" —
        // the size guards at the call sites own that case and report it as CAPTURE_FAILED.
        if (Bitmap.Num() == 0)
        {
            return false;
        }
        for (const FColor& Pixel : Bitmap)
        {
            // FColor::ToPackedARGB() folds all four channels into one comparison, so this
            // bails on the first non-zero byte — a real frame exits on an early pixel and
            // only a genuinely all-zero surface pays the full scan.
            if (Pixel.ToPackedARGB() != 0)
            {
                return false;
            }
        }
        return true;
    }

    bool EncodeBitmapToPng(int32 Width, int32 Height, const TArray<FColor>& Bitmap,
        TArray<uint8>& OutPng)
    {
        OutPng.Reset();

        // FImageUtils::PNGCompressImageArray builds an FImageView over Width*Height pixels
        // without validating the source count, so a bitmap smaller than Width*Height would
        // read out of bounds — reject degenerate/mismatched input up front.
        const int64 PixelCount = static_cast<int64>(Width) * static_cast<int64>(Height);
        if (Width <= 0 || Height <= 0 || Bitmap.Num() < PixelCount)
        {
            return false;
        }

        // Delegate to the engine's canonical FColor(BGRA8) -> PNG encoder — the same path
        // the sibling capture verbs use (e.g. PreviewViewportCaptureUtils, the level-viewport
        // branch of editor.screenshot) — so every FColor -> PNG capture shares one
        // implementation instead of a hand-rolled IImageWrapper SetRaw/GetCompressed dance.
        TArray64<uint8> Png64;
        FImageUtils::PNGCompressImageArray(Width, Height,
            TArrayView64<const FColor>(Bitmap.GetData(), Bitmap.Num()), Png64);
        OutPng = Png64;
        return OutPng.Num() > 0;
    }

    void ApplyFixedEv100ToPostProcessSettings(FPostProcessSettings& Settings, float Ev100)
    {
        Settings.bOverride_AutoExposureMethod = true;
        Settings.AutoExposureMethod = AEM_Manual;
        Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
        Settings.AutoExposureApplyPhysicalCameraExposure = true;
        Settings.bOverride_CameraISO = true;
        Settings.CameraISO = 100.0f;
        Settings.bOverride_DepthOfFieldFstop = true;
        Settings.DepthOfFieldFstop = 1.0f;
        Settings.bOverride_CameraShutterSpeed = true;
        Settings.CameraShutterSpeed = FMath::Exp2(Ev100);
        Settings.bOverride_AutoExposureBias = true;
        Settings.AutoExposureBias = 0.0f;
        Settings.bOverride_AutoExposureBiasCurve = true;
        Settings.AutoExposureBiasCurve = nullptr;
    }

    bool RenderSlateWidgetToSrgbColors(const TSharedRef<SWidget>& Widget, FIntPoint DrawSize,
        float DrawScale, TArray<FColor>& OutColorData, FString& OutErrorCode)
    {
        OutColorData.Reset();
        OutErrorCode.Reset();
        if (DrawSize.X <= 0 || DrawSize.Y <= 0 || !FMath::IsFinite(DrawScale)
            || DrawScale <= 0.0f)
        {
            OutErrorCode = TEXT("PREVIEW_ZERO_SIZE");
            return false;
        }

        TSharedPtr<FWidgetRenderer> WidgetRenderer =
            MakeShared<FWidgetRenderer>(/*bUseGammaCorrection=*/false);
        TStrongObjectPtr<UTextureRenderTarget2D> RenderTarget(FWidgetRenderer::CreateTargetFor(
            FVector2D(DrawSize.X, DrawSize.Y), TF_Bilinear, /*bUseGammaCorrection=*/true));
        if (!RenderTarget.IsValid())
        {
            OutErrorCode = TEXT("RT_CREATE_FAILED");
            return false;
        }

        WidgetRenderer->DrawWidget(RenderTarget.Get(), Widget, DrawScale,
            FVector2D(DrawSize.X, DrawSize.Y), /*DeltaTime=*/0.0f,
            /*bDeferRenderTargetUpdate=*/false);
        FlushRenderingCommands();

        FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
        if (!Resource || !Resource->ReadPixels(OutColorData))
        {
            OutColorData.Reset();
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            RenderTarget->ReleaseResource();
            return false;
        }
        RenderTarget->ReleaseResource();
        return true;
    }

    bool CompositePremultipliedSlateLayer(TArray<FColor>& Scene, const TArray<FColor>& Overlay)
    {
        if (Scene.Num() == 0 || Scene.Num() != Overlay.Num())
        {
            return false;
        }

        for (int32 Index = 0; Index < Scene.Num(); ++Index)
        {
            const float Alpha = static_cast<float>(Overlay[Index].A) / 255.0f;
            const FLinearColor SceneLinear = FLinearColor::FromSRGBColor(Scene[Index]);
            const FLinearColor OverlayLinear = FLinearColor::FromSRGBColor(Overlay[Index]);
            const FLinearColor Composite(
                OverlayLinear.R + SceneLinear.R * (1.0f - Alpha),
                OverlayLinear.G + SceneLinear.G * (1.0f - Alpha),
                OverlayLinear.B + SceneLinear.B * (1.0f - Alpha),
                1.0f);
            Scene[Index] = Composite.ToFColorSRGB();
        }
        return true;
    }

    bool CaptureGameViewportToPngFile(const FString& OutPath, int32& OutWidth, int32& OutHeight,
        FString& OutErrorCode, TArray<uint8>* OutPngData,
        const FGameViewportCaptureOptions* Options,
        FGameViewportCaptureMetadata* OutMetadata)
    {
        OutWidth = 0;
        OutHeight = 0;
        OutErrorCode.Reset();
        if (OutPngData)
        {
            OutPngData->Reset();
        }
        if (OutMetadata)
        {
            *OutMetadata = FGameViewportCaptureMetadata();
        }

        if (!GEngine || !GEngine->GameViewport)
        {
            OutErrorCode = TEXT("NO_VIEWPORT");
            return false;
        }
        UGameViewportClient* GameViewportClient = GEngine->GameViewport;
        FViewport* Viewport = GameViewportClient->Viewport;
        if (!Viewport)
        {
            OutErrorCode = TEXT("NO_VIEWPORT");
            return false;
        }

        const FGameViewportCaptureOptions DefaultOptions;
        const FGameViewportCaptureOptions& CaptureOptions = Options ? *Options : DefaultOptions;
        const bool bFixedSizeRequested = CaptureOptions.OutputSize != FIntPoint::ZeroValue;
        const int64 RequestedPixels = static_cast<int64>(CaptureOptions.OutputSize.X)
            * static_cast<int64>(CaptureOptions.OutputSize.Y);
        if ((bFixedSizeRequested && (CaptureOptions.OutputSize.X <= 0 || CaptureOptions.OutputSize.Y <= 0
                || CaptureOptions.OutputSize.X > MaxGameViewportCaptureDimension
                || CaptureOptions.OutputSize.Y > MaxGameViewportCaptureDimension
                || RequestedPixels > MaxGameViewportCapturePixels))
            || (CaptureOptions.bPinExposure && !FMath::IsFinite(CaptureOptions.ExposureEv100)))
        {
            OutErrorCode = TEXT("INVALID_ARGUMENT");
            return false;
        }

        FSceneViewport* SceneViewport = MCP_GAME_SCENE_VIEWPORT(GameViewportClient);
        if (bFixedSizeRequested && !SceneViewport)
        {
            OutErrorCode = ErrorCodes::ERR_FIXED_SIZE_CAPTURE_UNAVAILABLE;
            return false;
        }

        TSharedPtr<FGameViewportExposureViewExtension, ESPMode::ThreadSafe> ExposureScope;
        if (CaptureOptions.bPinExposure)
        {
            ExposureScope = FSceneViewExtensions::NewExtension<FGameViewportExposureViewExtension>(
                Viewport, CaptureOptions.ExposureEv100);
        }

        FScopedGameViewportSize SizeScope(SceneViewport, CaptureOptions.OutputSize);
        if (bFixedSizeRequested && !SizeScope.WasApplied())
        {
            OutErrorCode = ErrorCodes::ERR_FIXED_SIZE_CAPTURE_UNAVAILABLE;
            return false;
        }

        // The shared readback preamble, at the last point before ANY of the three readback
        // branches below. The viewport resize and the exposure extension above are the work whose
        // resources must be settled before a copy is set up against them.
        const bool bReadbackFlushed = FlushBeforeReadback();

        TArray<FColor> Bitmap;
        bool bUsedNativeBackBuffer = false;
        if (bFixedSizeRequested)
        {
            // SetFixedViewportSize has already drawn once. Draw again after the resize notification
            // so the scene view and every resize-aware HUD have observed the requested extent.
            Viewport->Draw();
            if (!Viewport->ReadPixels(Bitmap)
                || Bitmap.Num() < CaptureOptions.OutputSize.X * CaptureOptions.OutputSize.Y)
            {
                OutErrorCode = TEXT("CAPTURE_FAILED");
                return false;
            }
            OutWidth = CaptureOptions.OutputSize.X;
            OutHeight = CaptureOptions.OutputSize.Y;

            if (!FSlateApplication::IsInitialized())
            {
                OutErrorCode = ErrorCodes::ERR_FIXED_SIZE_CAPTURE_UNAVAILABLE;
                return false;
            }
            TSharedPtr<IGameLayerManager> GameLayerManager = GameViewportClient->GetGameLayerManager();
            TSharedPtr<SViewport> ViewportWidget = GameViewportClient->GetGameViewportWidget();
            if (!GameLayerManager.IsValid() || !ViewportWidget.IsValid())
            {
                OutErrorCode = ErrorCodes::ERR_FIXED_SIZE_CAPTURE_UNAVAILABLE;
                return false;
            }

            TArray<FColor> Overlay;
            FString SlateError;
            const float DrawScale = FMath::Max(
                ViewportWidget->GetCachedGeometry().Scale, UE_KINDA_SMALL_NUMBER);
            // SGameLayerManager contains the SViewport as well as the game layers. Hide only
            // that child while the virtual window paints, otherwise the scene is sampled into
            // Overlay and blended over the separate exact-size scene readback a second time.
            const float ViewportOpacity = ViewportWidget->GetRenderOpacity();
            ViewportWidget->SetRenderOpacity(0.0f);
            const bool bRenderedOverlay = RenderSlateWidgetToSrgbColors(
                MCP_GAME_LAYER_MANAGER_WIDGET(GameLayerManager), CaptureOptions.OutputSize,
                DrawScale, Overlay, SlateError);
            ViewportWidget->SetRenderOpacity(ViewportOpacity);
            if (!bRenderedOverlay || !CompositePremultipliedSlateLayer(Bitmap, Overlay))
            {
                OutErrorCode = ErrorCodes::ERR_FIXED_SIZE_CAPTURE_UNAVAILABLE;
                return false;
            }
        }
        else
        {
            if (CaptureOptions.bPinExposure)
            {
                // The native-size path normally reads the last presented back buffer. A scoped
                // exposure request must first draw one frame while its post-process blend is live.
                Viewport->Draw();
            }

            // Capture the live *composited* frame — the 3D scene PLUS the Slate/UMG viewport
            // overlay (HUD widgets added via AddToViewport) — by reading the game-viewport
            // widget's window back buffer through TakeSlateScreenshot.
            if (FSlateApplication::IsInitialized())
            {
                TSharedPtr<SViewport> ViewportWidget = GameViewportClient->GetGameViewportWidget();
                if (ViewportWidget.IsValid())
                {
                    FIntVector ImageSize(0, 0, 0);
                    if (TakeSlateScreenshot(
                            StaticCastSharedRef<SWidget>(ViewportWidget.ToSharedRef()), Bitmap, ImageSize)
                        && ImageSize.X > 0 && ImageSize.Y > 0
                        && Bitmap.Num() >= ImageSize.X * ImageSize.Y)
                    {
                        OutWidth = ImageSize.X;
                        OutHeight = ImageSize.Y;
                        bUsedNativeBackBuffer = true;
                    }
                    else
                    {
                        Bitmap.Reset();
                    }
                }
            }
        }

        // Fallback: scene-only viewport readback (no UMG overlay). Reached when Slate is
        // unavailable or the back-buffer capture failed (e.g. -RenderOffScreen headless
        // capture). Keeps the verb functional — a scene-only frame beats a hard failure.
        if (Bitmap.Num() == 0)
        {
            if (!Viewport->ReadPixels(Bitmap) || Bitmap.Num() == 0)
            {
                OutErrorCode = TEXT("CAPTURE_FAILED");
                return false;
            }
            const FIntPoint Size = Viewport->GetSizeXY();
            OutWidth = Size.X;
            OutHeight = Size.Y;
        }

        const bool bExposureApplied = ExposureScope.IsValid() && ExposureScope->WasApplied();
        const int32 ExposureViewCount = ExposureScope.IsValid()
            ? ExposureScope->GetViewCount()
            : 0;
        // Releasing the only strong reference removes the draw-scoped extension. It changed only
        // the finalized FSceneView settings, so there is no camera or viewport state to restore.
        ExposureScope.Reset();
        const bool bViewportRestored = SizeScope.Restore();
        if (!bViewportRestored)
        {
            OutErrorCode = ErrorCodes::ERR_VIEWPORT_RESTORE_FAILED;
            return false;
        }
        if (CaptureOptions.bPinExposure && !bExposureApplied)
        {
            OutErrorCode = ErrorCodes::ERR_EXPOSURE_PIN_FAILED;
            return false;
        }

        FGameViewportCaptureMetadata Metadata;
        Metadata.OriginalViewportSize = SizeScope.GetOriginalSize();
        Metadata.bViewportWasFixed = SizeScope.WasOriginallyFixed();
        Metadata.bViewportRestored = bViewportRestored;
        Metadata.bUsedOffscreenComposite = bFixedSizeRequested;
        Metadata.bUsedNativeBackBuffer = bUsedNativeBackBuffer;
        Metadata.DpiScale = GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(
            FIntPoint(OutWidth, OutHeight));
        Metadata.bExposureApplied = CaptureOptions.bPinExposure && bExposureApplied;
        Metadata.bExposureRestored = true;
        Metadata.ExposureViewCount = ExposureViewCount;
        Metadata.bReadbackFlushed = bReadbackFlushed;

        // Reject a never-drawn surface BEFORE the alpha stamp below rewrites it into a
        // plausible opaque-black frame. The readback itself is already GPU-coherent
        // (FSlateApplication::TakeScreenshot and FRenderTarget::ReadPixels each enqueue and
        // flush internally), so there is no further readback to force here — what leaked was
        // a *stale* surface: the scene-only fallback reads whatever the game viewport's
        // render target last held, which is all zeros when nothing has ever been presented
        // into it (headless / -RenderOffScreen, a minimized viewport, PIE before its first
        // frame). That produced a valid, correctly sized, entirely black PNG on disk plus a
        // success response — and, for ui.screenshot, the same black frame echoed back as
        // base64. A typed failure is the honest answer; silently shipping an empty frame is
        // the one outcome this path must never produce.
        if (IsBlankReadback(Bitmap))
        {
            OutErrorCode = TEXT("BLANK_CAPTURE");
            return false;
        }

        // Force opaque alpha for BOTH capture paths above, not just the back-buffer one.
        // The window back buffer carries sub-opaque alpha in HUD-transparent regions, and a
        // scene-only FViewport::ReadPixels carries alpha 0 over every scene pixel — the exact
        // shape of B-horizontal-orthographic-views-render-no-geometry, where a PNG that was
        // 99.97% transparent read as a blank frame in an alpha-compositing viewer while the
        // geometry sat untouched in the RGB planes. The fallback is the headless /
        // -RenderOffScreen path, so leaving it unstamped meant the transparency bug survived
        // exactly where it is least likely to be eyeballed. A screen capture is an opaque
        // frame either way, so there is nothing to preserve by stamping conditionally.
        ForceOpaqueAlpha(Bitmap);

        // Always PNG-encode. These verbs are contractually PNG (the .png OutPath, the
        // editor.screenshot "Capture a PNG screenshot" summary, and ui.screenshot's
        // mimeType:image/png), so route through EncodeBitmapToPng rather than
        // FImageUtils::ThumbnailCompressImageArray, which emitted JPEG into a var named
        // PngData while the genuine PNG encode sat in a dead PngData.Num()==0 fallback that
        // never ran for a valid bitmap (B-viewport-screenshot-writes-jpeg).
        TArray<uint8> PngData;
        if (!EncodeBitmapToPng(OutWidth, OutHeight, Bitmap, PngData))
        {
            OutErrorCode = TEXT("CAPTURE_FAILED");
            return false;
        }

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);
        if (!FFileHelper::SaveArrayToFile(PngData, *OutPath))
        {
            OutErrorCode = TEXT("WRITE_FAILED");
            return false;
        }

        if (OutPngData)
        {
            *OutPngData = MoveTemp(PngData);
        }
        if (OutMetadata)
        {
            *OutMetadata = Metadata;
        }
        return true;
    }

    namespace
    {
        // THE one address this plugin ever hands FSlateRenderer::PrepareToTakeScreenshot. Static
        // storage duration is the point: if a readback ever escapes to a later Slate frame despite
        // the disarm below, it writes into live memory instead of a returned stack frame.
        TArray<FColor>& GetSlateScreenshotDestination()
        {
            static TArray<FColor> Destination;
            return Destination;
        }

        // Game-thread only, like every path that touches ScreenshotState.
        bool bSlateScreenshotRequestPending = false;

        // Point the renderer's screenshot state at a window nothing can match. FSlateViewportInfo
        // pointers in WindowsToRender are never null, so a null capture window makes the state
        // permanently unmatchable - inert, and fully overwritten by the next real request, which
        // sets all four of its fields. FSlateApplication::TakeScreenshot / TakeHDRScreenshot are
        // the engine's only other callers and both arm and draw within one call, so any state
        // still armed when we get here is a leak, never someone else's live request.
        //
        // UE 5.5 AND LATER ONLY, and the guard is not a portability nicety. Through 5.4 the
        // renderer exposes no disarm at all: FSlateRHIRenderer::PrepareToTakeScreenshot resolves
        // the window with `*WindowToViewportInfo.Find(InScreenshotWindow)` (SlateRHIRenderer.cpp
        // :1658 on 5.4), which DEREFERENCES NULL for the unmatchable window this re-arm is built
        // on, and it raises bTakingAScreenShot unconditionally - so every argument that survives
        // the call arms the renderer instead of disarming it. What keeps a leaked request harmless
        // there is the other half of this design: the destination buffer has process lifetime, so
        // a readback that escapes to a later Slate frame writes into live memory rather than a
        // dead stack frame. 5.5 replaced the Find() with FindRef() and the loose flags with a
        // ScreenshotState struct, which is what makes the null-window re-arm safe and inert.
        void DisarmSlateScreenshotState()
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            if (FSlateApplication::IsInitialized())
            {
                if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
                {
                    Renderer->PrepareToTakeScreenshot(
                        FIntRect(), &GetSlateScreenshotDestination(), nullptr);
                }
            }
#endif
            bSlateScreenshotRequestPending = false;
        }
    }

    bool FlushBeforeReadback()
    {
        // Game thread only: FlushRenderingCommands blocks the calling thread on the render thread,
        // which is legal from the game thread and a deadlock from anywhere else.
        if (!IsInGameThread() || !GIsRHIInitialized)
        {
            return false;
        }
        // Drains the render command queue AND, through
        // ImmediateFlush(FlushRHIThreadFlushResources), the RHI thread and its pending resource
        // deletions -- so resources the preceding draws created, resized or released are settled
        // before the readback's copy is set up. Contract and its limits: ScreenshotUtils.h.
        FlushRenderingCommands();
        return true;
    }

    bool HasPendingSlateScreenshotRequest()
    {
        return bSlateScreenshotRequestPending;
    }

    int32 SlateScreenshotDestinationPixelCount()
    {
        return GetSlateScreenshotDestination().Num();
    }

    bool TakeSlateScreenshot(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutColorData,
        FIntVector& OutSize)
    {
        OutColorData.Reset();
        OutSize = FIntVector(0, 0, 0);

        if (!FSlateApplication::IsInitialized())
        {
            return false;
        }

        TArray<FColor>& Destination = GetSlateScreenshotDestination();
        Destination.Reset();

        bSlateScreenshotRequestPending = true;
        const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Widget, Destination, OutSize);
        // Unconditional, and before anything else can return: the engine clears its own state only
        // when the target window was in the pass, and reports no way to tell whether it was. Making
        // the disarm structural rather than conditional is what turns "no pending state on return"
        // into a property of this function instead of a property of the window's luck.
        DisarmSlateScreenshotState();

        const int64 ExpectedPixels = static_cast<int64>(OutSize.X) * static_cast<int64>(OutSize.Y);
        if (!bCaptured || OutSize.X <= 0 || OutSize.Y <= 0 || Destination.Num() < ExpectedPixels)
        {
            // The readback did not run in this call, so the window was not drawn in the pass Slate
            // just performed. Worth a line: the crash this guards against leaves no trace of its
            // own, and this is the only moment at which the condition is observable. Log, NOT
            // Warning: a UE_LOG warning raised while an automation test is running is elevated to
            // a test error (see Tests/TestSkipReporting.h), and this branch is exactly what the
            // regression test drives.
            UE_LOG(LogPinWrightSubsystem, Log,
                TEXT("Slate screenshot did not complete in-call (rect %dx%d, %d pixels read); "
                     "pending renderer state disarmed."),
                OutSize.X, OutSize.Y, Destination.Num());
            Destination.Empty();
            return false;
        }

        OutColorData = MoveTemp(Destination);
        Destination.Empty();
        return true;
    }
}
