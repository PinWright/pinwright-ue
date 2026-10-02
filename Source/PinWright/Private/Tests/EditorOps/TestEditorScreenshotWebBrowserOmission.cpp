// Copyright (c) 2026 Alexander Penkin. MIT License.

// B-screenshot-fixed-size-omits-webbrowser: editor.screenshot's fixed-size path paints the game
// layer tree off screen through FWidgetRenderer, and a CEF web browser view was missing from that
// layer while the response said nothing. The fixed-size path needs PIE, which the suite does not
// run, so this drives the same two utils it uses (RenderSlateWidgetToSrgbColors, then
// FindWebBrowsersMissingFromOverlay) against a real CEF browser in a fixture window.
//
// The page is solid magenta, and the test first waits until the live window's back buffer shows
// it, so the browser is known to be painting. Then it holds the contract either way the engine
// behaves: magenta in the off-screen layer and nothing reported, or no magenta and the browser
// reported. A layer the browser is known to have contributed nothing to (rendered with the
// browser at opacity 0) must always be reported - that half fails if the detector is removed.
// A host without CEF, or whose fixture page never reaches the window, skips with the marker.

#include "Misc/AutomationTest.h"

#include "Tests/AutomationCommon.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/ScreenshotUtils.h"

#include "Containers/StringConv.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "WebBrowser.h"
#include "WebBrowserModule.h"
#include "Widgets/SWindow.h"

namespace ScreenshotWebOmissionTest
{
    constexpr double ReadySeconds = 30.0;
    const FIntPoint DrawSize(320, 200);

    bool IsMagenta(const FColor& C)
    {
        return C.R >= 230 && C.G <= 25 && C.B >= 230;
    }

    int32 CountMagenta(const TArray<FColor>& Pixels)
    {
        int32 Count = 0;
        for (const FColor& C : Pixels)
        {
            Count += IsMagenta(C) ? 1 : 0;
        }
        return Count;
    }

    TSharedPtr<SWidget> FindBrowserView(const TSharedRef<SWidget>& Widget)
    {
        if (Widget->GetType() == FName(TEXT("SWebBrowserView")))
        {
            return Widget;
        }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
        {
            if (TSharedPtr<SWidget> Found = FindBrowserView(Children->GetChildAt(Index)))
            {
                return Found;
            }
        }
        return nullptr;
    }

    struct FFixture
    {
        TSharedPtr<SWindow> Window;
        TSharedPtr<SWidget> Content;
        TStrongObjectPtr<UWebBrowser> Browser;
        double Deadline = 0.0;
        double NextProbe = 0.0;

        ~FFixture()
        {
            if (Browser.IsValid())
            {
                Browser->ReleaseSlateResources(true);
            }
            if (Window.IsValid() && FSlateApplication::IsInitialized())
            {
                FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
                FSlateApplication::Get().Tick(ESlateTickType::All);
            }
        }
    };
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FScreenshotWebOmissionStep, TFunction<bool()>, Step);

