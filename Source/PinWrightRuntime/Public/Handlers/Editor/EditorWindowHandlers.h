// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericWindowDefinition.h"

class FHandlerContext;
class FJsonObject;
class SWindow;
struct FDriveWindowSelector;

// Shared by the runtime window verbs (PinWrightRuntime EditorWindowHandlers.cpp) and the editor
// module's editor.frame_graph.
namespace EditorWindowHandlers
{
    PINWRIGHTRUNTIME_API bool ResolveScreenshotWindow(
        const FDriveWindowSelector& Selector,
        TSharedPtr<SWindow>& OutWindow,
        FString& OutTitle,
        FString& OutErrorCode,
        FString& OutErrorMessage);
    PINWRIGHTRUNTIME_API void SetScreenshotWindowIdentityFields(
        FJsonObject& Result, const FString& WindowTitle, EWindowType WindowType);

    // Pump one full Slate frame and force-redraw HostWindow so a resize / state change / graph
    // panel update has painted before it is read back.
    PINWRIGHTRUNTIME_API void PumpEditorSlate(const TSharedPtr<SWindow>& HostWindow);
    // Optional `aspect` param: a "W:H" string or a positive float ratio. False with OutError only
    // when present but malformed.
    PINWRIGHTRUNTIME_API bool ParseAspectParam(const FHandlerContext& Ctx, bool& bHasAspect, double& Ratio, FString& OutError);
    PINWRIGHTRUNTIME_API TSharedPtr<FJsonObject> MakePointObject(const FVector2f& P);
}
