// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UMaterialFunction;


struct FMGIRDecompileResult
{
    bool bSuccess = false;
    FString MGIRText;
    TArray<FString> Warnings;

    static FMGIRDecompileResult MakeError(const FString& Message)
    {
        FMGIRDecompileResult Result;
        Result.Warnings.Add(Message);
        return Result;
    }
};

struct FMGIRDecompileOptions
{
    bool bEmitSubstrateSugar = false;
};

class PINWRIGHT_API FMGIRDecompiler
{
public:
    static FMGIRDecompileResult DecompileMaterial(
        UMaterial* Material,
        const FMGIRDecompileOptions& Options = FMGIRDecompileOptions());
    static FMGIRDecompileResult DecompileFunction(
        UMaterialFunction* Function,
        const FMGIRDecompileOptions& Options = FMGIRDecompileOptions());
    static FMGIRDecompileResult DecompileMaterialWithReferences(
        UMaterial* Material,
        const FMGIRDecompileOptions& Options = FMGIRDecompileOptions());
};
