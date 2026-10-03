// Copyright (c) 2026 Alexander Penkin. MIT License.

// B-drive-click-dead-after-gamepad-key: a gamepad key (drive.key Gamepad_*) switches CommonInput's
// per-player input type to Gamepad, which also moves the Slate cursor user onto a faux cursor.
// A real mouse switches it back through FCommonInputPreprocessor, but that preprocessor drops a
// synthetic pointer event unless the player's game viewport is in the Slate user's focus path, so
// drive.click left the player in Gamepad mode. Mouse injection now switches it back itself.
//
// The failure direction is the input type after drive.click: with the game viewport out of the
// focus path (where an agent-driven editor usually leaves it) it stays Gamepad without the fix.
// CommonInput is an optional engine plugin, reached by reflection; a host without it skips.
// So does a host whose CommonInput refuses the Gamepad type (PDS's ULyraInputUserSettings
// reports it unsupported), where no gamepad key can switch it.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
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
#include "Widgets/SViewport.h"

namespace DriveClickGamepadKeyTestHelpers
{
    // ECommonInputType (CommonInputTypeEnum.h).
    constexpr uint8 MouseAndKeyboard = 0;
    constexpr uint8 Gamepad = 1;

    enum class EStep : uint8
    {
        CreateProbe,
        BaselineClick,
        AwaitBaseline,
        GamepadKey,
        Click,
        AwaitClick,
        Done
    };

    struct FState
    {
        TStrongObjectPtr<UUserWidget> Probe;
        TWeakObjectPtr<ULocalPlayer> Player;
        FString ProbeName;
        FString Handle;
        TSharedPtr<FTestResponseCapture> Capture;
        TSharedRef<int32> Clicks = MakeShared<int32>(0);
        FPieViewportClearance Clearance;
        EStep Step = EStep::CreateProbe;
        uint64 WaitUntilFrame = 0;
        double Deadline = 0.0;
        double CleanupDeadline = 0.0;
    };

    UObject* FindCommonInputSubsystem(const ULocalPlayer* Player)
    {
        UClass* SubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonInput.CommonInputSubsystem"));
        return SubsystemClass && Player ? Player->GetSubsystemBase(SubsystemClass) : nullptr;
    }

    // GetCurrentInputType / SetCurrentInputType each carry one ECommonInputType, so a uint8 is the
    // whole parameter block.
    uint8 GetInputType(UObject* Subsystem)
    {
        uint8 Type = 0xFF;
        if (UFunction* Fn = Subsystem ? Subsystem->FindFunction(TEXT("GetCurrentInputType")) : nullptr)
        {
            Subsystem->ProcessEvent(Fn, &Type);
        }
        return Type;
    }

    void SetInputType(UObject* Subsystem, uint8 Type)
    {
        if (UFunction* Fn = Subsystem ? Subsystem->FindFunction(TEXT("SetCurrentInputType")) : nullptr)
        {
            Subsystem->ProcessEvent(Fn, &Type);
        }
    }

    TSharedRef<FJsonObject> ClickPayload(const FState& State)
    {
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("handle"), State.Handle);
        Payload->SetStringField(TEXT("instance_name"), State.ProbeName);
        Payload->SetNumberField(TEXT("settle_budget_ms"), 300);
        Payload->SetNumberField(TEXT("quiet_budget_ms"), 100);
        return Payload;
    }

    void Finish(FState& State)
    {
        // Gamepad mode swaps the editor's cursor user onto a faux cursor; never leave it there.
        if (UObject* Subsystem = FindCommonInputSubsystem(State.Player.Get()))
        {
            if (GetInputType(Subsystem) == Gamepad)
            {
                SetInputType(Subsystem, MouseAndKeyboard);
            }
        }
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().UsePlatformCursorForCursorUser(true);
        }
        State.Clearance.Restore();
        // Outered to the PIE game instance: detach and drop BEFORE FEndPlayMapCommand.
        if (State.Probe.IsValid())
        {
            State.Probe->RemoveFromParent();
            State.Probe.Reset();
        }
        State.Step = EStep::Done;
    }

    // Answered drive.click: true to continue; false after reporting (Finish already called).
    bool CheckClickAnswered(FAutomationTestBase& Test, FState& State, const TCHAR* What)
    {
        if (State.Clearance.FailIfOwnWindowStillOccludes(Test, *State.Capture))
        {
            Finish(State);
            return false;
        }
        if (State.Capture->ErrorCode == ErrorCodes::ERR_TARGET_OCCLUDED)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("pie_viewport_occluded"),
                FString::Printf(TEXT("A window the fixture does not clear (a toast, another process's) covers the PIE viewport: %s"), *State.Capture->Message));
            Finish(State);
            return false;
        }
        if (!Test.TestTrue(*FString::Printf(TEXT("%s: drive.click succeeds (%s %s)"), What,
                *State.Capture->ErrorCode, *State.Capture->Message), State.Capture->bSuccess))
        {
            Finish(State);
            return false;
        }
        return true;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRunDriveClickAfterGamepadKeyCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveClickGamepadKeyTestHelpers::FState>, State);

