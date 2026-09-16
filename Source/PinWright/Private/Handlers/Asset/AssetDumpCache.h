// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class IAssetRegistry;

namespace AssetDumpCache
{
    inline constexpr const TCHAR* DumpCacheFileName = TEXT(".dumpcache.json");
    inline constexpr int32 AssetDumpCacheVersion = 2;
    // 3: the shared property exporter changed for every aspect that serializes UPROPERTYs —
    //    a member the exporter cannot decompose is now an inline `_kind: unsupported` marker
    //    instead of one marker stamped over the whole enclosing struct/collection, and
    //    material-instance ExpressionGUIDs are dropped as non-semantic.
    inline constexpr int32 AssetDumpCoreVersion = 3;
    inline constexpr int32 AssetDumpDefaultAspectVersion = 1;

    enum class EAssetDumpSourceFingerprintKind : uint8
    {
        PackageSavedHash,
        FileStat,
        Uncached
    };

    struct FAssetDumpSourceFingerprint
    {
        EAssetDumpSourceFingerprintKind Kind = EAssetDumpSourceFingerprintKind::Uncached;
        FString PackageExtension;
        FString PackageSavedHash;
        int64 DiskSize = -1;
        int64 PackageFileSize = -1;
        FDateTime PackageFileMTimeUtc = FDateTime::MinValue();
        FString PackageFilename;
        FString CorrectCasePackageName;
        bool bPackageIsDirty = false;
    };

    struct FAssetDumpPackageFacts
    {
        FString PackageName;
        FString CorrectCasePackageName;
        FString PackageFilename;
        FString PackageExtension;
        FString PackageSavedHash;
        int64 RegistryDiskSize = -1;
        int64 PackageFileSize = -1;
        FDateTime PackageFileMTimeUtc = FDateTime::MinValue();
        bool bHasPackageData = false;
        bool bFileStatValid = false;
        bool bIsTextPackage = false;
        bool bIsMapOrWorldPackage = false;
        bool bPackageIsDirty = false;
    };

    struct FAssetDumpOptionsFingerprint
    {
        bool bIncludeWidgetScreenshot = false;
    };

    struct FAssetDumpDumperFingerprint
    {
        int32 DumpCoreVersion = AssetDumpCoreVersion;
        FString EngineVersion;
        // Informational only - the descriptor's marketing VersionName, recorded so a
        // record says which build wrote it. Freshness never compares it; see
        // AreDumperFingerprintsEqual.
        FString PluginVersion;
        TMap<FString, int32> AspectVersions;
    };

    struct FAssetDumpCacheRecord
    {
        int32 CacheVersion = AssetDumpCacheVersion;
        FString AssetPath;
        FString PackageName;
        FString CorrectCasePackageName;
        FAssetDumpSourceFingerprint Source;
        FAssetDumpDumperFingerprint Dumper;
        FAssetDumpOptionsFingerprint Options;
        TArray<FString> WrittenFiles;
        // Skip-stub marker: set when this record caches a failed-load stub dump
        // (meta.json only) instead of a successful full dump. Additive fields —
        // absent on older records, defaulted false/empty on read, no version bump.
        bool bSkipped = false;
        FString SkipReason;
    };

    struct FAssetDumpFreshnessResult
    {
        bool bFresh = false;
        FString Reason;

        static FAssetDumpFreshnessResult Fresh();
        static FAssetDumpFreshnessResult Stale(const FString& InReason);
    };

    PINWRIGHT_API FString GetCachePath(const FString& DumpDir);
    PINWRIGHT_API FString NormalizeRelativeWrittenFile(const FString& RelativePath);
    PINWRIGHT_API TArray<FString> MakeRelativeWrittenFiles(
        const FString& DumpDir,
        TConstArrayView<FString> WrittenPaths);

    PINWRIGHT_API int32 GetAspectVersion(const FString& RelativeFile);
    PINWRIGHT_API TMap<FString, int32> MakeCurrentAspectVersions(
        TConstArrayView<FString> RelativeWrittenFiles);
    PINWRIGHT_API FAssetDumpDumperFingerprint MakeCurrentDumperFingerprint(
        const TMap<FString, int32>& AspectVersions);

    PINWRIGHT_API FAssetDumpSourceFingerprint BuildSourceFingerprintFromFacts(
        const FAssetDumpPackageFacts& Facts);
    PINWRIGHT_API FAssetDumpSourceFingerprint BuildSourceFingerprintFromRegistry(
        IAssetRegistry& AssetRegistry,
        const FString& PackageName,
        bool bIsMapOrWorldPackage);
    PINWRIGHT_API bool IsPackageDirtyWithoutLoading(const FString& PackageName);

    PINWRIGHT_API FAssetDumpCacheRecord MakeCacheRecord(
        const FString& AssetPath,
        const FString& PackageName,
        const FString& CorrectCasePackageName,
        const FAssetDumpSourceFingerprint& Source,
        const FAssetDumpDumperFingerprint& Dumper,
        const FAssetDumpOptionsFingerprint& Options,
        TConstArrayView<FString> RelativeWrittenFiles);

    PINWRIGHT_API bool ReadCacheRecord(
        const FString& DumpDir,
        FAssetDumpCacheRecord& OutRecord,
        FString& OutError);
    PINWRIGHT_API bool WriteCacheRecord(
        const FString& DumpDir,
        const FAssetDumpCacheRecord& Record,
        FString& OutError);

    PINWRIGHT_API FAssetDumpFreshnessResult IsCacheFresh(
        const FString& DumpDir,
        const FString& PackageName,
        const FAssetDumpSourceFingerprint& CurrentSource,
        const FAssetDumpDumperFingerprint& ExpectedDumper,
        const FAssetDumpOptionsFingerprint& ExpectedOptions,
        const FAssetDumpCacheRecord& Record);
    PINWRIGHT_API FAssetDumpFreshnessResult IsCacheFresh(
        const FString& DumpDir,
        const FString& PackageName,
        const FAssetDumpSourceFingerprint& CurrentSource,
        const FAssetDumpDumperFingerprint& ExpectedDumper,
        const FAssetDumpOptionsFingerprint& ExpectedOptions);

    // Resolves the dump dir for PackageName under OutRoot and reports whether the
    // on-disk dump there is still fresh for the current source/dumper/options.
    // Shared by the folder-dump skip check and the dump-suggestion surface.
    PINWRIGHT_API bool IsDumpFresh(
        IAssetRegistry& AssetRegistry,
        const FString& PackageName,
        const FString& OutRoot,
        bool bIncludeWidgetScreenshot,
        bool bIsMapOrWorldPackage,
        FString& OutDumpDir);
}
