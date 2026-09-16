// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"


struct FMSIRResult
{
    bool bSuccess = false;
    FString Text;
    TArray<FString> Warnings;

    static FMSIRResult MakeError(const FString& Message)
    {
        FMSIRResult Result;
        Result.Warnings.Add(Message);
        return Result;
    }
};

class PINWRIGHT_API FMSIRDecompiler
{
public:
    static FMSIRResult BuildMetaSoundIrText(UObject* MetaSoundAsset);
};
