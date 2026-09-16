// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"

#include "Misc/EngineVersion.h"
#include "Misc/App.h"
#include "Dispatch/RpcDispatcher.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/GameplayStatics.h"
#include "Editor.h"

// ---- pipeline.get_status ----
REGISTER_RPC_HANDLER("pipeline.get_status", "pipeline", "Return automation bridge status information",
    RPC_NO_PARAMS)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // Connection status
    Result->SetBoolField(TEXT("connected"), true);
    Result->SetStringField(TEXT("bridgeType"), TEXT("HTTP JSON-RPC"));

    // Version sourced from the plugin descriptor (.uplugin VersionName) rather
    // than a hardcoded literal, so it tracks the shipped plugin version.
    FString Version = TEXT("unknown");
    if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright")))
    {
        const FString& Ver = Plugin->GetDescriptor().VersionName;
        if (!Ver.IsEmpty())
        {
            Version = Ver;
        }
    }
    Result->SetStringField(TEXT("version"), Version);
    Result->SetStringField(TEXT("engineVersion"), *FEngineVersion::Current().ToString());
    Result->SetNumberField(TEXT("engineMajor"), ENGINE_MAJOR_VERSION);
    Result->SetNumberField(TEXT("engineMinor"), ENGINE_MINOR_VERSION);

    // Capability flags
    Result->SetBoolField(TEXT("editorMode"), true);

    // Live registry statistics: total registered RPC handlers and the count of
    // distinct top-level namespaces (first dotted segment of each method name),
    // read from the dispatcher's handler map. Replaces the previously hardcoded
    // 1069 / 35 constants that drifted from the real registry.
    int32 TotalActions = 0;
    TSet<FString> Categories;
    if (UPinWrightSubsystem* Subsystem = Ctx.GetSubsystem())
    {
        if (const TSharedPtr<FRpcDispatcher> Dispatcher = Subsystem->GetDispatcher())
        {
            const TMap<FString, FAutomationHandler>& Handlers = Dispatcher->GetHandlers();
            TotalActions = Handlers.Num();
            for (const TPair<FString, FAutomationHandler>& Pair : Handlers)
            {
                FString Namespace;
                FString Verb;
                Categories.Add(Pair.Key.Split(TEXT("."), &Namespace, &Verb) ? Namespace : Pair.Key);
            }
        }
    }
    Result->SetNumberField(TEXT("totalActions"), TotalActions);
    Result->SetNumberField(TEXT("toolCategories"), Categories.Num());

    // Runtime info
    Result->SetStringField(TEXT("platform"), *UGameplayStatics::GetPlatformName());
    Result->SetBoolField(TEXT("isPlayInEditor"), GEditor ? GEditor->IsPlaySessionInProgress() : false);

    // Project info
    Result->SetStringField(TEXT("projectName"), FApp::GetProjectName());

    Ctx.SendSuccess(Result);
    return true;
}
