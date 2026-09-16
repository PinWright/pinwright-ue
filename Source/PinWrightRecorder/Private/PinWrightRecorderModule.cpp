// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogPinWrightRecorder, Log, All);

/**
 * Lightweight facade/writer module for the editor-only debug journal recorder. Holds no PIE
 * wiring (that lives in the gateway editor module, keeping this module free of UnrealEd) so
 * host-project runtime modules can link it without dragging in the heavy editor dependency tree.
 */
class FPinWrightRecorderModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogPinWrightRecorder, Log, TEXT("PinWright Recorder module initialized."));
    }

    virtual void ShutdownModule() override
    {
        UE_LOG(LogPinWrightRecorder, Log, TEXT("PinWright Recorder module shut down."));
    }
};

IMPLEMENT_MODULE(FPinWrightRecorderModule, PinWrightRecorder)
