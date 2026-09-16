// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Scene.h"
#include "Engine/UserInterfaceSettings.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SceneViewport.h"
#include "Slate/SGameLayerManager.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SViewport.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotFixedEv100PostProcessTest,
    "PinWright.editor.screenshot.Exposure.FixedEv100PostProcess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotFixedEv100PostProcessTest::RunTest(const FString& Parameters)
{
    FPostProcessSettings Settings;
    Settings.AutoExposureBias = 7.0f;
    Settings.AutoExposureBiasCurve = NewObject<UCurveFloat>();

    constexpr float RequestedEv100 = -1.75f;
    PinWrightScreenshotUtils::ApplyFixedEv100ToPostProcessSettings(Settings, RequestedEv100);

    TestTrue(TEXT("manual exposure method is overridden"), Settings.bOverride_AutoExposureMethod);
    TestEqual(TEXT("manual exposure method is selected"), Settings.AutoExposureMethod,
        TEnumAsByte<EAutoExposureMethod>(AEM_Manual));
    TestTrue(TEXT("physical camera exposure is overridden"),
        Settings.bOverride_AutoExposureApplyPhysicalCameraExposure);
    TestTrue(TEXT("physical camera exposure is enabled"),
        Settings.AutoExposureApplyPhysicalCameraExposure != 0);
    TestTrue(TEXT("ISO is overridden"), Settings.bOverride_CameraISO);
    TestEqual(TEXT("ISO is fixed at 100"), Settings.CameraISO, 100.0f);
    TestTrue(TEXT("aperture is overridden"), Settings.bOverride_DepthOfFieldFstop);
    TestEqual(TEXT("aperture is fixed at f/1"), Settings.DepthOfFieldFstop, 1.0f);
    TestTrue(TEXT("shutter speed is overridden"), Settings.bOverride_CameraShutterSpeed);
    TestTrue(TEXT("exposure compensation is overridden"), Settings.bOverride_AutoExposureBias);
    TestEqual(TEXT("exposure compensation is neutral"), Settings.AutoExposureBias, 0.0f);
    TestTrue(TEXT("bias curve is overridden"), Settings.bOverride_AutoExposureBiasCurve);
    TestNull(TEXT("bias curve cannot move the fixed exposure"), Settings.AutoExposureBiasCurve.Get());

    const float AppliedEv100 = FMath::Log2(
        FMath::Square(Settings.DepthOfFieldFstop) * Settings.CameraShutterSpeed
        * 100.0f / Settings.CameraISO);
    TestTrue(TEXT("physical camera fields encode the requested EV100"),
        FMath::IsNearlyEqual(AppliedEv100, RequestedEv100, UE_KINDA_SMALL_NUMBER));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotRejectsPartialFixedSizeTest,
    "PinWright.editor.screenshot.FixedSize.RejectsPartialDimensions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotRejectsPartialFixedSizeTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 1920);

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.screenshot handler is registered"),
        InvokeHandlerWithCapture(TEXT("editor.screenshot"), Payload, Capture));
    TestTrue(TEXT("handler returned a response"), Capture.bWasCalled);
    TestFalse(TEXT("one dimension is rejected"), Capture.bSuccess);
    TestEqual(TEXT("partial size uses the typed argument error"), Capture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotFixedSizeContractTest,
    "PinWright.editor.screenshot.FixedSize.Contract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotFixedSizeContractTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    TestTrue(TEXT("PinWright plugin resolves"), Plugin.IsValid());
    if (!Plugin.IsValid())
    {
        return false;
    }

    const FString ViewportHandlerPath = FPaths::Combine(Plugin->GetBaseDir(),
        TEXT("Source/PinWright/Private/Handlers/Editor/ViewportHandler.cpp"));
    const FString ScreenshotUtilsPath = FPaths::Combine(Plugin->GetBaseDir(),
        TEXT("Source/PinWright/Private/Utils/ScreenshotUtils.cpp"));
    FString ViewportHandlerSource;
    FString ScreenshotUtilsSource;
    TestTrue(TEXT("ViewportHandler.cpp loads"),
        FFileHelper::LoadFileToString(ViewportHandlerSource, *ViewportHandlerPath));
    TestTrue(TEXT("ScreenshotUtils.cpp loads"),
        FFileHelper::LoadFileToString(ScreenshotUtilsSource, *ScreenshotUtilsPath));
    if (ViewportHandlerSource.IsEmpty() || ScreenshotUtilsSource.IsEmpty())
    {
        return false;
    }

    ViewportHandlerSource = NeutralizeSourceText(ViewportHandlerSource);
    ScreenshotUtilsSource = NeutralizeSourceText(ScreenshotUtilsSource);
    TestTrue(TEXT("schema declares paired fixed-size inputs"),
        ViewportHandlerSource.Contains(TEXT("RPC_PARAM_OPT(\"width\", \"number\""))
        && ViewportHandlerSource.Contains(TEXT("RPC_PARAM_OPT(\"height\", \"number\"")));
    TestTrue(TEXT("fixed capture resizes the scene render target"),
        ScreenshotUtilsSource.Contains(TEXT("SetFixedViewportSize(DesiredSize.X, DesiredSize.Y)")));
    TestTrue(TEXT("fixed capture renders the game layers off screen"),
        ScreenshotUtilsSource.Contains(TEXT("GetGameLayerManager()"))
        && ScreenshotUtilsSource.Contains(TEXT("RenderSlateWidgetToSrgbColors(")));
    TestTrue(TEXT("the UI pass excludes the already-read scene"),
        ScreenshotUtilsSource.Contains(TEXT("ViewportWidget->SetRenderOpacity(0.0f)")));
    TestTrue(TEXT("native and scene-only capture modes are distinguishable"),
        ScreenshotUtilsSource.Contains(TEXT("bUsedNativeBackBuffer"))
        && ViewportHandlerSource.Contains(TEXT("sceneOnlyFallback")));
    TestTrue(TEXT("fixed exposure reaches the authoritative family and finalized view"),
        ScreenshotUtilsSource.Contains(TEXT("InViewFamily.ExposureSettings.bFixed = true"))
        && ScreenshotUtilsSource.Contains(TEXT("InView.FinalPostProcessSettings"))
        && ScreenshotUtilsSource.Contains(TEXT("return MIN_int32")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotFixedSizePieUmgCompositeTest,
    "PinWright.editor.screenshot.FixedSize.PieUmgComposite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotFixedSizePieUmgCompositeTest::RunTest(const FString& Parameters)
{
    if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-game-viewport"),
            TEXT("Skipped: no game/PIE viewport is bound."));
        return true;
    }

    UGameViewportClient* GameViewportClient = GEngine->GameViewport;
    FSceneViewport* SceneViewport = MCP_GAME_SCENE_VIEWPORT(GameViewportClient);
    if (!FSlateApplication::IsInitialized()
        || !SceneViewport || !GameViewportClient->GetGameLayerManager().IsValid()
        || !GameViewportClient->GetGameViewportWidget().IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-pie-slate-surface"),
            TEXT("Skipped: the game viewport has no SceneViewport/game layer manager."));
        return true;
    }

    UWorld* ViewportWorld = SceneViewport->GetClient()
        ? SceneViewport->GetClient()->GetWorld()
        : nullptr;
    if (!ViewportWorld || ViewportWorld->WorldType != EWorldType::PIE)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("viewport-world-not-pie"),
            TEXT("Skipped: the bound game viewport does not render a PIE world."));
        return true;
    }

    const FIntPoint OriginalSize = SceneViewport->GetSizeXY();
    const bool bOriginalFixed = SceneViewport->HasFixedSize();
    const TSharedPtr<SViewport> ViewportWidget = GameViewportClient->GetGameViewportWidget();
    const float OriginalViewportOpacity = ViewportWidget.IsValid()
        ? ViewportWidget->GetRenderOpacity()
        : 1.0f;
    if (OriginalSize.X <= 0 || OriginalSize.Y <= 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("zero-sized-game-viewport"),
            TEXT("Skipped: the bound game viewport has no drawable extent."));
        return true;
    }

    constexpr int32 CaptureWidth = 320;
    constexpr int32 CaptureHeight = 180;
    constexpr float MarkerLogicalWidth = 64.0f;
    constexpr float MarkerLogicalHeight = 32.0f;
    const FLinearColor BackgroundColor(0.25f, 0.25f, 0.25f, 0.5f);
    TSharedRef<SOverlay> TestOverlay = SNew(SOverlay)
        + SOverlay::Slot()
        [
            SNew(SColorBlock)
            .Color(BackgroundColor)
        ]
        + SOverlay::Slot()
        .HAlign(HAlign_Left)
        .VAlign(VAlign_Top)
        [
            SNew(SBox)
            .WidthOverride(MarkerLogicalWidth)
            .HeightOverride(MarkerLogicalHeight)
            [
                SNew(SColorBlock)
                .Color(FLinearColor::Green)
            ]
        ];
    GameViewportClient->AddViewportWidgetContent(TestOverlay, 99999);
    ON_SCOPE_EXIT
    {
        GameViewportClient->RemoveViewportWidgetContent(TestOverlay);
    };

    const FString Filename = FString::Printf(TEXT("fixed_pie_%s.png"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("filename"), Filename);
    Payload->SetNumberField(TEXT("width"), CaptureWidth);
    Payload->SetNumberField(TEXT("height"), CaptureHeight);
    Payload->SetNumberField(TEXT("exposure"), -1.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.screenshot handler is registered"),
        InvokeHandlerWithCapture(TEXT("editor.screenshot"), Payload, Capture));
    TestTrue(TEXT("tracked-job envelope succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    FString TicketId;
    TestTrue(TEXT("tracked-job envelope carries ticket_id"),
        Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId));
    FJobTicket Ticket;
    TestTrue(TEXT("screenshot ticket exists"),
        FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket));
    TestEqual(FString::Printf(TEXT("fixed-size capture completes (%s)"), *Ticket.Error),
        Ticket.Status, FString(TEXT("completed")));
    if (Ticket.Status != TEXT("completed") || !Ticket.Result.IsValid())
    {
        return false;
    }

    FString Path;
    Ticket.Result->TryGetStringField(TEXT("path"), Path);
    ON_SCOPE_EXIT
    {
        if (!Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    };

    TestEqual(TEXT("requested width is rendered"),
        static_cast<int32>(Ticket.Result->GetNumberField(TEXT("width"))), CaptureWidth);
    TestEqual(TEXT("requested height is rendered"),
        static_cast<int32>(Ticket.Result->GetNumberField(TEXT("height"))), CaptureHeight);
    TestTrue(TEXT("fixed-size mode is reported"),
        Ticket.Result->GetBoolField(TEXT("fixedSize")));
    TestEqual(TEXT("off-screen scene plus UMG mode is reported"),
        Ticket.Result->GetStringField(TEXT("captureMode")),
        FString(TEXT("fixedSizeScenePlusUmg")));
    TestTrue(TEXT("viewport restoration is reported"),
        Ticket.Result->GetBoolField(TEXT("viewportRestored")));
    TestEqual(TEXT("viewport size is restored before the job completes"),
        SceneViewport->GetSizeXY(), OriginalSize);
    TestEqual(TEXT("viewport fixed-size state is restored before the job completes"),
        SceneViewport->HasFixedSize(), bOriginalFixed);
    TestTrue(TEXT("scene widget opacity is restored after the isolated UMG pass"),
        ViewportWidget.IsValid()
            && FMath::IsNearlyEqual(ViewportWidget->GetRenderOpacity(), OriginalViewportOpacity));

    const float ExpectedDpi = GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(
        FIntPoint(CaptureWidth, CaptureHeight));
    TestTrue(TEXT("response reports the UIScaleCurve value for the requested size"),
        FMath::IsNearlyEqual(
            static_cast<float>(Ticket.Result->GetNumberField(TEXT("dpiScale"))),
            ExpectedDpi, UE_KINDA_SMALL_NUMBER));

    const TSharedPtr<FJsonObject>* Exposure = nullptr;
    TestTrue(TEXT("exposure result is present"),
        Ticket.Result->TryGetObjectField(TEXT("exposure"), Exposure) && Exposure && Exposure->IsValid());
    if (Exposure && Exposure->IsValid())
    {
        TestEqual(TEXT("fixed exposure mode is reported"),
            (*Exposure)->GetStringField(TEXT("mode")), FString(TEXT("fixed")));
        TestTrue(TEXT("fixed exposure reached a local view"),
            (*Exposure)->GetBoolField(TEXT("pinned")));
        TestTrue(TEXT("the draw-scoped exposure override was released"),
            (*Exposure)->GetBoolField(TEXT("restored")));
    }

    FImage Loaded;
    const bool bLoaded = !Path.IsEmpty() && FImageUtils::LoadImage(*Path, Loaded);
    TestTrue(TEXT("fixed-size PNG loads"), bLoaded);
    if (!bLoaded)
    {
        return false;
    }
    Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    const TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
    TestEqual(TEXT("PNG pixel count matches requested output"),
        Pixels.Num(), static_cast<int64>(CaptureWidth * CaptureHeight));
    if (Pixels.Num() != CaptureWidth * CaptureHeight)
    {
        return false;
    }

    // The marker is top-left. Measure the lower half so the green proof pixel cannot make an
    // otherwise all-black scene look spatially varied to FlatRegionStats.
    const int32 SceneProbeStartY = CaptureHeight / 2;
    const int32 SceneProbeHeight = CaptureHeight - SceneProbeStartY;
    const PinWrightFlatRegion::FFlatRegionStats Flat =
        PinWrightFlatRegion::MeasureLargestFlatRegion(
            TConstArrayView<FColor>(
                Pixels.GetData() + SceneProbeStartY * CaptureWidth,
                SceneProbeHeight * CaptureWidth),
            CaptureWidth, SceneProbeHeight);
    TestTrue(TEXT("marker-free scene flat-region measurement ran"), Flat.bMeasured);

    int32 MarkerMinX = CaptureWidth;
    int32 MarkerMinY = CaptureHeight;
    int32 MarkerMaxX = INDEX_NONE;
    int32 MarkerMaxY = INDEX_NONE;
    int32 MarkerPixelCount = 0;
    int32 SceneRegionPixelCount = 0;
    int32 BlackSceneRegionPixels = 0;
    int32 FailureColorSceneRegionPixels = 0;
    const FColor FailureColor = FLinearColor(
        BackgroundColor.R * BackgroundColor.A,
        BackgroundColor.G * BackgroundColor.A,
        BackgroundColor.B * BackgroundColor.A,
        1.0f).ToFColorSRGB();
    for (int32 Y = 0; Y < CaptureHeight; ++Y)
    {
        for (int32 X = 0; X < CaptureWidth; ++X)
        {
            const FColor& Pixel = Pixels[static_cast<int64>(Y) * CaptureWidth + X];
            if (Pixel.G >= 240 && Pixel.R <= 8 && Pixel.B <= 8)
            {
                MarkerMinX = FMath::Min(MarkerMinX, X);
                MarkerMinY = FMath::Min(MarkerMinY, Y);
                MarkerMaxX = FMath::Max(MarkerMaxX, X);
                MarkerMaxY = FMath::Max(MarkerMaxY, Y);
                ++MarkerPixelCount;
            }
            else
            {
                ++SceneRegionPixelCount;
                if (Pixel.R <= 2 && Pixel.G <= 2 && Pixel.B <= 2)
                {
                    ++BlackSceneRegionPixels;
                }
                constexpr int32 FailureColorTolerance = 8;
                if (FMath::Abs(static_cast<int32>(Pixel.R) - FailureColor.R)
                        <= FailureColorTolerance
                    && FMath::Abs(static_cast<int32>(Pixel.G) - FailureColor.G)
                        <= FailureColorTolerance
                    && FMath::Abs(static_cast<int32>(Pixel.B) - FailureColor.B)
                        <= FailureColorTolerance)
                {
                    ++FailureColorSceneRegionPixels;
                }
            }
        }
    }

    TestTrue(TEXT("the injected bright UMG marker appears in the captured pixels"),
        MarkerPixelCount > 0);
    TestTrue(TEXT("the scene region is not uniformly black"),
        Flat.bMeasured
            && !(Flat.LargestRegionFraction >= 0.99 && Flat.LargestRegionLevel <= 2)
            && SceneRegionPixelCount > 0
            && BlackSceneRegionPixels < SceneRegionPixelCount * 99 / 100);
    TestTrue(TEXT("the scene survives the translucent known-colour fixture"),
        SceneRegionPixelCount > 0
            && FailureColorSceneRegionPixels < SceneRegionPixelCount * 99 / 100);
    if (MarkerPixelCount > 0)
    {
        const int32 MarkerPixelWidth = MarkerMaxX - MarkerMinX + 1;
        const int32 MarkerPixelHeight = MarkerMaxY - MarkerMinY + 1;
        const int32 ExpectedMarkerWidth = FMath::RoundToInt(MarkerLogicalWidth * ExpectedDpi);
        const int32 ExpectedMarkerHeight = FMath::RoundToInt(MarkerLogicalHeight * ExpectedDpi);
        constexpr int32 RasterTolerancePixels = 2;
        TestTrue(*FString::Printf(TEXT("UMG marker width follows DPI (expected %d, got %d)"),
                ExpectedMarkerWidth, MarkerPixelWidth),
            FMath::Abs(MarkerPixelWidth - ExpectedMarkerWidth) <= RasterTolerancePixels);
        TestTrue(*FString::Printf(TEXT("UMG marker height follows DPI (expected %d, got %d)"),
                ExpectedMarkerHeight, MarkerPixelHeight),
            FMath::Abs(MarkerPixelHeight - ExpectedMarkerHeight) <= RasterTolerancePixels);
    }
    return true;
}
