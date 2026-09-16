// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/AssetDumpCache.h"

#include "Utils/AssetDumpWriter.h"
#include "Utils/SortedJsonWriter.h"
#include "PinWrightSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "IO/IoHash.h"
#include "Misc/EngineVersion.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr const TCHAR* FieldCacheVersion = TEXT("cacheVersion");
    constexpr const TCHAR* FieldAssetPath = TEXT("assetPath");
    constexpr const TCHAR* FieldPackageName = TEXT("packageName");
    constexpr const TCHAR* FieldCorrectCasePackageName = TEXT("correctCasePackageName");
    constexpr const TCHAR* FieldSource = TEXT("source");
    constexpr const TCHAR* FieldFingerprintKind = TEXT("fingerprintKind");
    constexpr const TCHAR* FieldPackageExtension = TEXT("packageExtension");
    constexpr const TCHAR* FieldPackageSavedHash = TEXT("packageSavedHash");
    constexpr const TCHAR* FieldDiskSize = TEXT("diskSize");
    constexpr const TCHAR* FieldPackageFileSize = TEXT("packageFileSize");
    constexpr const TCHAR* FieldMTimeUtc = TEXT("mtimeUtc");
    constexpr const TCHAR* FieldDumper = TEXT("dumper");
    constexpr const TCHAR* FieldDumpCoreVersion = TEXT("dumpCoreVersion");
    constexpr const TCHAR* FieldAspectVersions = TEXT("aspectVersions");
    constexpr const TCHAR* FieldEngineVersion = TEXT("engineVersion");
    constexpr const TCHAR* FieldPluginVersion = TEXT("pluginVersion");
    constexpr const TCHAR* FieldOptions = TEXT("options");
    constexpr const TCHAR* FieldIncludeWidgetScreenshot = TEXT("includeWidgetScreenshot");
    constexpr const TCHAR* FieldWrittenFiles = TEXT("writtenFiles");
    constexpr const TCHAR* FieldSkipped = TEXT("skipped");
    constexpr const TCHAR* FieldSkipReason = TEXT("skipReason");

    FString SourceKindToString(AssetDumpCache::EAssetDumpSourceFingerprintKind Kind)
    {
        switch (Kind)
        {
        case AssetDumpCache::EAssetDumpSourceFingerprintKind::PackageSavedHash:
            return TEXT("packageSavedHash");
        case AssetDumpCache::EAssetDumpSourceFingerprintKind::FileStat:
            return TEXT("fileStat");
        case AssetDumpCache::EAssetDumpSourceFingerprintKind::Uncached:
        default:
            return TEXT("uncached");
        }
    }

    bool TryParseSourceKind(const FString& Text, AssetDumpCache::EAssetDumpSourceFingerprintKind& OutKind)
    {
        if (Text == TEXT("packageSavedHash"))
        {
            OutKind = AssetDumpCache::EAssetDumpSourceFingerprintKind::PackageSavedHash;
            return true;
        }
        if (Text == TEXT("fileStat"))
        {
            OutKind = AssetDumpCache::EAssetDumpSourceFingerprintKind::FileStat;
            return true;
        }
        if (Text == TEXT("uncached"))
        {
            OutKind = AssetDumpCache::EAssetDumpSourceFingerprintKind::Uncached;
            return true;
        }
        return false;
    }

    FString PackageExtensionToCacheString(EPackageExtension Extension)
    {
        switch (Extension)
        {
        case EPackageExtension::Asset:
            return TEXT("Asset");
        case EPackageExtension::Map:
            return TEXT("Map");
        case EPackageExtension::TextAsset:
            return TEXT("TextAsset");
        case EPackageExtension::TextMap:
            return TEXT("TextMap");
        case EPackageExtension::Custom:
            return TEXT("Custom");
        case EPackageExtension::EmptyString:
            return TEXT("EmptyString");
        case EPackageExtension::Unspecified:
        default:
            return TEXT("Unspecified");
        }
    }

    FString FileExtensionToCacheString(const FString& Extension)
    {
        if (Extension.Equals(TEXT(".uasset"), ESearchCase::IgnoreCase))
        {
            return TEXT("Asset");
        }
        if (Extension.Equals(TEXT(".umap"), ESearchCase::IgnoreCase))
        {
            return TEXT("Map");
        }
        if (Extension.Equals(TEXT(".utxt"), ESearchCase::IgnoreCase))
        {
            return TEXT("TextAsset");
        }
        if (Extension.Equals(TEXT(".utxtmap"), ESearchCase::IgnoreCase))
        {
            return TEXT("TextMap");
        }
        return Extension;
    }

    bool IsMapExtensionString(const FString& Extension)
    {
        return Extension == TEXT("Map")
            || Extension == TEXT("TextMap")
            || Extension.Equals(TEXT(".umap"), ESearchCase::IgnoreCase)
            || Extension.Equals(TEXT(".utxtmap"), ESearchCase::IgnoreCase);
    }

    bool IsTextExtensionString(const FString& Extension)
    {
        return Extension == TEXT("TextAsset")
            || Extension == TEXT("TextMap")
            || Extension.Equals(TEXT(".utxt"), ESearchCase::IgnoreCase)
            || Extension.Equals(TEXT(".utxtmap"), ESearchCase::IgnoreCase);
    }

    bool IsZeroHashString(const FString& Hash)
    {
        if (Hash.IsEmpty())
        {
            return true;
        }
        for (TCHAR Char : Hash)
        {
            if (Char != TEXT('0'))
            {
                return false;
            }
        }
        return true;
    }

    FDateTime NormalizeFileMTimeForCache(FDateTime Value)
    {
        if (Value == FDateTime::MinValue())
        {
            return Value;
        }
        return FDateTime((Value.GetTicks() / ETimespan::TicksPerMillisecond) * ETimespan::TicksPerMillisecond);
    }

    FString MakeDirectoryRelativeBase(const FString& Dir)
    {
        FString AbsDir = FPaths::ConvertRelativePathToFull(Dir);
        FPaths::NormalizeDirectoryName(AbsDir);
        // MakePathRelativeTo treats a slashless base as a file path, not a directory.
        if (!AbsDir.EndsWith(TEXT("/")))
        {
            AbsDir += TEXT("/");
        }
        return AbsDir;
    }

    bool IsSafeRelativeDumpFile(const FString& RelativeFile)
    {
        return !RelativeFile.IsEmpty()
            && !RelativeFile.Contains(TEXT(":"))
            && !RelativeFile.StartsWith(TEXT("/"))
            && !RelativeFile.StartsWith(TEXT("\\"))
            && !RelativeFile.Contains(TEXT(".."))
            && RelativeFile != AssetDumpCache::DumpCacheFileName;
    }

    bool TryGetNumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, int64& OutValue)
    {
        double Number = 0.0;
        if (!Object.IsValid() || !Object->TryGetNumberField(FieldName, Number))
        {
            return false;
        }
        OutValue = static_cast<int64>(Number);
        return true;
    }

    bool TryGetNumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, int32& OutValue)
    {
        double Number = 0.0;
        if (!Object.IsValid() || !Object->TryGetNumberField(FieldName, Number))
        {
            return false;
        }
        OutValue = static_cast<int32>(Number);
        return true;
    }

    bool TryGetBoolField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, bool& OutValue)
    {
        return Object.IsValid() && Object->TryGetBoolField(FieldName, OutValue);
    }

    bool TryGetStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FString& OutValue)
    {
        return Object.IsValid() && Object->TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty();
    }

    bool TryReadSource(
        const TSharedPtr<FJsonObject>& Root,
        AssetDumpCache::FAssetDumpSourceFingerprint& OutSource,
        FString& OutError)
    {
        const TSharedPtr<FJsonObject>* SourceObject = nullptr;
        if (!Root->TryGetObjectField(FieldSource, SourceObject) || !SourceObject || !SourceObject->IsValid())
        {
            OutError = TEXT("missing source");
            return false;
        }

        FString KindText;
        if (!TryGetStringField(*SourceObject, FieldFingerprintKind, KindText)
            || !TryParseSourceKind(KindText, OutSource.Kind))
        {
            OutError = TEXT("invalid source fingerprintKind");
            return false;
        }

        if (!TryGetStringField(*SourceObject, FieldPackageExtension, OutSource.PackageExtension))
        {
            OutError = TEXT("missing source packageExtension");
            return false;
        }

        if (OutSource.Kind == AssetDumpCache::EAssetDumpSourceFingerprintKind::PackageSavedHash)
        {
            if (!TryGetStringField(*SourceObject, FieldPackageSavedHash, OutSource.PackageSavedHash)
                || !TryGetNumberField(*SourceObject, FieldDiskSize, OutSource.DiskSize))
            {
                OutError = TEXT("invalid packageSavedHash source");
                return false;
            }
        }
        else if (OutSource.Kind == AssetDumpCache::EAssetDumpSourceFingerprintKind::FileStat)
        {
            FString MTimeText;
            if (!TryGetNumberField(*SourceObject, FieldPackageFileSize, OutSource.PackageFileSize)
                || !TryGetStringField(*SourceObject, FieldMTimeUtc, MTimeText)
                || !FDateTime::ParseIso8601(*MTimeText, OutSource.PackageFileMTimeUtc))
            {
                OutError = TEXT("invalid fileStat source");
                return false;
            }
        }

        return true;
    }

    TSharedPtr<FJsonObject> BuildSourceJson(const AssetDumpCache::FAssetDumpSourceFingerprint& Source)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(FieldFingerprintKind, SourceKindToString(Source.Kind));
        Object->SetStringField(FieldPackageExtension, Source.PackageExtension);

        if (Source.Kind == AssetDumpCache::EAssetDumpSourceFingerprintKind::PackageSavedHash)
        {
            Object->SetStringField(FieldPackageSavedHash, Source.PackageSavedHash);
            Object->SetNumberField(FieldDiskSize, static_cast<double>(Source.DiskSize));
        }
        else if (Source.Kind == AssetDumpCache::EAssetDumpSourceFingerprintKind::FileStat)
        {
            Object->SetNumberField(FieldPackageFileSize, static_cast<double>(Source.PackageFileSize));
            Object->SetStringField(FieldMTimeUtc, Source.PackageFileMTimeUtc.ToIso8601());
        }

        return Object;
    }

    bool TryReadDumper(
        const TSharedPtr<FJsonObject>& Root,
        AssetDumpCache::FAssetDumpDumperFingerprint& OutDumper,
        FString& OutError)
    {
        const TSharedPtr<FJsonObject>* DumperObject = nullptr;
        if (!Root->TryGetObjectField(FieldDumper, DumperObject) || !DumperObject || !DumperObject->IsValid())
        {
            OutError = TEXT("missing dumper");
            return false;
        }

        if (!TryGetNumberField(*DumperObject, FieldDumpCoreVersion, OutDumper.DumpCoreVersion)
            || !TryGetStringField(*DumperObject, FieldEngineVersion, OutDumper.EngineVersion)
            || !TryGetStringField(*DumperObject, FieldPluginVersion, OutDumper.PluginVersion))
        {
            OutError = TEXT("invalid dumper fields");
            return false;
        }

        const TSharedPtr<FJsonObject>* AspectObject = nullptr;
        if (!(*DumperObject)->TryGetObjectField(FieldAspectVersions, AspectObject)
            || !AspectObject
            || !AspectObject->IsValid())
        {
            OutError = TEXT("missing aspectVersions");
            return false;
        }

        OutDumper.AspectVersions.Reset();
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : (*AspectObject)->Values)
        {
            if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number)
            {
                OutError = TEXT("invalid aspect version");
                return false;
            }
            OutDumper.AspectVersions.Add(Pair.Key, static_cast<int32>(Pair.Value->AsNumber()));
        }

        return true;
    }

    TSharedPtr<FJsonObject> BuildDumperJson(const AssetDumpCache::FAssetDumpDumperFingerprint& Dumper)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(FieldDumpCoreVersion, Dumper.DumpCoreVersion);
        Object->SetStringField(FieldEngineVersion, Dumper.EngineVersion);
        Object->SetStringField(FieldPluginVersion, Dumper.PluginVersion);

        TSharedPtr<FJsonObject> AspectVersions = MakeShared<FJsonObject>();
        TArray<FString> Keys;
        Dumper.AspectVersions.GetKeys(Keys);
        Keys.Sort();
        for (const FString& Key : Keys)
        {
            AspectVersions->SetNumberField(Key, Dumper.AspectVersions[Key]);
        }
        Object->SetObjectField(FieldAspectVersions, AspectVersions);

        return Object;
    }

    bool TryReadOptions(
        const TSharedPtr<FJsonObject>& Root,
        AssetDumpCache::FAssetDumpOptionsFingerprint& OutOptions,
        FString& OutError)
    {
        const TSharedPtr<FJsonObject>* OptionsObject = nullptr;
        if (!Root->TryGetObjectField(FieldOptions, OptionsObject) || !OptionsObject || !OptionsObject->IsValid())
        {
            OutError = TEXT("missing options");
            return false;
        }
        if (!TryGetBoolField(*OptionsObject, FieldIncludeWidgetScreenshot, OutOptions.bIncludeWidgetScreenshot))
        {
            OutError = TEXT("missing includeWidgetScreenshot option");
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> BuildOptionsJson(const AssetDumpCache::FAssetDumpOptionsFingerprint& Options)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetBoolField(FieldIncludeWidgetScreenshot, Options.bIncludeWidgetScreenshot);
        return Object;
    }

    bool TryReadWrittenFiles(
        const TSharedPtr<FJsonObject>& Root,
        TArray<FString>& OutWrittenFiles,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Root->TryGetArrayField(FieldWrittenFiles, Values) || !Values)
        {
            OutError = TEXT("missing writtenFiles");
            return false;
        }

        OutWrittenFiles.Reset();
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                OutError = TEXT("invalid writtenFiles entry");
                return false;
            }

            FString Relative = AssetDumpCache::NormalizeRelativeWrittenFile(Value->AsString());
            if (!IsSafeRelativeDumpFile(Relative))
            {
                OutError = TEXT("unsafe writtenFiles entry");
                return false;
            }
            OutWrittenFiles.AddUnique(MoveTemp(Relative));
        }
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> BuildWrittenFilesJson(TConstArrayView<FString> WrittenFiles)
    {
        TArray<FString> Sorted;
        for (const FString& WrittenFile : WrittenFiles)
        {
            Sorted.Add(WrittenFile);
        }
        Sorted.Sort();

        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FString& RelativeFile : Sorted)
        {
            Values.Add(MakeShared<FJsonValueString>(RelativeFile));
        }
        return Values;
    }

    bool AreSourceFingerprintsEqual(
        const AssetDumpCache::FAssetDumpSourceFingerprint& A,
        const AssetDumpCache::FAssetDumpSourceFingerprint& B)
    {
        if (A.Kind != B.Kind || A.PackageExtension != B.PackageExtension)
        {
            return false;
        }

        if (A.Kind == AssetDumpCache::EAssetDumpSourceFingerprintKind::PackageSavedHash)
        {
            return A.PackageSavedHash == B.PackageSavedHash
                && A.DiskSize == B.DiskSize;
        }

        if (A.Kind == AssetDumpCache::EAssetDumpSourceFingerprintKind::FileStat)
        {
            return A.PackageFileSize == B.PackageFileSize
                && A.PackageFileMTimeUtc == B.PackageFileMTimeUtc;
        }

        return false;
    }

    bool AreAspectVersionsEqual(
        const TMap<FString, int32>& A,
        const TMap<FString, int32>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }

        for (const TPair<FString, int32>& Pair : A)
        {
            const int32* Other = B.Find(Pair.Key);
            if (!Other || *Other != Pair.Value)
            {
                return false;
            }
        }
        return true;
    }

    // PluginVersion is deliberately NOT compared. It is the .uplugin descriptor's
    // marketing VersionName, bumped for release reasons that never change dump output,
    // and folding it in invalidated every record in the mirror on each bump - turning
    // the next incremental sweep into a full 22k-asset reload. What actually describes
    // the output is DumpCoreVersion plus the per-aspect schema versions, and any change
    // that alters a sidecar must already bump one of those to be correct; that is the
    // contract to enforce. The field is still written and read so a record stays
    // self-describing, and `force: true` remains the way to re-dump on demand.
    // Board: B-dumpcache-fingerprint-includes-marketing-version.
    bool AreDumperFingerprintsEqual(
        const AssetDumpCache::FAssetDumpDumperFingerprint& A,
        const AssetDumpCache::FAssetDumpDumperFingerprint& B)
    {
        return A.DumpCoreVersion == B.DumpCoreVersion
            && A.EngineVersion == B.EngineVersion
            && AreAspectVersionsEqual(A.AspectVersions, B.AspectVersions);
    }

    FString GetPluginVersion()
    {
        TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("unknown");
    }

    bool HasAllWrittenFiles(const FString& DumpDir, TConstArrayView<FString> WrittenFiles, FString& OutReason)
    {
        if (WrittenFiles.IsEmpty())
        {
            OutReason = TEXT("writtenFiles is empty");
            return false;
        }

        IFileManager& FileManager = IFileManager::Get();
        for (const FString& RelativeFile : WrittenFiles)
        {
            if (!IsSafeRelativeDumpFile(RelativeFile))
            {
                OutReason = FString::Printf(TEXT("unsafe written file '%s'"), *RelativeFile);
                return false;
            }
            const FString AbsoluteFile = DumpDir / RelativeFile;
            if (!FileManager.FileExists(*AbsoluteFile))
            {
                OutReason = FString::Printf(TEXT("missing written file '%s'"), *RelativeFile);
                return false;
            }
        }
        return true;
    }

    bool HasOnlyListedDumpFiles(const FString& DumpDir, TConstArrayView<FString> WrittenFiles, FString& OutReason)
    {
        TSet<FString> AllowedFiles;
        for (const FString& RelativeFile : WrittenFiles)
        {
            AllowedFiles.Add(RelativeFile);
        }
        AllowedFiles.Add(AssetDumpCache::DumpCacheFileName);

        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(
            Files,
            *DumpDir,
            TEXT("*"),
            /*Files=*/true,
            /*Directories=*/false,
            /*bClearFileNames=*/true);

        const FString AbsDir = MakeDirectoryRelativeBase(DumpDir);

        for (FString File : Files)
        {
            FPaths::NormalizeFilename(File);
            File = FPaths::ConvertRelativePathToFull(File);

            FString RelativeFile = File;
            if (!FPaths::MakePathRelativeTo(RelativeFile, *AbsDir))
            {
                OutReason = FString::Printf(TEXT("file outside dump dir '%s'"), *File);
                return false;
            }
            RelativeFile = AssetDumpCache::NormalizeRelativeWrittenFile(RelativeFile);

            if (!AllowedFiles.Contains(RelativeFile))
            {
                OutReason = FString::Printf(TEXT("unlisted dump file '%s'"), *RelativeFile);
                return false;
            }
        }
        return true;
    }

    bool ResolvePackageFile(
        IAssetRegistry& AssetRegistry,
        const FString& PackageName,
        const FString& PackageExtension,
        FString& OutPackageFilename)
    {
        FString CorrectCasePackageName;
        FString DiskExtension;
        if (AssetRegistry.DoesPackageExistOnDisk(FName(*PackageName), &CorrectCasePackageName, &DiskExtension))
        {
            const FString PackageNameForFile = CorrectCasePackageName.IsEmpty() ? PackageName : CorrectCasePackageName;
            if (!DiskExtension.IsEmpty()
                && FPackageName::TryConvertLongPackageNameToFilename(PackageNameForFile, OutPackageFilename, DiskExtension))
            {
                FPaths::NormalizeFilename(OutPackageFilename);
                OutPackageFilename = FPaths::ConvertRelativePathToFull(OutPackageFilename);
                return true;
            }
        }

        if (FPackageName::DoesPackageExist(PackageName, &OutPackageFilename))
        {
            FPaths::NormalizeFilename(OutPackageFilename);
            OutPackageFilename = FPaths::ConvertRelativePathToFull(OutPackageFilename);
            return true;
        }

        FString ExtensionForFile;
        if (PackageExtension == TEXT("Asset"))
        {
            ExtensionForFile = FPackageName::GetAssetPackageExtension();
        }
        else if (PackageExtension == TEXT("Map"))
        {
            ExtensionForFile = FPackageName::GetMapPackageExtension();
        }
        else if (PackageExtension == TEXT("TextAsset"))
        {
            ExtensionForFile = LexToString(EPackageExtension::TextAsset);
        }
        else if (PackageExtension == TEXT("TextMap"))
        {
            ExtensionForFile = LexToString(EPackageExtension::TextMap);
        }

        if (!ExtensionForFile.IsEmpty()
            && FPackageName::TryConvertLongPackageNameToFilename(PackageName, OutPackageFilename, ExtensionForFile))
        {
            FPaths::NormalizeFilename(OutPackageFilename);
            OutPackageFilename = FPaths::ConvertRelativePathToFull(OutPackageFilename);
            return true;
        }

        return false;
    }

    void AddFileStatFacts(AssetDumpCache::FAssetDumpPackageFacts& Facts)
    {
        if (Facts.PackageFilename.IsEmpty())
        {
            return;
        }

        const FFileStatData Stat = IFileManager::Get().GetStatData(*Facts.PackageFilename);
        if (!Stat.bIsValid || Stat.bIsDirectory || Stat.FileSize < 0)
        {
            return;
        }

        Facts.bFileStatValid = true;
        Facts.PackageFileSize = Stat.FileSize;
        Facts.PackageFileMTimeUtc = NormalizeFileMTimeForCache(Stat.ModificationTime);
    }
}

