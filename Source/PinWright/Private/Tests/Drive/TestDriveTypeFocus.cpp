// Copyright (c) 2026 Alexander Penkin. MIT License.

// drive.type into a PIE text field (board B-drive-type-pie-editable-text-no-input). Keys and
// characters go to whatever holds keyboard focus, so drive.type must type only once its target
// holds it:
//
//  - PieSelectAllSurvivesType (failure direction): type into an unfocused UMG EditableText, select
//    all with drive.key ctrl+A, then type again. The second drive.type must replace the selection.
//    It used to click the field first, which clears the selection and moves the caret, so the text
//    was appended ("PwDone" instead of "Done").
//  - PieFocusTakenElsewhereIsRefused (failure direction): a focusable button in a higher UMG root
//    covers the field, in the same window, so the focusing click gives the button focus. drive.type
//    must refuse with TARGET_NOT_FOCUSED and type nothing. It used to type into the button and
//    answer with a benign settle outcome.
//
//  - ChromePopupFocusIsAccepted (failure direction): the target is a button whose click opens a
//    child window and focuses a text field in it, as a combo or search box does. The keys a user
//    types next go to that field, so drive.type must type there instead of refusing.
//
// The PIE tests run in an owned, host-neutral PIE session in the level viewport, across real frames.
// Their probes are outered to the PIE game instance and are released before PIE ends. The popup
// test uses a uniquely titled editor_chrome fixture window.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/EditableText.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Guid.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/Bpir/TestWidgetWithStructBIE.h"
#include "Tests/Drive/HostNeutralPie.h"
#include "Tests/Drive/PieViewportClearance.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/PieState.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DriveTypeFocusTest
{
    struct FState
    {
        bool bCovered = false;
        TStrongObjectPtr<UUserWidget> Probe;
        TStrongObjectPtr<UUserWidget> Cover;
        FString ProbeName;
        FString Handle;
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        FPieViewportClearance Clearance;
        int32 Step = 0;
        double Deadline = 0.0;
        uint64 NotBeforeFrame = 0;
        double CleanupDeadline = 0.0;
    };

    // A concrete native UUserWidget (UUserWidget itself is abstract) with one root widget; no
    // Widget Blueprint asset is created. Added at a z-order above any host overlay.
    template <typename TRoot>
    UUserWidget* AddProbe(APlayerController* Owner, const FString& Name, int32 ZOrder)
    {
        UUserWidget* Widget = CreateWidget<UUserWidget>(Owner, UTestWidgetWithStructBIE::StaticClass(), FName(*Name));
        if (!Widget || !Widget->WidgetTree)
        {
            return nullptr;
        }
        Widget->WidgetTree->RootWidget = Widget->WidgetTree->ConstructWidget<TRoot>(TRoot::StaticClass());
        Widget->AddToViewport(ZOrder);
        return Widget;
    }

    UEditableText* TextOf(const FState& State)
    {
        return State.Probe.IsValid() && State.Probe->WidgetTree
            ? Cast<UEditableText>(State.Probe->WidgetTree->RootWidget) : nullptr;
    }

    FString TextValue(const FState& State)
    {
        const UEditableText* Text = TextOf(State);
        return Text ? Text->GetText().ToString() : FString();
    }

    // The live SEditableText behind the handle, for fixture preconditions.
    TSharedPtr<SEditableText> LiveText(const FState& State)
    {
        const FDriveResolveResult Resolved = FDriveLiveResolver::ResolveHandle(FDriveRootSelector{}, State.Handle);
        if (Resolved.Status != EDriveResolveStatus::Found || !Resolved.Widget.IsValid()
            || Resolved.Widget->GetTypeAsString() != TEXT("SEditableText"))
        {
            return nullptr;
        }
        return StaticCastSharedPtr<SEditableText>(Resolved.Widget);
    }

    // The probes are outered to the PIE game instance: detach them and drop the strong refs BEFORE
    // FEndPlayMapCommand, or the end-of-PIE leak check finds the game instance still referenced.
    void Release(FState& State)
    {
        for (TStrongObjectPtr<UUserWidget>* Widget : { &State.Probe, &State.Cover })
        {
            if (Widget->IsValid())
            {
                (*Widget)->RemoveFromParent();
                Widget->Reset();
            }
        }
        State.Clearance.Restore();
    }

    // The text field's handle, once the field is listed with geometry Slate measured this frame.
    bool FindTextHandle(FState& State)
    {
        TArray<FDriveElement> Elements;
        FString RootName;
        FString Code;
        FString Message;
        if (!FDriveLiveResolver::BuildElementList(FDriveRootSelector{}, Elements, RootName, Code, Message))
        {
            return false;
        }
        const FDriveElement* Field = Elements.FindByPredicate([&State](const FDriveElement& Element)
        {
            return Element.Root == State.ProbeName && Element.Type == TEXT("SEditableText")
                && Element.bVisible && !Element.bGeometryStale;
        });
        if (Field)
        {
            State.Handle = Field->Handle;
        }
        return Field != nullptr;
    }

    void Invoke(FAutomationTestBase& Test, FState& State, const TCHAR* Method, const TSharedRef<FJsonObject>& Payload)
    {
        Test.TestTrue(*FString::Printf(TEXT("%s handler found"), Method),
            InvokeHandlerWithSharedCapture(Method, Payload, State.Capture));
        State.Deadline = FPlatformTime::Seconds() + 10.0;
    }

    void InvokeType(FAutomationTestBase& Test, FState& State, const FString& Text)
    {
        const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("handle"), State.Handle);
        Payload->SetStringField(TEXT("text"), Text);
        Invoke(Test, State, TEXT("drive.type"), Payload);
    }

    // 0 = still waiting, 1 = arrived, -1 = timed out (reported).
    int32 Arrival(FAutomationTestBase& Test, const FState& State)
    {
        if (State.Capture->bWasCalled)
        {
            return 1;
        }
        if (FPlatformTime::Seconds() > State.Deadline)
        {
            Test.AddError(TEXT("The drive verb never answered within 10 s."));
            return -1;
        }
        return 0;
    }

    // A window the fixture does not clear (a toast, a menu, another process's window) takes the
    // pointer at the target, so the action is correctly refused before reaching it. The PIE tests
    // first fail on this editor's own regular windows (FPieViewportClearance), which they clear.
    bool SkipIfHostWindowCovers(FAutomationTestBase& Test, const FTestResponseCapture& Capture, const TCHAR* Reason)
    {
        if (Capture.bSuccess || Capture.ErrorCode != TEXT("TARGET_OCCLUDED"))
        {
            return false;
        }
        FString Occluder;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("occluding_window"), Occluder);
        }
        PinWrightTestSkip::SkipAssertions(Test, Reason,
            FString::Printf(TEXT("A window the fixture does not clear ('%s') covers the target: %s"), *Occluder, *Capture.Message));
        return true;
    }

    // Spawns the fixture once PIE has a player. Returns false while waiting.
    bool Spawn(FAutomationTestBase& Test, FState& State, bool& bOutDone)
    {
        bOutDone = false;
        UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
        UWorld* World = Viewport ? Viewport->GetWorld() : nullptr;
        APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
        if (!GEditor || !GEditor->PlayWorld || !Player)
        {
            if (FPlatformTime::Seconds() >= State.Deadline)
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("owned_pie_player_unavailable"),
                    TEXT("The owned PIE session did not bind a game viewport with a player controller."));
                bOutDone = true;
            }
            return false;
        }

        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        State.ProbeName = FString::Printf(TEXT("PWDriveTypeField_%s"), *Suffix);
        State.Probe.Reset(AddProbe<UEditableText>(Player, State.ProbeName, 900000));
        if (State.bCovered)
        {
            State.Cover.Reset(AddProbe<UButton>(Player, FString::Printf(TEXT("PWDriveTypeCover_%s"), *Suffix), 900001));
        }
        if (!TextOf(State) || (State.bCovered && !State.Cover.IsValid()))
        {
            Test.AddError(TEXT("Could not create the probe widgets."));
            bOutDone = true;
            return false;
        }
        // This editor's own windows over the level viewport would take the focusing click.
        State.Clearance.Clear();
        State.NotBeforeFrame = GFrameCounter + 10;
        State.Deadline = FPlatformTime::Seconds() + 10.0;
        return true;
    }

    // Waits for the field to be painted, then types the first text. Returns false while waiting.
    bool StartFirstType(FAutomationTestBase& Test, FState& State, bool& bOutDone)
    {
        bOutDone = false;
        if (GFrameCounter < State.NotBeforeFrame || !FindTextHandle(State))
        {
            if (GFrameCounter >= State.NotBeforeFrame && FPlatformTime::Seconds() >= State.Deadline)
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-field-not-painted"),
                    TEXT("The PIE text field was never listed with live geometry; this host did not paint the viewport."));
                bOutDone = true;
            }
            return false;
        }
        const TSharedPtr<SEditableText> Live = LiveText(State);
        if (!Test.TestTrue(TEXT("fixture: the field resolves to a live SEditableText"), Live.IsValid())
            || !Test.TestFalse(TEXT("fixture: the field does not hold keyboard focus before the first drive.type"),
                Live->HasKeyboardFocus())
            || !Test.TestTrue(TEXT("fixture: the field starts empty"), TextValue(State).IsEmpty()))
        {
            bOutDone = true;
            return false;
        }
        InvokeType(Test, State, TEXT("Pw"));
        return true;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDriveTypeFocusRunCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveTypeFocusTest::FState>, State);

