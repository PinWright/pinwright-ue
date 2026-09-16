// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "InputCoreTypes.h"

// The one delivery path for input aimed at a RUNNING GAME (a PIE session), as opposed to
// FDriveInput's raw Slate injection, which only ever reaches whatever widget currently holds
// keyboard focus — in a PinWright session that is an editor widget, so a key sent that way
// never reaches the possessed pawn (board B-simulate-input-key-events-never-reach-pie-pawn).
//
// What a real keystroke does in PIE is: OS -> FSlateApplication::ProcessKeyDownEvent ->
// the keyboard user's FOCUS PATH -> (any focused in-game UMG widget first) -> SViewport ->
// FSceneViewport::OnKeyDown -> UGameViewportClient::InputKey -> ULocalPlayer's
// APlayerController::InputKey -> UPlayerInput (Enhanced Input included). This router
// reproduces exactly that, and adds the two things a synthetic caller cannot assume:
//
//   1. FOCUS. The game viewport widget is put on the keyboard user's focus path first when
//      it is not already there, so the event travels the real route instead of landing on an
//      editor panel. Focus is left on the game (a real keystroke does not restore focus
//      either, and a key_down / key_up pair must share one focus target).
//   2. A MEASURED RESULT. Delivery is verified against the target player's own UPlayerInput
//      (the queued event count for that key), so "delivered and ignored" is distinguishable
//      from "never delivered" — never a hardcoded success.
//
// When the focus route could not be established, or delivered nothing at all (no widget
// consumed it AND the player's input stack never saw it), the key is handed straight to the
// resolved UGameViewportClient::InputKey with the target player's input device. That floor
// keeps gameplay drivable when Slate focus cannot be moved; it can never double-fire, because
// it is reached only when the first route provably delivered nothing.

class APlayerController;
class SWidget;
class UGameViewportClient;
class ULocalPlayer;
class UPlayerInput;
class UWorld;

// A resolved game-input destination: one PIE world, its viewport client, and the local
// player whose controller owns the keyboard.
struct FDriveGameInputTarget
{
    UWorld* World = nullptr;
    UGameViewportClient* ViewportClient = nullptr;
    ULocalPlayer* LocalPlayer = nullptr;
    APlayerController* PlayerController = nullptr;
    // The device selected for this local player. Keeping it on the resolved target makes the
    // route identity explicit and prevents delivery from accidentally falling back to device 0.
    FInputDeviceId InputDevice = INPUTDEVICEID_NONE;
    int32 PieInstance = INDEX_NONE;
    ENetMode NetMode = NM_Standalone;

    bool IsValid() const { return World != nullptr && ViewportClient != nullptr; }
};

// A target reduced to what selection reads. Pure data so the selection rule is unit-testable
// without a PIE session.
struct FDriveGameInputCandidate
{
    bool bHasViewportClient = false;
    bool bHasPlayerController = false;
    // True for the context whose viewport client is the engine's current GEngine->GameViewport.
    bool bIsCurrentViewport = false;
};

struct FDriveGameInputSelectionCandidate
{
    PieWorldSelector::FPieContextInfo Context;
    FDriveGameInputCandidate Priority;
    FDriveGameInputTarget Target;
};

// Authoritative result observed after the selected UPlayerInput processes the injected edge.
struct FDriveGameKeyObservation
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<UGameViewportClient> ViewportClient;
    TWeakObjectPtr<APlayerController> PlayerController;
    TWeakObjectPtr<UPlayerInput> PlayerInput;
    FKey Key;
    EInputEvent InputEvent = IE_MAX;
    FInputDeviceId InputDevice = INPUTDEVICEID_NONE;
    uint64 InjectionFrame = 0;
    uint32 EventId = 0;
    bool bHandled = false;
    bool bEventQueued = false;
    bool bDeliveredToGame = false;
    FString ConsumingRoute = TEXT("none");
};

class FDriveGameInputRequest
{
public:
    virtual ~FDriveGameInputRequest() = default;
    virtual void Cancel(const FString& Route) = 0;
};

class FDriveGameInput
{
public:
    // Pure selection rule for the default (no `world` selector) case, in priority order:
    // the engine's current game viewport when it has a player controller, then the first
    // context with a player controller, then the current game viewport, then the first
    // context with a viewport client at all. INDEX_NONE when nothing can receive input.
    static int32 ChooseTargetIndex(const TArray<FDriveGameInputCandidate>& Candidates);

    // Pure selector seam. Each candidate keeps its selector, priority, and receiver identity
    // together, so selection cannot pair a world with another player's viewport/device.
    static bool ResolveTargetIndex(const FString& WorldSelector,
        const TArray<FDriveGameInputSelectionCandidate>& Candidates,
        int32& OutIndex, FString& OutErrorCode, FString& OutErrorMessage);

    // Resolves the destination. WorldSelector is the `world` grammar shared with
    // editor.console_command ("server" / "client" / "client:N" / "pie:N"); empty selects by
    // ChooseTargetIndex. On failure fills OutErrorCode / OutErrorMessage (PIE_NOT_ACTIVE
    // when PIE is not running, WORLD_NOT_FOUND when a well-formed selector matches nothing,
    // INVALID_ARGUMENT for a malformed one) and returns false.
    static bool ResolveTarget(const FString& WorldSelector, FDriveGameInputTarget& OutTarget,
        FString& OutErrorCode, FString& OutErrorMessage);

    // Injects after the selected world's current actor tick and completes after its next actor
    // tick. Success is based only on the exact UPlayerInput event id reaching EventCounts.
    static TSharedPtr<FDriveGameInputRequest> BeginDeliverKey(
        const FDriveGameInputTarget& Target, const FKey& Key, EDriveKeyAction Action,
        TFunction<void(const FDriveGameKeyObservation&)> Completion);

    // Widget type currently holding keyboard focus ("none" when nothing does). Shared so the
    // editor and game destinations describe focus the same way in their responses.
    static FString DescribeKeyboardFocus();

    // ---- Pure helpers (no engine globals; unit-tested) ----

    // True when Ancestor is Widget itself or one of its Slate parents. Bounded walk, so a
    // detached or cyclic chain cannot hang the caller.
    static bool IsWidgetInFocusPath(const TSharedPtr<SWidget>& Widget, const TSharedPtr<SWidget>& Ancestor);
};
