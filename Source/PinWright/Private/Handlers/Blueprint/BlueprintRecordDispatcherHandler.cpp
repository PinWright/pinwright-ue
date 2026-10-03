// Copyright (c) 2026 Alexander Penkin. MIT License.

// blueprint.record_dispatcher - observe a dynamic multicast delegate firing on a live object.
//
// Python cannot bind a Blueprint-declared Event Dispatcher (unreal exposes only native
// delegates as attributes), so the only way to learn "OnFired just happened" was to poll a side
// effect. This verb binds a recorder (DispatcherRecorder.h) into the dispatcher's own invocation
// list for ONE bounded job and reports every broadcast with its parameters and timestamps.
//
// The binding cannot outlive the job: it is removed on every exit (count reached, timeout,
// target destroyed, job cancelled), and the result reports whether the recorder is measurably
// gone from the invocation list rather than asserting it.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Blueprint/DispatcherRecorder.h"
#include "Handlers/Editor/PieTimeControl.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Utils/PropertyExport.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

FName UPinWrightDispatcherRecorder::BoundFunctionName()
{
    return GET_FUNCTION_NAME_CHECKED(UPinWrightDispatcherRecorder, OnDispatcherFired);
}

void UPinWrightDispatcherRecorder::ProcessEvent(UFunction* Function, void* Parms)
{
    if (Function && Function->GetFName() == BoundFunctionName())
    {
        // The job state is game-thread only; a broadcast from another thread is not recorded.
        if (OnFired && IsInGameThread())
        {
            OnFired(Parms);
        }
        return;
    }
    Super::ProcessEvent(Function, Parms);
}

namespace BlueprintRecordDispatcher
{
    constexpr double MaxTimeoutSeconds = 600.0;
    constexpr int32 MaxRecordsCap = 1000;

    struct FRecordState
    {
        TWeakObjectPtr<UObject> Target;
        // No cached property: see FindProperty.
        TStrongObjectPtr<UPinWrightDispatcherRecorder> Recorder;
        FString ObjectPath;
        FString DispatcherName;
        int32 RequestedCount = 1;
        int32 MaxRecords = 16;
        double TimeoutSeconds = 30.0;
        bool bIncludeParams = true;
        bool bPauseOnFire = false;

        double StartSeconds = 0.0;
        int32 FireCount = 0;
        int32 RecordsDropped = 0;
        TArray<TSharedPtr<FJsonValue>> Records;
        bool bFinished = false;
        bool bPaused = false;
        FJobOnComplete OnComplete;
    };

    FScriptDelegate MakeBinding(const FRecordState& State)
    {
        FScriptDelegate Binding;
        Binding.BindUFunction(State.Recorder.Get(), UPinWrightDispatcherRecorder::BoundFunctionName());
        return Binding;
    }

    // Resolved from the object's ACTUAL class on every use. A Blueprint recompile compiles the
    // class in place and moves the old instance to a REINST_ duplicate that keeps the old layout,
    // while a cached TWeakFieldPtr re-resolves by name to the NEW class's property, whose offset
    // no longer matches the old instance's memory.
    FMulticastDelegateProperty* FindProperty(const UObject* Target, const FString& DispatcherName)
    {
        return Target ? FindFProperty<FMulticastDelegateProperty>(Target->GetClass(), *DispatcherName) : nullptr;
    }

    // True when the recorder is measurably absent from the target's invocation list. A target
    // (or property) that no longer exists holds no binding, so that also reads true. A target
    // that is garbage but not yet collected still holds its delegate, so it is read through.
    bool IsRecorderUnbound(const FRecordState& State)
    {
        UObject* Target = State.Target.Get(/*bEvenIfPendingKill=*/true);
        FMulticastDelegateProperty* Property = FindProperty(Target, State.DispatcherName);
        if (!Target || !Property || !State.Recorder.IsValid())
        {
            return true;
        }
        const FMulticastScriptDelegate* Delegate =
            Property->GetMulticastDelegate(Property->ContainerPtrToValuePtr<void>(Target));
        return !(Delegate && Delegate->Contains(State.Recorder.Get(), UPinWrightDispatcherRecorder::BoundFunctionName()));
    }

    bool Unbind(FRecordState& State)
    {
        UObject* Target = State.Target.Get(/*bEvenIfPendingKill=*/true);
        FMulticastDelegateProperty* Property = FindProperty(Target, State.DispatcherName);
        if (Target && Property && State.Recorder.IsValid())
        {
            // Safe inside a broadcast: ProcessDelegate iterates a copy of the invocation list, and
            // the delegate's access detector allows a write from inside a read on the same thread.
            Property->RemoveDelegate(MakeBinding(State), Target);
        }
        return IsRecorderUnbound(State);
    }

