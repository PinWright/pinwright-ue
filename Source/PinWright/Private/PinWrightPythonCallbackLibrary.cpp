// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWrightPythonCallbackLibrary.h"

#include "Utils/PythonCallbackRegistry.h"

// Thin reflected skin over PinWright::PythonCallbacks. All state lives in
// Utils/PythonCallbackRegistry.cpp so python.execute's leak count, the EndPIE warning and
// the python.callbacks verb read one record set rather than three that can drift.

FString UPinWrightPythonCallbackLibrary::NotifyRegistered(const FString& Kind, const FString& Source)
{
    return PinWright::PythonCallbacks::NotifyRegistered(Kind, Source);
}

void UPinWrightPythonCallbackLibrary::NotifyUnregistered(const FString& Id)
{
    PinWright::PythonCallbacks::NotifyUnregistered(Id);
}

bool UPinWrightPythonCallbackLibrary::IsTracked(const FString& Id)
{
    return PinWright::PythonCallbacks::IsTracked(Id);
}

void UPinWrightPythonCallbackLibrary::NotifyInvoked(const FString& Id, const FString& Error)
{
    PinWright::PythonCallbacks::NotifyInvoked(Id, Error);
}
