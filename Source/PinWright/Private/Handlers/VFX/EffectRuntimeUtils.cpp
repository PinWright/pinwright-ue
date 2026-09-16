// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/VFX/EffectRuntimeUtils.h"

#include "Handlers/ErrorCodes.h"

#include "Editor.h"
#include "GameFramework/Actor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystemInstanceController.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif

namespace PinWrightEffectRuntime
{
bool ResolveTarget(const FString& SystemName, FNiagaraTarget& OutTarget,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    OutTarget = FNiagaraTarget();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!GEditor)
    {
        OutErrorCode = ErrorCodes::ERR_EDITOR_NOT_AVAILABLE;
        OutErrorMessage = TEXT("Editor not available");
        return false;
    }

    UEditorActorSubsystem* ActorSubsystem =
        GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (!ActorSubsystem)
    {
        OutErrorCode = ErrorCodes::ERR_EDITOR_NOT_AVAILABLE;
        OutErrorMessage = TEXT("EditorActorSubsystem not available");
        return false;
    }

    for (AActor* Actor : ActorSubsystem->GetAllLevelActors())
    {
        if (!Actor || !Actor->GetActorLabel().Equals(SystemName, ESearchCase::IgnoreCase))
        {
            continue;
        }

        if (UNiagaraComponent* Component = Actor->FindComponentByClass<UNiagaraComponent>())
        {
            OutTarget.Actor = Actor;
            OutTarget.Component = Component;
            return true;
        }
    }

    OutErrorCode = ErrorCodes::ERR_SYSTEM_NOT_FOUND;
    OutErrorMessage = TEXT("Niagara system not found.");
    return false;
}

bool Activate(const FNiagaraTarget& Target, const bool bReset,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    UNiagaraComponent* Component = Target.Component.Get();
    if (!Component)
    {
        OutErrorCode = ErrorCodes::ERR_COMPONENT_NOT_FOUND;
        OutErrorMessage = TEXT("Niagara component is no longer available");
        return false;
    }

    Component->Activate(bReset);
    return true;
}

bool Advance(const FNiagaraTarget& Target, const int32 Steps, const float DeltaTime,
    FAdvanceResult& OutResult,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    OutResult = FAdvanceResult();
    UNiagaraComponent* Component = Target.Component.Get();
    if (!Component)
    {
        OutErrorCode = ErrorCodes::ERR_COMPONENT_NOT_FOUND;
        OutErrorMessage = TEXT("Niagara component is no longer available");
        return false;
    }

    const double RequestedSeconds = static_cast<double>(Steps) * static_cast<double>(DeltaTime);
    const FNiagaraSystemInstanceControllerConstPtr Controller =
        Component->GetSystemInstanceController();
    const bool bControllerValid = Controller.IsValid() && Controller->IsValid();
    const double StartAgeSeconds = bControllerValid
        ? static_cast<double>(Controller->GetAge())
        : 0.0;
    const bool bWasPaused = bControllerValid && Controller->IsPaused();
    const bool bWasComplete = bControllerValid && Controller->IsComplete();

    Component->AdvanceSimulation(Steps, DeltaTime);

    if (!bControllerValid)
    {
        OutResult.bStoppedEarly = true;
        OutResult.StopReason = TEXT("no_controller");
        return true;
    }

    const double EndAgeSeconds = static_cast<double>(Controller->GetAge());
    OutResult.SimulatedSeconds = FMath::Max(0.0, EndAgeSeconds - StartAgeSeconds);
    const double AgeTolerance = FMath::Max(1.0e-5, RequestedSeconds * 1.0e-5);
    if (OutResult.SimulatedSeconds + AgeTolerance < RequestedSeconds)
    {
        OutResult.bStoppedEarly = true;
        if (bWasPaused || Controller->IsPaused())
        {
            OutResult.StopReason = TEXT("paused");
        }
        else if (bWasComplete || Controller->IsComplete())
        {
            OutResult.StopReason = TEXT("complete");
        }
        else if (OutResult.SimulatedSeconds <= 0.0)
        {
            OutResult.StopReason = TEXT("no_progress");
        }
        else
        {
            OutResult.StopReason = TEXT("advanced_less_than_requested");
        }
    }
    return true;
}
}
