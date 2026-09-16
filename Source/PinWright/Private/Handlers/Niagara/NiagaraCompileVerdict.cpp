// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraCompileVerdict.h"

#include "Dom/JsonValue.h"

namespace PinWrightNiagara
{
namespace
{
    // NCS_Error is the only status that means "this script cannot run". Warning statuses are
    // terminal successes. NCS_Dirty is different: runnable cached bytecode may exist, but it is
    // stale relative to the authored graph, so a completion probe must not promote it to Passed.
    const TCHAR* CompileErrorStatus = TEXT("NCS_Error");

    bool IsSuccessfulCompileStatus(const FString& Status)
    {
        return Status.Equals(TEXT("NCS_UpToDate"), ESearchCase::IgnoreCase)
            || Status.Equals(TEXT("NCS_UpToDateWithWarnings"), ESearchCase::IgnoreCase)
            || Status.Equals(TEXT("NCS_ComputeUpToDateWithWarnings"), ESearchCase::IgnoreCase);
    }

    bool ReadBoolField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool& OutValue)
    {
        return Object.IsValid() && Object->TryGetBoolField(Field, OutValue);
    }
}

FCompileVerdict ReadCompileVerdict(const TSharedPtr<FJsonObject>& Compile)
{
    FCompileVerdict Verdict;
    if (!Compile.IsValid())
    {
        return Verdict;
    }

    bool bOutstanding = false;
    bool bActive = false;
    const bool bHasOutstanding = ReadBoolField(Compile, TEXT("hasOutstandingCompilationRequests"), bOutstanding);
    const bool bHasActive = ReadBoolField(Compile, TEXT("hasActiveCompilations"), bActive);
    Verdict.bPendingCompileKnown = bHasOutstanding || bHasActive;
    Verdict.bPendingCompile = bOutstanding || bActive;

    const TArray<TSharedPtr<FJsonValue>>* Scripts = nullptr;
    if (!Compile->TryGetArrayField(TEXT("scripts"), Scripts) || !Scripts)
    {
        return Verdict;
    }

    int32 ScriptsWithTerminalStatus = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Scripts)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            continue;
        }
        const TSharedPtr<FJsonObject> Entry = Value->AsObject();

        // A null compileStatus is the dumper's spelling of NCS_Unknown - "not compiled yet",
        // which BuildCompileDiagnosticsJson already reports as COMPILE_STATE_UNINITIALIZED. It is
        // not a failure and it is not a pass, so it only withholds the Passed verdict.
        FString Status;
        if (!Entry->TryGetStringField(TEXT("compileStatus"), Status) || Status.IsEmpty())
        {
            continue;
        }
        const bool bCompileError = Status.Equals(CompileErrorStatus, ESearchCase::IgnoreCase);
        if (!bCompileError && !IsSuccessfulCompileStatus(Status))
        {
            continue;
        }
        ++ScriptsWithTerminalStatus;
        if (!bCompileError)
        {
            continue;
        }

        FScriptCompileFailure Failure;
        Failure.CompileStatus = Status;
        Entry->TryGetStringField(TEXT("ownerKind"), Failure.OwnerKind);
        Entry->TryGetStringField(TEXT("ownerName"), Failure.OwnerName);
        Entry->TryGetStringField(TEXT("scriptUsage"), Failure.ScriptUsage);
        Entry->TryGetStringField(TEXT("path"), Failure.ScriptPath);

        const TArray<TSharedPtr<FJsonValue>>* CompileErrors = nullptr;
        if (Entry->TryGetArrayField(TEXT("compileErrors"), CompileErrors) && CompileErrors)
        {
            for (const TSharedPtr<FJsonValue>& ErrorValue : *CompileErrors)
            {
                FString Message;
                if (ErrorValue.IsValid() && ErrorValue->TryGetString(Message) && !Message.IsEmpty())
                {
                    Failure.Errors.Add(Message);
                }
            }
        }
        Verdict.FailedScripts.Add(MoveTemp(Failure));
    }

    if (Verdict.FailedScripts.Num() > 0)
    {
        Verdict.Check = EScriptCompileCheck::Failed;
    }
    else if (Scripts->Num() > 0 && ScriptsWithTerminalStatus == Scripts->Num())
    {
        Verdict.Check = EScriptCompileCheck::Passed;
    }
    return Verdict;
}

const TCHAR* ScriptCompileCheckToString(EScriptCompileCheck Check)
{
    switch (Check)
    {
    case EScriptCompileCheck::Passed:   return TEXT("passed");
    case EScriptCompileCheck::Failed:   return TEXT("failed");
    default:                            return TEXT("unverified");
    }
}

FCompileProbeVerdict SummarizeCompileProbe(
    const FCompileVerdict& ScriptVerdict,
    bool bCompilationPending,
    bool bCompileQueueObserved)
{
    FCompileProbeVerdict Probe;
    if (bCompilationPending)
    {
        Probe.Status = TEXT("compiling");
        return Probe;
    }
    if (!bCompileQueueObserved)
    {
        return Probe;
    }
    if (ScriptVerdict.Check == EScriptCompileCheck::Failed)
    {
        Probe.Status = TEXT("failed");
        Probe.bCompleted = true;
        return Probe;
    }
    if (ScriptVerdict.Check == EScriptCompileCheck::Passed)
    {
        Probe.Status = TEXT("completed");
        Probe.bCompleted = true;
        Probe.bSuccessful = true;
    }
    return Probe;
}

FString DescribeScriptCompileFailure(const FScriptCompileFailure& Failure)
{
    const FString Owner = Failure.OwnerName.IsEmpty() ? TEXT("<unnamed>") : Failure.OwnerName;
    const FString Usage = Failure.ScriptUsage.IsEmpty() ? TEXT("<unknown script>") : Failure.ScriptUsage;

    FString Message = FString::Printf(
        TEXT("Script '%s.%s' is at %s: its last compile failed, so the engine refuses to instance this system and nothing renders. "),
        *Owner,
        *Usage,
        *Failure.CompileStatus);

    if (Failure.Errors.Num() > 0)
    {
        // Capped: a translator failure routinely emits the same error once per generated line and
        // the whole list would bury every other issue in the response. The full list stays on the
        // issue's compileErrors field and in compile.scripts[].
        constexpr int32 MaxQuotedErrors = 3;
        const int32 Quoted = FMath::Min(Failure.Errors.Num(), MaxQuotedErrors);
        for (int32 Index = 0; Index < Quoted; ++Index)
        {
            Message += FString::Printf(TEXT("%s%s"), Index == 0 ? TEXT("Compiler said: ") : TEXT(" | "), *Failure.Errors[Index]);
        }
        if (Failure.Errors.Num() > Quoted)
        {
            Message += FString::Printf(TEXT(" (+%d more)"), Failure.Errors.Num() - Quoted);
        }
        Message += TEXT(". ");
    }
    else
    {
        Message += TEXT("The compiler's messages did not survive this session; run niagara.compile to reproduce them. ");
    }

    Message += TEXT("Fix the module or dynamic input the error names, then niagara.compile and validate again.");
    return Message;
}
}
