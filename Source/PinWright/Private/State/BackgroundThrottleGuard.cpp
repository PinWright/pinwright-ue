// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "State/BackgroundThrottleGuard.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Handlers/Editor/EditorQuitPolicy.h"
#include "State/ClientActivity.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

FBackgroundThrottleGuard::~FBackgroundThrottleGuard()
{
    // The throttle delegate captures `this`; an owner that forgot Unregister must not
    // leave the engine calling into freed memory.
    Unregister();
}

void FBackgroundThrottleGuard::Register()
{
    if (!GEditor || ThrottleDelegateHandle.IsValid())
    {
        return;
    }

    UEditorEngine::FShouldDisableCPUThrottling Delegate =
        UEditorEngine::FShouldDisableCPUThrottling::CreateLambda([this]() { return bHolding; });
    ThrottleDelegateHandle = Delegate.GetHandle();
    GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Delegate));

    SettingChangedHandle = GetMutableDefault<UEditorPerformanceSettings>()->OnSettingChanged()
        .AddRaw(this, &FBackgroundThrottleGuard::HandlePerformanceSettingChanged);
}

void FBackgroundThrottleGuard::Unregister()
{
    if (GEditor && ThrottleDelegateHandle.IsValid())
    {
        const FDelegateHandle Handle = ThrottleDelegateHandle;
        GEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
            [Handle](const UEditorEngine::FShouldDisableCPUThrottling& Delegate)
            {
                return Delegate.GetHandle() == Handle;
            });
    }
    ThrottleDelegateHandle.Reset();

    // CDO access is only valid while the object system is up; a guard destroyed during
    // static teardown has nothing left to restore into.
    if (!UObjectInitialized())
    {
        bHolding = false;
        SettingChangedHandle.Reset();
        return;
    }

    Release();
    if (SettingChangedHandle.IsValid())
    {
        GetMutableDefault<UEditorPerformanceSettings>()->OnSettingChanged().Remove(SettingChangedHandle);
        SettingChangedHandle.Reset();
    }
}

void FBackgroundThrottleGuard::Update(bool bSettingEnabled, bool bAgentActive)
{
    if (!(bSettingEnabled && bAgentActive))
    {
        Release();
        return;
    }

    UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
    if (!bHolding)
    {
        bSavedUserValue = Settings->bThrottleCPUWhenNotForeground;
        bSectionSavedDuringHold = false;
        bHolding = true;
    }
    // Re-applied every tick, not only on entry: a user edit of the flag during the hold
    // was adopted as the value to restore (HandlePerformanceSettingChanged), and the
    // hold still owns the in-memory value until it releases.
    Settings->bThrottleCPUWhenNotForeground = false;
}

void FBackgroundThrottleGuard::Release()
{
    if (!bHolding)
    {
        return;
    }
    bHolding = false;

    UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
    Settings->bThrottleCPUWhenNotForeground = bSavedUserValue;
    if (bSectionSavedDuringHold)
    {
        // The Editor Preferences panel saved the whole section while the flag read false.
        Settings->SaveConfig();
        bSectionSavedDuringHold = false;
    }
}

void FBackgroundThrottleGuard::HandlePerformanceSettingChanged(UObject* Settings, FPropertyChangedEvent& Event)
{
    if (!bHolding)
    {
        return;
    }
    // Broadcast from PostEditChangeProperty, before the settings panel saves the section.
    bSectionSavedDuringHold = true;
    if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UEditorPerformanceSettings, bThrottleCPUWhenNotForeground))
    {
        bSavedUserValue = GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground;
    }
}

bool FBackgroundThrottleGuard::IsAgentActive()
{
    double SecondsAgo = 0.0;
    if (ClientActivity::GetSecondsSinceLastDispatch(SecondsAgo)
        && SecondsAgo <= EditorQuitPolicy::InUseWindowSeconds)
    {
        return true;
    }
    return FPluginState::Get().GetJobRegistry().NumRunning() > 0;
}
