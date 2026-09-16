// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Utils/AssetSaveState.h"
#include "Utils/DerivedStateReport.h"

enum class EMGIRCompileMode : uint8
{
    Append,
    Extend,
};

struct FMGIRCompileOptions
{
    EMGIRCompileMode Mode = EMGIRCompileMode::Append;
    FString Context;
    bool bRunLayout = true;
    bool bSave = true;
};

struct FMGIRCompileResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> AssetPaths;
    int32 BlocksCompiled = 0;
    int32 ExpressionsCreated = 0;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;

    // Measured coverage of pushing the compiled masters into the consumers that cache
    // instances derived from them, summed over every material block in the document.
    // Default-constructs to "nothing measured, nothing refreshed", so a compile path that
    // forgets to push reports the failure rather than an unqualified success.
    PinWright::DerivedState::FConsumerRefreshReport ConsumerRefresh;

    // Things the compile DID that a caller must know about but which are not failures - today,
    // function pins a rebuild renamed or dropped, which disconnects every already-saved caller of
    // that pin. Surfaced into the response's warnings[] alongside the shader and consumer warnings.
    TArray<FString> Warnings;

    static FMGIRCompileResult MakeError(const FString& InCode, const FString& InMessage)
    {
        FMGIRCompileResult Result;
        Result.ErrorCode = InCode;
        Result.ErrorMessage = InMessage;
        return Result;
    }
};

class PINWRIGHT_API FMGIRCompiler
{
public:
    static FMGIRCompileResult Compile(const FString& Text, const FMGIRCompileOptions& Options);
};