bool FDriveTypeFocusRunCommand::Update()
{
    using namespace DriveTypeFocusTest;
    FState& S = *State;
    const auto Finish = [&S]() { Release(S); return true; };
    bool bDone = false;

    switch (S.Step)
    {
    case 0:
        if (Spawn(*Test, S, bDone)) { S.Step = 1; }
        return bDone ? Finish() : false;

    case 1:
        if (StartFirstType(*Test, S, bDone)) { S.Step = 2; }
        return bDone ? Finish() : false;

    case 2:
    {
        const int32 Arrived = Arrival(*Test, S);
        if (Arrived == 0) { return false; }
        if (Arrived < 0 || S.Clearance.FailIfOwnWindowStillOccludes(*Test, *S.Capture)
            || SkipIfHostWindowCovers(*Test, *S.Capture, TEXT("pie_viewport_occluded"))) { return Finish(); }
        const FTestResponseCapture& Capture = *S.Capture;

        if (S.bCovered)
        {
            Test->TestFalse(TEXT("drive.type into a field whose click focused another widget is not a success"), Capture.bSuccess);
            Test->TestEqual(TEXT("the refusal is TARGET_NOT_FOCUSED"), Capture.ErrorCode, FString(TEXT("TARGET_NOT_FOCUSED")));
            Test->TestTrue(TEXT("nothing was typed into the field"), TextValue(S).IsEmpty());
            FString Focused;
            Test->TestTrue(TEXT("the refusal names the focused widget"),
                Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("focused_widget"), Focused));
            Test->TestTrue(FString::Printf(TEXT("the focused widget is the covering button (got '%s')"), *Focused),
                Focused.StartsWith(TEXT("SButton")));
            return Finish();
        }

        // Uncovered: the focusing click put the caret in the empty field and the text landed.
        Test->TestTrue(FString::Printf(TEXT("first drive.type succeeds (%s %s)"), *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        Test->TestEqual(TEXT("first drive.type entered its text"), TextValue(S), FString(TEXT("Pw")));
        const TSharedPtr<SEditableText> Live = LiveText(S);
        if (!Test->TestTrue(TEXT("fixture: the field holds keyboard focus after the first drive.type"),
                Live.IsValid() && Live->HasKeyboardFocus()))
        {
            return Finish();
        }
        const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("key"), TEXT("A"));
        Payload->SetStringField(TEXT("modifiers"), TEXT("ctrl"));
        Invoke(*Test, S, TEXT("drive.key"), Payload);
        S.Step = 3;
        return false;
    }

    case 3:
    {
        const int32 Arrived = Arrival(*Test, S);
        if (Arrived == 0) { return false; }
        if (Arrived < 0) { return Finish(); }
        Test->TestTrue(FString::Printf(TEXT("drive.key ctrl+A succeeds (%s %s)"), *S.Capture->ErrorCode, *S.Capture->Message),
            S.Capture->bSuccess);
        const TSharedPtr<SEditableText> Live = LiveText(S);
        if (!Test->TestTrue(TEXT("fixture: ctrl+A selected the whole field"),
                Live.IsValid() && Live->AnyTextSelected() && Live->GetSelectedText().ToString() == TEXT("Pw")))
        {
            return Finish();
        }
        InvokeType(*Test, S, TEXT("Done"));
        S.Step = 4;
        return false;
    }

    case 4:
    {
        const int32 Arrived = Arrival(*Test, S);
        if (Arrived == 0) { return false; }
        if (Arrived < 0 || S.Clearance.FailIfOwnWindowStillOccludes(*Test, *S.Capture)
            || SkipIfHostWindowCovers(*Test, *S.Capture, TEXT("pie_viewport_occluded"))) { return Finish(); }
        Test->TestTrue(FString::Printf(TEXT("second drive.type succeeds (%s %s)"), *S.Capture->ErrorCode, *S.Capture->Message),
            S.Capture->bSuccess);
        Test->TestEqual(TEXT("typing over a selection replaces it (no focusing click cleared it)"),
            TextValue(S), FString(TEXT("Done")));
        return Finish();
    }

    default:
        return Finish();
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDriveTypeFocusCleanupCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveTypeFocusTest::FState>, State);

bool FDriveTypeFocusCleanupCommand::Update()
{
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return true;
    }
    if (State->CleanupDeadline == 0.0)
    {
        State->CleanupDeadline = FPlatformTime::Seconds() + 15.0;
    }
    if (FPlatformTime::Seconds() >= State->CleanupDeadline)
    {
        Test->AddError(TEXT("Timed out waiting for the owned PIE session to stop."));
        return true;
    }
    return false;
}

