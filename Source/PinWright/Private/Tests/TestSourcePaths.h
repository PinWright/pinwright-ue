// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

// Absolute path of a plugin-relative source file ("Source/PinWright/Private/Dispatch/X.cpp") that
// may have moved to PinWrightRuntime (docs/runtime-module.md keeps the path below the module root):
// a Source/PinWright/{Private,Public}/<tail> path missing from the editor module resolves to
// <tail> under PinWrightRuntime's Private, then Public root. Returns the unmoved path when no
// candidate exists, so the caller's own load assertion reports the miss. Separate from
// TestUtils.h because integration modules include that without a Projects dependency.
inline FString ResolvePluginSourcePath(const FString& RelativePath)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    const FString BaseDir = Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
    const FString Direct = BaseDir / RelativePath;
    if (FPaths::FileExists(Direct))
    {
        return Direct;
    }
    const FString Rel = RelativePath.Replace(TEXT("\\"), TEXT("/"));
    const TCHAR* const EditorRoots[] = { TEXT("Source/PinWright/Private/"), TEXT("Source/PinWright/Public/") };
    const TCHAR* const RuntimeRoots[] = { TEXT("Source/PinWrightRuntime/Private/"), TEXT("Source/PinWrightRuntime/Public/") };
    for (const TCHAR* EditorRoot : EditorRoots)
    {
        if (!Rel.StartsWith(EditorRoot, ESearchCase::CaseSensitive))
        {
            continue;
        }
        const FString Tail = Rel.RightChop(FCString::Strlen(EditorRoot));
        for (const TCHAR* RuntimeRoot : RuntimeRoots)
        {
            const FString Candidate = BaseDir / (FString(RuntimeRoot) + Tail);
            if (FPaths::FileExists(Candidate))
            {
                return Candidate;
            }
        }
    }
    return Direct;
}
