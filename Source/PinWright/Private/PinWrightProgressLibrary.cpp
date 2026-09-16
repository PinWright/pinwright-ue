// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWrightProgressLibrary.h"

#include "State/ActiveProgressSink.h"

bool UPinWrightProgressLibrary::ReportProgress(const FString& Message, float Progress, float Total)
{
    return PinWright::Progress::Report(
        Message, static_cast<double>(Progress), static_cast<double>(Total));
}

bool UPinWrightProgressLibrary::IsProgressObserved()
{
    return PinWright::Progress::IsActive();
}
