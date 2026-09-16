// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveGameInput.h"

#include "Handlers/Editor/PieWorldSelector.h"
#include "Handlers/ErrorCodes.h"

#include "Compat/EngineVersionCompat.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputKeyEventArgs.h"
#include "KeyState.h"
#include "UnrealClient.h"
#include "UnrealEngine.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"

// Uniquely-named (not anonymous) namespace: Unity merges this TU with its siblings, and a
// shared unnamed namespace would collide with same-named helpers elsewhere in the cluster.
namespace DriveGameInputLocal
{
    // The input device the target player's controller accepts. APlayerController::InputKey
    // drops events whose device maps to a different platform user when
    // UInputSettings::bFilterInputByPlatformUser is on, so the device is derived from the
    // local player rather than assumed to be device 0.
    FInputDeviceId ResolveInputDevice(const ULocalPlayer* LocalPlayer)
    {
        IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
        if (LocalPlayer)
        {
            const FInputDeviceId DeviceId = Mapper.GetPrimaryInputDeviceForUser(LocalPlayer->GetPlatformUserId());
            if (DeviceId.IsValid())
            {
                return DeviceId;
            }
        }
        return Mapper.GetDefaultInputDevice();
    }

    FDriveGameInputCandidate DescribeForSelection(const FDriveGameInputTarget& Target,
        const UGameViewportClient* CurrentViewport)
    {
        FDriveGameInputCandidate Candidate;
        Candidate.bHasViewportClient = Target.ViewportClient != nullptr;
        Candidate.bHasPlayerController = Target.PlayerController != nullptr;
        Candidate.bIsCurrentViewport = Target.ViewportClient != nullptr
            && CurrentViewport != nullptr
            && CurrentViewport == Target.ViewportClient;
        return Candidate;
    }

    TArray<FDriveGameInputSelectionCandidate> GatherCandidates(
        const TArray<PieWorldSelector::FPieContextInfo>& Contexts,
        const UGameViewportClient* CurrentViewport)
    {
        TArray<FDriveGameInputSelectionCandidate> Candidates;
        Candidates.Reserve(Contexts.Num());
        if (!GEngine)
        {
            return Candidates;
        }

        for (const PieWorldSelector::FPieContextInfo& Context : Contexts)
        {
            FDriveGameInputSelectionCandidate Candidate;
            Candidate.Context = Context;
            FDriveGameInputTarget& Target = Candidate.Target;
            Target.World = Context.World;
            Target.PieInstance = Context.PieInstance;
            Target.NetMode = Context.NetMode;
            Target.ViewportClient = Context.World ? Context.World->GetGameViewport() : nullptr;

            if (Target.ViewportClient)
            {
                Target.LocalPlayer = GEngine->GetFirstGamePlayer(Target.ViewportClient);
            }
            if (!Target.LocalPlayer && Target.World)
            {
                Target.LocalPlayer = Target.World->GetFirstLocalPlayerFromController();
            }
            Target.PlayerController = Target.LocalPlayer ? Target.LocalPlayer->PlayerController : nullptr;
            if (!Target.PlayerController && Target.World)
            {
                Target.PlayerController = GEngine->GetFirstLocalPlayerController(Target.World);
            }
            Candidate.Priority = DescribeForSelection(Target, CurrentViewport);
            Candidates.Add(Candidate);
        }
        return Candidates;
    }

    TArray<PieWorldSelector::FPieContextInfo> SelectorContexts(
        const TArray<FDriveGameInputSelectionCandidate>& Candidates)
    {
        TArray<PieWorldSelector::FPieContextInfo> Contexts;
        Contexts.Reserve(Candidates.Num());
        for (const FDriveGameInputSelectionCandidate& Candidate : Candidates)
        {
            Contexts.Add(Candidate.Context);
        }
        return Contexts;
    }

    TArray<FDriveGameInputCandidate> SelectionPriorities(
        const TArray<FDriveGameInputSelectionCandidate>& Candidates)
    {
        TArray<FDriveGameInputCandidate> Priorities;
        Priorities.Reserve(Candidates.Num());
        for (const FDriveGameInputSelectionCandidate& Candidate : Candidates)
        {
            Priorities.Add(Candidate.Priority);
        }
        return Priorities;
    }

