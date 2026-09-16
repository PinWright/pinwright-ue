// Copyright (c) 2026 Alexander Penkin. MIT License.

// asset.bulk_delete must report what happened, not what was asked for.
//
// The defect (board B-bulk-delete-unverified-deletions): the response was built entirely
// from the inputs and one integer. `deleted[]` was `ValidPaths` - the caller's own list,
// filled in BEFORE ObjectTools::DeleteObjects ran and never revisited - and `success` was
// `DeletedCount > 0`. The engine returns only a count on this route, so a partial batch was
// not merely unreported, it was unrepresentable: delete 1 of 20 and the answer was
// `success: true` with all 20 names in `deleted[]`. Nothing probed the asset registry or
// the disk. A second defect sat in the same block: a path failing `DoesAssetExist` /
// `LoadAsset` was dropped from `ValidPaths` and `ObjectsToDelete` with no `missing[]`, so
// `requested` counted what LOADED rather than what was asked, and no field could tell the
// caller a path was never attempted.
//
// COUNTERFACTUAL, per test:
//   * PartialBatchIsReportedAsPartial - revert `success` to `DeletedCount > 0` and rebuild
//     `deleted[]` from the requested list, and the surviving asset is named in `deleted[]`
//     under `success: true`. This is the false-success test.
//   * UnattemptedPathIsReportedAsMissing - drop the per-request record and `missing[]`
//     disappears while `requestedCount` collapses back to the count that loaded, so a
//     two-path request with one typo becomes indistinguishable from a one-path request.
//
// HOW THE PARTIAL BATCH IS FORCED, and why it is deterministic. `ObjectTools::DeleteObjects`
// -> `DeleteItems` -> `FAssetDeleteModel::DoDelete` -> `DeleteObjectsUnchecked`, which loops
// the batch and calls `ObjectTools::DeleteSingleObject` per object. That function broadcasts
// `FEditorDelegates::OnAssetsCanDelete` with a ONE-element array and returns false when any
// handler refuses - the object is skipped, uncounted and unlogged, while its neighbours
// delete normally. `DeleteItems` also broadcasts the same delegate once for the WHOLE batch
// beforehand, and a veto there aborts everything (the handler then errors honestly, which is
// not the case under test), so the fixture's handler refuses only when the broadcast carries
// exactly one object and it is the survivor. The refusal opens an `EAppMsgType::Ok`
// FMessageDialog, which returns its default unattended without a stack-trace dump.
//
// The other silent-loss routes on this verb (read-only package, `DoesPackageExist` cull in
// CleanupAfterSuccessfulDelete, the discarded `IFileManager::Delete` return) all need a real
// `.uasset` on the host's disk. These fixtures are never-saved in-memory packages announced
// with `FAssetRegistryModule::AssetCreated`, exactly as in TestBulkDeleteRedirectorScope.cpp,
// so nothing is ever written to or removed from the host's Content directory. The delegate
// veto reaches the same response defect without that cost.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Handlers/HandlerRegistration.h"
#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PinWrightSettings.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

// File-scope helper names carry the BulkDeleteVerifyTest_ namespace: bUseUnity merges
// translation units, so an unprefixed helper collides with a same-named static in a
// neighbouring test file (see CLAUDE.md > Building).

namespace BulkDeleteVerifyTest
{
    static const TCHAR* Folder = TEXT("/Game/PinWrightTests/BulkDeleteVerify");

    inline FString PackagePathFor(const FString& AssetName)
    {
        return FString::Printf(TEXT("%s/%s"), Folder, *AssetName);
    }

    inline FString ObjectPathFor(const FString& AssetName)
    {
        return FString::Printf(TEXT("%s/%s.%s"), Folder, *AssetName, *AssetName);
    }

    // Never-saved in-memory package announced to the registry, which indexes it exactly as
    // it does a freshly created real asset. Nothing reaches the host's Content directory.
    inline UMaterial* MakeMaterial(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        UMaterial* Material = NewObject<UMaterial>(
            Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            RF_Public | RF_Standalone);
        if (Material)
        {
            FAssetRegistryModule::AssetCreated(Material);
        }
        return Material;
    }

    // Forces modal suppression on for the body and restores both it and the process global
    // afterwards. InvokeHandler calls the registered function directly, so it never crosses
    // FRpcDispatcher's own FScopedUnattendedRpc - without this the handler's
    // ObjectTools::DeleteObjects would run unguarded. Mirrors the guard in
    // TestBulkDeleteRedirectorScope.cpp.
    struct FSuppression
    {
        UPinWrightSettings* Settings = GetMutableDefault<UPinWrightSettings>();
        bool bSavedSetting = false;
        bool bSavedGlobal = GIsRunningUnattendedScript;

