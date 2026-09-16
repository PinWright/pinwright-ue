// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

#include "AssetDumpFixtureHelpers.h"
#include "TestAssetDumpObjectRefsFixture.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/SoftObjectPath.h"

namespace
{
    using AssetDumpFixtureHelpers::MakeFixture;

    // Returns the references array from BuildMapReferencesJson, or empty if the result is null.
    TArray<TSharedPtr<FJsonValue>> GetReferences(UTestAssetDumpObjectRefsFixture* Fix)
    {
        TSharedPtr<FJsonObject> Obj = BuildMapReferencesJson(Fix);
        if (!Obj.IsValid())
        {
            return {};
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Obj->TryGetArrayField(TEXT("references"), Arr) || !Arr)
        {
            return {};
        }
        return *Arr;
    }

    bool FindEntryByProperty(
        const TArray<TSharedPtr<FJsonValue>>& Entries,
        const FString& PropertyName,
        TSharedPtr<FJsonObject>& OutEntry)
    {
        for (const TSharedPtr<FJsonValue>& Val : Entries)
        {
            const TSharedPtr<FJsonObject>& Entry = Val->AsObject();
            if (Entry.IsValid() && Entry->GetStringField(TEXT("property")) == PropertyName)
            {
                OutEntry = Entry;
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// AssetDumpMapReferences.TypedSoftWorldEmitted
// TSoftObjectPtr<UWorld> populated -> single references entry with soft-uworld source.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpMapReferences_TypedSoftWorldEmittedTest,
    "PinWright.utils.asset_dump_map_references.TypedSoftWorldEmitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpMapReferences_TypedSoftWorldEmittedTest::RunTest(const FString& Parameters)
{
    // Removing the IsChildOf(UWorld::StaticClass()) filter would NOT cause this test to fail —
    // it asserts the typed-soft-UWorld pass emits the expected entry. The negative gate is
    // covered by FAssetDumpMapReferences_NonWorldSoftRefIgnoredTest below.
    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;

    const FString MapPath = TEXT("/Engine/Maps/Templates/Template_Default.Template_Default");
    Fix->MapRef = FSoftObjectPath(MapPath);

    TArray<TSharedPtr<FJsonValue>> Entries = GetReferences(Fix);
    TestEqual(TEXT("Exactly one reference emitted"), Entries.Num(), 1);
    if (Entries.Num() != 1) return false;

    TSharedPtr<FJsonObject> Entry = Entries[0]->AsObject();
    TestNotNull(TEXT("Entry is a JSON object"), Entry.Get());
    if (!Entry) return false;

    TestEqual(TEXT("property is MapRef"), Entry->GetStringField(TEXT("property")), FString(TEXT("MapRef")));
    TestEqual(TEXT("path matches FSoftObjectPath"), Entry->GetStringField(TEXT("path")), MapPath);
    TestEqual(TEXT("source is soft-uworld"), Entry->GetStringField(TEXT("source")), FString(TEXT("soft-uworld")));
    return true;
}

// ============================================================================
// asset.map_references.TypedSoftWorldEmitted
// Live RPC emits the same typed soft-world reference shape as the dump utility.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetMapReferencesHandler_TypedSoftWorldEmittedTest,
    "PinWright.asset.map_references.TypedSoftWorldEmitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetMapReferencesHandler_TypedSoftWorldEmittedTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(
        TEXT("AssetMapReferencesHandler_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/%s"), *AssetName);
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so it
    // cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return false;
    }
    Package->SetFlags(RF_Transient);

    UTestAssetDumpObjectRefsFixture* Fixture = NewObject<UTestAssetDumpObjectRefsFixture>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("Fixture created"), Fixture))
    {
        return false;
    }

    const FString MapPath = TEXT("/Engine/Maps/Templates/Template_Default.Template_Default");
    Fixture->MapRef = FSoftObjectPath(MapPath);

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("asset.map_references"), Payload, Capture);
    TestTrue(TEXT("asset.map_references handler found"), bFound);
    TestTrue(TEXT("asset.map_references sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("asset.map_references succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("asset.map_references result present"), Capture.Result.Get());
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    double SchemaVersion = 0.0;
    TestTrue(TEXT("schemaVersion present"), Capture.Result->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion));
    TestEqual(TEXT("schemaVersion == 1"), static_cast<int32>(SchemaVersion), 1);

    const TArray<TSharedPtr<FJsonValue>>* References = nullptr;
    TestTrue(TEXT("references present"), Capture.Result->TryGetArrayField(TEXT("references"), References));
    if (!References)
    {
        return false;
    }

    TestEqual(TEXT("Exactly one reference emitted"), References->Num(), 1);
    if (References->Num() != 1)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Entry = (*References)[0]->AsObject();
    TestNotNull(TEXT("Entry is a JSON object"), Entry.Get());
    if (!Entry)
    {
        return false;
    }

    TestEqual(TEXT("property is MapRef"), Entry->GetStringField(TEXT("property")), FString(TEXT("MapRef")));
    TestEqual(TEXT("path matches FSoftObjectPath"), Entry->GetStringField(TEXT("path")), MapPath);
    TestEqual(TEXT("source is soft-uworld"), Entry->GetStringField(TEXT("source")), FString(TEXT("soft-uworld")));
    return true;
}