    TArray<TSharedPtr<FJsonValue>> DescribeSignature(const UFunction* Signature)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        if (!Signature)
        {
            return Out;
        }
        for (TFieldIterator<FProperty> It(Signature); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
            Param->SetStringField(TEXT("name"), It->GetName());
            Param->SetStringField(TEXT("type"), It->GetCPPType());
            Out.Add(MakeShared<FJsonValueObject>(Param));
        }
        return Out;
    }

    void Finish(FRecordState& State, const TCHAR* Outcome)
    {
        if (State.bFinished)
        {
            return;
        }
        State.bFinished = true;

        const bool bTargetAlive = State.Target.IsValid();
        const bool bUnbound = Unbind(State);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("objectPath"), State.ObjectPath);
        Result->SetStringField(TEXT("dispatcher"), State.DispatcherName);
        Result->SetStringField(TEXT("outcome"), Outcome);
        Result->SetBoolField(TEXT("met"), State.FireCount >= State.RequestedCount);
        Result->SetNumberField(TEXT("requestedCount"), State.RequestedCount);
        Result->SetNumberField(TEXT("fireCount"), State.FireCount);
        Result->SetArrayField(TEXT("records"), State.Records);
        Result->SetNumberField(TEXT("recordsDropped"), State.RecordsDropped);
        Result->SetNumberField(TEXT("elapsedSeconds"), FPlatformTime::Seconds() - State.StartSeconds);
        Result->SetBoolField(TEXT("targetAlive"), bTargetAlive);
        Result->SetBoolField(TEXT("bindingRemoved"), bUnbound);
        if (State.bPauseOnFire)
        {
            Result->SetBoolField(TEXT("paused"), State.bPaused);
            Result->SetBoolField(TEXT("uiFrozen"), PinWrightPieTime::IsFrozen());
        }

        if (State.OnComplete)
        {
            FJobOnComplete OnComplete = MoveTemp(State.OnComplete);
            OnComplete(true, Result, FString());
        }
    }

    void PausePie(FRecordState& State)
    {
        UObject* Target = State.Target.Get();
        UWorld* World = Target ? Target->GetWorld() : nullptr;
        // A dispatcher fired from PIE teardown (EndPlay, OnDestroyed) arrives after EndPIE has
        // been broadcast; freezing then would hook an EndPIE that already fired and strand the UI
        // clock, motion blur and widget ticks after the session. paused stays false.
        // ShouldEndPlayMap covers a queued end from the request through teardown (EndPlayMap clears
        // it last), including the EndPIE/OnPIEEnded window before bIsTearingDown is set. A direct
        // EndPlayMap() call skips the queue flag, so that window stays open for it.
        if (!GEditor || !World || World != GEditor->PlayWorld || GEditor->ShouldEndPlayMap()
            || World->bIsTearingDown || !World->HasBegunPlay())
        {
            return;
        }
        // Same levers as editor.pause; editor.resume releases both.
        World->bDebugPauseExecution = true;
        PinWrightPieTime::Freeze();
        State.bPaused = World->bDebugPauseExecution;
    }

    void OnFired(FRecordState& State, void* Parms)
    {
        if (State.bFinished)
        {
            return;
        }
        ++State.FireCount;

        if (State.Records.Num() < State.MaxRecords)
        {
            TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
            Record->SetNumberField(TEXT("seq"), State.FireCount);
            Record->SetNumberField(TEXT("elapsedSeconds"), FPlatformTime::Seconds() - State.StartSeconds);
            Record->SetNumberField(TEXT("frame"), static_cast<double>(GFrameCounter));
            UObject* Target = State.Target.Get();
            if (UWorld* World = Target ? Target->GetWorld() : nullptr)
            {
                Record->SetNumberField(TEXT("worldSeconds"), World->GetTimeSeconds());
            }
            // No live target (reinstanced or destroyed, broadcast reaching a copy): params skipped.
            FMulticastDelegateProperty* Property = FindProperty(Target, State.DispatcherName);
            if (State.bIncludeParams && Parms && Property && Property->SignatureFunction)
            {
                TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
                for (TFieldIterator<FProperty> It(Property->SignatureFunction); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
                {
                    if (It->HasAnyPropertyFlags(CPF_ReturnParm))
                    {
                        continue;
                    }
                    const TSharedPtr<FJsonValue> Value =
                        ExportPropertyToJsonValue(FPropertyExportSource::FromRaw(Parms), *It);
                    Params->SetField(It->GetName(), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
                }
                Record->SetObjectField(TEXT("params"), Params);
            }
            State.Records.Add(MakeShared<FJsonValueObject>(Record));
        }
        else
        {
            ++State.RecordsDropped;
        }

        if (State.FireCount >= State.RequestedCount)
        {
            // Pause inside the broadcast, so the frozen frame is the one the firing produced.
            if (State.bPauseOnFire)
            {
                PausePie(State);
            }
            Finish(State, TEXT("count_reached"));
        }
    }

    // Object path first (exact match only, so a bad tail cannot resolve to an ancestor), then the
    // shared actor resolver (PIE world first), which reports ambiguity with candidates.
    UObject* ResolveTargetOrSendError(const FHandlerContext& Ctx, const FString& Identifier)
    {
        if (Identifier.StartsWith(TEXT("/")))
        {
            UObject* Found = FindObject<UObject>(nullptr, *Identifier);
            if (Found && Found->GetPathName().Equals(Identifier, ESearchCase::IgnoreCase))
            {
                // FindObject still returns a destroyed (garbage, not yet collected) object; the job's
                // weak pointer would read it as null right after "started" was sent.
                if (!IsValid(Found))
                {
                    Ctx.SendError(ErrorCodes::ERR_OBJECT_NOT_FOUND, FString::Printf(
                        TEXT("'%s' is destroyed (pending garbage collection); pass a live object."), *Identifier));
                    return nullptr;
                }
                return Found;
            }
        }
        AActor* Actor = nullptr;
        if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, Identifier, Actor))
        {
            return nullptr;
        }
        return Actor;
    }
}

