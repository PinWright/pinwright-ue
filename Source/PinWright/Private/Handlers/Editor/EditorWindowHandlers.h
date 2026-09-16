// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericWindowDefinition.h"

class FJsonObject;
class SWindow;
struct FDriveWindowSelector;

namespace EditorWindowHandlers
{
    bool ResolveScreenshotWindow(
        const FDriveWindowSelector& Selector,
        TSharedPtr<SWindow>& OutWindow,
        FString& OutTitle,
        FString& OutErrorCode,
        FString& OutErrorMessage);
    void SetScreenshotWindowIdentityFields(
        FJsonObject& Result, const FString& WindowTitle, EWindowType WindowType);
}
