// Copyright (c) 2026 Alexander Penkin. MIT License.

// widget.screenshot_designer target:"preview" -- cold frames and Designer chrome.
//
// B-screenshot-designer-cold-capture-missing-images-and-text: the first preview capture of a
// widget drew its textures while they were still compiling (the engine's grey checker
// placeholder, or nothing) and its gradient text while the font material had no game-thread
// shader map (nothing), with a success payload identical to a finished frame's. The capture
// now collects every texture and material the preview tree references and waits for them,
// bounded, and publishes previewComplete + readiness.notReady for whatever is still pending.
//
// B-screenshot-designer-preview-draws-dashed-outlines: the preview's own Slate tree carries the
// Designer's dashed per-panel outline borders. The capture now collapses them for the draw.
//
// Counterfactuals:
//   * Delete the PWDesignerCaptureWaitForRenderAssets call in CapturePreviewToPng and
//     ColdTextureIsDrawnFinished fails: the frame shows the placeholder/previous texture, not
//     red, and bPreviewComplete stays at its default false.
//   * Hardcode previewComplete:true in the handler (or stop recording what is pending) and
//     ExpiredReadinessBudgetReportsIncomplete fails: a zero budget draws a still-compiling
//     texture and the response must name it.
//   * Stop collecting FSlateFontInfo::FontMaterial (the ticket's missing gradient text) and
//     ReadinessCollectsBrushTexturesAndFontMaterials fails.
//   * Delete FPWScopedHideDesignerOutlines and PreviewOmitsDesignerDashedOutlines fails: the
//     outline draws over the solid fill at the canvas edge, and the hidden count reads 0.
//
// The cold state is created by re-sourcing a texture the widget already references and calling
// PostEditChange, which queues an async rebuild that only the game thread can finalize. A host
// with async texture compilation disabled rebuilds synchronously, so there is no cold state to
// observe; the two cold tests emit PINWRIGHT_ASSERTIONS_SKIPPED there rather than pass hollow.

#include "Misc/AutomationTest.h"

#include "Handlers/UI/WidgetDesignerCaptureInternal.h"
#include "Handlers/UI/WidgetDesignerCaptureUtil.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetPreviewAlphaFixtures.h"
#include "Tests/Widget/WidgetTestFixtures.h"

#include "AssetCompilingManager.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Settings/WidgetDesignerSettings.h"
#include "Styling/SlateBrush.h"
#include "UObject/Package.h"

namespace
{
    // 200x100 design size captured at max_size 200: one design unit per pixel, so a full-canvas
    // fill has no fractional edge and every pixel of it is the fill colour.
    constexpr double PWWdccDesignWidth = 200.0;
    constexpr double PWWdccDesignHeight = 100.0;
    constexpr int32 PWWdccMaxSize = 200;
    constexpr int32 PWWdccTolerance = 8;

    void PWWdccFillSource(UTexture2D* Texture, const FColor& Color)
    {
        TArray<uint8> Bytes;
        for (int32 Index = 0; Index < 8 * 8; ++Index)
        {
            Bytes.Add(Color.B);
            Bytes.Add(Color.G);
            Bytes.Add(Color.R);
            Bytes.Add(Color.A);
        }
        Texture->Source.Init(8, 8, 1, 1, TSF_BGRA8, Bytes.GetData());
    }

