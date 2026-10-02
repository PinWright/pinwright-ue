// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraScriptCompileReport.h"

#include "Dom/JsonObject.h"
#include "NiagaraScript.h"

namespace PinWrightNiagara
{
    FString DescribeScriptCompileStatus(const UNiagaraScript& Script)
    {
        switch (Script.GetLastCompileStatus())
        {
        case ENiagaraScriptCompileStatus::NCS_UpToDate:
            return TEXT("succeeded");
        case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
        case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
            return TEXT("succeededWithWarnings");
        case ENiagaraScriptCompileStatus::NCS_Error:
            return TEXT("failed");
        case ENiagaraScriptCompileStatus::NCS_Dirty:
            return TEXT("dirty");
        case ENiagaraScriptCompileStatus::NCS_BeingCreated:
            return TEXT("compiling");
        default:
            return TEXT("notCompiled");
        }
    }

    TArray<TSharedPtr<FJsonValue>> BuildScriptCompileEventsJson(const UNiagaraScript& Script, bool bErrorsOnly)
    {
        TArray<TSharedPtr<FJsonValue>> Events;
#if WITH_EDITORONLY_DATA
        const FNiagaraVMExecutableData& Data = Script.GetVMExecutableData();
        for (const FNiagaraCompileEvent& Event : Data.LastCompileEvents)
        {
            const bool bError = Event.Severity == FNiagaraCompileEventSeverity::Error;
            if (bErrorsOnly && !bError)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("severity"),
                bError ? TEXT("error")
                : Event.Severity == FNiagaraCompileEventSeverity::Warning ? TEXT("warning")
                : TEXT("info"));
            Entry->SetStringField(TEXT("message"), Event.Message);
            if (Event.NodeGuid.IsValid())
            {
                Entry->SetStringField(TEXT("nodeGuid"), Event.NodeGuid.ToString());
            }
            if (Event.PinGuid.IsValid())
            {
                Entry->SetStringField(TEXT("pinGuid"), Event.PinGuid.ToString());
            }
            Events.Add(MakeShared<FJsonValueObject>(Entry));
        }
        if (Events.Num() == 0
            && Script.GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_Error
            && !Data.ErrorMsg.IsEmpty())
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("severity"), TEXT("error"));
            Entry->SetStringField(TEXT("message"), Data.ErrorMsg);
            Events.Add(MakeShared<FJsonValueObject>(Entry));
        }
#endif
        return Events;
    }
}