namespace DriveTypeFocusTest
{
    void Run(FAutomationTestBase& Test, bool bCovered)
    {
        if (!GEditor || !FSlateApplication::IsInitialized())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("editor_or_slate_unavailable"),
                TEXT("Owned PIE requires GEditor and Slate."));
            return;
        }
        if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("preexisting_pie_session"),
                TEXT("The test never borrows or stops ambient PIE."));
            return;
        }
        if (!GEditor->GetActiveViewport())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("level_viewport_unavailable"),
                TEXT("PIE in the level viewport requires an active level viewport."));
            return;
        }

        const TSharedRef<FState> State = MakeShared<FState>();
        State->bCovered = bCovered;
        State->Deadline = FPlatformTime::Seconds() + 20.0;
        ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(&Test));
        ADD_LATENT_AUTOMATION_COMMAND(FDriveTypeFocusRunCommand(&Test, State));
        ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
        ADD_LATENT_AUTOMATION_COMMAND(FDriveTypeFocusCleanupCommand(&Test, State));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveTypeFocusSelectAllSurvivesTypeTest,
    "PinWright.drive.type_focus.PieSelectAllSurvivesType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveTypeFocusSelectAllSurvivesTypeTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    DriveTypeFocusTest::Run(*this, /*bCovered=*/false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveTypeFocusTakenElsewhereIsRefusedTest,
    "PinWright.drive.type_focus.PieFocusTakenElsewhereIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveTypeFocusTakenElsewhereIsRefusedTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    DriveTypeFocusTest::Run(*this, /*bCovered=*/true);
    return true;
}

