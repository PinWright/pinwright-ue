// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/AssetDumpCache.h"
#include "Utils/SortedJsonWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    FString MakeSortedJsonTestDir(const FString& Name)
    {
        FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
            / TEXT("PinWrightTests")
            / TEXT("AssetDumpCacheSortedJson")
            / Name;
        FPaths::NormalizeDirectoryName(Dir);
        IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true);
        IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
        return Dir;
    }

    AssetDumpCache::FAssetDumpCacheRecord MakeDeterminismRecord()
    {
        AssetDumpCache::FAssetDumpPackageFacts Facts;
        Facts.PackageName = TEXT("/Game/Test/Foo");
        Facts.CorrectCasePackageName = TEXT("/Game/Test/Foo");
        Facts.PackageExtension = TEXT("Asset");
        Facts.PackageSavedHash = TEXT("1111111111111111111111111111111111111111");
        Facts.RegistryDiskSize = 42;
        Facts.bHasPackageData = true;

        const TArray<FString> WrittenFiles{TEXT("meta.json"), TEXT("properties.json")};
        const TMap<FString, int32> AspectVersions = AssetDumpCache::MakeCurrentAspectVersions(WrittenFiles);
        return AssetDumpCache::MakeCacheRecord(
            TEXT("/Game/Test/Foo"),
            TEXT("/Game/Test/Foo"),
            TEXT("/Game/Test/Foo"),
            AssetDumpCache::BuildSourceFingerprintFromFacts(Facts),
            AssetDumpCache::MakeCurrentDumperFingerprint(AspectVersions),
            AssetDumpCache::FAssetDumpOptionsFingerprint(),
            WrittenFiles);
    }
}

// WriteCacheRecord must route through SortedJsonWriter, which emits object keys in
// lexical order. Raw FJsonSerializer::Serialize preserves the hash-ordered
// FJsonObject::Values TMap instead, so the top-level keys would appear in insertion/
// hash order rather than sorted order — this test would fail if the fix were reverted.
// (Sorted-line comparison cannot catch that: TMap hash order is stable within a single
// run, so two same-run serializations are byte-identical regardless of the fix. The
// non-determinism is inter-run; the observable property the fix guarantees within any
// run is sorted key order, which is what this asserts against production output.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpCacheSortedJsonDeterminismTest,
    "PinWright.AssetDumpCache.SortedJsonDeterminism",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpCacheSortedJsonDeterminismTest::RunTest(const FString& Parameters)
{
    const FString Dir = MakeSortedJsonTestDir(TEXT("Record"));
    const AssetDumpCache::FAssetDumpCacheRecord Record = MakeDeterminismRecord();

    FString Error;
    TestTrue(TEXT("record written"), AssetDumpCache::WriteCacheRecord(Dir, Record, Error));
    TestTrue(TEXT("write error empty"), Error.IsEmpty());

    FString Json;
    TestTrue(TEXT("read cache file"),
        FFileHelper::LoadFileToString(Json, *AssetDumpCache::GetCachePath(Dir)));

    // Top-level keys WriteCacheRecord emits, in their (unsorted) insertion order. The
    // sorted writer must reorder these to lexical order in the file; raw serialize would
    // leave them in this order.
    const TArray<FString> TopLevelKeys{
        TEXT("cacheVersion"),
        TEXT("assetPath"),
        TEXT("packageName"),
        TEXT("correctCasePackageName"),
        TEXT("source"),
        TEXT("dumper"),
        TEXT("options"),
        TEXT("writtenFiles")};

    // Capture the byte position of each top-level key's quoted identifier. A top-level
    // identifier sits at the start of its line indented by two spaces under pretty policy;
    // matching on "\n\t<key>" (TPrettyJsonPrintPolicy uses a tab) anchors to the top level
    // and avoids colliding with the same name nested inside source/dumper objects.
    TArray<int32> Positions;
    for (const FString& Key : TopLevelKeys)
    {
        const FString Needle = FString::Printf(TEXT("\n\t\"%s\""), *Key);
        const int32 Pos = Json.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        TestTrue(FString::Printf(TEXT("top-level key present: %s"), *Key), Pos != INDEX_NONE);
        Positions.Add(Pos);
    }

    // The positions, when reordered by sorted key name, must be strictly increasing —
    // i.e. the file lists the keys in lexical order. This is the SortedJsonWriter contract.
    TArray<FString> SortedKeys = TopLevelKeys;
    SortedKeys.Sort();

    int32 PreviousPos = INDEX_NONE;
    for (const FString& Key : SortedKeys)
    {
        const int32 Index = TopLevelKeys.IndexOfByKey(Key);
        const int32 Pos = Positions[Index];
        TestTrue(
            FString::Printf(TEXT("key '%s' appears after the previous sorted key"), *Key),
            Pos > PreviousPos);
        PreviousPos = Pos;
    }

    return true;
}

// asset.set_metadata serializes nested object/array metadata values through
// SortedJsonWriter::SerializeSortedJsonValue, and the resulting blob persists into the
// .uasset bytes. The default case must emit object keys in lexical order so those bytes
// are deterministic; the prior raw FJsonSerializer::Serialize left them in hash-ordered
// FJsonObject::Values order. This asserts the SerializeSortedJsonValue contract on an
// object value whose keys are inserted in reverse-lexical order — it fails if the
// AssetMetadataHandler default case is reverted to raw serialize.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetMetadataSortedJsonValueTest,
    "PinWright.AssetMetadata.SortedJsonValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetMetadataSortedJsonValueTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
    Inner->SetStringField(TEXT("zulu"), TEXT("z"));
    Inner->SetStringField(TEXT("mike"), TEXT("m"));
    Inner->SetStringField(TEXT("alpha"), TEXT("a"));

    const TSharedPtr<FJsonValue> ObjectValue = MakeShared<FJsonValueObject>(Inner);
    const FString Serialized = SortedJsonWriter::SerializeSortedJsonValue(ObjectValue);

    const TArray<FString> SortedKeyOrder{TEXT("alpha"), TEXT("mike"), TEXT("zulu")};

    int32 PreviousPos = INDEX_NONE;
    for (const FString& Key : SortedKeyOrder)
    {
        const FString Needle = FString::Printf(TEXT("\"%s\""), *Key);
        const int32 Pos = Serialized.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        TestTrue(FString::Printf(TEXT("key present: %s"), *Key), Pos != INDEX_NONE);
        TestTrue(
            FString::Printf(TEXT("key '%s' appears after the previous sorted key"), *Key),
            Pos > PreviousPos);
        PreviousPos = Pos;
    }

    return true;
}