        FSuppression()
        {
            PinWrightAutomationMode::ResetForTests();
            if (Settings)
            {
                bSavedSetting = Settings->bSuppressModalDialogsDuringRpc;
                Settings->bSuppressModalDialogsDuringRpc = true;
            }
            GIsRunningUnattendedScript = false;
        }

        ~FSuppression()
        {
            PinWrightAutomationMode::ResetForTests();
            if (Settings)
            {
                Settings->bSuppressModalDialogsDuringRpc = bSavedSetting;
            }
            GIsRunningUnattendedScript = bSavedGlobal;
        }
    };

    // Reads a top-level string array. bOutPresent separates "the field is missing" from
    // "the field is an empty array" - before the fix `missing[]` did not exist at all, and
    // an assertion that could not see the difference would have passed on the old shape.
    inline TArray<FString> StringArrayField(const TSharedPtr<FJsonObject>& Response,
                                            const TCHAR* FieldName, bool& bOutPresent)
    {
        TArray<FString> Values;
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        bOutPresent = Response.IsValid()
            && Response->TryGetArrayField(FieldName, Array) && Array != nullptr;
        if (bOutPresent)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Array)
            {
                if (Value.IsValid() && Value->Type == EJson::String)
                {
                    Values.Add(Value->AsString());
                }
            }
        }
        return Values;
    }

    // The results[] entry whose "path" equals Path, or an invalid pointer.
    inline TSharedPtr<FJsonObject> EntryForPath(const TSharedPtr<FJsonObject>& Response,
                                                const FString& Path)
    {
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!Response.IsValid() || !Response->TryGetArrayField(TEXT("results"), Entries)
            || Entries == nullptr)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Entries)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString EntryPath;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry
                && (*Entry)->TryGetStringField(TEXT("path"), EntryPath) && EntryPath == Path)
            {
                return *Entry;
            }
        }
        return nullptr;
    }

    // Field value, or bFallback when the field (or the entry) is absent. Every call site
    // passes the value that FAILS its assertion, so a missing field is a red test rather
    // than an accidental pass.
    inline bool EntryBool(const TSharedPtr<FJsonObject>& Entry, const TCHAR* FieldName,
                          bool bFallback)
    {
        bool bValue = false;
        if (Entry.IsValid() && Entry->TryGetBoolField(FieldName, bValue))
        {
            return bValue;
        }
        return bFallback;
    }

    // Field value, or -1 when absent (no count the handler emits can be negative).
    inline int32 IntField(const TSharedPtr<FJsonObject>& Response, const TCHAR* FieldName)
    {
        int32 Value = -1;
        if (Response.IsValid() && Response->TryGetNumberField(FieldName, Value))
        {
            return Value;
        }
        return -1;
    }
}