    class FPendingGameKeyRequest final
        : public FDriveGameInputRequest
        , public TSharedFromThis<FPendingGameKeyRequest>
    {
    public:
        FPendingGameKeyRequest(const FDriveGameInputTarget& Target, const FKey& InKey,
            EDriveKeyAction Action, TFunction<void(const FDriveGameKeyObservation&)> InCompletion)
            : PieInstance(Target.PieInstance)
            , Completion(MoveTemp(InCompletion))
        {
            Observation.World = Target.World;
            Observation.ViewportClient = Target.ViewportClient;
            Observation.PlayerController = Target.PlayerController;
            Observation.PlayerInput = Target.PlayerController ? Target.PlayerController->PlayerInput : nullptr;
            Observation.Key = InKey;
            Observation.InputEvent = Action == EDriveKeyAction::Up ? IE_Released : IE_Pressed;
            Observation.InputDevice = Target.InputDevice.IsValid()
                ? Target.InputDevice
                : ResolveInputDevice(Target.LocalPlayer);
        }

        void Start()
        {
            // Delegate AddSP bindings are weak. Keep the request alive until one terminal path
            // removes every binding and releases this explicit self-reference.
            SelfKeepAlive = AsShared();
            Deadline = FPlatformTime::Seconds() + 2.0;
            PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddSP(
                AsShared(), &FPendingGameKeyRequest::OnWorldPostActorTick);
            WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddSP(
                AsShared(), &FPendingGameKeyRequest::OnWorldCleanup);
            EndPieHandle = FEditorDelegates::EndPIE.AddSP(
                AsShared(), &FPendingGameKeyRequest::OnEndPie);
            DeadlineHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateSP(AsShared(), &FPendingGameKeyRequest::OnDeadline));
        }

        virtual void Cancel(const FString& Route) override
        {
            Complete(Route);
        }

    private:
        bool IdentityIsCurrent() const
        {
            UWorld* World = Observation.World.Get();
            UGameViewportClient* ViewportClient = Observation.ViewportClient.Get();
            APlayerController* PlayerController = Observation.PlayerController.Get();
            UPlayerInput* PlayerInput = Observation.PlayerInput.Get();
            return World && World->WorldType == EWorldType::PIE
                && World->GetPackage()->GetPIEInstanceID() == PieInstance
                && ViewportClient && ViewportClient->GetWorld() == World
                && PlayerController && PlayerController->GetWorld() == World
                && PlayerInput && PlayerController->PlayerInput == PlayerInput;
        }

        void OnWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
        {
            if (bTerminal || World != Observation.World.Get())
            {
                return;
            }
            if (!IdentityIsCurrent())
            {
                Complete(TEXT("pie_ended_before_observation"));
                return;
            }

            UPlayerInput* PlayerInput = Observation.PlayerInput.Get();
            if (Phase == EPhase::AwaitInjection)
            {
                FKeyState* BeforeState = PlayerInput->GetKeyState(Observation.Key);
                const int32 BeforeCount = BeforeState
                    ? BeforeState->EventAccumulator[Observation.InputEvent].Num()
                    : 0;
                UGameViewportClient* ViewportClient = Observation.ViewportClient.Get();
                FScopedConditionalWorldSwitcher WorldSwitcher(ViewportClient);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
                const FInputKeyEventArgs Args(ViewportClient->Viewport, Observation.InputDevice,
                    Observation.Key, Observation.InputEvent,
                    Observation.InputEvent == IE_Released ? 0.0f : 1.0f,
                    /*bIsTouchEvent*/ false, FPlatformTime::Cycles64());
#else
                const FInputKeyEventArgs Args(ViewportClient->Viewport, Observation.InputDevice,
                    Observation.Key, Observation.InputEvent,
                    Observation.InputEvent == IE_Released ? 0.0f : 1.0f,
                    /*bIsTouchEvent*/ false);
#endif
                Observation.InjectionFrame = GFrameCounter;
                Observation.bHandled = ViewportClient->InputKey(Args);
                FKeyState* AfterState = PlayerInput->GetKeyState(Observation.Key);
                if (AfterState
                    && AfterState->EventAccumulator[Observation.InputEvent].Num() > BeforeCount)
                {
                    Observation.bEventQueued = true;
                    Observation.EventId = AfterState->EventAccumulator[Observation.InputEvent].Last();
                }
                Phase = EPhase::AwaitObservation;
                return;
            }

            const FKeyState* KeyState = PlayerInput->GetKeyState(Observation.Key);
            const bool bExactEventProcessed = Observation.bEventQueued && KeyState
                && KeyState->EventCounts[Observation.InputEvent].Contains(Observation.EventId);
            if (Observation.InputEvent == IE_Pressed)
            {
                Observation.bDeliveredToGame = bExactEventProcessed
                    && PlayerInput->WasJustPressed(Observation.Key)
                    && PlayerInput->IsPressed(Observation.Key);
            }
            else
            {
                Observation.bDeliveredToGame = bExactEventProcessed
                    && PlayerInput->WasJustReleased(Observation.Key)
                    && !PlayerInput->IsPressed(Observation.Key);
            }

            if (Observation.bDeliveredToGame)
            {
                Complete(TEXT("player_input"));
            }
            else if (Observation.bHandled && !Observation.bEventQueued)
            {
                Complete(TEXT("viewport_client_before_player_input"));
            }
            else if (Observation.bEventQueued)
            {
                Complete(TEXT("player_input_not_processed"));
            }
            else
            {
                Complete(TEXT("none"));
            }
        }

