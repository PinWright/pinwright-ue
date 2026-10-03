// Copyright (c) 2026 Alexander Penkin. MIT License.

// B-drive-hover-no-umg-mouse-enter: drive.hover entered a PIE UUserWidget, but Slate's per-frame
// synthetic cursor move re-hit-tests without the inactive-input flag the injection held. With the
// application inactive and another application's window over the point
// (IsCursorDirectlyOverSlateWindow false), that move routes an empty path and sends OnMouseLeave
// one frame later, so a hover-driven animation never shows. HoverAt now holds the flag until the
// cursor leaves the point.
//
// Under -RenderOffScreen the platform application is FNullApplication, whose
// IsCursorDirectlyOverSlateWindow is always true, so the leave itself cannot be reproduced here.
// The failure direction is the hold: without the fix the flag is back to its prior value as soon
// as drive.hover's injection returns.
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

namespace DriveHoverHoldTestHelpers
{
    enum class EStep : uint8
    {
        CreateProbe,
        ParkAway,
        StartHover,
        AwaitHover,
        HoldFrames,
        LeavePoint,
        Done
    };

    struct FState
    {
        TStrongObjectPtr<UUserWidget> Probe;
        FString ProbeName;
        FString Handle;
        FVector2D Away = FVector2D::ZeroVector;
        TSharedPtr<FTestResponseCapture> Capture;
        FPieViewportClearance Clearance;
        bool bPrevHandleInactive = false;
        EStep Step = EStep::CreateProbe;
        uint64 WaitUntilFrame = 0;
        double Deadline = 0.0;
        double CleanupDeadline = 0.0;
    };

    void Finish(FState& State)
    {
        // Outered to the PIE game instance: detach and drop BEFORE FEndPlayMapCommand.
        if (State.Probe.IsValid())
        {
            State.Probe->RemoveFromParent();
            State.Probe.Reset();
        }
        FDriveInput::ReleaseHoverHold();
        State.Clearance.Restore();
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(State.bPrevHandleInactive);
        }
        State.Step = EStep::Done;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRunDriveHoverHoldCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveHoverHoldTestHelpers::FState>, State);