// ============================================================================
// The false-success test. One of two assets survives the delete; the response must say so.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBulkDeletePartialBatchReportedTest,
    "PinWright.asset.bulk_delete.PartialBatchIsReportedAsPartial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBulkDeletePartialBatchReportedTest::RunTest(const FString& /*Parameters*/)
{
    using namespace BulkDeleteVerifyTest;

    FSuppression Suppression;

    const FString DoomedPath   = PackagePathFor(TEXT("M_PartialDoomed"));
    const FString SurvivorPath = PackagePathFor(TEXT("M_PartialSurvivor"));

    // Declared FIRST so it runs LAST: ON_SCOPE_EXIT is LIFO, and the teardown below must
    // force-delete the survivor with the veto delegate already unhooked - otherwise the
    // fixture's own handler refuses the cleanup and leaves the asset in the host.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(DoomedPath);
        CleanupTestAsset(SurvivorPath);
    };

    UMaterial* Doomed = MakeMaterial(DoomedPath);
    if (!TestNotNull(TEXT("the asset that should be deleted was created"), Doomed)) return false;
    UMaterial* Survivor = MakeMaterial(SurvivorPath);
    if (!TestNotNull(TEXT("the asset that should survive was created"), Survivor)) return false;

    // Refuses exactly one object, and only on the per-object query DeleteSingleObject makes.
    // See the file header for why the batch-wide broadcast must be allowed through.
    FDelegateHandle VetoHandle = FEditorDelegates::OnAssetsCanDelete.AddLambda(
        [Survivor](const TArray<UObject*>& Objects, FCanDeleteAssetResult& OutResult)
        {
            if (Objects.Num() == 1 && Objects[0] == Survivor)
            {
                OutResult.Set(false);
            }
        });
    ON_SCOPE_EXIT
    {
        FEditorDelegates::OnAssetsCanDelete.Remove(VetoHandle);
    };

    TArray<TSharedPtr<FJsonValue>> PathsToDelete;
    PathsToDelete.Add(MakeShared<FJsonValueString>(DoomedPath));
    PathsToDelete.Add(MakeShared<FJsonValueString>(SurvivorPath));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("assetPaths"), PathsToDelete);
    // The redirector fixup is a separate concern with its own coverage in
    // TestBulkDeleteRedirectorScope.cpp; switching it off keeps this test to the response.
    Payload->SetBoolField(TEXT("fixupRedirectors"), false);

    FTestResponseCapture Capture;
    {
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("asset.bulk_delete handler found"),
            InvokeHandlerWithCapture(TEXT("asset.bulk_delete"), Payload, Capture));
    }

    TestTrue(TEXT("the delete responded"), Capture.bWasCalled);

    // The fixture has to actually be partial before any response assertion means anything:
    // one asset gone, one still there. Without this the checks below would also pass on a
    // batch that deleted nothing at all.
    TestFalse(TEXT("the unvetoed asset really is gone from the registry"),
        UEditorAssetLibrary::DoesAssetExist(DoomedPath));
    TestTrue(TEXT("the vetoed asset really is still in the registry"),
        UEditorAssetLibrary::DoesAssetExist(SurvivorPath));
    TestNotNull(TEXT("the vetoed asset really is still in memory"),
        FindObject<UMaterial>(nullptr, *ObjectPathFor(TEXT("M_PartialSurvivor"))));

    // THE assertion. A batch that lost one of the two assets it named is a failed request.
    TestFalse(TEXT("a partial batch is not an envelope success"), Capture.bSuccess);
    TestEqual(TEXT("reported as a bulk delete failure"),
        Capture.ErrorCode, FString(TEXT("BULK_DELETE_FAILED")));

    if (!TestTrue(TEXT("the failure still carries a response body"), Capture.Result.IsValid()))
    {
        return false;
    }

    bool bSuccessField = true;
    TestTrue(TEXT("the body declares success explicitly"),
        Capture.Result->TryGetBoolField(TEXT("success"), bSuccessField));
    TestFalse(TEXT("success is false when anything the caller named survived"), bSuccessField);

    bool bExistsAfter = false;
    Capture.Result->TryGetBoolField(TEXT("existsAfter"), bExistsAfter);
    TestTrue(TEXT("existsAfter says something survived"), bExistsAfter);

    // deleted[] must be the PROBE, not the request. The survivor was in the caller's own
    // list, which is exactly what the old response echoed back into this field.
    bool bDeletedPresent = false;
    bool bFailedPresent = false;
    const TArray<FString> Deleted = StringArrayField(Capture.Result, TEXT("deleted"), bDeletedPresent);
    const TArray<FString> Failed = StringArrayField(Capture.Result, TEXT("failed"), bFailedPresent);

    TestTrue(TEXT("the response carries deleted[]"), bDeletedPresent);
    TestTrue(TEXT("the response carries failed[]"), bFailedPresent);
    TestTrue(TEXT("the asset that went is reported deleted"), Deleted.Contains(DoomedPath));
    TestFalse(TEXT("the surviving asset is NOT reported deleted"), Deleted.Contains(SurvivorPath));
    TestTrue(TEXT("the surviving asset is reported failed"), Failed.Contains(SurvivorPath));
    TestFalse(TEXT("the deleted asset is not also reported failed"), Failed.Contains(DoomedPath));

    TestEqual(TEXT("deletedCount counts the probe, not the request"),
        IntField(Capture.Result, TEXT("deletedCount")), 1);
    TestEqual(TEXT("failedCount counts the survivor"),
        IntField(Capture.Result, TEXT("failedCount")), 1);
    TestEqual(TEXT("requestedCount is the caller's array length"),
        IntField(Capture.Result, TEXT("requestedCount")), 2);

    // The engine's own figure survives as a separately named claim, and the point of the
    // fix is that it is positive here while the request is reported failed.
    TestTrue(TEXT("the engine reported at least one deletion"),
        IntField(Capture.Result, TEXT("engineDeletedCount")) > 0);

    const TSharedPtr<FJsonObject> SurvivorEntry = EntryForPath(Capture.Result, SurvivorPath);
    if (TestTrue(TEXT("results[] carries an entry for the surviving path"),
            SurvivorEntry.IsValid()))
    {
        TestTrue(TEXT("the survivor is recorded as having existed before"),
            EntryBool(SurvivorEntry, TEXT("existedBefore"), false));
        TestTrue(TEXT("the survivor was handed to the engine"),
            EntryBool(SurvivorEntry, TEXT("attempted"), false));
        TestTrue(TEXT("the survivor still exists afterwards"),
            EntryBool(SurvivorEntry, TEXT("existsAfter"), false));
        TestFalse(TEXT("the survivor's entry does not claim a deletion"),
            EntryBool(SurvivorEntry, TEXT("deleted"), true));
    }

    const TSharedPtr<FJsonObject> DoomedEntry = EntryForPath(Capture.Result, DoomedPath);
    if (TestTrue(TEXT("results[] carries an entry for the deleted path"), DoomedEntry.IsValid()))
    {
        TestTrue(TEXT("the deleted entry claims its deletion"),
            EntryBool(DoomedEntry, TEXT("deleted"), false));
        TestFalse(TEXT("the deleted entry does not still exist"),
            EntryBool(DoomedEntry, TEXT("existsAfter"), true));
        TestFalse(TEXT("the deleted entry left no file on disk"),
            EntryBool(DoomedEntry, TEXT("existsOnDisk"), true));
    }

    return true;
}