REGISTER_RPC_HANDLER("blueprint.record_dispatcher", "blueprint",
    "Record a Blueprint Event Dispatcher (or any dynamic multicast delegate) firing on a live object: binds a recorder for one bounded job, reports each broadcast with its parameters and timestamps, and unbinds on every exit (count reached, timeout, target destroyed, cancel). Returns a job ticket; read the result with system.job_status.",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Live object to watch: an object path (e.g. a PIE actor's or component's path) or an actor label/name, resolved in the PIE world first."),
        RPC_PARAM_REQ("dispatcher", "string", "Dispatcher (multicast delegate property) name on the object's class, e.g. OnFired."),
        RPC_PARAM_DEF("count", "integer", "Broadcasts to record before the job completes (1..1000000).", "1"),
        RPC_PARAM_DEF("timeoutSeconds", "number", "Give up after this many seconds (0 < t <= 600). The job then completes with met:false.", "30"),
        RPC_PARAM_DEF("maxRecords", "integer", "Per-broadcast records kept (0..1000); broadcasts past it are counted in fireCount and recordsDropped only.", "16"),
        RPC_PARAM_DEF("includeParams", "boolean", "Export each broadcast's parameters into records[].params.", "true"),
        RPC_PARAM_DEF("pauseOnFire", "boolean", "Pause the PIE session (as editor.pause) inside the broadcast that completes the count, so the next capture shows that frame. Needs the object in the running PIE world (single PIE world only); resume with editor.resume.", "false")
    ))
{
    using namespace BlueprintRecordDispatcher;

    const FString Identifier = Ctx.GetString(TEXT("objectPath")).TrimStartAndEnd();
    const FString DispatcherName = Ctx.GetString(TEXT("dispatcher")).TrimStartAndEnd();
    if (Identifier.IsEmpty() || DispatcherName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, TEXT("objectPath and dispatcher must be non-empty."));
        return true;
    }

    const TOptional<int32> CountArg = Ctx.GetIntOr(TEXT("count"));
    const TOptional<int32> MaxRecordsArg = Ctx.GetIntOr(TEXT("maxRecords"));
    const bool bHasCount = Ctx.GetRawPayload()->HasField(TEXT("count"));
    const bool bHasMaxRecords = Ctx.GetRawPayload()->HasField(TEXT("maxRecords"));
    if ((bHasCount && !CountArg.IsSet()) || (bHasMaxRecords && !MaxRecordsArg.IsSet()))
    {
        return true; // GetIntOr already sent INVALID_PARAMS
    }
    const int32 Count = CountArg.Get(1);
    const int32 MaxRecords = MaxRecordsArg.Get(16);
    const double TimeoutSeconds = Ctx.GetNumber(TEXT("timeoutSeconds"), 30.0);
    if (Count < 1 || Count > 1000000 || MaxRecords < 0 || MaxRecords > MaxRecordsCap
        || !(TimeoutSeconds > 0.0) || TimeoutSeconds > MaxTimeoutSeconds)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("Out of range: count=%d (1..1000000), maxRecords=%d (0..%d), timeoutSeconds=%g (0 < t <= %g)."),
            Count, MaxRecords, MaxRecordsCap, TimeoutSeconds, MaxTimeoutSeconds));
        return true;
    }

    UObject* Target = ResolveTargetOrSendError(Ctx, Identifier);
    if (!Target)
    {
        return true;
    }

    FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(Target->GetClass(), *DispatcherName);
    if (Target->IsTemplate())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("'%s' is a class default object or archetype; it never broadcasts at runtime. Pass a live instance (in PIE, the PIE object's path)."),
            *Target->GetPathName()));
        return true;
    }

    if (!Property)
    {
        TArray<TSharedPtr<FJsonValue>> Available;
        for (TFieldIterator<FMulticastDelegateProperty> It(Target->GetClass()); It; ++It)
        {
            Available.Add(MakeShared<FJsonValueString>(It->GetName()));
        }
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("objectPath"), Target->GetPathName());
        Data->SetStringField(TEXT("class"), Target->GetClass()->GetPathName());
        Data->SetArrayField(TEXT("available"), Available);
        Ctx.SendError(ErrorCodes::ERR_DISPATCHER_NOT_FOUND, FString::Printf(
            TEXT("'%s' has no dispatcher (multicast delegate property) named '%s'; available[] lists the ones it has."),
            *Target->GetName(), *DispatcherName), Data);
        return true;
    }

    const bool bPauseOnFire = Ctx.GetBool(TEXT("pauseOnFire"), false);
    if (bPauseOnFire && (!GEditor || !GEditor->PlayWorld || Target->GetWorld() != GEditor->PlayWorld))
    {
        Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_SESSION, FString::Printf(
            TEXT("pauseOnFire needs '%s' to live in the running PIE world (GEditor->PlayWorld; single PIE world only, not a client world of a multi-player session); start PIE (editor.play) and pass the PIE object's path."),
            *Target->GetPathName()));
        return true;
    }

    TSharedRef<FRecordState> State = MakeShared<FRecordState>();
    State->Target = Target;
    State->ObjectPath = Target->GetPathName();
    State->DispatcherName = Property->GetName();
    State->RequestedCount = Count;
    State->MaxRecords = MaxRecords;
    State->TimeoutSeconds = TimeoutSeconds;
    State->bIncludeParams = Ctx.GetBool(TEXT("includeParams"), true);
    State->bPauseOnFire = bPauseOnFire;
    State->Recorder.Reset(NewObject<UPinWrightDispatcherRecorder>(GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UPinWrightDispatcherRecorder::StaticClass(), TEXT("PinWrightDispatcherRecorder"))));
    const TWeakPtr<FRecordState> WeakState = State;
    State->Recorder->OnFired = [WeakState](void* Parms)
    {
        if (const TSharedPtr<FRecordState> Pinned = WeakState.Pin())
        {
            OnFired(*Pinned, Parms);
        }
    };

    FJobBindArgs Args;
    Args.Method = Ctx.GetMethod();
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("objectPath"), State->ObjectPath);
    Args.StartedPayload->SetStringField(TEXT("dispatcher"), State->DispatcherName);
    Args.StartedPayload->SetStringField(TEXT("delegateKind"), Property->GetClass()->GetName());
    Args.StartedPayload->SetArrayField(TEXT("signature"), DescribeSignature(Property->SignatureFunction));
    Args.StartedPayload->SetStringField(TEXT("message"), FString::Printf(
        TEXT("Recording %s.%s; poll system.job_status for the result."), *Target->GetName(), *State->DispatcherName));
    Args.BindNativeDelegate = [State](FJobOnComplete OnComplete)
    {
        State->OnComplete = MoveTemp(OnComplete);
        State->StartSeconds = FPlatformTime::Seconds();
        FMulticastDelegateProperty* Property = FindProperty(State->Target.Get(), State->DispatcherName);
        if (!Property)
        {
            Finish(*State, TEXT("target_destroyed"));
            return;
        }
        Property->AddDelegate(MakeBinding(*State), State->Target.Get());
        // The ticker owns the state; it drops it on the tick after the job finishes by any route.
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State](float)
        {
            if (State->bFinished)
            {
                return false;
            }
            if (!State->Target.IsValid())
            {
                Finish(*State, TEXT("target_destroyed"));
                return false;
            }
            if (FPlatformTime::Seconds() - State->StartSeconds >= State->TimeoutSeconds)
            {
                Finish(*State, TEXT("timed_out"));
                return false;
            }
            return true;
        }));
    };
    const FString TicketId = Ctx.StartJob(Args);
    FPluginState::Get().GetJobRegistry().SetCancelCallback(TicketId, [WeakState]()
    {
        if (const TSharedPtr<FRecordState> Pinned = WeakState.Pin())
        {
            // The registry already marked the ticket cancelled; just stop recording and unbind.
            Pinned->bFinished = true;
            Pinned->OnComplete = nullptr;
            Unbind(*Pinned);
        }
    });
    return true;
}