namespace AssetDumpCache
{

FAssetDumpFreshnessResult FAssetDumpFreshnessResult::Fresh()
{
    FAssetDumpFreshnessResult Result;
    Result.bFresh = true;
    return Result;
}

FAssetDumpFreshnessResult FAssetDumpFreshnessResult::Stale(const FString& InReason)
{
    FAssetDumpFreshnessResult Result;
    Result.bFresh = false;
    Result.Reason = InReason;
    return Result;
}

FString GetCachePath(const FString& DumpDir)
{
    return DumpDir / DumpCacheFileName;
}

FString NormalizeRelativeWrittenFile(const FString& RelativePath)
{
    FString Normalized = RelativePath;
    FPaths::NormalizeFilename(Normalized);
    while (Normalized.StartsWith(TEXT("./")))
    {
        Normalized.RightChopInline(2);
    }
    return Normalized;
}

TArray<FString> MakeRelativeWrittenFiles(const FString& DumpDir, TConstArrayView<FString> WrittenPaths)
{
    const FString AbsDir = MakeDirectoryRelativeBase(DumpDir);

    TArray<FString> RelativeFiles;
    for (FString Path : WrittenPaths)
    {
        if (Path.IsEmpty())
        {
            continue;
        }

        FPaths::NormalizeFilename(Path);
        if (!FPaths::IsRelative(Path))
        {
            Path = FPaths::ConvertRelativePathToFull(Path);
            if (!FPaths::MakePathRelativeTo(Path, *AbsDir))
            {
                continue;
            }
        }

        Path = NormalizeRelativeWrittenFile(Path);
        if (IsSafeRelativeDumpFile(Path))
        {
            RelativeFiles.AddUnique(MoveTemp(Path));
        }
    }
    RelativeFiles.Sort();
    return RelativeFiles;
}

int32 GetAspectVersion(const FString& RelativeFile)
{
    // Bump an aspect here when its generator's serialized output changes.
    // Stale caches with a lower number get invalidated automatically on the
    // next dump pass; aspects not listed stay at AssetDumpDefaultAspectVersion.
    // See plugin CLAUDE.md "Aspect Version Bumping" for the discipline.
    static const TMap<FString, int32> Versions = {
        { TEXT("tree.xml"),                 3 },
        { TEXT("widget_animations.json"),   2 },
        // 5: component-template properties go through the shared exporter, so a template
        //    holding an undecodable member (FNavAgentProperties, TObjectPtr<UThumbnailInfo>)
        //    now carries the rest of its fields instead of one whole-value marker.
        { TEXT("scs.json"),                 5 },
        { TEXT("scs.txt"),                  1 },
        // 3: each enum staticSwitchInputs entry now carries enumOptions, the index/name/label
        //    table its numeric `value` is expressed in.
        // 4: a staticSwitchInputs override whose stored value cannot be decoded no longer
        //    fabricates a `value`; it carries rawValue + valueError instead.
        { TEXT("niagara_model.json"),       4 },
        { TEXT("niagara_system.json"),      2 },
        { TEXT("niagara_emitters.json"),    2 },
        // 2: moduleInputs (typed per-input schema) added to each stack module object.
        // 3: enumOptions added to each enum staticSwitchInputs entry, as for niagara_model.json.
        // 4: undecodable staticSwitchInputs override carries rawValue + valueError and no
        //    `value`, as for niagara_model.json.
        // 5: each owner's shared graph is walked once instead of once per script, so a module
        //    appears once (was 4x per emitter / 2x per system) and carries a `scriptUsage`
        //    naming its stage; `index` now restarts per stage rather than per script.
        // 6: each module carries `entryKey`, the owner-qualified "<ownerName>:<entryId>" form of
        //    its id; `entryId` alone is a NodeGuid that duplicated emitters share.
        // 7: each moduleInputs entry carries `reachable` (plus `gatedBy` when a static switch
        //    strands it), and a `valueMode: "default"` entry carries the script-declared
        //    `defaultMode` / `defaultValue` / `defaultBinding`.
        // 8: `defaultValue` is read off the script variable's `Variable` rather than its
        //    `DefaultValueVariant`, which a panel-authored declaration leaves zero-filled; a 7
        //    dump reports the type's zero for every such input.
        { TEXT("niagara_stack.json"),       8 },
        // 2: every rapid-iteration entry whose module input also carries a graph override pin now
        //    carries `overridden` plus an `override` object naming that pin's mode and value, and
        //    the document carries a `rapidIterationNote` stating what this store can and cannot
        //    see. First row this aspect has had -- it was served at AssetDumpDefaultAspectVersion,
        //    so 1 -> 2 invalidates exactly like a bump on any listed aspect.
        { TEXT("niagara_parameters.json"),  2 },
        // 3: a linked input pin no longer reports its inert stored default as `defaultValue`;
        //    a non-empty one moves to rawDefaultValue + defaultValueError.
        // 4: a Parameter Map Get default pin names the output pin it backs
        //    (defaultForOutputPin / defaultForPinId); previously it was anonymous and unjoinable.
        { TEXT("niagara_graphs.json"),      4 },
        { TEXT("niagara_compile.json"),     3 },
        { TEXT("meta.json"),                6 },
        // 8: an unsupported member is marked in place instead of collapsing its enclosing
        //    struct, array, map, or set to a single marker; material-instance parameter
        //    ExpressionGUIDs are dropped (session-dependent cache of the parent material's
        //    expression link, not authored state).
        { TEXT("properties.json"),          8 },
        { TEXT("map_references.json"),      2 },
        { TEXT("texture.json"),             4 },
        { TEXT("texture.txt"),              2 },
        // 5: a node holding an editfixedsize array parallel to ChildNodes (Random Weights,
        //    Mixer/Concatenator InputVolume, GroupControl GroupSizes) now carries childValues,
        //    which pairs those entries with the child count. A short or zero-filled array is
        //    invisible in the CDO diff and is what makes a Random node deterministic.
        { TEXT("sound_cue.json"),           5 },
        { TEXT("sound_wave.json"),          2 },
        // 3: rootGraph.isPreset now reads the 5.8 document Template instead of the cleared
        //    (deprecated) PresetOptions.bIsPreset, so cached dumps of presets carry a wrong value.
        { TEXT("metasound.json"),           3 },
        { TEXT("data_table.json"),          2 },
        // 2: sections, LOD0 slotUsage, UV channel counts, and light-map coordinate index added.
        { TEXT("static_mesh.json"),         2 },
        { TEXT("static_mesh.txt"),          2 },
        { TEXT("level_sequence.json"),      3 },
        { TEXT("anim_sequence.json"),       3 },
        { TEXT("anim_graph.json"),          4 },
        { TEXT("skeletal_mesh.json"),       2 },
        // 7: UK2Node_Message no longer emits as a plain `call` — it is a
        //    `message Interface::Function(...)` line, so any Blueprint holding an
        //    interface message node dumps different bytes.
        // 8: split struct input pins now emit a standalone `make<Struct>(...)` value
        //    instead of raw hidden-parent child arguments; disconnected default exec
        //    outputs now emit a bare `end` terminator between sibling blocks; Enhanced
        //    Input action roots now emit `entry input_action` instead of unknown output.
        // 9: an entry node the grammar cannot name and that gates no exec body no longer
        //    renders as an anonymous `entry event UnknownEntry() {}` stub, so AnimGraphs go
        //    back to the single empty-graph marker.
        { TEXT("bpir.txt"),                 9 },
        // 3: entry material blocks now carry the material-level properties (blend mode,
        //    shading model, two-sided, domain, translucency lighting mode, ...) as
        //    `property Name: Value` lines above the graph.
        // 4: a Custom HLSL node no longer restates its input pins as a reflected
        //    `Inputs: [(InputName="UV",Input=(...))]` array beside the named pin arguments
        //    that already carry them, so every material holding one dumps different bytes.
        { TEXT("mgir.txt"),                 4 },
        { TEXT("agir.txt"),                 2 },
        // 2: every graph-node-backed line now leads its field list with `nodeId: <guid>`, the
        //    id the nodeId-keyed behavior_tree verbs resolve. First row this aspect has had --
        //    it was served at AssetDumpDefaultAspectVersion, so 1 -> 2 invalidates exactly like
        //    a bump on any listed aspect.
        { TEXT("btir.txt"),                 2 },
        { TEXT("crir.txt"),                 2 },
        { TEXT("nir.txt"),                  3 },
        // 5: preset detection moved off the cleared PresetOptions.bIsPreset, so a preset's MSIR
        //    now emits its `preset` document shape instead of a full patch/source body.
        { TEXT("msir.txt"),                 5 },
        { TEXT("skeleton.json"),            2 },
        { TEXT("physics_asset.json"),       2 },
        { TEXT("material_instance.json"),   2 },
        { TEXT("cascade.json"),             2 },
        // 2: the widget Designer preview is now stamped opaque before encoding
        //    (WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng), so every pixel's alpha is
        //    0xFF where it used to carry the render target's transparent clear. The PNG bytes
        //    differ for every widget with an uncovered region, which is nearly all of them.
        //    First row this aspect has ever had -- it was served at
        //    AssetDumpDefaultAspectVersion, and MakeCurrentAspectVersions materialises that
        //    default into a record explicitly, so 1 -> 2 compares exactly like a bump on any
        //    listed aspect.
        //
        //    INERT TODAY, deliberately kept. A widget-screenshot dump is not cached at either
        //    end: AssetDumpHandler's WriteDumpCacheForSuccessfulBaseline refuses to write a
        //    record when bIncludeWidgetScreenshot is set, and IsCacheFresh below short-circuits
        //    on the same flag before comparing anything -- so no .dumpcache.json has ever held
        //    a preview.png entry and none can go stale. The row is here because the bump rule
        //    is written against changed bytes rather than against reachability, because the
        //    number is the provenance marker recorded for anything that ever does cache this
        //    aspect, and because lifting that bypass without it would hand callers a stale
        //    transparent PNG. PinWright.AssetDumpCache.WidgetScreenshotDumpsBypassTheCacheEntirely
        //    is the tripwire that fires if the bypass goes.
        //
        // 3: the preview is no longer sRGB-encoded twice. The Slate shader now draws in linear
        //    space and only the render target's ROP encodes, so EVERY colour byte in EVERY
        //    widget preview.png changes (B-screenshot-designer-double-srgb). Still inert for
        //    the reason above, and still recorded for the same three reasons.
        { TEXT("preview.png"),              3 },
    };
    if (const int32* Found = Versions.Find(RelativeFile))
    {
        return *Found;
    }
    return AssetDumpDefaultAspectVersion;
}

TMap<FString, int32> MakeCurrentAspectVersions(TConstArrayView<FString> RelativeWrittenFiles)
{
    TMap<FString, int32> Result;
    for (const FString& RelativeFile : RelativeWrittenFiles)
    {
        const FString Normalized = NormalizeRelativeWrittenFile(RelativeFile);
        if (IsSafeRelativeDumpFile(Normalized))
        {
            Result.Add(Normalized, GetAspectVersion(Normalized));
        }
    }
    return Result;
}

FAssetDumpDumperFingerprint MakeCurrentDumperFingerprint(const TMap<FString, int32>& AspectVersions)
{
    FAssetDumpDumperFingerprint Result;
    Result.DumpCoreVersion = AssetDumpCoreVersion;
    Result.EngineVersion = FEngineVersion::Current().ToString();
    Result.PluginVersion = GetPluginVersion();
    Result.AspectVersions = AspectVersions;
    return Result;
}

FAssetDumpSourceFingerprint BuildSourceFingerprintFromFacts(const FAssetDumpPackageFacts& Facts)
{
    FAssetDumpSourceFingerprint Result;
    Result.PackageExtension = Facts.PackageExtension;
    Result.PackageFilename = Facts.PackageFilename;
    Result.CorrectCasePackageName = Facts.CorrectCasePackageName;
    Result.bPackageIsDirty = Facts.bPackageIsDirty;

    if (Facts.bIsMapOrWorldPackage || IsMapExtensionString(Facts.PackageExtension))
    {
        Result.Kind = EAssetDumpSourceFingerprintKind::Uncached;
        return Result;
    }

    const bool bTextPackage = Facts.bIsTextPackage || IsTextExtensionString(Facts.PackageExtension);
    if (Facts.bHasPackageData
        && !bTextPackage
        && !IsZeroHashString(Facts.PackageSavedHash)
        && Facts.RegistryDiskSize >= 0)
    {
        Result.Kind = EAssetDumpSourceFingerprintKind::PackageSavedHash;
        Result.PackageSavedHash = Facts.PackageSavedHash;
        Result.DiskSize = Facts.RegistryDiskSize;
        return Result;
    }

    if (Facts.bFileStatValid
        && Facts.PackageFileSize >= 0
        && Facts.PackageFileMTimeUtc != FDateTime::MinValue())
    {
        Result.Kind = EAssetDumpSourceFingerprintKind::FileStat;
        Result.PackageFileSize = Facts.PackageFileSize;
        Result.PackageFileMTimeUtc = NormalizeFileMTimeForCache(Facts.PackageFileMTimeUtc);
        return Result;
    }

    Result.Kind = EAssetDumpSourceFingerprintKind::Uncached;
    return Result;
}

FAssetDumpSourceFingerprint BuildSourceFingerprintFromRegistry(
    IAssetRegistry& AssetRegistry,
    const FString& PackageName,
    bool bIsMapOrWorldPackage)
{
    FAssetDumpPackageFacts Facts;
    Facts.PackageName = PackageName;
    Facts.CorrectCasePackageName = PackageName;
    Facts.bIsMapOrWorldPackage = bIsMapOrWorldPackage;
    Facts.bPackageIsDirty = IsPackageDirtyWithoutLoading(PackageName);

    FAssetPackageData PackageData;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    // The 3-argument overload (with OutCorrectCasePackageName) was added in UE 5.5
    FName CorrectCasePackageName;
    if (AssetRegistry.TryGetAssetPackageData(
            FName(*PackageName),
            PackageData,
            CorrectCasePackageName) == UE::AssetRegistry::EExists::Exists)
    {
        Facts.bHasPackageData = true;
        Facts.CorrectCasePackageName = CorrectCasePackageName.IsNone()
            ? PackageName
            : CorrectCasePackageName.ToString();
#else
    // UE 5.4 only has the 2-argument overload; correct-case name unavailable
    if (AssetRegistry.TryGetAssetPackageData(
            FName(*PackageName),
            PackageData) == UE::AssetRegistry::EExists::Exists)
    {
        Facts.bHasPackageData = true;
        Facts.CorrectCasePackageName = PackageName;
#endif
        Facts.RegistryDiskSize = PackageData.DiskSize;
        Facts.PackageExtension = PackageExtensionToCacheString(PackageData.Extension);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        Facts.PackageSavedHash = LexToString(PackageData.GetPackageSavedHash());
#else
        // UE 5.4 replaced the public FMD5Hash CookedHash member with GetPackageSavedHash()
        // (FIoHash). On 5.3 read CookedHash directly.
        Facts.PackageSavedHash = LexToString(PackageData.CookedHash);
#endif
        Facts.bIsTextPackage = FPackageName::IsTextPackageExtension(PackageData.Extension);
        Facts.bIsMapOrWorldPackage = Facts.bIsMapOrWorldPackage
            || PackageData.Extension == EPackageExtension::Map
            || PackageData.Extension == EPackageExtension::TextMap;
    }

    FString DiskCorrectCasePackageName;
    FString DiskExtension;
    if (AssetRegistry.DoesPackageExistOnDisk(FName(*PackageName), &DiskCorrectCasePackageName, &DiskExtension))
    {
        if (!DiskCorrectCasePackageName.IsEmpty())
        {
            Facts.CorrectCasePackageName = DiskCorrectCasePackageName;
        }
        if (!DiskExtension.IsEmpty())
        {
            Facts.PackageExtension = FileExtensionToCacheString(DiskExtension);
        }
        Facts.bIsTextPackage = Facts.bIsTextPackage || IsTextExtensionString(DiskExtension);
        Facts.bIsMapOrWorldPackage = Facts.bIsMapOrWorldPackage || IsMapExtensionString(DiskExtension);
    }

    if (Facts.PackageExtension.IsEmpty())
    {
        Facts.PackageExtension = TEXT("Unspecified");
    }

    if (ResolvePackageFile(AssetRegistry, PackageName, Facts.PackageExtension, Facts.PackageFilename))
    {
        if (Facts.PackageExtension == TEXT("Unspecified"))
        {
            Facts.PackageExtension = FileExtensionToCacheString(
                FPaths::GetExtension(Facts.PackageFilename, /*bIncludeDot=*/true));
        }
        Facts.bIsTextPackage = Facts.bIsTextPackage || IsTextExtensionString(Facts.PackageExtension);
        Facts.bIsMapOrWorldPackage = Facts.bIsMapOrWorldPackage || IsMapExtensionString(Facts.PackageExtension);
        AddFileStatFacts(Facts);
    }

    return BuildSourceFingerprintFromFacts(Facts);
}

bool IsPackageDirtyWithoutLoading(const FString& PackageName)
{
    if (UPackage* Package = FindPackage(nullptr, *PackageName))
    {
        return Package->IsDirty();
    }
    return false;
}

FAssetDumpCacheRecord MakeCacheRecord(
    const FString& AssetPath,
    const FString& PackageName,
    const FString& CorrectCasePackageName,
    const FAssetDumpSourceFingerprint& Source,
    const FAssetDumpDumperFingerprint& Dumper,
    const FAssetDumpOptionsFingerprint& Options,
    TConstArrayView<FString> RelativeWrittenFiles)
{
    FAssetDumpCacheRecord Record;
    Record.CacheVersion = AssetDumpCacheVersion;
    Record.AssetPath = AssetPath;
    Record.PackageName = PackageName;
    Record.CorrectCasePackageName = CorrectCasePackageName.IsEmpty() ? PackageName : CorrectCasePackageName;
    Record.Source = Source;
    Record.Dumper = Dumper;
    Record.Options = Options;

    for (const FString& RelativeFile : RelativeWrittenFiles)
    {
        const FString Normalized = NormalizeRelativeWrittenFile(RelativeFile);
        if (IsSafeRelativeDumpFile(Normalized))
        {
            Record.WrittenFiles.AddUnique(Normalized);
        }
    }
    Record.WrittenFiles.Sort();
    return Record;
}

bool ReadCacheRecord(const FString& DumpDir, FAssetDumpCacheRecord& OutRecord, FString& OutError)
{
    OutRecord = FAssetDumpCacheRecord();
    OutError.Reset();

    FString Body;
    const FString CachePath = GetCachePath(DumpDir);
    if (!FFileHelper::LoadFileToString(Body, *CachePath))
    {
        OutError = FString::Printf(TEXT("failed to read %s"), *CachePath);
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = TEXT("invalid cache json");
        return false;
    }

    if (!TryGetNumberField(Root, FieldCacheVersion, OutRecord.CacheVersion)
        || !TryGetStringField(Root, FieldAssetPath, OutRecord.AssetPath)
        || !TryGetStringField(Root, FieldPackageName, OutRecord.PackageName)
        || !TryGetStringField(Root, FieldCorrectCasePackageName, OutRecord.CorrectCasePackageName))
    {
        OutError = TEXT("missing cache identity fields");
        return false;
    }

    if (!TryReadSource(Root, OutRecord.Source, OutError)
        || !TryReadDumper(Root, OutRecord.Dumper, OutError)
        || !TryReadOptions(Root, OutRecord.Options, OutError)
        || !TryReadWrittenFiles(Root, OutRecord.WrittenFiles, OutError))
    {
        return false;
    }

    // Optional skip-stub fields: absent on records written for successful dumps
    // (and by older plugin versions); OutRecord defaults keep them false/empty.
    TryGetBoolField(Root, FieldSkipped, OutRecord.bSkipped);
    Root->TryGetStringField(FieldSkipReason, OutRecord.SkipReason);

    return true;
}

bool WriteCacheRecord(const FString& DumpDir, const FAssetDumpCacheRecord& Record, FString& OutError)
{
    OutError.Reset();

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(FieldCacheVersion, Record.CacheVersion);
    Root->SetStringField(FieldAssetPath, Record.AssetPath);
    Root->SetStringField(FieldPackageName, Record.PackageName);
    Root->SetStringField(FieldCorrectCasePackageName, Record.CorrectCasePackageName);
    Root->SetObjectField(FieldSource, BuildSourceJson(Record.Source));
    Root->SetObjectField(FieldDumper, BuildDumperJson(Record.Dumper));
    Root->SetObjectField(FieldOptions, BuildOptionsJson(Record.Options));
    Root->SetArrayField(FieldWrittenFiles, BuildWrittenFilesJson(Record.WrittenFiles));

    // Skip-stub marker fields are written only when set so records for successful
    // dumps stay byte-identical to the pre-skip-stub format.
    if (Record.bSkipped)
    {
        Root->SetBoolField(FieldSkipped, true);
        if (!Record.SkipReason.IsEmpty())
        {
            Root->SetStringField(FieldSkipReason, Record.SkipReason);
        }
    }

    const FString Body = SortedJsonWriter::SerializeSortedJsonObject(Root);

    IFileManager& FileManager = IFileManager::Get();
    const FString CachePath = GetCachePath(DumpDir);
    const FString TempPath = CachePath + TEXT(".tmp");
    FileManager.MakeDirectory(*FPaths::GetPath(CachePath), /*Tree=*/true);

    if (!FFileHelper::SaveStringToFile(
            Body,
            *TempPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        FileManager.Delete(*TempPath, /*RequireExists=*/false);
        OutError = FString::Printf(TEXT("failed to write cache temp file %s"), *TempPath);
        return false;
    }

    if (!FileManager.Move(*CachePath, *TempPath, /*bReplace=*/true, /*bEvenReadOnly=*/false))
    {
        FileManager.Delete(*TempPath, /*RequireExists=*/false);
        OutError = FString::Printf(TEXT("failed to replace cache file %s"), *CachePath);
        return false;
    }

    return true;
}

FAssetDumpFreshnessResult IsCacheFresh(
    const FString& DumpDir,
    const FString& PackageName,
    const FAssetDumpSourceFingerprint& CurrentSource,
    const FAssetDumpDumperFingerprint& ExpectedDumper,
    const FAssetDumpOptionsFingerprint& ExpectedOptions,
    const FAssetDumpCacheRecord& Record)
{
    if (ExpectedOptions.bIncludeWidgetScreenshot)
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("includeWidgetScreenshot bypasses cache"));
    }
    if (Record.CacheVersion != AssetDumpCacheVersion)
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("cache version mismatch"));
    }
    if (Record.PackageName != PackageName)
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("package mismatch"));
    }
    if (!CurrentSource.CorrectCasePackageName.IsEmpty()
        && Record.CorrectCasePackageName != CurrentSource.CorrectCasePackageName)
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("correct-case package mismatch"));
    }
    if (CurrentSource.bPackageIsDirty)
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("package is dirty"));
    }
    if (CurrentSource.Kind == EAssetDumpSourceFingerprintKind::Uncached)
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("source is uncached"));
    }
    if (!AreSourceFingerprintsEqual(Record.Source, CurrentSource))
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("source fingerprint mismatch"));
    }
    if (!AreDumperFingerprintsEqual(Record.Dumper, ExpectedDumper))
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("dumper fingerprint mismatch"));
    }
    if (Record.Options.bIncludeWidgetScreenshot != ExpectedOptions.bIncludeWidgetScreenshot)
    {
        return FAssetDumpFreshnessResult::Stale(TEXT("options mismatch"));
    }

    FString Reason;
    if (!HasAllWrittenFiles(DumpDir, Record.WrittenFiles, Reason))
    {
        return FAssetDumpFreshnessResult::Stale(Reason);
    }
    if (!HasOnlyListedDumpFiles(DumpDir, Record.WrittenFiles, Reason))
    {
        return FAssetDumpFreshnessResult::Stale(Reason);
    }

    return FAssetDumpFreshnessResult::Fresh();
}

