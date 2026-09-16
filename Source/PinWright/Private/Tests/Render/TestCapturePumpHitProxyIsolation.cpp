// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-capture-open-level-hitproxy-colorrt-assert-kills-editor: the shared
// capture pump (PinWrightRenderCapture::PumpViewport, the ONE pump every viewport capture verb
// runs through CaptureEditorViewportToPng) must stay off the editor's hit-proxy path.
//
// THE DEFECT. PumpViewport called SlateApp.PumpMessages() and SlateApp.Tick(ESlateTickType::All),
// and invalidated hit proxies as well as the display, three-plus times per capture, on the live
// viewport CaptureEditorViewportToPng had just resized:
//
//   * `All` adds the PlatformAndInput phase, whose FSlateUser::UpdateCursor() runs a cursor query
//     into the widget under the mouse -> FSceneViewport::OnCursorQuery ->
//     FLevelEditorViewportClient::GetCursor -> FViewport::GetHitProxy -> GetRawHitProxyData. A
//     screenshot needs no hit proxies; that buffer only answers "what is under this pixel".
//   * The no-argument Invalidate() overloads invalidate the hit-proxy map along with the display,
//     which is what forces GetRawHitProxyData off its cache early-out and into the regeneration
//     branch -- the branch that builds an FRHIRenderPassInfo from
//     HitProxyMap.GetRenderTargetTexture(), the render target SetFixedViewportSize releases and
//     rebuilds. A regeneration landing in that window asserts `ColorRT` on the RENDERING thread and
//     appErrors the shared editor with every other agent's unsaved work in memory.
//
// WHY THESE TESTS ARE SOURCE LINTS RATHER THAN A DRIVEN CAPTURE. The fix is the ABSENCE of two
// calls, and neither absence has a public observable: bHitProxiesCached, bShouldCheckHitProxy and
// FHitProxyMap are all private engine state, and the only public way to read the cache is
// FViewport::GetRawHitProxyData -- the crashing call itself. The failure is also a race by the
// reporters' own measurements (three identical spawn-then-capture pairs, the third killed the
// editor), so a capture-driving test would be green on the broken code most of the time. Same
// shape and same reason as the codebase's other absence lints (Core.error_codes,
// Infra.contract.*, TestNoParamHandlersReadNoArgs): scan the source, fail on the call.
//
// Red before green by construction: both assertions name text that was in PumpViewport's body
// verbatim before the fix.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"

