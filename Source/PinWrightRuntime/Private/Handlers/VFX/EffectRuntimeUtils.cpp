// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/VFX/EffectRuntimeUtils.h"

#include "Handlers/ErrorCodes.h"
#include "Dispatch/PinWrightWorld.h"
#include "Utils/JsonUtils.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "NiagaraComponent.h"
#include "NiagaraSystemInstanceController.h"
#if WITH_EDITOR
#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#endif

namespace PinWrightEffectRuntime
{
FString GetActorLabel(const AActor& Actor)
{
#if WITH_EDITOR
    if (GIsEditor)
    {
        return Actor.GetActorLabel();
    }
#endif
    return Actor.GetActorNameOrLabel();
}

TArray<AActor*> GetWorldActors(UWorld& World)
{
    TArray<AActor*> Actors;
    for (TActorIterator<AActor> It(&World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
    {
        if (!It->IsA(AWorldSettings::StaticClass()))
        {
            Actors.Add(*It);
        }
    }
    return Actors;
}

FVector ParseLocationFromPayload(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName, FVector Default)
{
    if (!Payload.IsValid() || !Payload->HasField(FieldName))
        return Default;

    const TSharedPtr<FJsonValue> Val = Payload->TryGetField(FieldName);
    if (!Val.IsValid())
        return Default;

    if (Val->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Arr = Val->AsArray();
        if (Arr.Num() >= 3)
            return FVector((float)Arr[0]->AsNumber(), (float)Arr[1]->AsNumber(), (float)Arr[2]->AsNumber());
    }
    else if (Val->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> O = Val->AsObject();
        if (O.IsValid())
            return FVector(
                (float)(O->HasField(TEXT("x")) ? GetJsonNumberField(O, TEXT("x")) : Default.X),
                (float)(O->HasField(TEXT("y")) ? GetJsonNumberField(O, TEXT("y")) : Default.Y),
                (float)(O->HasField(TEXT("z")) ? GetJsonNumberField(O, TEXT("z")) : Default.Z));
    }
    return Default;
}

bool ResolveTarget(const FString& SystemName, FNiagaraTarget& OutTarget,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    OutTarget = FNiagaraTarget();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    TArray<AActor*> Actors;
#if WITH_EDITOR
    if (GIsEditor)
    {
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
        Actors = ActorSubsystem->GetAllLevelActors();
    }
    else
#endif
    {
        UWorld* World = PinWright::GetTargetWorld();
        if (!World)
        {
            OutErrorCode = ErrorCodes::ERR_NO_WORLD;
            OutErrorMessage = TEXT("No game world available");
            return false;
        }
        Actors = GetWorldActors(*World);
    }

    for (AActor* Actor : Actors)
    {
        if (!Actor || !GetActorLabel(*Actor).Equals(SystemName, ESearchCase::IgnoreCase))
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
