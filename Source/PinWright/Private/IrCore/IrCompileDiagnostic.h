// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

struct PINWRIGHT_API FIrCompileDiagnostic
{
    int32 Line = -1;
    FString Message;

    FIrCompileDiagnostic() = default;
    FIrCompileDiagnostic(int32 InLine, const FString& InMessage)
        : Line(InLine)
        , Message(InMessage)
    {
    }
};