bool FRunDriveHoverHoldCommand::Update()
{
    using namespace DriveHoverHoldTestHelpers;
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
        if (!GEditor || !GEditor->PlayWorld || !Player)
        {
            if (FPlatformTime::Seconds() < State->Deadline)
            {
                return false;
            }
            PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_player_unavailable"),
                TEXT("The owned PIE session did not bind a game viewport with a player controller."));
            Finish(*State);
            return true;
        }
        State->ProbeName = FString::Printf(TEXT("PWDriveHoverProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UUserWidget* Probe = CreateWidget<UUserWidget>(Player, UTestWidgetWithStructBIE::StaticClass(), FName(*State->ProbeName));
        if (!Probe || !Probe->WidgetTree)
        {
            Test->AddError(TEXT("Could not create the probe UUserWidget."));
            Finish(*State);
            return true;
        }
        Probe->WidgetTree->RootWidget =
            Probe->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), FName(TEXT("PWDriveHoverProbeButton")));
        State->Probe.Reset(Probe);
        Probe->AddToViewport(900000);
        // This editor's own windows over the level viewport would take the hover (the probe fills it).
        State->Clearance.Clear();
        State->Step = EStep::ParkAway;
        State->WaitUntilFrame = GFrameCounter + 10;
        return false;
    }

    case EStep::ParkAway:
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
        if (!Test->TestNotNull(*FString::Printf(TEXT("probe button is listed (%s %s)"), *Code, *Message), Button))
        {
            Finish(*State);
            return true;
        }
        State->Handle = Button->Handle;
        // Just outside the probe, which fills the PIE viewport: still inside the editor window.
        State->Away = Button->AbsolutePosition - FVector2D(4.0, 4.0);
        FDriveInput::MoveTo(State->Away);
        State->Step = EStep::StartHover;
        State->WaitUntilFrame = GFrameCounter + 2;
        return false;
    }

    case EStep::StartHover:
    {
        // Fixture precondition: nothing is hovered or held before the verb runs.
        if (!Test->TestFalse(TEXT("precondition: probe is not hovered with the cursor parked outside it"),
                State->Probe->IsHovered())
            || !Test->TestFalse(TEXT("precondition: no hover hold is active"), FDriveInput::IsHoverHeld()))
        {
            Finish(*State);
            return true;
        }
        SlateApp.SetHandleDeviceInputWhenApplicationNotActive(false);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("handle"), State->Handle);
        Payload->SetStringField(TEXT("instance_name"), State->ProbeName);
        Payload->SetNumberField(TEXT("settle_budget_ms"), 300);
        Payload->SetNumberField(TEXT("quiet_budget_ms"), 100);
        State->Capture = MakeShared<FTestResponseCapture>();
        Test->TestTrue(TEXT("drive.hover handler found"),
            InvokeHandlerWithSharedCapture(TEXT("drive.hover"), Payload, State->Capture.ToSharedRef()));
        State->Deadline = FPlatformTime::Seconds() + 10.0;
        State->Step = EStep::AwaitHover;
        return false;
    }

    case EStep::AwaitHover:
    {
        if (!State->Capture->bWasCalled)
        {
            if (FPlatformTime::Seconds() < State->Deadline)
            {
                return false;
            }
            Test->AddError(TEXT("drive.hover never answered within 10 s."));
            Finish(*State);
            return true;
        }
        if (State->Clearance.FailIfOwnWindowStillOccludes(*Test, *State->Capture))
        {
            Finish(*State);
            return true;
        }
        if (State->Capture->ErrorCode == ErrorCodes::ERR_TARGET_OCCLUDED)
        {
            PinWrightTestSkip::SkipAssertions(*Test, TEXT("pie_viewport_occluded"),
                FString::Printf(TEXT("A window the fixture does not clear (a toast, another process's) covers the PIE viewport: %s"), *State->Capture->Message));
            Finish(*State);
            return true;
        }
        if (!Test->TestTrue(*FString::Printf(TEXT("drive.hover succeeds (%s %s)"),
                *State->Capture->ErrorCode, *State->Capture->Message), State->Capture->bSuccess))
        {
            Finish(*State);
            return true;
        }
        // Several frames of Slate's synthetic cursor moves after the verb answered.
        State->Step = EStep::HoldFrames;
        State->WaitUntilFrame = GFrameCounter + 5;
        return false;
    }

    case EStep::HoldFrames:
    {
        Test->TestTrue(TEXT("the PIE UUserWidget got OnMouseEnter and is still hovered frames later"),
            State->Probe->IsHovered());
        Test->TestTrue(TEXT("drive.hover holds the hover after its injection returned"), FDriveInput::IsHoverHeld());
        Test->TestTrue(TEXT("the inactive-input flag is held so synthetic moves keep the hovered path"),
            SlateApp.GetHandleDeviceInputWhenApplicationNotActive());

        FDriveInput::MoveTo(State->Away);
        State->Step = EStep::LeavePoint;
        State->WaitUntilFrame = GFrameCounter + 3;
        return false;
    }

    case EStep::LeavePoint:
    {
        Test->TestFalse(TEXT("moving the cursor off the point releases the hold"), FDriveInput::IsHoverHeld());
        Test->TestFalse(TEXT("the released hold restores the flag's prior value"),
            SlateApp.GetHandleDeviceInputWhenApplicationNotActive());
        Test->TestFalse(TEXT("the probe got OnMouseLeave"), State->Probe->IsHovered());
        Finish(*State);
        return true;
    }

    case EStep::Done:
    default:
        return true;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCleanupDriveHoverHoldCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveHoverHoldTestHelpers::FState>, State);

bool FCleanupDriveHoverHoldCommand::Update()
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveHoverHoldPieUserWidgetTest,
    "PinWright.drive.hover_hold.PieUserWidgetStaysHovered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveHoverHoldPieUserWidgetTest::RunTest(const FString& Parameters)
{
    using DriveHoverHoldTestHelpers::FState;
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
    State->bPrevHandleInactive = FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
    State->Deadline = FPlatformTime::Seconds() + 20.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FRunDriveHoverHoldCommand(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupDriveHoverHoldCommand(this, State));
    return true;
}
