// Copyright (c) 2026 Alexander Penkin. MIT License.

// Log capture utilities for PinWright
#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

// RAII log capture that collects UE_LOG output during a scope.
// Attach to GLog to capture serialized log messages.
struct PINWRIGHT_API FMcpOutputCapture : public FOutputDevice
{
    TArray<FString> Lines;

    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                           const FName& Category) override;

    // Return all captured lines and clear the internal buffer.
    TArray<FString> Consume();
};