    // A warm, uncompressed UI texture. Transient: nothing here is ever saved.
    UTexture2D* PWWdccMakeWarmTexture(const FColor& Color)
    {
        UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("PWColdCaptureTexture")),
            RF_Transient);
        Texture->CompressionSettings = TC_EditorIcon;
        Texture->MipGenSettings = TMGS_NoMipmaps;
        Texture->LODGroup = TEXTUREGROUP_UI;
        Texture->SRGB = true;
        PWWdccFillSource(Texture, Color);
        Texture->PostEditChange();
        TArray<UObject*> Settle{ Texture };
        FAssetCompilingManager::Get().FinishCompilationForObjects(Settle);
        return Texture;
    }

    // Re-source the texture and queue its rebuild: until the game thread finalizes it, a draw
    // samples the placeholder (or the previous resource), never the new colour.
    bool PWWdccMakeCold(UTexture2D* Texture, const FColor& Color)
    {
        PWWdccFillSource(Texture, Color);
        Texture->PostEditChange();
        return Texture->IsCompiling();
    }

    // Root canvas with an image filling it (brush = Texture) and a small text label top-left,
    // the ticket's two missing content kinds. The sampled region below avoids the label.
    UWidgetBlueprint* PWWdccMakeWidget(const FString& WidgetPath, UTexture2D* Texture)
    {
        UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
        UCanvasPanel* Root = WBP ? Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget) : nullptr;
        if (!Root)
        {
            return WBP;
        }

        UImage* Image = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ColdImage"));
        FSlateBrush Brush;
        Brush.SetResourceObject(Texture);
        Brush.ImageSize = FVector2D(8.0, 8.0);
        Image->SetBrush(Brush);
        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Root->AddChild(Image)))
        {
            CanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
            CanvasSlot->SetOffsets(FMargin(0.0f));
        }
        WidgetTestFixtures::RegisterWidgetVariable(WBP, Image->GetFName());

        UTextBlock* Label = WidgetTestFixtures::AddTextBlockToPanel(WBP, Root, TEXT("ColdLabel"));
        if (Label)
        {
            Label->SetText(FText::FromString(TEXT("COLD")));
            if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Label->Slot))
            {
                CanvasSlot->SetPosition(FVector2D(0.0, 0.0));
                CanvasSlot->SetSize(FVector2D(80.0, 30.0));
            }
        }

        FKismetEditorUtilities::CompileBlueprint(WBP);
        WidgetPreviewAlphaFixtures::PinDesignerPreviewSize(WBP, FVector2D(PWWdccDesignWidth, PWWdccDesignHeight));
        return WBP;
    }

    bool PWWdccDecodePng(const TArray<uint8>& Png, TArray64<uint8>& OutBgra)
    {
        IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
        TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
        return Wrapper.IsValid()
            && Wrapper->SetCompressed(Png.GetData(), Png.Num())
            && Wrapper->GetRaw(ERGBFormat::BGRA, 8, OutBgra);
    }

    bool PWWdccNear(const uint8* Bgra, const FColor& Expected)
    {
        return FMath::Abs(int32(Bgra[0]) - int32(Expected.B)) <= PWWdccTolerance
            && FMath::Abs(int32(Bgra[1]) - int32(Expected.G)) <= PWWdccTolerance
            && FMath::Abs(int32(Bgra[2]) - int32(Expected.R)) <= PWWdccTolerance;
    }

    // Pixels in [X0,X1) x [Y0,Y1) of a Width-wide BGRA image within tolerance of Expected.
    int32 PWWdccCountNear(const TArray64<uint8>& Bgra, int32 Width, int32 X0, int32 Y0, int32 X1,
        int32 Y1, const FColor& Expected)
    {
        int32 Count = 0;
        for (int32 Y = Y0; Y < Y1; ++Y)
        {
            for (int32 X = X0; X < X1; ++X)
            {
                const int64 Offset = (int64(Y) * Width + X) * 4;
                if (Offset + 3 < Bgra.Num() && PWWdccNear(&Bgra[Offset], Expected))
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    void PWWdccSettle(UTexture2D* Texture)
    {
        if (Texture)
        {
            TArray<UObject*> Settle{ Texture };
            FAssetCompilingManager::Get().FinishCompilationForObjects(Settle);
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerColdTextureIsDrawnFinishedTest,
    "PinWright.widget.screenshot_designer.ColdTextureIsDrawnFinished",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerColdTextureIsDrawnFinishedTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }

    const FColor Warm(0, 0, 255);
    const FColor Cold(255, 0, 0);
    UTexture2D* Texture = PWWdccMakeWarmTexture(Warm);
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_ColdCaptureTexture"));
    UWidgetBlueprint* WBP = PWWdccMakeWidget(WidgetPath, Texture);
    ON_SCOPE_EXIT
    {
        PWWdccSettle(Texture);
        WidgetPreviewAlphaFixtures::CloseAndCleanupWidget(WBP, WidgetPath);
    };
    if (!TestNotNull(TEXT("widget blueprint built"), WBP))
    {
        return false;
    }

    if (!PWWdccMakeCold(Texture, Cold))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("texture-compiled-synchronously"),
            TEXT("PostEditChange rebuilt the texture synchronously (async texture compilation is off on this host), so there was no cold texture for the capture to wait on."));
        return true;
    }

    TArray<uint8> Png;
    FString Error;
    WidgetDesignerCaptureUtil::FCaptureInfo Info;
    if (!TestTrue(FString::Printf(TEXT("preview capture succeeds (error='%s')"), *Error),
        WidgetDesignerCaptureUtil::CapturePreviewToPng(WBP, PWWdccMaxSize, Png, Error, &Info)))
    {
        return false;
    }

    TestTrue(FString::Printf(TEXT("readiness collected the brush texture (textures=%d)"), Info.ReadinessTextures),
        Info.ReadinessTextures >= 1);
    TestTrue(FString::Printf(TEXT("the frame is reported complete (notReady=[%s])"),
        *FString::Join(Info.NotReadyAssets, TEXT(", "))), Info.bPreviewComplete);
    TestFalse(TEXT("the texture finished compiling before the draw"), Texture->IsCompiling());

    TArray64<uint8> Bgra;
    if (!TestTrue(TEXT("preview PNG decodes"), PWWdccDecodePng(Png, Bgra))
        || !TestEqual(TEXT("capture is 200 px wide"), Info.Width, PWWdccMaxSize))
    {
        return false;
    }
    // Bottom-right block, clear of the text label.
    const int32 Sampled = 70 * 40;
    const int32 Red = PWWdccCountNear(Bgra, Info.Width, 120, 50, 190, 90, Cold);
    TestTrue(FString::Printf(
        TEXT("the image draws the re-sourced texture, not the placeholder or the previous colour (%d of %d sampled pixels are red)"),
        Red, Sampled), Red >= Sampled * 9 / 10);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerExpiredReadinessBudgetReportsIncompleteTest,
    "PinWright.widget.screenshot_designer.ExpiredReadinessBudgetReportsIncomplete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerExpiredReadinessBudgetReportsIncompleteTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }

    UTexture2D* Texture = PWWdccMakeWarmTexture(FColor(0, 0, 255));
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_ColdCaptureBudget"));
    UWidgetBlueprint* WBP = PWWdccMakeWidget(WidgetPath, Texture);
    TArray<FString> ScreenshotPaths;
    const double SavedBudget = WidgetDesignerCaptureInternal::ReadinessBudgetSeconds();
    ON_SCOPE_EXIT
    {
        WidgetDesignerCaptureInternal::ReadinessBudgetSeconds() = SavedBudget;
        for (const FString& Path : ScreenshotPaths)
        {
            IFileManager::Get().Delete(*Path);
        }
        PWWdccSettle(Texture);
        WidgetPreviewAlphaFixtures::CloseAndCleanupWidget(WBP, WidgetPath);
    };
    if (!TestNotNull(TEXT("widget blueprint built"), WBP))
    {
        return false;
    }
    if (!PWWdccMakeCold(Texture, FColor(255, 0, 0)))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("texture-compiled-synchronously"),
            TEXT("PostEditChange rebuilt the texture synchronously (async texture compilation is off on this host), so there was no cold texture to report."));
        return true;
    }

    auto Capture = [&](FTestResponseCapture& Out) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        Payload->SetStringField(TEXT("target"), TEXT("preview"));
        Payload->SetNumberField(TEXT("max_size"), PWWdccMaxSize);
        TestTrue(TEXT("widget.screenshot_designer handler found"),
            InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Out));
        TestTrue(FString::Printf(TEXT("preview capture succeeds (error='%s')"), *Out.ErrorCode), Out.bSuccess);
        if (!Out.bSuccess || !Out.Result.IsValid())
        {
            return false;
        }
        FString Path;
        if (Out.Result->TryGetStringField(TEXT("path"), Path))
        {
            ScreenshotPaths.Add(Path);
        }
        return true;
    };

    // Zero budget: the texture is still compiling when the frame is drawn, and the response
    // has to say so and name it.
    WidgetDesignerCaptureInternal::ReadinessBudgetSeconds() = 0.0;
    FTestResponseCapture Expired;
    if (!Capture(Expired))
    {
        return false;
    }
    bool bComplete = true;
    TestTrue(TEXT("response carries previewComplete"),
        Expired.Result->TryGetBoolField(TEXT("previewComplete"), bComplete));
    TestFalse(TEXT("a frame drawn over a still-compiling texture is reported incomplete"), bComplete);
    const TSharedPtr<FJsonObject>* Readiness = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* NotReady = nullptr;
    bool bNamed = false;
    if (TestTrue(TEXT("response carries readiness"), Expired.Result->TryGetObjectField(TEXT("readiness"), Readiness))
        && TestTrue(TEXT("readiness carries notReady"), (*Readiness)->TryGetArrayField(TEXT("notReady"), NotReady)))
    {
        for (const TSharedPtr<FJsonValue>& Value : *NotReady)
        {
            bNamed |= Value.IsValid() && Value->AsString() == Texture->GetPathName();
        }
    }
    TestTrue(FString::Printf(TEXT("notReady names the still-compiling texture %s"), *Texture->GetPathName()), bNamed);

    // Normal budget: the same capture waits and reports complete.
    WidgetDesignerCaptureInternal::ReadinessBudgetSeconds() = SavedBudget;
    FTestResponseCapture Waited;
    if (!Capture(Waited))
    {
        return false;
    }
    bComplete = false;
    TestTrue(TEXT("response carries previewComplete"),
        Waited.Result->TryGetBoolField(TEXT("previewComplete"), bComplete));
    TestTrue(TEXT("with the budget restored the capture waits and reports complete"), bComplete);
    if (TestTrue(TEXT("response carries readiness"), Waited.Result->TryGetObjectField(TEXT("readiness"), Readiness)))
    {
        TestTrue(TEXT("readiness counts the brush texture"), (*Readiness)->GetNumberField(TEXT("textures")) >= 1.0);
        TestTrue(TEXT("notReady is empty"),
            (*Readiness)->TryGetArrayField(TEXT("notReady"), NotReady) && NotReady->Num() == 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerReadinessCollectsFontMaterialsTest,
    "PinWright.widget.screenshot_designer.ReadinessCollectsBrushTexturesAndFontMaterials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerReadinessCollectsFontMaterialsTest::RunTest(const FString& Parameters)
{
    // Pure reflection walk; no renderer needed. The ticket's missing text was gradient text
    // whose FontMaterial had no shader map yet, so the font material must be in the wait set.
    UTexture2D* Texture = PWWdccMakeWarmTexture(FColor(0, 255, 0));
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_ColdCaptureCollect"));
    UWidgetBlueprint* WBP = PWWdccMakeWidget(WidgetPath, Texture);
    ON_SCOPE_EXIT
    {
        WidgetPreviewAlphaFixtures::CloseAndCleanupWidget(WBP, WidgetPath);
    };
    if (!TestNotNull(TEXT("widget blueprint built"), WBP))
    {
        return false;
    }
    UTextBlock* Label = Cast<UTextBlock>(WBP->WidgetTree->FindWidget(TEXT("ColdLabel")));
    UMaterialInterface* FontMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
    if (!TestNotNull(TEXT("label exists"), Label) || !TestNotNull(TEXT("engine default material"), FontMaterial))
    {
        return false;
    }
    Label->SetFontMaterial(FontMaterial);

    TArray<UTexture*> Textures;
    TArray<UMaterialInterface*> Materials;
    WidgetDesignerCaptureUtil::CollectRenderAssets(WBP->WidgetTree, Textures, Materials);
    TestTrue(TEXT("the image brush's texture is collected"), Textures.Contains(Texture));
    TestTrue(TEXT("the text block's font material is collected"), Materials.Contains(FontMaterial));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerPreviewOmitsDashedOutlinesTest,
    "PinWright.widget.screenshot_designer.PreviewOmitsDesignerDashedOutlines",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerPreviewOmitsDashedOutlinesTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }

    // The Designer reads this when the editor opens; force outlines on so the preview's Slate
    // tree has them whatever this host's preference is.
    UWidgetDesignerSettings* Settings = GetMutableDefault<UWidgetDesignerSettings>();
    const bool bSavedShowOutlines = Settings->bShowOutlines;
    Settings->bShowOutlines = true;

    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_DesignerOutlines"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    ON_SCOPE_EXIT
    {
        Settings->bShowOutlines = bSavedShowOutlines;
        WidgetPreviewAlphaFixtures::CloseAndCleanupWidget(WBP, WidgetPath);
    };
    UCanvasPanel* Root = WBP ? Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget) : nullptr;
    if (!TestNotNull(TEXT("root canvas"), Root))
    {
        return false;
    }

    // Solid green over the whole canvas: any pixel that is not green is something drawn on
    // top of the content -- the outline border sits in the overlay slot above it.
    const FColor Green(0, 255, 0);
    UImage* Fill = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("SolidFill"));
    Fill->SetBrush(FSlateColorBrush(FLinearColor(0.0f, 1.0f, 0.0f)));
    if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Root->AddChild(Fill)))
    {
        CanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
        CanvasSlot->SetOffsets(FMargin(0.0f));
    }
    WidgetTestFixtures::RegisterWidgetVariable(WBP, Fill->GetFName());
    FKismetEditorUtilities::CompileBlueprint(WBP);
    WidgetPreviewAlphaFixtures::PinDesignerPreviewSize(WBP, FVector2D(PWWdccDesignWidth, PWWdccDesignHeight));

    TArray<uint8> Png;
    FString Error;
    WidgetDesignerCaptureUtil::FCaptureInfo Info;
    if (!TestTrue(FString::Printf(TEXT("preview capture succeeds (error='%s')"), *Error),
        WidgetDesignerCaptureUtil::CapturePreviewToPng(WBP, PWWdccMaxSize, Png, Error, &Info)))
    {
        return false;
    }

    // Precondition and measurement in one: the root canvas is a panel, so with outlines on its
    // preview Slate tree carries one dashed border, and the capture must have found it.
    TestTrue(FString::Printf(TEXT("the capture found and hid the Designer outline (hidden=%d)"), Info.DesignerOutlinesHidden),
        Info.DesignerOutlinesHidden >= 1);

    TArray64<uint8> Bgra;
    if (!TestTrue(TEXT("preview PNG decodes"), PWWdccDecodePng(Png, Bgra)) || Info.Width <= 0 || Info.Height <= 0)
    {
        return false;
    }
    const int32 Total = Info.Width * Info.Height;
    const int32 GreenPixels = PWWdccCountNear(Bgra, Info.Width, 0, 0, Info.Width, Info.Height, Green);
    TestEqual(TEXT("every pixel is the fill: no dashed outline is drawn over the content"), GreenPixels, Total);
    return true;
}
