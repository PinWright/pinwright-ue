// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class USoundCue;


struct FSCIRResult
{
    bool bSuccess = false;
    FString Text;
    TArray<FString> Warnings;

    static FSCIRResult MakeError(const FString& Message)
    {
        FSCIRResult Result;
        Result.Warnings.Add(Message);
        return Result;
    }
};

class PINWRIGHT_API SCIRDecompiler
{
public:
    static FSCIRResult BuildSoundCueIrText(USoundCue* Cue);
};
