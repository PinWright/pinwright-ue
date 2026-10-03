// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/AssetDumpSuggestion.h"
#include "Utils/AssetDumpWriter.h"
#include "Utils/AssetDumpBuilder.h"
#include "Handlers/Asset/AssetDumpCache.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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
        // Maps have no cache fingerprint; the mirror's own meta.json source stamp is the
        // freshness record. A mirror without one (written before the stamp existed) cannot
        // be judged and stays silent.
        // ponytail: compares the map file only. An actor in its own external package (OFPA)
        // moved and saved changes that package, not the map: actors/manifest.json stamps each
        // entry, but hashing every one on every call is too slow for a hint.
        if (bMirrorExists)
        {
            FString MetaText;
            TSharedPtr<FJsonObject> Meta;
            const TSharedPtr<FJsonObject>* RecordedSource = nullptr;
            if (!FFileHelper::LoadFileToString(MetaText, *(DumpDir / TEXT("meta.json")))
                || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MetaText), Meta)
                || !Meta.IsValid()
                || !Meta->TryGetObjectField(TEXT("source"), RecordedSource))
            {
                return TEXT("");
            }
            FString RecordedMd5;
            FString CurrentMd5;
            (*RecordedSource)->TryGetStringField(TEXT("fileMd5"), RecordedMd5);
            AssetDumpBuilder::BuildSourceStampJson(PackagePath)->TryGetStringField(TEXT("fileMd5"), CurrentMd5);
            if (RecordedMd5 == CurrentMd5)
            {
                return TEXT("");
            }
            bStale = true;
        }
    }

    // ContainingFolder is the package path minus its asset-name segment
    // (e.g. /MyPlugin/Sub/BP_X -> /MyPlugin/Sub). Derived from the path, never a
    // hardcoded mount list, since this plugin ships to projects with their own mounts.
    FString ContainingFolder;
    {
        FString AssetName;
        if (!PackagePath.Split(TEXT("/"), &ContainingFolder, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
            || ContainingFolder.IsEmpty())
        {
            ContainingFolder = PackagePath;
        }
    }

    // A subject directly under a mount root (/Game/BP_X) has the mount root as its folder,
    // i.e. a whole-project or whole-plugin sweep; offer only the single-subject dump then.
    const bool bOfferFolder = ContainingFolder.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd) > 0;

    if (Kind == EDumpSubjectKind::Level && bStale)
    {
        return FString::Printf(
            TEXT("The asset-dump mirror for level %s is STALE (the map file changed since it was dumped; ")
            TEXT("meta.json source.fileMd5 no longer matches) — re-dump it before reading it: ")
            TEXT("asset.dump({\"assetPath\":\"%s\"})."),
            *PackagePath, *PackagePath);
    }

    if (Kind == EDumpSubjectKind::Level)
    {
        // Lead with the single-level dump; the folder form needs includeLevels because
        // levels are excluded from folder dumps by default.
        FString Hint = FString::Printf(
            TEXT("No asset-dump mirror for level %s — dump it before further inspection: ")
            TEXT("asset.dump({\"assetPath\":\"%s\"})."),
            *PackagePath, *PackagePath);
        if (bOfferFolder)
        {
            Hint += FString::Printf(
                TEXT(" To dump every asset in its folder instead: ")
                TEXT("asset.dump_folder({\"folderPath\":\"%s\",\"includeLevels\":true}). Folder dumps skip ")
                TEXT("levels by default, so includeLevels:true is required to include maps/worlds."),
                *ContainingFolder);
        }
        return Hint;
    }

    // Asset kind: opener depends on missing vs stale. The staleness is per asset and the
    // editor may be shared, so lead with the single-asset refresh; a folder sweep is only
    // offered for this asset's own folder, and never when that folder is a mount root.
    FString Hint = bStale
        ? FString::Printf(TEXT("The asset-dump mirror for %s is STALE (source changed since last dump)."), *PackagePath)
        : FString::Printf(TEXT("No asset-dump mirror for %s — repeated inspection is far cheaper from the cache."), *PackagePath);

    Hint += FString::Printf(
        TEXT(" Dump just this asset before further inspection: asset.dump({\"assetPath\":\"%s\"})."),
        *PackagePath);
    if (bOfferFolder)
    {
        Hint += FString::Printf(
            TEXT(" To refresh its whole folder instead, asset.dump_folder({\"folderPath\":\"%s\"}) ")
            TEXT("dumps every non-level asset under that path (levels are skipped by default; ")
            TEXT("incremental: unchanged assets are skipped)."),
            *ContainingFolder);
    }
    return Hint;
}

}
