// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"

#include "Containers/Set.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utils/PythonCallbackRegistry.h"

// An alias, not a using-directive: this file compiles inside a unity blob, where pulling
// names like Snapshot and FRecord into an enclosing scope invites an ambiguity with some
// unrelated neighbour's.
namespace PwCallbacks = PinWright::PythonCallbacks;

namespace PinWrightPythonCallbacksHandler
{
    TSharedPtr<FJsonObject> Describe(const PwCallbacks::FRecord& Record)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("id"), Record.Id);
        Entry->SetStringField(TEXT("kind"), Record.Kind);
        Entry->SetStringField(TEXT("slot"), Record.RequestId);
        Entry->SetStringField(TEXT("source"), Record.Source);
        Entry->SetStringField(TEXT("registeredAt"), Record.RegisteredAt.ToIso8601());
        Entry->SetNumberField(TEXT("invocations"), static_cast<double>(Record.Invocations));
        Entry->SetStringField(TEXT("lastError"), Record.LastError);
        return Entry;
    }

    TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    TSet<FString> IdsOf(const TArray<PwCallbacks::FRecord>& Records)
    {
        TSet<FString> Ids;
        Ids.Reserve(Records.Num());
        for (const PwCallbacks::FRecord& Record : Records)
        {
            Ids.Add(Record.Id);
        }
        return Ids;
    }
}

// ---- python.callbacks ----
REGISTER_RPC_HANDLER("python.callbacks", "python",
    "List or clear the Slate tick / Python shutdown callbacks Python scripts registered; they outlive the python.execute call that registered them",
    RPC_PARAMS(
        RPC_PARAM_DEF("action", "string", "list (default, read-only) or clear", "list"),
        RPC_PARAM_OPT("ids", "array", "clear only: the callback ids to unregister, as reported by a list call"),
        RPC_PARAM_OPT("all", "boolean", "clear only: unregister every tracked callback. Mutually exclusive with ids")
    ))
{
    const FString Action = Ctx.GetString(TEXT("action"), TEXT("list"));
    const bool bClear = Action == TEXT("clear");
    if (!bClear && Action != TEXT("list"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Invalid action '%s'. Expected: list, clear"), *Action));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* IdValues = Ctx.GetArray(TEXT("ids"));
    const bool bAll = Ctx.GetBool(TEXT("all"), false);

    if (!bClear && (IdValues || bAll))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("'ids' and 'all' apply to action 'clear' only; 'list' always reports every tracked callback."));
        return true;
    }
    if (bClear && IdValues && bAll)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("Pass either 'ids' or 'all': true, not both."));
        return true;
    }
    // Clearing every callback in an editor several agents share is never implied by the
    // absence of an argument: the caller has to say so.
    if (bClear && !IdValues && !bAll)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("action 'clear' requires 'ids': [...] or 'all': true."));
        return true;
    }

    // This verb is the reset path for a remembered install failure: python.execute keeps
    // the sticky answer so a broken host does not get a traceback per script, and calling
    // python.callbacks is how someone who fixed the cause retries without an editor restart.
    switch (PwCallbacks::EnsureTrackingReady(/*bRetryFailedInstall=*/true))
    {
    case PwCallbacks::EReadyStatus::PythonNotAvailable:
        Ctx.SendError(ErrorCodes::ERR_PYTHON_NOT_AVAILABLE,
            TEXT("PythonScriptPlugin is not loaded. Enable it in Edit > Plugins."));
        return true;
    case PwCallbacks::EReadyStatus::PythonInitFailed:
        Ctx.SendError(ErrorCodes::ERR_PYTHON_INIT_FAILED,
            TEXT("Python could not be initialized. Check Output Log for details."));
        return true;
    case PwCallbacks::EReadyStatus::ShimInstallFailed:
        Ctx.SendError(ErrorCodes::ERR_PYTHON_CALLBACK_TRACKING_UNAVAILABLE,
            TEXT("The callback tracking shim (pinwright_callbacks.py) would not install, so no ")
            TEXT("Python-registered callback can be listed or cleared. Check Output Log for the import error."));
        return true;
    case PwCallbacks::EReadyStatus::Ready:
        break;
    }

    const TArray<PwCallbacks::FRecord> Before = PwCallbacks::Snapshot();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action"), Action);
    // A caller has to be able to tell "nothing is registered" from "nothing registered
    // since tracking started": callbacks registered before this instant are invisible here
    // and cannot be cleared by this verb at all.
    Result->SetStringField(TEXT("trackingInstalledAt"), PwCallbacks::GetTrackingInstalledAt().ToIso8601());

    if (!bClear)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Reserve(Before.Num());
        for (const PwCallbacks::FRecord& Record : Before)
        {
            Entries.Add(MakeShared<FJsonValueObject>(PinWrightPythonCallbacksHandler::Describe(Record)));
        }
        Result->SetNumberField(TEXT("count"), Before.Num());
        Result->SetArrayField(TEXT("callbacks"), Entries);
        Ctx.SendSuccess(Result);
        return true;
    }

    TArray<FString> Requested;
    if (bAll)
    {
        for (const PwCallbacks::FRecord& Record : Before)
        {
            Requested.Add(Record.Id);
        }
    }
    else
    {
        for (const TSharedPtr<FJsonValue>& Value : *IdValues)
        {
            FString Id;
            if (Value.IsValid() && Value->TryGetString(Id) && !Id.IsEmpty())
            {
                Requested.AddUnique(Id);
            }
        }
    }

    const TSet<FString> BeforeIds = PinWrightPythonCallbacksHandler::IdsOf(Before);
    TArray<FString> Known;
    TArray<FString> NotFound;
    for (const FString& Id : Requested)
    {
        (BeforeIds.Contains(Id) ? Known : NotFound).Add(Id);
    }

    const bool bScriptRan = PwCallbacks::RequestClear(Known);

    // The verdict comes from re-reading the registry, not from what the script claimed:
    // the shim removes a record only after the engine's unregister call returned.
    const TArray<PwCallbacks::FRecord> After = PwCallbacks::Snapshot();
    const TSet<FString> AfterIds = PinWrightPythonCallbacksHandler::IdsOf(After);

    TArray<FString> Cleared;
    TArray<FString> Failed;
    for (const FString& Id : Known)
    {
        (AfterIds.Contains(Id) ? Failed : Cleared).Add(Id);
    }

    Result->SetArrayField(TEXT("cleared"), PinWrightPythonCallbacksHandler::ToJsonStrings(Cleared));
    Result->SetArrayField(TEXT("failed"), PinWrightPythonCallbacksHandler::ToJsonStrings(Failed));
    Result->SetArrayField(TEXT("notFound"), PinWrightPythonCallbacksHandler::ToJsonStrings(NotFound));
    Result->SetNumberField(TEXT("remaining"), After.Num());
    if (!bScriptRan)
    {
        Result->SetBoolField(TEXT("clearScriptFailed"), true);
    }
    Ctx.SendSuccess(Result);
    return true;
}
