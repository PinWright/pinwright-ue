// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWrightSubsystem.h"

#include "PinWrightProjectSettings.h"
#include "State/BackgroundThrottleGuard.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Utils/AssetUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "CoreGlobals.h"

// FEditorDelegates::OnEditorInitialized is a one-shot notification. Keep a
// module-lifetime latch so a subsystem instance recreated later in the same
// editor process can catch up instead of remaining permanently unready.
static bool GPinWrightEditorInitializationObserved = false;

void UPinWrightSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    if (IsRunningCommandlet())
    {
        return;
    }

    // Migration warning: AssetDumpRootDirectory / WikiOutputDirectory moved from
    // the per-user UPinWrightSettings to the project-level UPinWrightProjectSettings.
    // If the new setting is unset but a stale per-user value remains on disk, tell
    // the user to re-set it — never rewrite config files automatically.
    {
        const UPinWrightProjectSettings* ProjectSettings =
            GetDefault<UPinWrightProjectSettings>();
        auto WarnIfStalePerUserValue =
            [](const FString& NewValue, const TCHAR* PropertyName)
        {
            if (!NewValue.IsEmpty() || !GConfig)
            {
                return;
            }
            FString StaleValue;
            if (GConfig->GetString(TEXT("/Script/PinWright.PinWrightSettings"),
                                   PropertyName, StaleValue, GEditorPerProjectIni)
                && !StaleValue.IsEmpty())
            {
                UE_LOG(LogPinWrightSubsystem, Warning,
                       TEXT("Setting %s moved to Project Settings -> Plugins -> PinWright (Project); ")
                       TEXT("the old per-user value (%s) is ignored and must be re-set there."),
                       PropertyName, *StaleValue);
            }
        };
        WarnIfStalePerUserValue(ProjectSettings->AssetDumpRootDirectory,
                                TEXT("AssetDumpRootDirectory"));
        WarnIfStalePerUserValue(ProjectSettings->WikiOutputDirectory,
                                TEXT("WikiOutputDirectory"));
    }

    ThrottleGuard = MakeShared<FBackgroundThrottleGuard>();
    ThrottleGuard->Register();

    // One hook instead of a fix in every create verb: the engine can refuse the verb's own
    // MarkPackageDirty (e.g. under GIsPlayInEditorWorld), and a clean never-saved package is
    // invisible to editor.save_all, editor.list_dirty_packages and the quit prompt.
    InMemoryAssetCreatedHandle = IAssetRegistry::GetChecked().OnInMemoryAssetCreated().AddLambda(
        [](UObject* Asset)
        {
            if (Asset)
            {
                MarkNeverSavedPackageDirty(Asset->GetOutermost());
            }
        });

    TickHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UPinWrightSubsystem::Tick), 0.1f);
}

void UPinWrightSubsystem::Deinitialize()
{
    if (TickHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
    }

    // Restores the user's background-throttle preference if a hold is still live.
    if (ThrottleGuard.IsValid())
    {
        ThrottleGuard->Unregister();
        ThrottleGuard.Reset();
    }

    if (InMemoryAssetCreatedHandle.IsValid())
    {
        if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
        {
            AssetRegistry->OnInMemoryAssetCreated().Remove(InMemoryAssetCreatedHandle);
        }
        InMemoryAssetCreatedHandle.Reset();
    }

    Super::Deinitialize();
}

bool UPinWrightSubsystem::Tick(float DeltaTime)
{
    if (ThrottleGuard.IsValid())
    {
        ThrottleGuard->Update(
            GetDefault<UPinWrightProjectSettings>()->bDisableBackgroundThrottleWhileAgentActive,
            FBackgroundThrottleGuard::IsAgentActive());
    }
    return true;
}

void UPinWrightSubsystem::RecordEditorInitialized(double StartupDurationSeconds)
{
    GPinWrightEditorInitializationObserved = true;
    UE_LOG(LogPinWrightSubsystem, Log,
           TEXT("Editor initialization completed after %.2f seconds; PinWright operational readiness can now be evaluated."),
           StartupDurationSeconds);
}

bool UPinWrightSubsystem::HasObservedEditorInitialized()
{
    return GPinWrightEditorInitializationObserved;
}

bool UPinWrightSubsystem::IsBridgeActive() const
{
    const UPinWrightRuntimeSubsystem* Runtime = UPinWrightRuntimeSubsystem::Get();
    return Runtime && Runtime->IsBridgeActive();
}

EPinWrightState UPinWrightSubsystem::GetBridgeState() const
{
    return IsBridgeActive() ? EPinWrightState::Connected : EPinWrightState::Disconnected;
}

int32 UPinWrightSubsystem::GetBoundHttpPort() const
{
    const UPinWrightRuntimeSubsystem* Runtime = UPinWrightRuntimeSubsystem::Get();
    return Runtime ? Runtime->GetBoundHttpPort() : 0;
}

EMcpServerStatus UPinWrightSubsystem::GetServerStatus() const
{
    const UPinWrightRuntimeSubsystem* Runtime = UPinWrightRuntimeSubsystem::Get();
    return Runtime ? Runtime->GetServerStatus() : EMcpServerStatus::Disabled;
}

int32 UPinWrightSubsystem::GetContestedHttpPort() const
{
    const UPinWrightRuntimeSubsystem* Runtime = UPinWrightRuntimeSubsystem::Get();
    return Runtime ? Runtime->GetContestedHttpPort() : 0;
}

int32 UPinWrightSubsystem::GetBindAttemptCount() const
{
    const UPinWrightRuntimeSubsystem* Runtime = UPinWrightRuntimeSubsystem::Get();
    return Runtime ? Runtime->GetBindAttemptCount() : 0;
}

bool UPinWrightSubsystem::IsBindRetryExhausted() const
{
    const UPinWrightRuntimeSubsystem* Runtime = UPinWrightRuntimeSubsystem::Get();
    return Runtime && Runtime->IsBindRetryExhausted();
}

bool UPinWrightSubsystem::IsEditorReady() const
{
    const UPinWrightRuntimeSubsystem* Runtime = UPinWrightRuntimeSubsystem::Get();
    return Runtime && Runtime->IsEditorReady();
}

bool UPinWrightSubsystem::IsHoldingBackgroundThrottle() const
{
    return ThrottleGuard.IsValid() && ThrottleGuard->IsHolding();
}