bool FScreenshotWebOmissionStep::Update()
{
    return Step();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotWebBrowserOmissionTest,
    "PinWright.editor.screenshot.FixedSize.WebBrowserContentIsCompositedOrReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotWebBrowserOmissionTest::RunTest(const FString& Parameters)
{
    using namespace ScreenshotWebOmissionTest;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate_not_initialized"),
            TEXT("FSlateApplication is not initialized; the web fixture window cannot be built."));
        return true;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("nocef")) || !IWebBrowserModule::Get().IsWebModuleAvailable())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cef-unavailable"),
            TEXT("The WebBrowser module reports no CEF runtime on this host (or -nocef); no web fixture can load."));
        return true;
    }

    const FTCHARToUTF8 Utf8(TEXT("<!doctype html><html><body style=\"margin:0;background:rgb(255,0,255)\"></body></html>"));
    const TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    const FString Url = TEXT("data:text/html;base64,") + FBase64::Encode(Bytes);

    TSharedPtr<FFixture> F = MakeShared<FFixture>();
    UWebBrowser* Browser = NewObject<UWebBrowser>(GetTransientPackage());
    F->Browser.Reset(Browser);
    // Construction-time page (protected, so set by reflection), as the drive.weblive fixture does.
    if (FStrProperty* InitialUrl = FindFProperty<FStrProperty>(UWebBrowser::StaticClass(), TEXT("InitialURL")))
    {
        InitialUrl->SetPropertyValue_InContainer(Browser, Url);
    }
    F->Window = SNew(SWindow)
        .Title(FText::FromString(FString::Printf(TEXT("PW_WebOmission_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits))))
        .ClientSize(FVector2D(DrawSize.X, DrawSize.Y))
        .FocusWhenFirstShown(false)
        .IsTopmostWindow(true)
        .SupportsMaximize(false)
        .SupportsMinimize(false);
    F->Content = Browser->TakeWidget();
    F->Window->SetContent(F->Content.ToSharedRef());
    FSlateApplication::Get().AddWindow(F->Window.ToSharedRef(), /*bShowImmediately=*/true);
    F->Deadline = FPlatformTime::Seconds() + ReadySeconds;

    ADD_LATENT_AUTOMATION_COMMAND(FScreenshotWebOmissionStep([this, F]() -> bool
    {
        const double Now = FPlatformTime::Seconds();
        if (Now < F->NextProbe)
        {
            return false;
        }
        F->NextProbe = Now + 0.5;

        // Ground truth first: the page must be on the live window, or there is nothing to omit.
        TArray<FColor> Native;
        FIntVector NativeSize;
        const bool bNativeShowsPage = PinWrightScreenshotUtils::TakeSlateScreenshot(
                F->Window.ToSharedRef(), Native, NativeSize)
            && CountMagenta(Native) > Native.Num() / 4;
        if (!bNativeShowsPage)
        {
            if (Now > F->Deadline)
            {
                PinWrightTestSkip::SkipAssertions(*this, TEXT("web-fixture-not-painted"),
                    FString::Printf(TEXT("The CEF fixture page never reached the window back buffer within %.0f s."),
                        ReadySeconds));
                return true;
            }
            return false;
        }

        const TSharedRef<SWidget> Root = F->Content.ToSharedRef();
        const TSharedPtr<SWidget> View = FindBrowserView(Root);
        if (!TestTrue(TEXT("the fixture hosts an SWebBrowserView"), View.IsValid()))
        {
            return true;
        }

        TArray<FColor> Overlay;
        FString Error;
        if (!TestTrue(TEXT("the off-screen layer renders"),
                PinWrightScreenshotUtils::RenderSlateWidgetToSrgbColors(Root, DrawSize, 1.0f, Overlay, Error)))
        {
            return true;
        }
        const bool bComposited = CountMagenta(Overlay) > Overlay.Num() / 4;
        const TArray<FString> Omitted =
            PinWrightScreenshotUtils::FindWebBrowsersMissingFromOverlay(Root, DrawSize, 1.0f, Overlay);
        AddInfo(FString::Printf(TEXT("off-screen layer drew the CEF page: %s; reported omitted: %d"),
            bComposited ? TEXT("yes") : TEXT("no"), Omitted.Num()));
        if (bComposited)
        {
            TestEqual(TEXT("a browser that reached the layer is not reported"), Omitted.Num(), 0);
        }
        else
        {
            TestEqual(TEXT("a browser missing from the layer is reported"), Omitted.Num(), 1);
            if (Omitted.Num() == 1)
            {
                TestEqual(TEXT("the report names the widget type"), Omitted[0], FString(TEXT("SWebBrowserView")));
            }
        }

        // Known-empty contribution: the layer as it looks without the browser must be reported.
        const float Opacity = View->GetRenderOpacity();
        View->SetRenderOpacity(0.0f);
        TArray<FColor> WithoutBrowser;
        const bool bRenderedWithout = PinWrightScreenshotUtils::RenderSlateWidgetToSrgbColors(
            Root, DrawSize, 1.0f, WithoutBrowser, Error);
        View->SetRenderOpacity(Opacity);
        if (TestTrue(TEXT("the browser-less layer renders"), bRenderedWithout))
        {
            TestEqual(TEXT("a layer the browser contributed nothing to reports it"),
                PinWrightScreenshotUtils::FindWebBrowsersMissingFromOverlay(
                    Root, DrawSize, 1.0f, WithoutBrowser).Num(), 1);
        }
        return true;
    }));
    return true;
}
