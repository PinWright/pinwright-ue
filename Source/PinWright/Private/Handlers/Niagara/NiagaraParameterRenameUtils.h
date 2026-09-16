// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraTypes.h"

class UNiagaraSystem;

namespace PinWrightNiagara
{
    struct FNiagaraParameterRenameResult
    {
        FNiagaraVariable OldVariable;
        FNiagaraVariable NewVariable;
        int32 GraphsVisited = 0;
        int32 GraphsRenamed = 0;
        int32 AssignmentTargetsRenamed = 0;
        bool bUserStoreRenamed = false;
        bool bUserScriptMetadataRenamed = false;
        bool bSystemRenameHookInvoked = false;
    };

    bool RenameNiagaraParameterWithExportedApis(
        UNiagaraSystem& System,
        const FString& OldName,
        const FString& NewName,
        FNiagaraParameterRenameResult& OutResult,
        FString& OutErrorCode,
        FString& OutErrorMessage);
}