// ============================================================================
// editor_chrome: focus moved into a popup the click opened counts as the target's focus.
// ============================================================================

namespace DriveTypeFocusPopupTest
{
    // What the button's click creates; the button's lambda holds it, the fixture reads it.
    struct FPopup
    {
        TWeakPtr<SWindow> Parent;
        TSharedPtr<SWindow> Window;
        TSharedPtr<SEditableText> Text;
        bool bFocusSet = false;
    };

    struct FFixture
    {
        FString Title;
        TSharedPtr<SWindow> Window;
        TSharedRef<FPopup> Popup = MakeShared<FPopup>();
        FString ButtonHandle;
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        double Deadline = 0.0;

        ~FFixture()
        {
            if (!FSlateApplication::IsInitialized())
            {
                return;
            }
            if (Window.IsValid())
            {
                // Destroys the popup with it: it is a child window.
                FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
            }
            Popup->Window.Reset();
            Popup->Text.Reset();
            FSlateApplication::Get().Tick(ESlateTickType::All);
        }
    };

    TSharedPtr<FFixture> Build(FAutomationTestBase& Test)
    {
        if (!FSlateApplication::IsInitialized())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("slate_not_initialized"),
                TEXT("FSlateApplication is not initialized; the popup fixture cannot be built."));
            return nullptr;
        }
        TSharedPtr<FFixture> Fixture = MakeShared<FFixture>();
        Fixture->Title = FString::Printf(TEXT("PW_TypeFocusPopup_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        // Clear of every window this editor already shows, as the click_occlusion fixture places it.
        TArray<TSharedRef<SWindow>> Visible;
        FSlateApplication::Get().GetAllVisibleWindowsOrdered(Visible);
        double ClearX = 420.0;
        for (const TSharedRef<SWindow>& Other : Visible)
        {
            ClearX = FMath::Max(ClearX, FVector2D(Other->GetPositionInScreen()).X + FVector2D(Other->GetSizeInScreen()).X + 80.0);
        }
        const FVector2D Position(ClearX, 320.0);

        const TSharedRef<FPopup> Popup = Fixture->Popup;
        Fixture->Window = SNew(SWindow)
            .Title(FText::FromString(Fixture->Title))
            .ScreenPosition(Position)
            .ClientSize(FVector2D(360.0f, 200.0f))
            .FocusWhenFirstShown(false)
            .IsTopmostWindow(true)
            .SupportsMaximize(false)
            .SupportsMinimize(false);
        Fixture->Window->SetContent(
            SNew(SButton)
            .OnClicked_Lambda([Popup, Position]()
            {
                const TSharedPtr<SWindow> Parent = Popup->Parent.Pin();
                if (!Parent.IsValid() || Popup->Window.IsValid())
                {
                    return FReply::Handled();
                }
                Popup->Text = SNew(SEditableText);
                Popup->Window = SNew(SWindow)
                    .Title(FText::FromString(TEXT("PW_TypeFocusPopupChild")))
                    .ScreenPosition(Position + FVector2D(20.0, 220.0))
                    .ClientSize(FVector2D(240.0f, 60.0f))
                    .IsTopmostWindow(true)
                    .SupportsMaximize(false)
                    .SupportsMinimize(false)
                    [
                        Popup->Text.ToSharedRef()
                    ];
                FSlateApplication& SlateApp = FSlateApplication::Get();
                SlateApp.AddWindowAsNativeChild(Popup->Window.ToSharedRef(), Parent.ToSharedRef(), /*bShowImmediately=*/true);
                Popup->bFocusSet = SlateApp.SetUserFocus(SlateApp.GetUserIndexForKeyboard(), Popup->Text, EFocusCause::SetDirectly);
                return FReply::Handled();
            })
            [
                SNew(STextBlock).Text(FText::FromString(TEXT("PwTypeFocusOpenPopup")))
            ]);
        Popup->Parent = Fixture->Window;
        FSlateApplication::Get().AddWindow(Fixture->Window.ToSharedRef(), /*bShowImmediately=*/true);
        for (int32 Tick = 0; Tick < 8; ++Tick)
        {
            FSlateApplication::Get().Tick(ESlateTickType::All);
        }

        FDriveWindowSelector Selector;
        Selector.Title = Fixture->Title;
        TArray<FDriveElement> Elements;
        FString WindowTitle;
        FString ErrorCode;
        FString ErrorMessage;
        if (!FDriveEditorChrome::BuildElementList(Selector, Elements, WindowTitle, ErrorCode, ErrorMessage))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-window-not-enumerated"),
                FString::Printf(TEXT("The fixture window could not be walked: %s - %s"), *ErrorCode, *ErrorMessage));
            return nullptr;
        }
        for (const FDriveElement& Element : Elements)
        {
            if (Element.Type == TEXT("SButton") && !Element.bGeometryStale && !Element.Handle.Contains(TEXT("SWindowTitleBar")))
            {
                Fixture->ButtonHandle = Element.Handle;
                break;
            }
        }
        if (Fixture->ButtonHandle.IsEmpty())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-button-not-painted"),
                TEXT("The fixture button was not listed with live geometry; this host did not paint the window."));
            return nullptr;
        }
        return Fixture;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FDriveTypeFocusPopupPoll, TFunction<bool()>, Poll);

