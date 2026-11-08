// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Editor duties the runtime host calls into (docs/runtime-module.md "Host and activation").
// Set by the PinWright editor module at StartupModule, before engine subsystems initialize;
// null in a game process, where every caller takes its game branch.
class IPinWrightEditorHooks
{
public:
    virtual ~IPinWrightEditorHooks() = default;

    // Before draining auto-registrations: load the integration sub-modules whose engine plugin is enabled.
    virtual void BeforeDrainRegistrations() = 0;

    // After catalog init (not in an isolated test child): write the on-disk wiki.
    virtual void AfterCatalogInit() = 0;

    // Editor startup snapshot: initialized boundary seen, package loading, selection set exists.
    virtual void GetReadiness(bool& bOutInitialized, bool& bOutLoadingPackage, bool& bOutSelectionSet) = 0;

    // The engine plugin whose disabled integration owns Method, or empty.
    virtual FString FindSkippedPluginForMethod(const FString& Method) = 0;

    // Wiki location for the transport's tools/call docs paths. Called on the transport I/O thread.
    virtual FString WikiOutputDirectory() = 0;
    // On-disk page path for a method or namespace (normalized); the root index for an empty name.
    virtual FString WikiPagePath(const FString& MethodOrNamespace) = 0;
    // Live render of a page when the disk tree lacks it.
    virtual bool RenderWikiPage(const FString& MethodOrNamespace, FString& OutMarkdown) = 0;
};

namespace PinWright
{
    PINWRIGHTRUNTIME_API void SetEditorHooks(IPinWrightEditorHooks* Hooks);
    PINWRIGHTRUNTIME_API IPinWrightEditorHooks* GetEditorHooks();
}