// ============================================================================
// AssetDumpMapReferences.NullSoftWorldOmitted
// Null TSoftObjectPtr<UWorld> -> no entry for that property.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpMapReferences_NullSoftWorldOmittedTest,
    "PinWright.utils.asset_dump_map_references.NullSoftWorldOmitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpMapReferences_NullSoftWorldOmittedTest::RunTest(const FString& Parameters)
{
    // Removing the null-skip in AppendIfNonNull would cause this test to fail because a null
    // MapRef would still produce an entry, and the function would return a non-null JSON object
    // instead of nullptr (yielding a non-empty references array).
    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;

    // Leave MapRef null.
    TSharedPtr<FJsonObject> Result = BuildMapReferencesJson(Fix);
    // With every soft-world UPROPERTY null, the function returns nullptr per the empty-result contract.
    TestFalse(TEXT("BuildMapReferencesJson returns nullptr when no soft-world refs present"), Result.IsValid());

    // Defense in depth: even if the contract changes to return an empty object, no MapRef entry should appear.
    if (Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Result->TryGetArrayField(TEXT("references"), Arr) && Arr)
        {
            TSharedPtr<FJsonObject> Unused;
            TestFalse(TEXT("No entry for null MapRef"), FindEntryByProperty(*Arr, TEXT("MapRef"), Unused));
        }
    }
    return true;
}

// ============================================================================
// AssetDumpMapReferences.ArrayOfSoftWorlds
// TArray<TSoftObjectPtr<UWorld>> with two paths -> two entries with indexed property names.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpMapReferences_ArrayOfSoftWorldsTest,
    "PinWright.utils.asset_dump_map_references.ArrayOfSoftWorlds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpMapReferences_ArrayOfSoftWorldsTest::RunTest(const FString& Parameters)
{
    // Removing the FArrayProperty traversal branch in BuildMapReferencesJson would cause this test
    // to fail because the TArray<TSoftObjectPtr<UWorld>> would be ignored entirely and zero entries
    // would be emitted for MapList.
    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;

    const FString PathA = TEXT("/Engine/Maps/Templates/Template_Default.Template_Default");
    const FString PathB = TEXT("/Engine/Maps/Entry.Entry");
    Fix->MapList.Add(TSoftObjectPtr<UWorld>(FSoftObjectPath(PathA)));
    Fix->MapList.Add(TSoftObjectPtr<UWorld>(FSoftObjectPath(PathB)));

    TArray<TSharedPtr<FJsonValue>> Entries = GetReferences(Fix);
    TestEqual(TEXT("Two array entries emitted"), Entries.Num(), 2);
    if (Entries.Num() != 2) return false;

    // Entries are sorted by property name; "MapList[0]" < "MapList[1]".
    TSharedPtr<FJsonObject> First  = Entries[0]->AsObject();
    TSharedPtr<FJsonObject> Second = Entries[1]->AsObject();
    if (!First || !Second) return false;

    TestEqual(TEXT("First entry property is MapList[0]"),  First->GetStringField(TEXT("property")),  FString(TEXT("MapList[0]")));
    TestEqual(TEXT("Second entry property is MapList[1]"), Second->GetStringField(TEXT("property")), FString(TEXT("MapList[1]")));
    TestEqual(TEXT("First entry path"),  First->GetStringField(TEXT("path")),  PathA);
    TestEqual(TEXT("Second entry path"), Second->GetStringField(TEXT("path")), PathB);
    TestEqual(TEXT("Source is soft-uworld"), First->GetStringField(TEXT("source")), FString(TEXT("soft-uworld")));
    return true;
}

// ============================================================================
// AssetDumpMapReferences.NonWorldSoftRefIgnored
// TSoftObjectPtr<UTexture2D> populated -> proves the IsChildOf(UWorld) gate skips non-world refs.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpMapReferences_NonWorldSoftRefIgnoredTest,
    "PinWright.utils.asset_dump_map_references.NonWorldSoftRefIgnored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpMapReferences_NonWorldSoftRefIgnoredTest::RunTest(const FString& Parameters)
{
    // Removing the IsChildOf(UWorld::StaticClass()) filter would cause this test to fail because
    // the texture soft-ref would appear alongside the world refs.
    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;

    Fix->NotAMap = FSoftObjectPath(TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));

    TArray<TSharedPtr<FJsonValue>> Entries = GetReferences(Fix);
    TSharedPtr<FJsonObject> Unused;
    TestFalse(TEXT("Texture soft-ref is filtered out by IsChildOf(UWorld) gate"),
        FindEntryByProperty(Entries, TEXT("NotAMap"), Unused));
    // With only NotAMap populated, the function returns nullptr (no qualifying refs).
    TestEqual(TEXT("Zero entries when only non-world soft-refs are set"), Entries.Num(), 0);
    return true;
}
