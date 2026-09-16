// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Handlers/HandlerContext.h"   // FJobOnComplete

// Spawns a child process via CreateProc and polls GetProcReturnCode every 0.5s.
// Calls OnComplete with `bSuccess = (exit == 0)` and result `{exit_code: <code>}`.
// On CreateProc failure, calls OnComplete(false, nullptr, "CREATEPROC_FAILED") and returns.
inline void BindProcPollCompletion(
    const FString& Executable,
    const FString& Arguments,
    FJobOnComplete OnComplete)
{
    uint32 PID = 0;
    FProcHandle Handle = FPlatformProcess::CreateProc(
        *Executable, *Arguments,
        /*bLaunchDetached=*/false, /*bLaunchHidden=*/true,
        /*bLaunchReallyHidden=*/true, &PID, 0, nullptr, nullptr);
    if (!Handle.IsValid())
    {
        OnComplete(false, nullptr, TEXT("CREATEPROC_FAILED"));
        return;
    }
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [OnComplete, Handle](float) mutable -> bool
    {
        int32 ReturnCode = 0;
        if (FPlatformProcess::GetProcReturnCode(Handle, &ReturnCode))
        {
            FPlatformProcess::CloseProc(Handle);
            auto R = MakeShared<FJsonObject>();
            R->SetNumberField(TEXT("exit_code"), ReturnCode);
            OnComplete(ReturnCode == 0, R,
                ReturnCode == 0 ? FString() : TEXT("UBT_NONZERO_EXIT"));
            return false;
        }
        return true;
    }), 0.5f);
}
