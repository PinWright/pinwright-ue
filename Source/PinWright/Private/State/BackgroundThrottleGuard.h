// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"

struct FPropertyChangedEvent;

// Keeps an unfocused editor at full frame rate while an agent is driving it.
//
// An agent works from another window, so the editor it drives is almost always in
// the background, where the engine caps it at 3 FPS (UEditorEngine::ShouldThrottleCPUUsage
// feeding GetMaxTickRate) and turns every editor viewport's realtime off. The request
// pump runs once per engine frame, so every RPC waited up to 333 ms before dispatch and
// every multi-frame wait (PIE start/stop, captures, PCG, compile waits, benchmarks)
// multiplied that.
//
// The engine decides the two halves separately, so the hold has two levers:
//   1. A delegate in GEditor->ShouldDisableCPUThrottlingDelegates. ShouldThrottleCPUUsage
//      asks those first and returns "do not throttle" when any says true, which also
//      covers the all-windows-minimized case.
//   2. UEditorPerformanceSettings::bThrottleCPUWhenNotForeground, cleared on the CDO in
//      memory. The "Background Process" viewport realtime override in UEditorEngine::Tick
//      reads that flag directly and never consults the delegates.
//
// The flag is the user's Editor Preference, so it is never persisted while held: the
// user's value is restored when activity lapses and on Unregister, and if the Editor
// Preferences panel saved the section during the hold (which would write the cleared
// value), the restore saves the user's value back. A user edit of the flag itself during
// the hold is adopted as their new value.
//
// Game thread only.
class FBackgroundThrottleGuard
{
public:
    ~FBackgroundThrottleGuard();

    // Binds the throttle delegate and the preferences-changed hook. No-op without GEditor.
    void Register();

    // Removes both bindings and restores the user's flag. Safe to call twice.
    void Unregister();

    // Called every subsystem tick. Holds while bSettingEnabled && bAgentActive, releases
    // otherwise; with the setting off the delegate answers false and the flag is untouched.
    void Update(bool bSettingEnabled, bool bAgentActive);

    // True while this guard holds the throttle off (what the delegate answers).
    bool IsHolding() const { return bHolding; }

    // The handle of the delegate bound into GEditor->ShouldDisableCPUThrottlingDelegates.
    FDelegateHandle GetThrottleDelegateHandle() const { return ThrottleDelegateHandle; }

    // An RPC was dispatched within EditorQuitPolicy::InUseWindowSeconds (the same "in use"
    // window editor.quit applies), or a job is still running.
    static bool IsAgentActive();

private:
    void Release();
    void HandlePerformanceSettingChanged(UObject* Settings, FPropertyChangedEvent& Event);

    bool bHolding = false;
    bool bSavedUserValue = true;
    bool bSectionSavedDuringHold = false;
    FDelegateHandle ThrottleDelegateHandle;
    FDelegateHandle SettingChangedHandle;
};
