// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/AssetDumpSuggestion.h"
#include "Utils/AssetDumpWriter.h"
#include "Handlers/Asset/AssetDumpCache.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace AssetDumpSuggestion
{

FString BuildDumpSuggestionHint(const FString& PackagePath, EDumpSubjectKind Kind)
{
    // No subject to anchor a suggestion on.
    if (PackagePath.IsEmpty())
    {
        return TEXT("");
    }

    // ResolveDumpDir returns the absolutized mirror directory for this subject.
    const FString DumpDir = AssetDumpWriter::ResolveDumpDir(PackagePath, TEXT(""));

    // bMirrorExists distinguishes a never-dumped subject (missing) from a dumped-but-
    // outdated one (stale); the two states get different wording below.
    const bool bMirrorExists = IFileManager::Get().DirectoryExists(*DumpDir);
    bool bStale = false;

    if (Kind == EDumpSubjectKind::Asset)
    {
        if (bMirrorExists)
        {
            // Freshness only matters once a mirror exists. Without the registry we
            // cannot judge freshness, so stay silent rather than crash or false-flag.
            IAssetRegistry* Registry = IAssetRegistry::Get();
            if (!Registry)
            {
                return TEXT("");
            }

            IAssetRegistry& AssetRegistry = *Registry;
            FString OutDir;
            const bool bFresh = AssetDumpCache::IsDumpFresh(AssetRegistry, PackagePath, TEXT(""), /*bIncludeWidgetScreenshot=*/false, /*bIsMapOrWorldPackage=*/false, /*OutDumpDir*/ OutDir);
            if (bFresh)
            {
                // Dumped and fresh — no nudge.
                return TEXT("");
            }

            // Mirror is present but the cache says the source moved past it.
            bStale = true;
        }
    }
    else // EDumpSubjectKind::Level
    {
        // Freshness isn't tracked for maps/worlds, so an existing mirror is enough.
        if (bMirrorExists)
        {
            return TEXT("");
        }
    }

    // Derive suggestion targets purely from the package path — never a hardcoded mount
    // list, since this plugin ships to third-party projects with their own mounts.
    // MountRoot is the first path segment (e.g. /MyPlugin/Sub/BP_X -> /MyPlugin).
    FString MountRoot;
    {
        FString Trimmed = PackagePath;
        Trimmed.RemoveFromStart(TEXT("/"));
        FString First;
        FString Rest;
        if (Trimmed.Split(TEXT("/"), &First, &Rest))
        {
            MountRoot = TEXT("/") + First;
        }
        else
        {
            MountRoot = TEXT("/") + Trimmed;
        }
    }

    // ContainingFolder is the package path minus its asset-name segment
    // (e.g. /MyPlugin/Sub/BP_X -> /MyPlugin/Sub).
    FString ContainingFolder;
    {
        FString AssetName;
        if (!PackagePath.Split(TEXT("/"), &ContainingFolder, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
            || ContainingFolder.IsEmpty())
        {
            ContainingFolder = MountRoot;
        }
    }

    const bool bUnderGame = (MountRoot == TEXT("/Game"));

    if (Kind == EDumpSubjectKind::Level)
    {
        // Levels are excluded from folder dumps by default, so includeLevels is required;
        // asset.dump is the single-subject alternative.
        return FString::Printf(
            TEXT("No asset-dump mirror for level %s — dump it before further inspection: ")
            TEXT("asset.dump_folder({\"folderPath\":\"%s\",\"includeLevels\":true}) or ")
            TEXT("asset.dump({\"assetPath\":\"%s\"}). Levels are skipped by default, so ")
            TEXT("includeLevels:true is required to include maps/worlds in a folder dump."),
            *PackagePath, *ContainingFolder, *PackagePath);
    }

    // Asset kind: opener depends on missing vs stale; the target choices follow.
    FString Opener;
    if (bStale)
    {
        Opener = FString::Printf(
            TEXT("The asset-dump mirror for %s is STALE (source changed since last dump) — ")
            TEXT("re-running asset.dump_folder is incremental, so no force is needed."),
            *PackagePath);
    }
    else
    {
        Opener = FString::Printf(
            TEXT("No asset-dump mirror for %s — repeated inspection is far cheaper from the cache."),
            *PackagePath);
    }

    if (bUnderGame)
    {
        // Subject already lives under /Game — full project dump is the obvious default;
        // the containing folder is the narrower option. Don't re-list /Game as a fallback.
        return Opener + FString::Printf(
            TEXT(" Run a dump before further inspection: asset.dump_folder({\"folderPath\":\"/Game\"}) ")
            TEXT("for a full project dump (the usual default), or narrow to ")
            TEXT("{\"folderPath\":\"%s\"} if only that part of /Game matters."),
            *ContainingFolder);
    }

    // Subject under a plugin/other mount: lead with the mount root (where the agent is
    // working), then the containing folder, then /Game as the usual full-dump default.
    return Opener + FString::Printf(
        TEXT(" Pick a dump scope and run it before further inspection: ")
        TEXT("asset.dump_folder({\"folderPath\":\"%s\"}) (this plugin, where you're working), or ")
        TEXT("{\"folderPath\":\"%s\"} for just this folder, or ")
        TEXT("{\"folderPath\":\"/Game\"} for a full project dump (the usual default)."),
        *MountRoot, *ContainingFolder);
}

}
