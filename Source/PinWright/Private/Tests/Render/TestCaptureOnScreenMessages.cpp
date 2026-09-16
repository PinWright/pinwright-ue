// Copyright (c) 2026 Alexander Penkin. MIT License.

// The fifth contamination channel: text the engine draws into the frame.
//
// WHAT THIS DEFENDS. render.capture_open_level publishes four channels of contamination warnings
// and missed the one that was actually burned into ten acceptance frames -- "Video memory has been
// exhausted (1197.488 MB over budget). Expect extremely poor performance." in red across the upper
// third -- while `showFlagOverrides.forced` was empty, `editorSprites.visible` false, `gameView`
// true and `blank`/`crushed`/`blownOut` clean. It is not a show flag, `hideEditorSprites` governs
// billboards only and game view does not touch it, so there was no caller-side way to detect it
// short of opening the PNG. The whole set was discarded and re-shot in a fresh editor.
//
// TWO CHANNELS, BOTH ASSERTED, BECAUSE THE OBVIOUS ONE DOES NOT EXIST. `GEngine->
// GetOnScreenDebugMessages()` is not an API in UE 5.8 -- `ScreenMessages` / `PriorityScreenMessages`
// are private UEngine members with no accessor and no UPROPERTY (Engine.h:2186-2206), so they are
// unreachable by call and by reflection. What IS reachable from the game thread is
// FCoreDelegates::OnGetOnScreenMessages (public, broadcast by the engine's own draw path) and the
// renderer's demoted-local-memory banner, whose condition and text are a pure function of
// GDemotedLocalMemorySize and r.DemotedLocalMemoryWarning (SceneRendering.cpp:4452-4457, 4804).
// Both are asserted here without a GPU and without writing to GRHIGlobals.
#include "Misc/AutomationTest.h"

#include "Utils/OnScreenMessageSurvey.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    // Prefixed because anonymous namespaces merge inside one Unity translation unit.
    bool PwOnScreenReadSource(const FString& RelativePath, FString& OutSource)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return false;
        }
        return FFileHelper::LoadFileToString(
            OutSource, *FPaths::Combine(Plugin->GetBaseDir(), RelativePath));
    }

    // The engine draws none of these strings with either gate against it, so the survey honours
    // both gates. The test sets them rather than skipping on a host that happens to have messages
    // suppressed: a skip here would step over the only assertions this file has.
    struct FPwScreenMessageGateScope
    {
        FPwScreenMessageGateScope()
            : bPreviousEnabled(GAreScreenMessagesEnabled)
            , bPreviousSuppressed(GEngine && GEngine->bSuppressMapWarnings)
        {
            GAreScreenMessagesEnabled = true;
            if (GEngine)
            {
                GEngine->bSuppressMapWarnings = 0;
            }
        }

        ~FPwScreenMessageGateScope()
        {
            GAreScreenMessagesEnabled = bPreviousEnabled;
            if (GEngine)
            {
                GEngine->bSuppressMapWarnings = bPreviousSuppressed ? 1 : 0;
            }
        }

        FPwScreenMessageGateScope(const FPwScreenMessageGateScope&) = delete;
        FPwScreenMessageGateScope& operator=(const FPwScreenMessageGateScope&) = delete;

        bool bPreviousEnabled;
        bool bPreviousSuppressed;
    };

    const TCHAR* PwProbeMessageText =
        TEXT("PinWright on-screen message probe: this frame carries engine text.");
}