bool FDriveTypeFocusPopupPoll::Update()
{
    return Poll();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveTypeFocusChromePopupFocusIsAcceptedTest,
    "PinWright.drive.type_focus.ChromePopupFocusIsAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveTypeFocusChromePopupFocusIsAcceptedTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    using namespace DriveTypeFocusPopupTest;

    TSharedPtr<FFixture> Fixture = Build(*this);
    if (!Fixture.IsValid())
    {
        return true;
    }

    const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("handle"), Fixture->ButtonHandle);
    Payload->SetStringField(TEXT("text"), TEXT("PwPopup"));
    Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
    Payload->SetStringField(TEXT("window_title"), Fixture->Title);
    TestTrue(TEXT("drive.type handler found"), InvokeHandlerWithSharedCapture(TEXT("drive.type"), Payload, Fixture->Capture));
    Fixture->Deadline = FPlatformTime::Seconds() + 10.0;

    ADD_LATENT_AUTOMATION_COMMAND(FDriveTypeFocusPopupPoll([this, Fixture]() -> bool
    {
        const FTestResponseCapture& Capture = *Fixture->Capture;
        if (!Capture.bWasCalled)
        {
            if (FPlatformTime::Seconds() <= Fixture->Deadline)
            {
                return false;
            }
            AddError(TEXT("drive.type never answered within 10 s."));
            return true;
        }
        if (DriveTypeFocusTest::SkipIfHostWindowCovers(*this, Capture, TEXT("chrome_fixture_occluded")))
        {
            return true;
        }
        const TSharedPtr<SEditableText> Text = Fixture->Popup->Text;
        if (!TestTrue(TEXT("fixture: the click opened the popup with its field"), Text.IsValid())
            || !TestTrue(TEXT("fixture: the popup moved keyboard focus to its field"), Fixture->Popup->bFocusSet))
        {
            return true;
        }
        TestTrue(FString::Printf(TEXT("drive.type through a click-opened popup succeeds (%s %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess);
        TestEqual(TEXT("the text landed in the popup's field"), Text->GetText().ToString(), FString(TEXT("PwPopup")));
        return true;
    }));
    return true;
}
