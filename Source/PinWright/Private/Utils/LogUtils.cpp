// Copyright (c) 2026 Alexander Penkin. MIT License.

// Log capture utilities for PinWright
#include "Utils/LogUtils.h"

void FMcpOutputCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                                   const FName& Category)
{
    if (!V) return;
    FString S(V);
    // Remove trailing newlines for cleaner payloads
    while (S.EndsWith(TEXT("\n")))
        S.RemoveAt(S.Len() - 1);
    Lines.Add(S);
}

TArray<FString> FMcpOutputCapture::Consume()
{
    TArray<FString> Tmp = MoveTemp(Lines);
    Lines.Empty();
    return Tmp;
}