#include "Interfaces/IPluginManager.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with the identically shaped helpers in the
// sibling Tests/Render/*.cpp files.
namespace CapturePumpHitProxyIsolationTestLocal
{
    // The file that owns the shared pump, resolved through IPluginManager the way the other
    // source-scanning tests resolve their scan roots (never a path relative to the CWD, which is
    // the project directory under one runner and the engine directory under another).
    FString CaptureUtilsSourcePath()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir()
            / TEXT("Source") / TEXT("PinWright") / TEXT("Private")
            / TEXT("Handlers") / TEXT("Render") / TEXT("PreviewViewportCaptureUtils.cpp");
    }

    // Line comments removed before anything is matched, for two reasons: PumpViewport is preceded
    // by a long comment that QUOTES every call these tests forbid (it is the record of why they
    // are forbidden), and brace matching below must not trip over a brace inside a comment.
    FString StripLineComments(const FString& Source)
    {
        TArray<FString> Lines;
        Source.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
        FString Out;
        for (const FString& Line : Lines)
        {
            const int32 Slashes = Line.Find(TEXT("//"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
            Out += (Slashes == INDEX_NONE) ? Line : Line.Left(Slashes);
            Out += TEXT("\n");
        }
        return Out;
    }

    // The BODY of PumpViewport -- signature line excluded, so the explanatory comment above it and
    // the rest of the file cannot satisfy or violate an assertion by accident.
    bool ReadPumpViewportBody(FString& OutBody, FString& OutWhyNot)
    {
        const FString Path = CaptureUtilsSourcePath();
        if (Path.IsEmpty())
        {
            OutWhyNot = TEXT("PinWright plugin not found through IPluginManager");
            return false;
        }

        FString Source;
        if (!FFileHelper::LoadFileToString(Source, *Path))
        {
            OutWhyNot = FString::Printf(TEXT("could not read %s"), *Path);
            return false;
        }

        const FString Stripped = StripLineComments(Source);
        const int32 SignatureAt = Stripped.Find(TEXT("void PumpViewport("), ESearchCase::CaseSensitive);
        if (SignatureAt == INDEX_NONE)
        {
            OutWhyNot = FString::Printf(
                TEXT("PumpViewport is no longer declared in %s -- if the shared pump moved, move ")
                TEXT("these tests with it rather than deleting them"), *Path);
            return false;
        }

        const int32 OpenBrace = Stripped.Find(TEXT("{"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, SignatureAt);
        if (OpenBrace == INDEX_NONE)
        {
            OutWhyNot = TEXT("PumpViewport has no body");
            return false;
        }

        int32 Depth = 0;
        for (int32 Index = OpenBrace; Index < Stripped.Len(); ++Index)
        {
            const TCHAR Ch = Stripped[Index];
            if (Ch == TEXT('{'))
            {
                ++Depth;
            }
            else if (Ch == TEXT('}'))
            {
                --Depth;
                if (Depth == 0)
                {
                    OutBody = Stripped.Mid(OpenBrace, Index - OpenBrace + 1);
                    return true;
                }
            }
        }

        OutWhyNot = TEXT("PumpViewport's body is unbalanced");
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCapturePumpDoesNotProcessEditorInputTest,
    "PinWright.render.capture_pump.DoesNotProcessEditorInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCapturePumpDoesNotProcessEditorInputTest::RunTest(const FString& Parameters)
{
    FString Body;
    FString WhyNot;
    if (!CapturePumpHitProxyIsolationTestLocal::ReadPumpViewportBody(Body, WhyNot))
    {
        AddError(WhyNot);
        return false;
    }

    // FSlateApplication::PumpMessages DISPATCHES on Windows (FWindowsApplication::DeferMessage only
    // defers while GPumpingMessagesOutsideOfMainLoop), so a real click that arrives mid-capture is
    // delivered synchronously into the viewport the capture has resized -- ProcessClick, and a hit
    // proxy query with it.
    TestFalse(TEXT("the capture pump must not pump platform messages"),
        Body.Contains(TEXT("PumpMessages"), ESearchCase::CaseSensitive));

    // ESlateTickType::All == Time | PlatformAndInput | Widgets. The PlatformAndInput phase is the
    // one that runs FSlateUser::UpdateCursor() -> FSceneViewport::OnCursorQuery ->
    // FLevelEditorViewportClient::GetCursor -> FViewport::GetHitProxy.
    TestFalse(TEXT("the capture pump must not tick Slate with ESlateTickType::All"),
        Body.Contains(TEXT("ESlateTickType::All"), ESearchCase::CaseSensitive));

    // Positive half, so the test fails on a pump that stopped ticking Slate at all rather than
    // silently passing an assertion made entirely of absences.
    TestTrue(TEXT("the capture pump must still tick Slate time and widgets"),
        Body.Contains(TEXT("ESlateTickType::TimeAndWidgets"), ESearchCase::CaseSensitive));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCapturePumpDoesNotInvalidateHitProxiesTest,
    "PinWright.render.capture_pump.DoesNotInvalidateHitProxies",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCapturePumpDoesNotInvalidateHitProxiesTest::RunTest(const FString& Parameters)
{
    FString Body;
    FString WhyNot;
    if (!CapturePumpHitProxyIsolationTestLocal::ReadPumpViewportBody(Body, WhyNot))
    {
        AddError(WhyNot);
        return false;
    }

    // `Invalidate();` matches ONLY the no-argument overloads -- FEditorViewportClient::Invalidate()
    // and FViewport::Invalidate() -- both of which take the hit-proxy map with them. It does not
    // match InvalidateDisplay(); or the two-argument form asserted below.
    TestFalse(TEXT("the capture pump must not use the hit-proxy-invalidating Invalidate() overloads"),
        Body.Contains(TEXT("Invalidate();"), ESearchCase::CaseSensitive));

    // The two replacements, asserted positively so a pump that stopped invalidating the DISPLAY
    // (and therefore stopped producing fresh frames) fails here rather than shipping stale pixels.
    TestTrue(TEXT("the capture pump must invalidate the viewport client's display only"),
        Body.Contains(TEXT("/*bInvalidateHitProxies=*/false"), ESearchCase::CaseSensitive));
    TestTrue(TEXT("the capture pump must invalidate the scene viewport's display only"),
        Body.Contains(TEXT("InvalidateDisplay()"), ESearchCase::CaseSensitive));

    return true;
}