        void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
        {
            if (World == Observation.World.Get())
            {
                Complete(TEXT("pie_ended_before_observation"));
            }
        }

        void OnEndPie(bool bSimulating)
        {
            Complete(TEXT("pie_ended_before_observation"));
        }

        bool OnDeadline(float DeltaSeconds)
        {
            if (FPlatformTime::Seconds() >= Deadline)
            {
                Complete(TEXT("player_input_not_processed"));
                return false;
            }
            return !bTerminal;
        }

        void Complete(const FString& Route)
        {
            if (bTerminal)
            {
                return;
            }
            bTerminal = true;
            const TSharedPtr<FPendingGameKeyRequest> KeepAliveUntilReturn = SelfKeepAlive;
            SelfKeepAlive.Reset();
            Observation.ConsumingRoute = Route;
            FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
            FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
            FEditorDelegates::EndPIE.Remove(EndPieHandle);
            if (DeadlineHandle.IsValid())
            {
                FTSTicker::GetCoreTicker().RemoveTicker(DeadlineHandle);
                DeadlineHandle.Reset();
            }
            TFunction<void(const FDriveGameKeyObservation&)> LocalCompletion = MoveTemp(Completion);
            if (LocalCompletion)
            {
                LocalCompletion(Observation);
            }
        }

        enum class EPhase : uint8 { AwaitInjection, AwaitObservation };
        EPhase Phase = EPhase::AwaitInjection;
        int32 PieInstance = INDEX_NONE;
        double Deadline = 0.0;
        bool bTerminal = false;
        FDriveGameKeyObservation Observation;
        TFunction<void(const FDriveGameKeyObservation&)> Completion;
        FDelegateHandle PostTickHandle;
        FDelegateHandle WorldCleanupHandle;
        FDelegateHandle EndPieHandle;
        FTSTicker::FDelegateHandle DeadlineHandle;
        TSharedPtr<FPendingGameKeyRequest> SelfKeepAlive;
    };

    FString DescribeFocusedWidget(const TSharedPtr<SWidget>& Widget)
    {
        return Widget.IsValid() ? Widget->GetTypeAsString() : FString(TEXT("none"));
    }
}

int32 FDriveGameInput::ChooseTargetIndex(const TArray<FDriveGameInputCandidate>& Candidates)
{
    int32 CurrentWithController = INDEX_NONE;
    int32 FirstWithController = INDEX_NONE;
    int32 CurrentViewport = INDEX_NONE;
    int32 FirstWithViewport = INDEX_NONE;

    for (int32 Index = 0; Index < Candidates.Num(); ++Index)
    {
        const FDriveGameInputCandidate& Candidate = Candidates[Index];
        if (!Candidate.bHasViewportClient)
        {
            continue;
        }
        if (Candidate.bIsCurrentViewport && Candidate.bHasPlayerController && CurrentWithController == INDEX_NONE)
        {
            CurrentWithController = Index;
        }
        if (Candidate.bHasPlayerController && FirstWithController == INDEX_NONE)
        {
            FirstWithController = Index;
        }
        if (Candidate.bIsCurrentViewport && CurrentViewport == INDEX_NONE)
        {
            CurrentViewport = Index;
        }
        if (FirstWithViewport == INDEX_NONE)
        {
            FirstWithViewport = Index;
        }
    }

    // A window that can actually receive gameplay input beats one that merely happens to be
    // the engine's current viewport (a dedicated-server PIE window has no player to drive).
    if (CurrentWithController != INDEX_NONE) { return CurrentWithController; }
    if (FirstWithController != INDEX_NONE)   { return FirstWithController; }
    if (CurrentViewport != INDEX_NONE)       { return CurrentViewport; }
    return FirstWithViewport;
}