// ============================================================================
// A path the handler never hands to the engine must still appear in the answer. It used to
// be dropped from the request silently, taking `requested` down with it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBulkDeleteUnattemptedPathMissingTest,
    "PinWright.asset.bulk_delete.UnattemptedPathIsReportedAsMissing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBulkDeleteUnattemptedPathMissingTest::RunTest(const FString& /*Parameters*/)
{
    using namespace BulkDeleteVerifyTest;

    FSuppression Suppression;

    const FString RealPath = PackagePathFor(TEXT("M_MissingReal"));
    // Never created. The typo case the verb used to swallow whole.
    const FString TypoPath = PackagePathFor(TEXT("M_MissingTypo"));

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(RealPath);
    };

    if (!TestNotNull(TEXT("the asset that does exist was created"), MakeMaterial(RealPath)))
    {
        return false;
    }
    if (!TestFalse(TEXT("the typo'd path really does not exist"),
            UEditorAssetLibrary::DoesAssetExist(TypoPath)))
    {
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> PathsToDelete;
    PathsToDelete.Add(MakeShared<FJsonValueString>(RealPath));
    PathsToDelete.Add(MakeShared<FJsonValueString>(TypoPath));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("assetPaths"), PathsToDelete);
    Payload->SetBoolField(TEXT("fixupRedirectors"), false);

    FTestResponseCapture Capture;
    {
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("asset.bulk_delete handler found"),
            InvokeHandlerWithCapture(TEXT("asset.bulk_delete"), Payload, Capture));
    }

    TestTrue(TEXT("the delete responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("the response carries a body"), Capture.Result.IsValid())) return false;

    // Nothing survived, so this is still a success - a path that was already absent is not
    // a failure of the delete. What it must never be is invisible.
    TestTrue(TEXT("an already-absent path does not fail the call"), Capture.bSuccess);

    bool bMissingPresent = false;
    bool bDeletedPresent = false;
    const TArray<FString> Missing = StringArrayField(Capture.Result, TEXT("missing"), bMissingPresent);
    const TArray<FString> Deleted = StringArrayField(Capture.Result, TEXT("deleted"), bDeletedPresent);

    TestTrue(TEXT("the response carries missing[] at all"), bMissingPresent);
    TestTrue(TEXT("the path that was never attempted is reported missing"),
        Missing.Contains(TypoPath));
    TestTrue(TEXT("the response carries deleted[]"), bDeletedPresent);
    TestTrue(TEXT("the real asset is reported deleted"), Deleted.Contains(RealPath));
    TestFalse(TEXT("the typo'd path is NOT reported deleted"), Deleted.Contains(TypoPath));

    // The count the caller can reconcile against their own array. It used to be the count
    // that LOADED, so a two-path request with one typo read as a one-path request.
    TestEqual(TEXT("requestedCount is the caller's array length"),
        IntField(Capture.Result, TEXT("requestedCount")), 2);
    TestEqual(TEXT("attemptedCount is what was handed to the engine"),
        IntField(Capture.Result, TEXT("attemptedCount")), 1);
    TestEqual(TEXT("missingCount names the drop"),
        IntField(Capture.Result, TEXT("missingCount")), 1);
    TestEqual(TEXT("deletedCount counts only the real deletion"),
        IntField(Capture.Result, TEXT("deletedCount")), 1);

    const TSharedPtr<FJsonObject> TypoEntry = EntryForPath(Capture.Result, TypoPath);
    if (TestTrue(TEXT("results[] carries an entry for the path that was never attempted"),
            TypoEntry.IsValid()))
    {
        TestFalse(TEXT("it did not exist before"),
            EntryBool(TypoEntry, TEXT("existedBefore"), true));
        TestFalse(TEXT("it was never handed to the engine"),
            EntryBool(TypoEntry, TEXT("attempted"), true));
        TestTrue(TEXT("it is flagged missing"), EntryBool(TypoEntry, TEXT("missing"), false));
        TestFalse(TEXT("an absent path is not counted as a deletion this call performed"),
            EntryBool(TypoEntry, TEXT("deleted"), true));
    }

    return true;
}