bool FRunDriveClickAfterGamepadKeyCommand::Update()
{
    using namespace DriveClickGamepadKeyTestHelpers;
    if (GFrameCounter < State->WaitUntilFrame)
    {
        return false;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();

    switch (State->Step)
    {
    case EStep::CreateProbe:
    {
        UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
        UWorld* World = Viewport ? Viewport->GetWorld() : nullptr;
        APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
        if (!GEditor || !GEditor->PlayWorld || !Player || !Player->GetLocalPlayer())
        {
            if (FPlatformTime::Seconds() < State->Deadline)
            {
                return false;
            }
            PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_player_unavailable"),
                TEXT("The owned PIE session did not bind a game viewport with a local player."));
            Finish(*State);
            return true;
        }
        State->Player = Player->GetLocalPlayer();
        if (!FindCommonInputSubsystem(State->Player.Get()))
        {
            PinWrightTestSkip::SkipAssertions(*Test, TEXT("commoninput_unavailable"),
                TEXT("The CommonInput plugin is not enabled on this host, so there is no input type to switch."));
            Finish(*State);
            return true;
        }
        State->ProbeName = FString::Printf(TEXT("PWDriveGamepadProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UUserWidget* Probe = CreateWidget<UUserWidget>(Player, UTestWidgetWithStructBIE::StaticClass(), FName(*State->ProbeName));
        if (!Probe || !Probe->WidgetTree)
        {
            Test->AddError(TEXT("Could not create the probe UUserWidget."));
            Finish(*State);
            return true;
        }
        Probe->WidgetTree->RootWidget =
            Probe->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), FName(TEXT("PWDriveGamepadProbeButton")));
        State->Probe.Reset(Probe);
        Probe->AddToViewport(900000);
        // This editor's own windows over the level viewport would take the clicks (the probe fills it).
        State->Clearance.Clear();
        State->Step = EStep::BaselineClick;
        State->WaitUntilFrame = GFrameCounter + 10;
        return false;
    }

    case EStep::BaselineClick:
    {
        FDriveRootSelector Selector;
        Selector.InstanceName = State->ProbeName;
        TArray<FDriveElement> Elements;
        FString RootName;
        FString Code;
        FString Message;
        const FDriveElement* Button = nullptr;
        if (FDriveLiveResolver::BuildElementList(Selector, Elements, RootName, Code, Message))
        {
            Button = Elements.FindByPredicate([](const FDriveElement& E) { return E.Type.Contains(TEXT("SButton")); });
        }
        const UButton* ProbeButton = Cast<UButton>(State->Probe->WidgetTree->RootWidget);
        const TSharedPtr<SWidget> Slate = ProbeButton ? ProbeButton->GetCachedWidget() : nullptr;
        if (!Test->TestNotNull(*FString::Printf(TEXT("probe button is listed (%s %s)"), *Code, *Message), Button)
            || !Test->TestTrue(TEXT("precondition: the probe UButton is backed by an SButton"),
                Slate.IsValid() && Slate->GetType() == FName(TEXT("SButton"))))
        {
            Finish(*State);
            return true;
        }
        State->Handle = Button->Handle;
        // Count clicks at the Slate button: the probe needs no UFUNCTION to observe one.
        const TSharedRef<int32> Clicks = State->Clicks;
        StaticCastSharedPtr<SButton>(Slate)->SetOnClicked(FOnClicked::CreateLambda([Clicks]()
        {
            ++*Clicks;
            return FReply::Handled();
        }));

        State->Capture = MakeShared<FTestResponseCapture>();
        Test->TestTrue(TEXT("drive.click handler found"),
            InvokeHandlerWithSharedCapture(TEXT("drive.click"), ClickPayload(*State), State->Capture.ToSharedRef()));
        State->Deadline = FPlatformTime::Seconds() + 10.0;
        State->Step = EStep::AwaitBaseline;
        return false;
    }

    case EStep::AwaitBaseline:
    {
        if (!State->Capture->bWasCalled)
        {
            if (FPlatformTime::Seconds() < State->Deadline)
            {
                return false;
            }
            Test->AddError(TEXT("The baseline drive.click never answered within 10 s."));
            Finish(*State);
            return true;
        }
        if (!CheckClickAnswered(*Test, *State, TEXT("baseline")))
        {
            return true;
        }
        // Fixture precondition: a click lands before any gamepad key, so a later miss is the key's.
        if (!Test->TestEqual(TEXT("precondition: the baseline click fires the probe button"), *State->Clicks, 1))
        {
            Finish(*State);
            return true;
        }
        State->Step = EStep::GamepadKey;
        return false;
    }

    case EStep::GamepadKey:
    {
        UObject* Subsystem = FindCommonInputSubsystem(State->Player.Get());
        UGameViewportClient* ViewportClient = State->Player.IsValid() ? State->Player->ViewportClient.Get() : nullptr;
        const TSharedPtr<SViewport> ViewportWidget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
        const int32 SlateUser = SlateApp.GetUserIndexForKeyboard();
        if (!Test->TestTrue(TEXT("precondition: the PIE player has a game viewport widget"), ViewportWidget.IsValid()))
        {
            Finish(*State);
            return true;
        }
        // CommonInput only applies an input-type change from a viewport in the focus path (PIE).
        SlateApp.SetUserFocus(SlateUser, ViewportWidget, EFocusCause::SetDirectly);
        // What drive.key sends for a key with no handle.
        FDriveInput::PressKey(EKeys::Gamepad_Special_Right);
        if (GetInputType(Subsystem) != Gamepad)
        {
            // A host can refuse the Gamepad type outright through
            // UCommonInputSubsystem::GetOnPlatformInputSupportOverride (PDS's
            // ULyraInputUserSettings does), and then no gamepad key can ever switch it. Tell that
            // apart from a key that never reached CommonInput by setting the type directly.
            SetInputType(Subsystem, Gamepad);
            if (GetInputType(Subsystem) != Gamepad)
            {
                PinWrightTestSkip::SkipAssertions(*Test, TEXT("commoninput_gamepad_unsupported_by_host"),
                    TEXT("This host's CommonInput refuses the Gamepad input type even when set directly (a platform input support override), so there is no Gamepad state for drive.click to leave."));
                Finish(*State);
                return true;
            }
            SetInputType(Subsystem, MouseAndKeyboard);
        }
        if (!Test->TestEqual(TEXT("precondition: the injected gamepad key switched CommonInput to Gamepad"),
                GetInputType(Subsystem), Gamepad))
        {
            Finish(*State);
            return true;
        }
        // The agent-driven state: keyboard focus left the game viewport (editor chrome, a dialog).
        SlateApp.ClearUserFocus(SlateUser, EFocusCause::SetDirectly);

        // Mouse input that is not over this player's viewport (editor chrome just outside it) must
        // leave the player alone, as a real mouse there would.
        const FVector2D Outside = FVector2D(ViewportWidget->GetCachedGeometry().GetAbsolutePosition()) - FVector2D(4.0, 4.0);
        const FWidgetPath OutsidePath = SlateApp.LocateWindowUnderMouse(Outside, SlateApp.GetInteractiveTopLevelWindows());
        if (!Test->TestTrue(TEXT("precondition: a point just outside the PIE viewport is editor chrome"),
                OutsidePath.IsValid() && !OutsidePath.ContainsWidget(ViewportWidget.Get())))
        {
            Finish(*State);
            return true;
        }
        FDriveInput::MoveTo(Outside);
        if (!Test->TestEqual(TEXT("mouse input outside the player's viewport leaves its Gamepad input type"),
                GetInputType(Subsystem), Gamepad))
        {
            Finish(*State);
            return true;
        }
        State->Step = EStep::Click;
        State->WaitUntilFrame = GFrameCounter + 3;
        return false;
    }

    case EStep::Click:
    {
        Test->TestTrue(TEXT("drive.click handler found"),
            InvokeHandlerWithSharedCapture(TEXT("drive.click"), ClickPayload(*State), State->Capture.ToSharedRef()));
        State->Deadline = FPlatformTime::Seconds() + 10.0;
        State->Step = EStep::AwaitClick;
        return false;
    }

    case EStep::AwaitClick:
    {
        if (!State->Capture->bWasCalled)
        {
            if (FPlatformTime::Seconds() < State->Deadline)
            {
                return false;
            }
            Test->AddError(TEXT("drive.click after the gamepad key never answered within 10 s."));
            Finish(*State);
            return true;
        }
        if (!CheckClickAnswered(*Test, *State, TEXT("after the gamepad key")))
        {
            return true;
        }
        Test->TestEqual(TEXT("drive.click returned CommonInput to MouseAndKeyboard, as a real mouse does"),
            GetInputType(FindCommonInputSubsystem(State->Player.Get())), MouseAndKeyboard);
        Test->TestEqual(TEXT("the click after the gamepad key fires the probe button"), *State->Clicks, 2);
        Finish(*State);
        return true;
    }

    case EStep::Done:
    default:
        return true;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCleanupDriveClickAfterGamepadKeyCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveClickGamepadKeyTestHelpers::FState>, State);

bool FCleanupDriveClickAfterGamepadKeyCommand::Update()
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveClickAfterGamepadKeyTest,
    "PinWright.drive.click_after_gamepad_key.ReturnsCommonInputToMouse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveClickAfterGamepadKeyTest::RunTest(const FString& Parameters)
{
    using DriveClickGamepadKeyTestHelpers::FState;
    if (!GEditor || !FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor_or_slate_unavailable"),
            TEXT("Owned PIE requires GEditor and Slate."));
        return true;
    }
    if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The test never borrows or stops ambient PIE."));
        return true;
    }
    if (!GEditor->GetActiveViewport())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level_viewport_unavailable"),
            TEXT("PIE in the level viewport requires an active level viewport."));
        return true;
    }

    const TSharedRef<FState> State = MakeShared<FState>();
    State->Deadline = FPlatformTime::Seconds() + 20.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FRunDriveClickAfterGamepadKeyCommand(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupDriveClickAfterGamepadKeyCommand(this, State));
    return true;
}
