// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UObject;


struct FNIRResult
{
    bool bSuccess = false;
    FString Text;
    TArray<FString> Warnings;

    static FNIRResult MakeError(const FString& Message)
    {
        FNIRResult Result;
        Result.Warnings.Add(Message);
        return Result;
    }
};

class PINWRIGHT_API NIRDecompiler
{
public:
    // Dispatches on UNiagaraSystem / UNiagaraEmitter / UNiagaraScript and emits the
    // shared Niagara IR text used by niagara.decompile_nir and asset.dump nir.txt.
    static FNIRResult BuildNiagaraIrText(UObject* NiagaraAsset);
};