// ============================================================================
// A message on the engine's own on-screen channel is collected, published and flagged.
//
// The counterfactual: without the collector -- an unmeasured survey, which is what every capture
// response carried before this -- `onScreenMessages` is ABSENT from the viewport block and no
// warning is raised, so a frame with a renderer banner across it reads clean on every field.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureOnScreenMessagesPublishedTest,
    "PinWright.render.capture_on_screen_messages.EngineMessagesArePublishedAndFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureOnScreenMessagesPublishedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightOnScreenMessages;

    // The absence half first, so it is asserted against the same publisher the presence half uses.
    {
        const FOnScreenMessageSurvey Unmeasured;
        const TSharedPtr<FJsonObject> Viewport = MakeShared<FJsonObject>();
        AddOnScreenMessageFields(Unmeasured, Viewport);
        TestFalse(TEXT("a path that never surveyed publishes no array"),
            Viewport->HasField(TEXT("onScreenMessages")));
        TestFalse(TEXT("a path that never surveyed raises no warning"),
            Viewport->HasField(TEXT("onScreenMessageWarning")));
    }

    // A clean frame: surveyed, nothing found, so the empty array is a positive statement.
    {
        FOnScreenMessageSurvey Clean;
        Clean.bMeasured = true;
        const TSharedPtr<FJsonObject> Viewport = MakeShared<FJsonObject>();
        AddOnScreenMessageFields(Clean, Viewport);
        TestTrue(TEXT("a surveyed frame publishes the array even when empty"),
            Viewport->HasTypedField<EJson::Array>(TEXT("onScreenMessages")));
        TestEqual(TEXT("an empty survey publishes a zero count"),
            static_cast<int32>(Viewport->GetNumberField(TEXT("onScreenMessageCount"))), 0);
        TestFalse(TEXT("a clean frame carries no warning"),
            Viewport->HasField(TEXT("onScreenMessageWarning")));
    }

    // The live collector, driven through the channel the engine itself broadcasts.
    {
        const FPwScreenMessageGateScope GateScope;
        const FDelegateHandle Handle = FCoreDelegates::OnGetOnScreenMessages.AddLambda(
            [](FCoreDelegates::FSeverityMessageMap& OutMessages)
            {
                OutMessages.Add(FCoreDelegates::EOnScreenMessageSeverity::Error,
                    FText::FromString(PwProbeMessageText));
            });

        const FOnScreenMessageSurvey Surveyed = Survey();
        FCoreDelegates::OnGetOnScreenMessages.Remove(Handle);

        TestTrue(TEXT("the survey ran"), Surveyed.bMeasured);
        const FOnScreenMessage* Found = Surveyed.Messages.FindByPredicate(
            [](const FOnScreenMessage& Message)
            {
                return Message.Text.Contains(TEXT("PinWright on-screen message probe"));
            });
        if (TestNotNull(TEXT("a message on the engine's channel is collected"), Found))
        {
            TestEqual(TEXT("its severity is carried through"), Found->Severity, FString(TEXT("error")));
            TestEqual(TEXT("its source channel is named"), Found->Source, FString(TEXT("coreDelegate")));
            // The engine's own severity->colour mapping (UnrealEngine.cpp:13448-13462), so the
            // published colour is the colour in the pixels.
            TestEqual(TEXT("an error is published red"), Found->Color.R, 1.0f);
            TestEqual(TEXT("an error is published red, not white"), Found->Color.G, 0.0f);
        }

        const TSharedPtr<FJsonObject> Viewport = MakeShared<FJsonObject>();
        AddOnScreenMessageFields(Surveyed, Viewport);
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (TestTrue(TEXT("the array is published"),
                Viewport->TryGetArrayField(TEXT("onScreenMessages"), Entries) && Entries))
        {
            TestTrue(TEXT("the array carries the probe"), Entries->Num() > 0);
        }
        TestTrue(TEXT("a frame carrying engine text is flagged"),
            Viewport->HasTypedField<EJson::String>(TEXT("onScreenMessageWarning")));
        TestTrue(TEXT("the warning quotes the text that is in the pixels"),
            Viewport->GetStringField(TEXT("onScreenMessageWarning"))
                .Contains(TEXT("PinWright on-screen message probe")));
    }

    // The engine's suppression gates are honoured, so an empty list is never a false clean.
    {
        const FPwScreenMessageGateScope GateScope;
        GAreScreenMessagesEnabled = false;
        const FDelegateHandle Handle = FCoreDelegates::OnGetOnScreenMessages.AddLambda(
            [](FCoreDelegates::FSeverityMessageMap& OutMessages)
            {
                OutMessages.Add(FCoreDelegates::EOnScreenMessageSeverity::Error,
                    FText::FromString(PwProbeMessageText));
            });

        const FOnScreenMessageSurvey Surveyed = Survey();
        FCoreDelegates::OnGetOnScreenMessages.Remove(Handle);

        TestFalse(TEXT("the suppression state is published"), Surveyed.bScreenMessagesEnabled);
        TestEqual(TEXT("nothing is drawn, so nothing is reported"), Surveyed.Messages.Num(), 0);
    }

    return true;
}

// ============================================================================
// The renderer's VRAM banner is reproduced byte-for-byte, under the renderer's own condition.
//
// The counterfactual: drop the demoted-local-memory branch from Survey() and the exact message
// that cost ten acceptance frames -- the one that is NOT on any delegate, because
// SceneRendering.cpp prints it inline -- goes back to being invisible to every caller.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureOnScreenMessagesVramBannerTest,
    "PinWright.render.capture_on_screen_messages.VramBannerMatchesTheRendererText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureOnScreenMessagesVramBannerTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightOnScreenMessages;

    constexpr uint64 OneHundredMegabytes = 100ull * 1048576ull;

    // The renderer compares the cvar against 1 exactly and requires a non-zero overrun; both
    // halves matter, because the banner fired at 377 MB in one report and 1197 MB in another, so
    // no threshold of ours may be inserted here.
    TestTrue(TEXT("an overrun with the warning cvar at 1 reports"),
        ShouldReportDemotedLocalMemory(OneHundredMegabytes, 1));
    TestFalse(TEXT("no overrun reports nothing"),
        ShouldReportDemotedLocalMemory(0, 1));
    TestFalse(TEXT("the warning cvar off reports nothing"),
        ShouldReportDemotedLocalMemory(OneHundredMegabytes, 0));
    TestFalse(TEXT("a non-1 cvar value is not treated as truthy"),
        ShouldReportDemotedLocalMemory(OneHundredMegabytes, 2));

    // The exact sentence the renderer draws (SceneRendering.cpp:4804), so a caller can grep the
    // published text against the pixels rather than matching it by eye.
    TestEqual(TEXT("the banner text matches the renderer's format string"),
        MakeDemotedLocalMemoryText(OneHundredMegabytes),
        FString(TEXT("Video memory has been exhausted (100.000 MB over budget). "
                     "Expect extremely poor performance.")));

    // Both capture verbs publish the survey; a collector nothing calls closes nothing.
    FString RenderSource;
    if (TestTrue(TEXT("RenderHandler.cpp is readable"), PwOnScreenReadSource(
            TEXT("Source/PinWright/Private/Handlers/Render/RenderHandler.cpp"), RenderSource)))
    {
        TestTrue(TEXT("render.capture_open_level publishes the survey"),
            RenderSource.Contains(TEXT("PinWrightOnScreenMessages::AddOnScreenMessageFields(")));
    }
    FString ViewportSource;
    if (TestTrue(TEXT("ViewportHandler.cpp is readable"), PwOnScreenReadSource(
            TEXT("Source/PinWright/Private/Handlers/Editor/ViewportHandler.cpp"), ViewportSource)))
    {
        TestTrue(TEXT("editor.screenshot publishes the survey"),
            ViewportSource.Contains(TEXT("PinWrightOnScreenMessages::AddOnScreenMessageFields(")));
    }

    return true;
}
