// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Editor/EditorWindowHandlers.h"
#include "Handlers/HandlerContext.h"
#include "Input/Events.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SWindow.h"

namespace EditorScreenshotWindowSelectorTestLocal
{
    FDriveWindowSelector ParseSelector(const TSharedPtr<FJsonObject>& Payload)
    {
        const FHandlerContext Context = FHandlerContext::MakeTestContext(
            TEXT("screenshot-window-selector-test"), TEXT("editor.screenshot_window"), Payload);
        return FDriveHandlerCommon::ParseWindowSelector(Context);
    }

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotWindowDefaultTargetTest,
    "PinWright.editor.screenshot_window.DefaultsToMainFrameWithoutSelector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotWindowDefaultTargetTest::RunTest(const FString& Parameters)
{
    using namespace EditorScreenshotWindowSelectorTestLocal;

    if (!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Slate is unavailable, so editor-window resolution was not exercised."));
        return false;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();
    if (!SlateApp.GetRenderer())
    {
        AddError(TEXT("Slate has no renderer, so synthetic window activation was not exercised."));
        return false;
    }

    IMainFrameModule* MainFrameModule =
        FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame"));
    TSharedPtr<SWindow> MainFrameWindow;
    if (MainFrameModule && MainFrameModule->IsWindowInitialized())
    {
        MainFrameWindow = MainFrameModule->GetParentWindow();
    }
    if (!MainFrameWindow.IsValid() || !MainFrameWindow->IsVisible() ||
        MainFrameWindow->IsWindowMinimized())
    {
        AddError(TEXT("The main editor frame is invalid, hidden, or minimized."));
        return false;
    }
    if (SlateApp.GetActiveModalWindow().IsValid())
    {
        AddError(TEXT("An active modal window prevents deterministic activation of the temporary fixture."));
        return false;
    }

    const FString FixtureTitle = FString::Printf(TEXT("PW_ScreenshotWindowSelector_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const TSharedRef<SWindow> Fixture = SNew(SWindow)
        .Type(EWindowType::Normal)
        .Title(FText::FromString(FixtureTitle))
        .ScreenPosition(FVector2D(120.0f, 120.0f))
        .ClientSize(FVector2D(320.0f, 180.0f))
        .FocusWhenFirstShown(false)
        .SupportsMaximize(false)
        .SupportsMinimize(false);

    const TSharedPtr<SWindow> PreviousActiveWindow = SlateApp.GetActiveTopLevelWindow();
    SlateApp.AddWindow(Fixture, /*bShowImmediately=*/true);
    ON_SCOPE_EXIT
    {
        TSharedPtr<SWindow> RestoreWindow = PreviousActiveWindow;
        if (!RestoreWindow.IsValid() || !RestoreWindow->IsVisible() ||
            RestoreWindow->IsWindowMinimized())
        {
            RestoreWindow = MainFrameWindow;
        }
        if (RestoreWindow.IsValid() && RestoreWindow->IsVisible() &&
            !RestoreWindow->IsWindowMinimized())
        {
            SlateApp.ProcessWindowActivatedEvent(
                FWindowActivateEvent(FWindowActivateEvent::EA_Activate, RestoreWindow.ToSharedRef()));
        }
        SlateApp.RequestDestroyWindow(Fixture);
    };

    for (int32 Tick = 0; Tick < 8; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }

    TArray<TSharedRef<SWindow>> VisibleWindows;
    SlateApp.GetAllVisibleWindowsOrdered(VisibleWindows);
    const bool bFixtureIsVisible = VisibleWindows.ContainsByPredicate(
        [&Fixture](const TSharedRef<SWindow>& Candidate)
        {
            return &Candidate.Get() == &Fixture.Get();
        });
    if (!bFixtureIsVisible)
    {
        AddError(TEXT("Slate did not expose the registered temporary window as visible."));
        return false;
    }

    SlateApp.ProcessWindowActivatedEvent(
        FWindowActivateEvent(FWindowActivateEvent::EA_Activate, Fixture));
    const TSharedPtr<SWindow> ActiveWindow = SlateApp.GetActiveTopLevelWindow();
    if (!ActiveWindow.IsValid() || ActiveWindow.Get() != &Fixture.Get())
    {
        AddError(TEXT("Slate did not accept deterministic activation of the temporary window."));
        return false;
    }
    TestTrue(TEXT("temporary foreign window is the active top-level window"),
        ActiveWindow.Get() == &Fixture.Get());

    TSharedPtr<SWindow> ResolvedWindow;
    FString ResolvedTitle;
    FString ErrorCode;
    FString ErrorMessage;
    const FDriveWindowSelector EmptySelector = ParseSelector(MakeShared<FJsonObject>());
    const bool bDefaultResolved = EditorWindowHandlers::ResolveScreenshotWindow(
        EmptySelector, ResolvedWindow, ResolvedTitle, ErrorCode, ErrorMessage);
    TestTrue(TEXT("omitted selector resolves successfully"), bDefaultResolved);
    TestTrue(TEXT("omitted selector resolves the exact main editor frame, not the active fixture"),
        ResolvedWindow.Get() == MainFrameWindow.Get());
    TestEqual(TEXT("omitted selector returns the main editor frame title"),
        ResolvedTitle, MainFrameWindow->GetTitle().ToString());
    TestTrue(TEXT("successful default resolution leaves no error code"), ErrorCode.IsEmpty());
    TestTrue(TEXT("successful default resolution leaves no error message"), ErrorMessage.IsEmpty());

    TSharedPtr<FJsonObject> EmptyTitles = MakeShared<FJsonObject>();
    EmptyTitles->SetStringField(TEXT("window_title"), TEXT(""));
    EmptyTitles->SetStringField(TEXT("title"), TEXT(""));
    const bool bEmptyTitlesResolved = EditorWindowHandlers::ResolveScreenshotWindow(
        ParseSelector(EmptyTitles), ResolvedWindow, ResolvedTitle, ErrorCode, ErrorMessage);
    TestTrue(TEXT("effectively empty title selectors resolve successfully"), bEmptyTitlesResolved);
    TestTrue(TEXT("effectively empty title selectors resolve the exact main editor frame"),
        ResolvedWindow.Get() == MainFrameWindow.Get());

    TSharedPtr<FJsonObject> CanonicalTitle = MakeShared<FJsonObject>();
    CanonicalTitle->SetStringField(TEXT("window_title"), FixtureTitle);
    const bool bCanonicalTitleResolved = EditorWindowHandlers::ResolveScreenshotWindow(
        ParseSelector(CanonicalTitle), ResolvedWindow, ResolvedTitle, ErrorCode, ErrorMessage);
    TestTrue(TEXT("window_title resolves successfully"), bCanonicalTitleResolved);
    TestTrue(TEXT("window_title resolves the named fixture through the shared resolver"),
        ResolvedWindow.Get() == &Fixture.Get());
    TestTrue(TEXT("window_title does not force the main editor frame"),
        ResolvedWindow.Get() != MainFrameWindow.Get());
    TestEqual(TEXT("window_title returns the fixture title"), ResolvedTitle, FixtureTitle);
    TestTrue(TEXT("successful window_title resolution leaves no error code"), ErrorCode.IsEmpty());
    TestTrue(TEXT("successful window_title resolution leaves no error message"), ErrorMessage.IsEmpty());

    TSharedPtr<FJsonObject> TitleAlias = MakeShared<FJsonObject>();
    TitleAlias->SetStringField(TEXT("title"), FixtureTitle);
    const bool bTitleAliasResolved = EditorWindowHandlers::ResolveScreenshotWindow(
        ParseSelector(TitleAlias), ResolvedWindow, ResolvedTitle, ErrorCode, ErrorMessage);
    TestTrue(TEXT("title alias resolves successfully"), bTitleAliasResolved);
    TestTrue(TEXT("title alias resolves the named fixture through the shared resolver"),
        ResolvedWindow.Get() == &Fixture.Get());

    TSharedPtr<FJsonObject> CanonicalIndex = MakeShared<FJsonObject>();
    CanonicalIndex->SetNumberField(TEXT("window_index"), 0);
    const FDriveWindowSelector CanonicalIndexSelector = ParseSelector(CanonicalIndex);
    TestTrue(TEXT("window_index zero remains explicitly set"), CanonicalIndexSelector.Index.IsSet());
    if (CanonicalIndexSelector.Index.IsSet())
    {
        TestEqual(TEXT("window_index preserves explicit zero"),
            CanonicalIndexSelector.Index.GetValue(), 0);
    }

    TSharedPtr<FJsonObject> IndexAlias = MakeShared<FJsonObject>();
    IndexAlias->SetNumberField(TEXT("index"), 0);
    const FDriveWindowSelector IndexAliasSelector = ParseSelector(IndexAlias);
    TestTrue(TEXT("index alias zero remains explicitly set"), IndexAliasSelector.Index.IsSet());
    if (IndexAliasSelector.Index.IsSet())
    {
        TestEqual(TEXT("index alias preserves explicit zero"), IndexAliasSelector.Index.GetValue(), 0);
    }

    TestEqual(TEXT("Normal window type string"),
        FDriveEditorChrome::WindowTypeToString(EWindowType::Normal), FString(TEXT("Normal")));
    TestEqual(TEXT("Menu window type string"),
        FDriveEditorChrome::WindowTypeToString(EWindowType::Menu), FString(TEXT("Menu")));
    TestEqual(TEXT("ToolTip window type string"),
        FDriveEditorChrome::WindowTypeToString(EWindowType::ToolTip), FString(TEXT("ToolTip")));
    TestEqual(TEXT("Notification window type string"),
        FDriveEditorChrome::WindowTypeToString(EWindowType::Notification), FString(TEXT("Notification")));
    TestEqual(TEXT("CursorDecorator window type string"),
        FDriveEditorChrome::WindowTypeToString(EWindowType::CursorDecorator), FString(TEXT("CursorDecorator")));
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    TestEqual(TEXT("GameWindow window type string"),
        FDriveEditorChrome::WindowTypeToString(EWindowType::GameWindow), FString(TEXT("GameWindow")));
#endif
    TestEqual(TEXT("unknown window type string"),
        FDriveEditorChrome::WindowTypeToString(static_cast<EWindowType>(255)), FString(TEXT("Unknown")));

    if (!bTitleAliasResolved || !ResolvedWindow.IsValid())
    {
        return false;
    }
    TSharedPtr<FJsonObject> Identity = MakeShared<FJsonObject>();
    EditorWindowHandlers::SetScreenshotWindowIdentityFields(
        *Identity, ResolvedTitle, ResolvedWindow->GetType());

    FString WindowTitle;
    TestTrue(TEXT("response includes windowTitle"),
        Identity->TryGetStringField(TEXT("windowTitle"), WindowTitle));
    TestEqual(TEXT("response windowTitle identifies the captured window"),
        WindowTitle, FixtureTitle);

    FString WindowType;
    TestTrue(TEXT("response includes windowType"),
        Identity->TryGetStringField(TEXT("windowType"), WindowType));
    TestEqual(TEXT("response windowType uses the production type mapping"),
        WindowType, FDriveEditorChrome::WindowTypeToString(ResolvedWindow->GetType()));

    return true;
}
