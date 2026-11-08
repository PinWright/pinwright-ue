// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Containers/Ticker.h"
#include "Templates/SharedPointer.h"
#include "PinWrightRuntimeSubsystem.h"
#include "PinWrightSubsystem.generated.h"

class FBackgroundThrottleGuard;

// Editor duties of the PinWright host (docs/runtime-module.md): settings migration warning,
// background-throttle guard, never-saved-asset dirty hook, and status getters for the setup
// screen. Transport, dispatch and catalog live in UPinWrightRuntimeSubsystem; the editor hooks
// (readiness, integrations, wiki) are registered by the module at startup.
UCLASS()
class PINWRIGHT_API UPinWrightSubsystem
    : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // Registered by the module at startup, before editor subsystems are created,
    // so the one-shot editor-initialized boundary cannot be missed by a late or
    // recreated subsystem instance.
    static void RecordEditorInitialized(double StartupDurationSeconds);
    static bool HasObservedEditorInitialized();

    UFUNCTION(BlueprintCallable, Category = "MCP Automation")
    bool IsBridgeActive() const;

    UFUNCTION(BlueprintCallable, Category = "MCP Automation")
    EPinWrightState GetBridgeState() const;

    // Status of the runtime host, for the setup screen and status surfaces.
    int32 GetBoundHttpPort() const;
    EMcpServerStatus GetServerStatus() const;
    int32 GetContestedHttpPort() const;
    int32 GetBindAttemptCount() const;
    bool IsBindRetryExhausted() const;
    bool IsEditorReady() const;

    // True while the background-throttle guard holds the engine's CPU throttle off.
    bool IsHoldingBackgroundThrottle() const;

private:
    bool Tick(float DeltaTime);

    // Holds the engine's background throttle off while an agent is active
    // (State/BackgroundThrottleGuard.h); updated every tick.
    TSharedPtr<FBackgroundThrottleGuard> ThrottleGuard;

    // Handle for the IAssetRegistry::OnInMemoryAssetCreated subscription that keeps a newly
    // created, never-saved asset's package dirty (MarkNeverSavedPackageDirty, Utils/AssetUtils.h).
    FDelegateHandle InMemoryAssetCreatedHandle;

    FTSTicker::FDelegateHandle TickHandle;
};