bool FDriveGameInput::ResolveTargetIndex(const FString& WorldSelector,
    const TArray<FDriveGameInputSelectionCandidate>& Candidates,
    int32& OutIndex, FString& OutErrorCode, FString& OutErrorMessage)
{
    OutIndex = INDEX_NONE;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    const FString TrimmedSelector = WorldSelector.TrimStartAndEnd();
    const PieWorldSelector::FParsedSelector Parsed = PieWorldSelector::Parse(TrimmedSelector);
    if (Parsed.Kind == PieWorldSelector::ESelectorKind::Invalid)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = Parsed.Error;
        return false;
    }

    if (!TrimmedSelector.IsEmpty() && Parsed.Kind == PieWorldSelector::ESelectorKind::Editor)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("world:\"editor\" is not a game-input destination. ")
            TEXT("Use target:\"editor\" to send the event to the focused editor widget, ")
            TEXT("or a PIE selector ('server', 'client', 'client:N', 'pie:N').");
        return false;
    }

    if (Candidates.Num() == 0)
    {
        OutErrorCode = ErrorCodes::ERR_PIE_NOT_ACTIVE;
        OutErrorMessage = TEXT("No PIE session is running, so there is no game to receive the input. ")
            TEXT("Start one with editor.play, or pass target:\"editor\" to send the event to the focused editor widget.");
        return false;
    }

    int32 ChosenIndex = INDEX_NONE;
    if (TrimmedSelector.IsEmpty())
    {
        ChosenIndex = ChooseTargetIndex(DriveGameInputLocal::SelectionPriorities(Candidates));
        if (ChosenIndex == INDEX_NONE)
        {
            OutErrorCode = ErrorCodes::ERR_PIE_NOT_ACTIVE;
            OutErrorMessage = FString::Printf(
                TEXT("PIE is running but no context has a game viewport to receive input (contexts: %s)."),
                *PieWorldSelector::DescribeContexts(DriveGameInputLocal::SelectorContexts(Candidates)));
            return false;
        }
    }
    else
    {
        const TArray<PieWorldSelector::FPieContextInfo> Contexts =
            DriveGameInputLocal::SelectorContexts(Candidates);
        ChosenIndex = PieWorldSelector::ResolveSelector(Parsed, Contexts);
        if (ChosenIndex == INDEX_NONE)
        {
            OutErrorCode = ErrorCodes::ERR_WORLD_NOT_FOUND;
            OutErrorMessage = FString::Printf(TEXT("No PIE world matches world selector '%s'. Available: %s"),
                *WorldSelector, *PieWorldSelector::DescribeContexts(Contexts));
            return false;
        }
    }

    if (!Candidates.IsValidIndex(ChosenIndex))
    {
        OutErrorCode = ErrorCodes::ERR_PIE_NOT_ACTIVE;
        OutErrorMessage = TEXT("The selected PIE input destination is no longer available.");
        return false;
    }

    OutIndex = ChosenIndex;
    return true;
}

bool FDriveGameInput::ResolveTarget(const FString& WorldSelector, FDriveGameInputTarget& OutTarget,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    const TArray<FDriveGameInputSelectionCandidate> Candidates =
        DriveGameInputLocal::GatherCandidates(PieWorldSelector::GatherPieContexts(),
            GEngine ? GEngine->GameViewport : nullptr);
    int32 ChosenIndex = INDEX_NONE;
    if (!ResolveTargetIndex(WorldSelector, Candidates,
        ChosenIndex, OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    OutTarget = Candidates[ChosenIndex].Target;
    if (!OutTarget.IsValid())
    {
        OutErrorCode = ErrorCodes::ERR_PIE_NOT_ACTIVE;

        OutErrorMessage = FString::Printf(
            TEXT("PIE context pie:%d has no game viewport to receive input (contexts: %s)."),
            OutTarget.PieInstance,
            *PieWorldSelector::DescribeContexts(DriveGameInputLocal::SelectorContexts(Candidates)));
        return false;
    }
    OutTarget.InputDevice = DriveGameInputLocal::ResolveInputDevice(OutTarget.LocalPlayer);
    return true;
}

FString FDriveGameInput::DescribeKeyboardFocus()
{
    if (!FSlateApplication::IsInitialized())
    {
        return TEXT("none");
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();
    return DriveGameInputLocal::DescribeFocusedWidget(
        SlateApp.GetUserFocusedWidget(static_cast<uint32>(SlateApp.GetUserIndexForKeyboard())));
}

bool FDriveGameInput::IsWidgetInFocusPath(const TSharedPtr<SWidget>& Widget, const TSharedPtr<SWidget>& Ancestor)
{
    if (!Widget.IsValid() || !Ancestor.IsValid())
    {
        return false;
    }

    TSharedPtr<SWidget> Current = Widget;
    // Bounded so a detached or cyclic parent chain cannot spin the game thread.
    for (int32 Depth = 0; Current.IsValid() && Depth < 256; ++Depth)
    {
        if (Current == Ancestor)
        {
            return true;
        }
        Current = Current->GetParentWidget();
    }
    return false;
}

TSharedPtr<FDriveGameInputRequest> FDriveGameInput::BeginDeliverKey(
    const FDriveGameInputTarget& Target, const FKey& Key, EDriveKeyAction Action,
    TFunction<void(const FDriveGameKeyObservation&)> Completion)
{
    if (!Key.IsValid() || !Target.IsValid() || !Target.PlayerController
        || !Target.PlayerController->PlayerInput || !Completion)
    {
        return nullptr;
    }
    TSharedRef<DriveGameInputLocal::FPendingGameKeyRequest> Request =
        MakeShared<DriveGameInputLocal::FPendingGameKeyRequest>(Target, Key, Action, MoveTemp(Completion));
    Request->Start();
    return Request;
}
