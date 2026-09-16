// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test: blueprint.list with a native (non-Blueprint) class filter must NOT
// surface in-memory-only objects (the phenomenon reported as placed level actor
// INSTANCES) — only authorable on-disk assets. Guards the fix in
// PinWright_BlueprintHandlers_List.cpp that sets FARFilter::bIncludeOnlyOnDiskAssets = true
// before IAssetRegistry::GetAssets.
//
// Board ticket: B-blueprint-list-class-surfaces-instances. Without the flag, GetAssets
// returns in-memory objects (a loaded World-Partition level's :PersistentLevel. actor
// sub-objects) alongside real on-disk packages. The default class="Blueprint" path dodged
// it (no UBlueprint instances live in levels), but the native-class branch leaked every
// placed actor whose class descends from the filter. Same root cause + one-line fix that
// B-dump-folder-includes-level-subobjects (DONE) shipped for asset.dump_folder.
//
// Fixture: a WP level with placed external actors is heavy and environment-specific, so we
// reproduce the essential phenomenon in-code — an in-memory-only asset (never saved to
// disk) whose own class descends from the native filter class. bIncludeOnlyOnDiskAssets is
// precisely the flag that excludes such live, in-memory-only rows from GetAssets, so the
// production handler must drop it. A direct AssetRegistry counterfactual first proves the
// fixture is a genuine leak candidate (present with the flag off, absent with it on), so a
// pass can never be a silent "fixture never existed".

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Engine/DataTable.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/TopLevelAssetPath.h"
#include "Misc/Guid.h"

namespace
{
    // True if any FAssetData in Assets resolves to the given object path.
    bool AssetListHasObjectPath(const TArray<FAssetData>& Assets, const FString& ObjectPath)
    {
        for (const FAssetData& Asset : Assets)
        {
            if (Asset.GetSoftObjectPath().ToString() == ObjectPath)
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintListExcludesInMemoryInstancesTest,
    "PinWright.blueprint.list.ExcludesInMemoryInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintListExcludesInMemoryInstancesTest::RunTest(const FString& Parameters)
{
    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

    // 1. Build an in-memory-only asset (never saved to disk) whose own class is a concrete
    //    native, non-Blueprint asset class. UDataTable descends from UObject, reports
    //    IsAsset()==true, and its class path is /Script/Engine.DataTable — the native-class
    //    branch of blueprint.list matches it recursively, exactly as it matched placed
    //    AActor instances in the field report. A GUID suffix keeps re-runs isolated.
    const FString UniqueName = FString::Printf(TEXT("PW_OnDiskFlag_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/%s"), *UniqueName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *UniqueName);

    UPackage* Pkg = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("in-memory package created"), Pkg))
    {
        return false;   // required fixture failed to build — a real failure, not a skip
    }

    UDataTable* InMemoryAsset =
        NewObject<UDataTable>(Pkg, FName(*UniqueName), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("in-memory UDataTable created"), InMemoryAsset))
    {
        return false;
    }
    // A valid RowStruct keeps the DataTable's teardown quiet (EmptyTable() logs an error
    // when RowStruct is null); it does not affect IsAsset() or class-filter matching.
    InMemoryAsset->RowStruct = FTableRowBase::StaticStruct();

    // Force the registry to complete any pending scan of the folder (nothing is on disk
    // there, so this cannot add our asset to the on-disk State — it stays live-only, which
    // is the whole point of the flag under test). Mirrors the proven sibling recipe
    // (TestAssetSearchNativeSubclassLive.cpp).
    AssetRegistry.ScanPathsSynchronous({ TEXT("/Engine/Transient") }, /*bForceRescan=*/true);

    ON_SCOPE_EXIT
    {
        FAssetRegistryModule::AssetDeleted(InMemoryAsset);
        InMemoryAsset->ClearFlags(RF_Standalone | RF_Public);
        InMemoryAsset->RemoveFromRoot();
        InMemoryAsset->MarkAsGarbage();
        Pkg->ClearFlags(RF_Standalone | RF_Public);
        Pkg->RemoveFromRoot();
        Pkg->MarkAsGarbage();
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    };

    // Filter mirrors what blueprint.list builds for a native-class query, scoped to the
    // transient folder so only our in-memory asset is in scope.
    FARFilter BaseFilter;
    BaseFilter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
    BaseFilter.bRecursiveClasses = true;
    BaseFilter.PackagePaths.Add(FName(TEXT("/Engine/Transient")));
    BaseFilter.bRecursivePaths = true;

    // 2. Counterfactual precondition (engine AssetRegistry API — proves the fixture is a
    //    genuine leak candidate and that bIncludeOnlyOnDiskAssets is the discriminator).
    //    Probe BaseFilter with the on-disk flag toggled: the fixture must be present with it
    //    off (the buggy behavior) and absent with it on (the fix).
    auto IsPresentWithOnDiskFlag = [&](bool bOnlyOnDisk)
    {
        FARFilter Probe = BaseFilter;
        Probe.bIncludeOnlyOnDiskAssets = bOnlyOnDisk;
        TArray<FAssetData> ProbeList;
        AssetRegistry.GetAssets(Probe, ProbeList);
        return AssetListHasObjectPath(ProbeList, ObjectPath);
    };
    TestTrue(TEXT("precondition: in-memory asset IS returned with the on-disk flag off "
                  "(fixture is a genuine leak candidate)"),
        IsPresentWithOnDiskFlag(false));
    TestFalse(TEXT("precondition: in-memory asset is EXCLUDED with the on-disk flag on "
                   "(the flag is the discriminator)"),
        IsPresentWithOnDiskFlag(true));

    // 3. Production assertion: the real blueprint.list handler, given a native-class filter,
    //    must NOT surface the in-memory-only asset. This assertion fails if the fix
    //    (Filter.bIncludeOnlyOnDiskAssets = true) is reverted. The full class path exercises
    //    the native-class branch deterministically (no short-name resolution ambiguity).
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("class"), TEXT("/Script/Engine.DataTable"));
    Payload->SetStringField(TEXT("path"), TEXT("/Engine/Transient"));
    Payload->SetNumberField(TEXT("limit"), -1.0);   // unlimited — no pagination truncation

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(TEXT("blueprint.list"), Payload, Capture);
    TestTrue(TEXT("blueprint.list handler found"), bInvoked);
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("response reports success"), Capture.bSuccess);

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(TEXT("blueprint.list did not return a successful result"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* BlueprintsArray = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("blueprints"), BlueprintsArray) || !BlueprintsArray)
    {
        AddError(TEXT("blueprint.list response missing 'blueprints' array"));
        return false;
    }

    bool bLeaked = false;
    for (const TSharedPtr<FJsonValue>& Val : *BlueprintsArray)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Val.IsValid() || !Val->TryGetObject(Obj) || !Obj)
        {
            continue;
        }
        FString RowPath;
        (*Obj)->TryGetStringField(TEXT("path"), RowPath);
        if (RowPath == ObjectPath)
        {
            bLeaked = true;
            break;
        }
    }

    TestFalse(
        TEXT("blueprint.list with a native class filter excludes the in-memory-only asset "
             "(no bIncludeOnlyOnDiskAssets leak)"),
        bLeaked);

    return true;
}
