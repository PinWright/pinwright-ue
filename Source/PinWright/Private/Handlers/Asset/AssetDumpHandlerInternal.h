// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Handlers/Asset/AssetDumpCache.h"
#include "Utils/AssetDumpWriter.h"

// Shared declarations for AssetDumpHandler internals. Production code and
// the optional internal test module both use these helpers without going
// through the full BuildAllFilesForAsset path.
//
// The API export is required for cross-DLL visibility when the optional
// internal test module is enabled.

class UBlueprint;
class UWidgetBlueprint;

namespace AssetDumpHandler
{
    struct FBlueprintPropertiesAspectStatus
    {
        FString Status;
        FString Reason;
        FString BlueprintType;
        int32 PropertyCount = 0;
    };

    PINWRIGHT_API void BuildWidgetTreeAspect_Internal(
        UWidgetBlueprint* WBP,
        TArray<AssetDumpWriter::FDumpFile>& OutFiles,
        TArray<TPair<FString, FString>>* OutFileErrors);

    // Emits properties.json for Blueprints with a valid GeneratedClass:
    //   * GeneratedClass valid  → CDO-vs-parent diff via BuildClassPropertyJson.
    //   * GeneratedClass null   → no sidecar; diagnostic/status records generated_class_missing.
    PINWRIGHT_API void BuildBlueprintPropertiesAspect_Internal(
        UBlueprint* BP,
        TArray<AssetDumpWriter::FDumpFile>& OutFiles,
        TArray<TPair<FString, FString>>* OutFileErrors,
        FBlueprintPropertiesAspectStatus* OutPropertiesStatus = nullptr);

    PINWRIGHT_API TSharedPtr<FJsonObject> BuildBlueprintPropertiesStatusJson(
        const FBlueprintPropertiesAspectStatus& Status);

    // Returns the canonical enum-name string for UBlueprint::BlueprintType.
    // Shared with meta.json blueprintType emission so the two surfaces stay in lockstep.
    PINWRIGHT_API FString BlueprintTypeToStatusString(const UBlueprint* BP);

    PINWRIGHT_API void AttachBlueprintPropertiesStatusToMeta(
        const TSharedPtr<FJsonObject>& Meta,
        const FBlueprintPropertiesAspectStatus& Status);

    // Emits a properties.json containing just `{ "redirectsTo": "<target path>" }`
    // for a UObjectRedirector asset. No deep walk of the redirector's UObject
    // properties (which would surface as an opaque empty object). When the redirect
    // target is null, `redirectsTo` is the empty string but the field is still
    // emitted so consumers know the asset is a redirector with an unresolved target.
    PINWRIGHT_API void BuildRedirectorPropertiesAspect_Internal(
        UObject* Asset,
        TArray<AssetDumpWriter::FDumpFile>& OutFiles,
        TArray<TPair<FString, FString>>* OutFileErrors);

    // True when a .dumpcache.json record may be written for PackageName after a
    // successful dump. Uncached sources never cache. A dirty package vetoes only
    // when the user already had it dirty before the sweep began (present in
    // BaselineDirty): dirt induced by the dump's own LoadObject (Blueprint
    // compile-on-load) still matches the on-disk bytes the fingerprint hashed,
    // so it does not invalidate the record.
    PINWRIGHT_API bool IsCacheEligibleSource(
        const AssetDumpCache::FAssetDumpSourceFingerprint& Source,
        const FString& PackageName,
        const TSet<FName>& BaselineDirty);

    // Writes the .dumpcache.json record beside a successful non-diff dump.
    // BaselineDirty is the sweep's pre-dump dirty-package snapshot consumed by
    // the eligibility gate above. Returns false (no record) for screenshot
    // dumps, empty written-file lists, ineligible sources, or write failures.
    PINWRIGHT_API bool WriteDumpCacheForSuccessfulBaseline(
        const FString& PackageName,
        const FString& DumpDir,
        TConstArrayView<FString> WrittenPaths,
        bool bIncludeWidgetScreenshot,
        const TSet<FName>& BaselineDirty);

    // Deterministically picks the package's primary asset row:
    //   1. a single row wins outright;
    //   2. else the row whose AssetName equals the package tail;
    //   3. else the registry's IsUAsset() row;
    //   4. else the total-order minimum by (AssetName, AssetClassPath) — covers
    //      multi-asset packages with no tail-named row (e.g. "Bake Out Materials"
    //      GUID packages holding an MI_* instance plus a T_* texture).
    // Returns an invalid FAssetData when PackageAssets is empty.
    PINWRIGHT_API FAssetData SelectPrimaryAssetData(
        const TArray<FAssetData>& PackageAssets,
        const FString& PackageName);
}
