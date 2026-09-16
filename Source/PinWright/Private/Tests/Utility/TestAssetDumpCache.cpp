// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/AssetDumpCache.h"
#include "Handlers/Asset/AssetDumpHandler.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    FString MakeCacheTestDir(const FString& Name)
    {
        FString Dir = FPaths::ProjectIntermediateDir()
            / TEXT("PinWrightTests")
            / TEXT("AssetDumpCache")
            / Name;
        FPaths::NormalizeDirectoryName(Dir);
        IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true);
        IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
        return Dir;
    }

    bool WriteTestFile(const FString& Path, const FString& Content = TEXT("{}"))
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
        return FFileHelper::SaveStringToFile(
            Content,
            *Path,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    AssetDumpCache::FAssetDumpSourceFingerprint MakeSavedHashSource(
        const FString& Hash = TEXT("1111111111111111111111111111111111111111"))
    {
        AssetDumpCache::FAssetDumpPackageFacts Facts;
        Facts.PackageName = TEXT("/Game/Test/Foo");
        Facts.CorrectCasePackageName = TEXT("/Game/Test/Foo");
        Facts.PackageExtension = TEXT("Asset");
        Facts.PackageSavedHash = Hash;
        Facts.RegistryDiskSize = 42;
        Facts.bHasPackageData = true;
        return AssetDumpCache::BuildSourceFingerprintFromFacts(Facts);
    }

    AssetDumpCache::FAssetDumpCacheRecord MakeRecord(
        const AssetDumpCache::FAssetDumpSourceFingerprint& Source,
        const TArray<FString>& WrittenFiles,
        AssetDumpCache::FAssetDumpOptionsFingerprint Options = AssetDumpCache::FAssetDumpOptionsFingerprint())
    {
        const TMap<FString, int32> AspectVersions = AssetDumpCache::MakeCurrentAspectVersions(WrittenFiles);
        const AssetDumpCache::FAssetDumpDumperFingerprint Dumper =
            AssetDumpCache::MakeCurrentDumperFingerprint(AspectVersions);
        return AssetDumpCache::MakeCacheRecord(
            TEXT("/Game/Test/Foo"),
            TEXT("/Game/Test/Foo"),
            TEXT("/Game/Test/Foo"),
            Source,
            Dumper,
            Options,
            WrittenFiles);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheSerializationTest,
    "PinWright.AssetDumpCache.SerializationRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheSerializationTest::RunTest(const FString& Parameters)
{
    const FString Dir = MakeCacheTestDir(TEXT("SerializationRoundTrip"));
    const TArray<FString> WrittenFiles{TEXT("meta.json"), TEXT("properties.json")};
    for (const FString& RelativeFile : WrittenFiles)
    {
        TestTrue(FString::Printf(TEXT("write %s"), *RelativeFile), WriteTestFile(Dir / RelativeFile));
    }

    const AssetDumpCache::FAssetDumpSourceFingerprint Source = MakeSavedHashSource();
    const AssetDumpCache::FAssetDumpCacheRecord Record = MakeRecord(Source, WrittenFiles);

    FString Error;
    TestTrue(TEXT("cache record written"), AssetDumpCache::WriteCacheRecord(Dir, Record, Error));
    TestTrue(TEXT("write error empty"), Error.IsEmpty());

    AssetDumpCache::FAssetDumpCacheRecord ReadRecord;
    TestTrue(TEXT("cache record read"), AssetDumpCache::ReadCacheRecord(Dir, ReadRecord, Error));
    TestEqual(TEXT("cache version"), ReadRecord.CacheVersion, AssetDumpCache::AssetDumpCacheVersion);
    TestEqual(TEXT("packageName"), ReadRecord.PackageName, FString(TEXT("/Game/Test/Foo")));
    TestEqual(TEXT("source kind"),
        static_cast<uint8>(ReadRecord.Source.Kind),
        static_cast<uint8>(AssetDumpCache::EAssetDumpSourceFingerprintKind::PackageSavedHash));
    TestEqual(TEXT("source hash"), ReadRecord.Source.PackageSavedHash, Source.PackageSavedHash);
    TestEqual(TEXT("written file count"), ReadRecord.WrittenFiles.Num(), 2);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheFreshMatchTest,
    "PinWright.AssetDumpCache.FreshMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheFreshMatchTest::RunTest(const FString& Parameters)
{
    const FString Dir = MakeCacheTestDir(TEXT("FreshMatch"));
    const TArray<FString> WrittenFiles{TEXT("meta.json"), TEXT("properties.json")};
    for (const FString& RelativeFile : WrittenFiles)
    {
        TestTrue(FString::Printf(TEXT("write %s"), *RelativeFile), WriteTestFile(Dir / RelativeFile));
    }

    const AssetDumpCache::FAssetDumpSourceFingerprint Source = MakeSavedHashSource();
    const AssetDumpCache::FAssetDumpCacheRecord Record = MakeRecord(Source, WrittenFiles);
    const AssetDumpCache::FAssetDumpFreshnessResult Freshness = AssetDumpCache::IsCacheFresh(
        Dir,
        TEXT("/Game/Test/Foo"),
        Source,
        Record.Dumper,
        Record.Options,
        Record);

    TestTrue(TEXT("cache is fresh"), Freshness.bFresh);
    TestTrue(TEXT("fresh reason empty"), Freshness.Reason.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheSourceMismatchTest,
    "PinWright.AssetDumpCache.SourceMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheSourceMismatchTest::RunTest(const FString& Parameters)
{
    const FString Dir = MakeCacheTestDir(TEXT("SourceMismatch"));
    const TArray<FString> WrittenFiles{TEXT("meta.json")};
    TestTrue(TEXT("write meta"), WriteTestFile(Dir / TEXT("meta.json")));

    const AssetDumpCache::FAssetDumpSourceFingerprint CachedSource =
        MakeSavedHashSource(TEXT("1111111111111111111111111111111111111111"));
    const AssetDumpCache::FAssetDumpCacheRecord Record = MakeRecord(CachedSource, WrittenFiles);
    AssetDumpCache::FAssetDumpSourceFingerprint CurrentSource =
        MakeSavedHashSource(TEXT("2222222222222222222222222222222222222222"));

    const AssetDumpCache::FAssetDumpFreshnessResult Freshness = AssetDumpCache::IsCacheFresh(
        Dir,
        TEXT("/Game/Test/Foo"),
        CurrentSource,
        Record.Dumper,
        Record.Options,
        Record);

    TestFalse(TEXT("cache is stale"), Freshness.bFresh);
    TestTrue(TEXT("reason mentions source"), Freshness.Reason.Contains(TEXT("source")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheVersionMismatchTest,
    "PinWright.AssetDumpCache.VersionMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheVersionMismatchTest::RunTest(const FString& Parameters)
{
    const FString Dir = MakeCacheTestDir(TEXT("VersionMismatch"));
    const TArray<FString> WrittenFiles{TEXT("meta.json"), TEXT("properties.json")};
    for (const FString& RelativeFile : WrittenFiles)
    {
        TestTrue(FString::Printf(TEXT("write %s"), *RelativeFile), WriteTestFile(Dir / RelativeFile));
    }

    const AssetDumpCache::FAssetDumpSourceFingerprint Source = MakeSavedHashSource();
    AssetDumpCache::FAssetDumpCacheRecord Record = MakeRecord(Source, WrittenFiles);

    AssetDumpCache::FAssetDumpCacheRecord CacheVersionMismatch = Record;
    CacheVersionMismatch.CacheVersion = AssetDumpCache::AssetDumpCacheVersion + 1;
    TestFalse(TEXT("cacheVersion mismatch stale"), AssetDumpCache::IsCacheFresh(
        Dir, TEXT("/Game/Test/Foo"), Source, Record.Dumper, Record.Options, CacheVersionMismatch).bFresh);

    AssetDumpCache::FAssetDumpDumperFingerprint ExpectedDumper = Record.Dumper;
    ExpectedDumper.DumpCoreVersion = AssetDumpCache::AssetDumpCoreVersion + 1;
    TestFalse(TEXT("dumpCoreVersion mismatch stale"), AssetDumpCache::IsCacheFresh(
        Dir, TEXT("/Game/Test/Foo"), Source, ExpectedDumper, Record.Options, Record).bFresh);

    AssetDumpCache::FAssetDumpDumperFingerprint AspectMismatch = Record.Dumper;
    // Bump relative to the recorded version so the mismatch survives future GetAspectVersion bumps.
    AspectMismatch.AspectVersions.Add(
        TEXT("properties.json"),
        AspectMismatch.AspectVersions.FindRef(TEXT("properties.json")) + 1);
    TestFalse(TEXT("aspectVersions mismatch stale"), AssetDumpCache::IsCacheFresh(
        Dir, TEXT("/Game/Test/Foo"), Source, AspectMismatch, Record.Options, Record).bFresh);

    // The .uplugin marketing version is recorded but never compared: a release bump
    // that changes no sidecar must not invalidate the mirror and force a full reload.
    // Board: B-dumpcache-fingerprint-includes-marketing-version.
    AssetDumpCache::FAssetDumpDumperFingerprint PluginVersionMismatch = Record.Dumper;
    PluginVersionMismatch.PluginVersion = Record.Dumper.PluginVersion + TEXT("-bumped");
    TestTrue(TEXT("pluginVersion mismatch stays fresh"), AssetDumpCache::IsCacheFresh(
        Dir, TEXT("/Game/Test/Foo"), Source, PluginVersionMismatch, Record.Options, Record).bFresh);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheAnimSequenceAspectVersionTest,
    "PinWright.AssetDumpCache.AnimSequenceAspectVersion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheAnimSequenceAspectVersionTest::RunTest(const FString& Parameters)
{
    // Bumped to 3 alongside boneTracks[] being added to anim_sequence.json (per-bone-track readback,
    // ticket E-rpc-animation-bone-track-readback) — the serialized shape changed, so the aspect
    // version had to bump to invalidate stale caches.
    TestEqual(TEXT("anim_sequence.json explicit aspect version"),
        AssetDumpCache::GetAspectVersion(DumpFileNames::AnimSequence),
        static_cast<int32>(3));

    const TArray<FString> WrittenFiles{DumpFileNames::AnimSequence};
    const TMap<FString, int32> AspectVersions = AssetDumpCache::MakeCurrentAspectVersions(WrittenFiles);
    TestEqual(TEXT("anim_sequence.json current aspect version"),
        AspectVersions.FindRef(DumpFileNames::AnimSequence),
        static_cast<int32>(3));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheBpirAspectVersionTest,
    "PinWright.AssetDumpCache.BpirAspectVersion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheBpirAspectVersionTest::RunTest(const FString& Parameters)
{
    // Bumped to 4 alongside the 2026-06-14 generic-emit change (commit 05c1f3bc): pure UK2Nodes with
    // no specific handler now route through EmitGenericNode -> recompilable "call K2Node_<Class>(...)",
    // replacing the old non-recompilable title-derived tokens ("Resolve_Soft_Reference", "Equal").
    // That commit changed bpir.txt's serialized shape but omitted the aspect bump, so pre-Jun-14 caches
    // kept serving the stale, un-recompilable bytes as "fresh" (ticket B-asset-dump-bpir-stale-generic-emit).
    // Bumped to 5 alongside the 2026-07-23 source-graph-name fix: cloned composite-bearing graphs
    // previously emitted the session-dependent "EdGraph_N" clone name in entry signatures
    // (ticket B-bpir-clone-graph-entry-name-leak).
    // Bumped to 6 after extending that fix to orphan-node warnings, which still read the
    // auto-suffixed working-clone name instead of the captured source graph name.
    // Bumped to 7 alongside the interface-message form: UK2Node_Message derives from
    // UK2Node_CallFunction and used to emit as an indistinguishable plain "call Foo(...)";
    // it now emits "message Interface::Foo(Target: ...)" and keeps a Target that resolves to
    // `self`, so every Blueprint holding an interface message node dumps different bytes
    // (ticket B-bpir-interface-call-never-dispatches).
    // Bumped to 8 for split struct input regrouping, explicit terminal-chain serialization, and
    // Enhanced Input roots: hidden parent pins now emit a standalone make<Struct>(...) value,
    // disconnected default exec outputs emit `end`, and bound Enhanced Input nodes emit
    // `entry input_action` instead of unknown output (tickets B-bpir-split-input-pin-args-unresolvable,
    // B-bpir-decompile-adjacent-terminal-blocks-read-as-fallthrough, and
    // F-author-enhanced-input-action-node).
    // Bumped to 9 for the anonymous-entry-stub fix: an entry node the grammar cannot name
    // (AnimGraph roots reach the decompiler through the engine compile-root backstop) and
    // that gates no exec body no longer renders as `entry event UnknownEntry() {}`, so
    // AnimGraph-bearing Blueprints go back to the single empty-graph marker.
    TestEqual(TEXT("bpir.txt explicit aspect version"),
        AssetDumpCache::GetAspectVersion(DumpFileNames::BpirTxt),
        static_cast<int32>(9));

    const TArray<FString> WrittenFiles{DumpFileNames::BpirTxt};
    const TMap<FString, int32> AspectVersions = AssetDumpCache::MakeCurrentAspectVersions(WrittenFiles);
    TestEqual(TEXT("bpir.txt current aspect version"),
        AspectVersions.FindRef(DumpFileNames::BpirTxt),
        static_cast<int32>(9));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheFileValidationTest,
    "PinWright.AssetDumpCache.FileValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheFileValidationTest::RunTest(const FString& Parameters)
{
    const FString MissingDir = MakeCacheTestDir(TEXT("MissingSidecar"));
    const TArray<FString> WrittenFiles{TEXT("meta.json"), TEXT("properties.json")};
    TestTrue(TEXT("write only meta"), WriteTestFile(MissingDir / TEXT("meta.json")));

    const AssetDumpCache::FAssetDumpSourceFingerprint Source = MakeSavedHashSource();
    const AssetDumpCache::FAssetDumpCacheRecord MissingRecord = MakeRecord(Source, WrittenFiles);
    TestFalse(TEXT("missing sidecar stale"), AssetDumpCache::IsCacheFresh(
        MissingDir, TEXT("/Game/Test/Foo"), Source, MissingRecord.Dumper, MissingRecord.Options, MissingRecord).bFresh);

    const FString ExtraDir = MakeCacheTestDir(TEXT("GeneratedExtra"));
    TestTrue(TEXT("write meta"), WriteTestFile(ExtraDir / TEXT("meta.json")));
    TestTrue(TEXT("write properties"), WriteTestFile(ExtraDir / TEXT("properties.json")));
    TestTrue(TEXT("write diff extra"), WriteTestFile(ExtraDir / TEXT("properties_diff.txt")));

    const AssetDumpCache::FAssetDumpCacheRecord ExtraRecord = MakeRecord(Source, WrittenFiles);
    const AssetDumpCache::FAssetDumpFreshnessResult ExtraFreshness = AssetDumpCache::IsCacheFresh(
        ExtraDir,
        TEXT("/Game/Test/Foo"),
        Source,
        ExtraRecord.Dumper,
        ExtraRecord.Options,
        ExtraRecord);

    TestFalse(TEXT("generated extra stale"), ExtraFreshness.bFresh);
    TestTrue(TEXT("reason mentions unlisted file"), ExtraFreshness.Reason.Contains(TEXT("unlisted")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheSourceFallbackTest,
    "PinWright.AssetDumpCache.SourceFallbacks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheSourceFallbackTest::RunTest(const FString& Parameters)
{
    AssetDumpCache::FAssetDumpPackageFacts TextFacts;
    TextFacts.PackageExtension = TEXT("TextAsset");
    TextFacts.PackageSavedHash = TEXT("1111111111111111111111111111111111111111");
    TextFacts.RegistryDiskSize = 100;
    TextFacts.PackageFileSize = 101;
    TextFacts.PackageFileMTimeUtc = FDateTime(2026, 5, 14, 1, 2, 3, 456);
    TextFacts.bHasPackageData = true;
    TextFacts.bFileStatValid = true;
    TextFacts.bIsTextPackage = true;
    const AssetDumpCache::FAssetDumpSourceFingerprint TextSource =
        AssetDumpCache::BuildSourceFingerprintFromFacts(TextFacts);
    TestEqual(TEXT("text package uses file stat"),
        static_cast<uint8>(TextSource.Kind),
        static_cast<uint8>(AssetDumpCache::EAssetDumpSourceFingerprintKind::FileStat));
    TestEqual(TEXT("text package file size"), TextSource.PackageFileSize, static_cast<int64>(101));
    TestEqual(TEXT("text package mtime preserves millisecond precision"),
        TextSource.PackageFileMTimeUtc,
        FDateTime(2026, 5, 14, 1, 2, 3, 456));

    AssetDumpCache::FAssetDumpPackageFacts ZeroHashFacts = TextFacts;
    ZeroHashFacts.PackageExtension = TEXT("Asset");
    ZeroHashFacts.PackageSavedHash = TEXT("0000000000000000000000000000000000000000");
    ZeroHashFacts.bIsTextPackage = false;
    ZeroHashFacts.PackageFileMTimeUtc = FDateTime(FDateTime(2026, 5, 14, 1, 2, 3, 456).GetTicks() + 1);
    const AssetDumpCache::FAssetDumpSourceFingerprint ZeroHashSource =
        AssetDumpCache::BuildSourceFingerprintFromFacts(ZeroHashFacts);
    TestEqual(TEXT("zero hash uses file stat"),
        static_cast<uint8>(ZeroHashSource.Kind),
        static_cast<uint8>(AssetDumpCache::EAssetDumpSourceFingerprintKind::FileStat));
    TestEqual(TEXT("file stat mtime is cache-serialized precision"),
        ZeroHashSource.PackageFileMTimeUtc,
        FDateTime(2026, 5, 14, 1, 2, 3, 456));

    AssetDumpCache::FAssetDumpPackageFacts MapFacts = TextFacts;
    MapFacts.PackageExtension = TEXT("Map");
    MapFacts.bIsMapOrWorldPackage = true;
    const AssetDumpCache::FAssetDumpSourceFingerprint MapSource =
        AssetDumpCache::BuildSourceFingerprintFromFacts(MapFacts);
    TestEqual(TEXT("map package is uncached"),
        static_cast<uint8>(MapSource.Kind),
        static_cast<uint8>(AssetDumpCache::EAssetDumpSourceFingerprintKind::Uncached));

    AssetDumpCache::FAssetDumpPackageFacts MissingFacts;
    MissingFacts.PackageExtension = TEXT("Asset");
    MissingFacts.bHasPackageData = false;
    MissingFacts.bFileStatValid = false;
    const AssetDumpCache::FAssetDumpSourceFingerprint MissingSource =
        AssetDumpCache::BuildSourceFingerprintFromFacts(MissingFacts);
    TestEqual(TEXT("missing package data and stat is uncached"),
        static_cast<uint8>(MissingSource.Kind),
        static_cast<uint8>(AssetDumpCache::EAssetDumpSourceFingerprintKind::Uncached));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheIneligibleOptionsTest,
    "PinWright.AssetDumpCache.IneligibleOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheIneligibleOptionsTest::RunTest(const FString& Parameters)
{
    const FString Dir = MakeCacheTestDir(TEXT("IneligibleOptions"));
    const TArray<FString> WrittenFiles{TEXT("meta.json")};
    TestTrue(TEXT("write meta"), WriteTestFile(Dir / TEXT("meta.json")));

    const AssetDumpCache::FAssetDumpSourceFingerprint Source = MakeSavedHashSource();
    const AssetDumpCache::FAssetDumpCacheRecord Record = MakeRecord(Source, WrittenFiles);

    AssetDumpCache::FAssetDumpOptionsFingerprint ScreenshotOptions;
    ScreenshotOptions.bIncludeWidgetScreenshot = true;
    TestFalse(TEXT("screenshot option bypasses cache"), AssetDumpCache::IsCacheFresh(
        Dir, TEXT("/Game/Test/Foo"), Source, Record.Dumper, ScreenshotOptions, Record).bFresh);

    AssetDumpCache::FAssetDumpCacheRecord OptionMismatch = Record;
    OptionMismatch.Options.bIncludeWidgetScreenshot = true;
    TestFalse(TEXT("cached option mismatch stale"), AssetDumpCache::IsCacheFresh(
        Dir, TEXT("/Game/Test/Foo"), Source, Record.Dumper, Record.Options, OptionMismatch).bFresh);

    AssetDumpCache::FAssetDumpSourceFingerprint DirtySource = Source;
    DirtySource.bPackageIsDirty = true;
    TestFalse(TEXT("dirty package stale"), AssetDumpCache::IsCacheFresh(
        Dir, TEXT("/Game/Test/Foo"), DirtySource, Record.Dumper, Record.Options, Record).bFresh);

    return true;
}
