// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CoreMinimal.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "PinWrightEditorHooks.h"
#include "Transport/ModalStateProbe.h"

namespace PinWright
{
namespace Detail
{
    IPinWrightEditorHooks* GEditorHooks = nullptr;
}

void SetEditorHooks(IPinWrightEditorHooks* Hooks)
{
    Detail::GEditorHooks = Hooks;
}

IPinWrightEditorHooks* GetEditorHooks()
{
    return Detail::GEditorHooks;
}
}

class FPinWrightRuntimeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // The editor module registers the probe itself, after declining the auto-save prompt;
        // a game process has no such ordering and registers it here.
        if (!GIsEditor && !IsRunningCommandlet())
        {
            MCP_ON_POST_ENGINE_INIT.AddRaw(this, &FPinWrightRuntimeModule::OnPostEngineInit);
        }
    }

    virtual void ShutdownModule() override
    {
        MCP_ON_POST_ENGINE_INIT.RemoveAll(this);
        ModalStateProbe::Unregister();
    }

private:
    void OnPostEngineInit()
    {
        ModalStateProbe::Register();
    }
};

IMPLEMENT_MODULE(FPinWrightRuntimeModule, PinWrightRuntime)