FAssetDumpFreshnessResult IsCacheFresh(
    const FString& DumpDir,
    const FString& PackageName,
    const FAssetDumpSourceFingerprint& CurrentSource,
    const FAssetDumpDumperFingerprint& ExpectedDumper,
    const FAssetDumpOptionsFingerprint& ExpectedOptions)
{
    FAssetDumpCacheRecord Record;
    FString Error;
    if (!ReadCacheRecord(DumpDir, Record, Error))
    {
        return FAssetDumpFreshnessResult::Stale(Error);
    }
    return IsCacheFresh(DumpDir, PackageName, CurrentSource, ExpectedDumper, ExpectedOptions, Record);
}

bool IsDumpFresh(
    IAssetRegistry& AssetRegistry,
    const FString& PackageName,
    const FString& OutRoot,
    bool bIncludeWidgetScreenshot,
    bool bIsMapOrWorldPackage,
    FString& OutDumpDir)
{
    OutDumpDir = AssetDumpWriter::ResolveDumpDir(PackageName, OutRoot);

    FAssetDumpCacheRecord Record;
    FString ReadError;
    if (!ReadCacheRecord(OutDumpDir, Record, ReadError))
    {
        UE_LOG(LogPinWrightSubsystem, Log,
            TEXT("asset.dump cache: stale dump for '%s' (dumpDir='%s'): %s"),
            *PackageName, *OutDumpDir, *ReadError);
        return false;
    }

    const FAssetDumpSourceFingerprint CurrentSource =
        BuildSourceFingerprintFromRegistry(
            AssetRegistry,
            PackageName,
            bIsMapOrWorldPackage);
    const TMap<FString, int32> AspectVersions =
        MakeCurrentAspectVersions(Record.WrittenFiles);
    const FAssetDumpDumperFingerprint ExpectedDumper =
        MakeCurrentDumperFingerprint(AspectVersions);

    FAssetDumpOptionsFingerprint ExpectedOptions;
    ExpectedOptions.bIncludeWidgetScreenshot = bIncludeWidgetScreenshot;

    const FAssetDumpFreshnessResult Freshness = IsCacheFresh(
        OutDumpDir,
        PackageName,
        CurrentSource,
        ExpectedDumper,
        ExpectedOptions,
        Record);
    if (!Freshness.bFresh)
    {
        // Emitted only for stale entries so log volume tracks the queued count.
        UE_LOG(LogPinWrightSubsystem, Log,
            TEXT("asset.dump cache: stale dump for '%s' (dumpDir='%s'): %s"),
            *PackageName, *OutDumpDir, *Freshness.Reason);
    }
    return Freshness.bFresh;
}

}
