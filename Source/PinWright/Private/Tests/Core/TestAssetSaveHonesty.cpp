// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for the three Utils/AssetUtils helpers that used to be constants
// wearing measurement names, and for the fields handlers publish from them.
//
// The defect class: McpSafeAssetSave returned the literal `true` for any non-null
// pointer while writing nothing; AddAssetVerification emitted `existsAfter: true` as a
// literal fifteen lines above the real probe VerifyAssetExists; SaveLoadedAssetThrottled
// returned `true` on both the transient-package early-out and the throttle skip whose own
// log line says "skipping save". A handler wiring `saved` and `existsAfter` from those
// three got two corroborating fields that were both unconditional, so the write path and
// the verify path agreed with each other while both disagreed with the disk. The 0.5s
// FSaveThrottler window (State/SaveThrottler.h) is well inside the rate an agent issues
// sequential edits to one asset, and the on-disk hardening could not see it because it
// probed existence and the .uasset from the PREVIOUS save was right there.
//
// Every assertion below is written so it FAILS if the corresponding helper is reverted to
// its constant. That direction is the point: these are not "the verb returns success"
// tests, they are "the verb reports failure when the underlying operation did not happen"
// tests.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PinWrightHelpers.h"
#include "State/PluginState.h"
#include "State/SaveThrottler.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
// Distinct helper names (not the anonymous-namespace helpers of the sibling Core test
// TUs) so a Unity merge cannot collide on an ODR duplicate.
FString MakeSaveHonestyPackagePath(const TCHAR* Prefix)
{
    return FString::Printf(
        TEXT("/Game/PinWrightTests/SaveHonesty_%s_%s"),
        Prefix,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

// A live, never-written /Game package holding a top-level object: exactly the state
// McpSafeAssetSave leaves behind. Not RF_Transient — the transient case is covered
// separately and must classify differently.
UObject* MakeSaveHonestyInMemoryAsset(const FString& PackagePath)
{
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }
    // UObject::StaticClass() is abstract, so allocating one in a package trips the engine
    // ensure at UObjectGlobals. These tests care about save and existence semantics, not
    // about which concrete class the asset is.
    UObject* Asset = NewObject<UMaterial>(
        Package,
        *FPackageName::GetLongPackageAssetName(PackagePath),
        RF_Public | RF_Standalone);
    if (Asset)
    {
        Asset->AddToRoot();
    }
    return Asset;
}

// An asset that genuinely lives on disk and holds no unsaved changes — the only state
// for which "saved" is true. The engine default material ships with every install and is
// never authored by these tests.
const TCHAR* GSaveHonestyOnDiskAssetPath = TEXT("/Engine/EngineMaterials/DefaultMaterial");
} // namespace

// WasSavePersisted is the single place that decides which SaveLoadedAssetThrottled
// outcomes count as "the asset's current state is on disk". If it ever collapses back to
// "anything that is not Failed", the throttle-skip lie is restored wholesale, so this
// enumerates all five outcomes rather than only the interesting one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveOutcomeClassificationTest,
    "PinWright.core.asset_save_honesty.was_save_persisted.Classification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSaveOutcomeClassificationTest::RunTest(const FString& Parameters)
{
    // The two outcomes that used to return `true` from the bool signature. These are the
    // regression guards: both wrote nothing, and both were reported as saved.
    TestFalse(TEXT("a throttle skip on a DIRTY package is not persisted"),
        WasSavePersisted(ESaveLoadedAssetOutcome::SkippedThrottledDirty));
    TestFalse(TEXT("a transient-package asset is never persisted"),
        WasSavePersisted(ESaveLoadedAssetOutcome::NotPersistable));

    TestFalse(TEXT("a failed or refused save is not persisted"),
        WasSavePersisted(ESaveLoadedAssetOutcome::Failed));

    // The two honest positives. A throttle skip on a CLEAN package wrote nothing either,
    // but there was nothing to write, so the on-disk revision already matches memory.
    TestTrue(TEXT("a completed save is persisted"),
        WasSavePersisted(ESaveLoadedAssetOutcome::Saved));
    TestTrue(TEXT("a throttle skip on an already-clean package is persisted"),
        WasSavePersisted(ESaveLoadedAssetOutcome::SkippedAlreadyClean));

    return true;
}

// The demonstrated failure, driven end to end through the real helper: edit an asset,
// save it, edit it again inside the throttle window. The second call must not claim the
// edit reached disk. Pre-fix SaveLoadedAssetThrottled returned true here (AssetUtils.cpp
// "return true; // Throttled") and the caller published saved:true.
//
// The throttler's last-save timestamp is seeded directly rather than by performing a real
// save, so the test needs no writable content path and cannot be flaky on save duration.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThrottleSkipOnDirtyPackageIsNotPersistedTest,
    "PinWright.core.asset_save_honesty.save_loaded_asset_throttled.ThrottleSkipOnDirtyPackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FThrottleSkipOnDirtyPackageIsNotPersistedTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeSaveHonestyPackagePath(TEXT("Throttle"));
    UObject* Asset = MakeSaveHonestyInMemoryAsset(PackagePath);
    if (!TestNotNull(TEXT("built an in-memory asset for the throttle probe"), Asset))
    {
        return false;
    }
    UPackage* Package = Asset->GetPackage();
    ON_SCOPE_EXIT
    {
        // Leave no dirty /Game package behind for the editor's save-on-exit prompt.
        if (Package)
        {
            Package->SetDirtyFlag(false);
        }
        Asset->RemoveFromRoot();
    };

    if (!TestNotNull(TEXT("the in-memory asset has a package"), Package))
    {
        return false;
    }

    FSaveThrottler& Throttler = FPluginState::Get().SaveThrottle();
    // "This asset was saved a moment ago" — the state the first edit's save leaves.
    Throttler.RecordSave(Asset->GetPathName());
    // "and then it was edited again" — SetDirtyFlag rather than MarkPackageDirty because
    // MarkPackageDirty is conditional on editor/load state and would make the setup
    // non-deterministic.
    Package->SetDirtyFlag(true);
    TestTrue(TEXT("the package is dirty going into the throttled save"), Package->IsDirty());

    const ESaveLoadedAssetOutcome Outcome = SaveLoadedAssetThrottled(Asset);

    TestEqual(TEXT("a throttled save of a dirty package classifies as SkippedThrottledDirty"),
        static_cast<int32>(Outcome),
        static_cast<int32>(ESaveLoadedAssetOutcome::SkippedThrottledDirty));
    // The load-bearing assertion: this is the exact call whose bool return used to be
    // `true` with the edit still only in memory.
    TestFalse(TEXT("a throttled save of a dirty package must NOT report persisted"),
        WasSavePersisted(Outcome));
    TestTrue(TEXT("the throttled save left the package dirty (nothing was written)"),
        Package->IsDirty());

    return true;
}

// A transient-package asset can never reach disk at all. The early-out used to
// `return true`, which is how create verbs whose asset landed in the transient package
// reported saved:true for something discarded at editor shutdown.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransientAssetIsNotPersistableTest,
    "PinWright.core.asset_save_honesty.save_loaded_asset_throttled.TransientNotPersistable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTransientAssetIsNotPersistableTest::RunTest(const FString& Parameters)
{
    UObject* TransientAsset = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Public);
    if (!TestNotNull(TEXT("built a transient-package object"), TransientAsset))
    {
        return false;
    }
    TransientAsset->AddToRoot();
    ON_SCOPE_EXIT { TransientAsset->RemoveFromRoot(); };

    const ESaveLoadedAssetOutcome Outcome = SaveLoadedAssetThrottled(TransientAsset);
    TestEqual(TEXT("a transient-package asset classifies as NotPersistable"),
        static_cast<int32>(Outcome),
        static_cast<int32>(ESaveLoadedAssetOutcome::NotPersistable));
    TestFalse(TEXT("a transient-package asset must NOT report persisted"),
        WasSavePersisted(Outcome));

    // A null asset is a failure, not a silent success.
    TestFalse(TEXT("a null asset must NOT report persisted"),
        WasSavePersisted(SaveLoadedAssetThrottled(nullptr)));

    return true;
}

// IsAssetPersistedToDisk is the primitive every mark-dirty-only report is now derived
// from. Both halves of its definition are load-bearing and are asserted separately:
// on-disk alone is the existence-not-freshness hole, clean alone passes a package that
// was never written.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIsAssetPersistedToDiskTest,
    "PinWright.core.asset_save_honesty.is_asset_persisted_to_disk.MemoryVsDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIsAssetPersistedToDiskTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("null is not persisted"), IsAssetPersistedToDisk(nullptr));

    UObject* TransientAsset = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Public);
    if (TestNotNull(TEXT("built a transient-package object"), TransientAsset))
    {
        TransientAsset->AddToRoot();
        TestFalse(TEXT("a transient-package asset is never persisted"),
            IsAssetPersistedToDisk(TransientAsset));
        TransientAsset->RemoveFromRoot();
    }

    // The state McpSafeAssetSave leaves: registered, resolvable, dirty, no file. This is
    // the case the old constant `true` reported as saved.
    const FString PackagePath = MakeSaveHonestyPackagePath(TEXT("InMemory"));
    UObject* InMemory = MakeSaveHonestyInMemoryAsset(PackagePath);
    if (TestNotNull(TEXT("built an in-memory /Game asset"), InMemory))
    {
        UPackage* Package = InMemory->GetPackage();
        if (Package)
        {
            Package->SetDirtyFlag(true);
        }
        TestFalse(TEXT("a dirty, never-written /Game asset is not persisted"),
            IsAssetPersistedToDisk(InMemory));

        // Clean but still never written: the FactoryCreateNew shape. The dirty check
        // alone would wave this through, which is why the on-disk probe is required too.
        if (Package)
        {
            Package->SetDirtyFlag(false);
        }
        TestFalse(TEXT("a CLEAN but never-written /Game asset is still not persisted"),
            IsAssetPersistedToDisk(InMemory));

        InMemory->RemoveFromRoot();
    }

    // Positive control: a real, clean, on-disk asset must report persisted, otherwise the
    // helper would be a constant `false` and equally useless.
    // Typed load: LoadObject<UObject> on a bare package path can resolve to the UPackage
    // itself rather than the asset inside it.
    UObject* OnDisk = LoadObject<UMaterialInterface>(nullptr, GSaveHonestyOnDiskAssetPath);
    if (TestNotNull(TEXT("loaded the engine default material from disk"), OnDisk))
    {
        UPackage* Package = OnDisk->GetPackage();
        if (Package && Package->IsDirty())
        {
            // Something else in this run dirtied engine content; the positive control
            // cannot be evaluated, and silently passing would hide a broken probe.
            AddWarning(FString::Printf(
                TEXT("%s is dirty in this session; skipping the persisted-positive control."),
                GSaveHonestyOnDiskAssetPath));
        }
        else
        {
            TestTrue(TEXT("a clean asset with a .uasset on disk IS persisted"),
                IsAssetPersistedToDisk(OnDisk));
        }
    }

    return true;
}

// AddMarkDirtySaveReport is what every former `SetBoolField("saved", McpSafeAssetSave(X))`
// site now calls. The contract it must hold: after a mark-dirty of an unwritten asset the
// answer is saved:false + pendingFlush:true, never saved:true.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarkDirtySaveReportTest,
    "PinWright.core.asset_save_honesty.add_mark_dirty_save_report.DeferredIsNotSaved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMarkDirtySaveReportTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeSaveHonestyPackagePath(TEXT("Report"));
    UObject* Asset = MakeSaveHonestyInMemoryAsset(PackagePath);
    if (!TestNotNull(TEXT("built an in-memory asset for the report probe"), Asset))
    {
        return false;
    }
    UPackage* Package = Asset->GetPackage();
    ON_SCOPE_EXIT
    {
        if (Package)
        {
            Package->SetDirtyFlag(false);
        }
        Asset->RemoveFromRoot();
        CleanupTestAsset(Asset->GetPathName());
    };

    // Exactly what McpSafeAssetSave leaves behind.
    McpSafeAssetSave(Asset);

    TSharedPtr<FJsonObject> Requested = MakeShared<FJsonObject>();
    AddMarkDirtySaveReport(Requested, Asset, /*bSaveRequested=*/true);

    bool bValue = true;
    TestTrue(TEXT("saveRequested is present"), Requested->TryGetBoolField(TEXT("saveRequested"), bValue));
    TestTrue(TEXT("saveRequested echoes the request"), bValue);

    bValue = false;
    TestTrue(TEXT("markedForSave is present"), Requested->TryGetBoolField(TEXT("markedForSave"), bValue));
    TestTrue(TEXT("markedForSave records that the package was dirtied"), bValue);

    // The regression guard. Pre-fix this field was McpSafeAssetSave's constant `true`.
    bValue = true;
    TestTrue(TEXT("saved is present"), Requested->TryGetBoolField(TEXT("saved"), bValue));
    TestFalse(TEXT("a mark-dirty of an unwritten asset must report saved:false"), bValue);

    bValue = false;
    TestTrue(TEXT("pendingFlush is present when the edit did not reach disk"),
        Requested->TryGetBoolField(TEXT("pendingFlush"), bValue));
    TestTrue(TEXT("pendingFlush is true when the edit did not reach disk"), bValue);

    // No save asked for: still saved:false, but pendingFlush must be absent so a caller
    // can tell "asked and deferred" from "never asked".
    TSharedPtr<FJsonObject> NotRequested = MakeShared<FJsonObject>();
    AddMarkDirtySaveReport(NotRequested, Asset, /*bSaveRequested=*/false);

    bValue = true;
    TestTrue(TEXT("saved is present when no save was requested"),
        NotRequested->TryGetBoolField(TEXT("saved"), bValue));
    TestFalse(TEXT("saved is false when no save was requested"), bValue);
    TestFalse(TEXT("pendingFlush is absent when no save was requested"),
        NotRequested->HasField(TEXT("pendingFlush")));

    return true;
}

// AddAssetVerification's existsAfter was the literal `true`. It is now measured, and the
// response additionally separates "resolvable" from "durable" via existsOnDisk /
// pendingSave — the distinction the single constant erased.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetVerificationIsMeasuredTest,
    "PinWright.core.asset_save_honesty.add_asset_verification.ExistsAfterIsMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetVerificationIsMeasuredTest::RunTest(const FString& Parameters)
{
    // A transient-package asset can never be persisted, so both existence facts are
    // false. Pre-fix this response said existsAfter:true.
    // A CONCRETE class: UObject::StaticClass() carries CLASS_Abstract, and allocating one
    // in a package trips the engine ensure at UObjectGlobals "Class which was marked
    // abstract was trying to be loaded". UDataAsset is abstract too; UMaterial is not.
    // The test needs any object in the transient package, not a bare UObject.
    UObject* TransientAsset = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Public);
    if (TestNotNull(TEXT("built a transient-package object"), TransientAsset))
    {
        TransientAsset->AddToRoot();
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        AddAssetVerification(Response, TransientAsset);

        bool bValue = true;
        TestTrue(TEXT("existsAfter is present for a transient asset"),
            Response->TryGetBoolField(TEXT("existsAfter"), bValue));
        TestFalse(TEXT("a transient-package asset must report existsAfter:false"), bValue);

        bValue = true;
        TestTrue(TEXT("existsOnDisk is present for a transient asset"),
            Response->TryGetBoolField(TEXT("existsOnDisk"), bValue));
        TestFalse(TEXT("a transient-package asset must report existsOnDisk:false"), bValue);

        TransientAsset->RemoveFromRoot();
    }

    // A never-written /Game asset IS resolvable (the registry knows it) but is NOT
    // durable. Both must be reported, because existsAfter alone reads as durable.
    const FString PackagePath = MakeSaveHonestyPackagePath(TEXT("Verify"));
    UObject* InMemory = MakeSaveHonestyInMemoryAsset(PackagePath);
    if (TestNotNull(TEXT("built an in-memory /Game asset"), InMemory))
    {
        UPackage* Package = InMemory->GetPackage();
        McpSafeAssetSave(InMemory);

        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        AddAssetVerification(Response, InMemory);

        bool bValue = true;
        TestTrue(TEXT("existsOnDisk is present for an in-memory asset"),
            Response->TryGetBoolField(TEXT("existsOnDisk"), bValue));
        TestFalse(TEXT("a never-written /Game asset must report existsOnDisk:false"), bValue);

        bValue = false;
        TestTrue(TEXT("pendingSave is present while the package holds unsaved changes"),
            Response->TryGetBoolField(TEXT("pendingSave"), bValue));
        TestTrue(TEXT("pendingSave is true while the package holds unsaved changes"), bValue);

        if (Package)
        {
            Package->SetDirtyFlag(false);
        }
        InMemory->RemoveFromRoot();
        CleanupTestAsset(InMemory->GetPathName());
    }

    // Positive control: a real on-disk asset must report both facts true, otherwise the
    // probe has simply become a constant `false`.
    // Typed load: LoadObject<UObject> on a bare package path can resolve to the UPackage
    // itself rather than the asset inside it.
    UObject* OnDisk = LoadObject<UMaterialInterface>(nullptr, GSaveHonestyOnDiskAssetPath);
    if (TestNotNull(TEXT("loaded the engine default material from disk"), OnDisk))
    {
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        AddAssetVerification(Response, OnDisk);

        bool bValue = false;
        TestTrue(TEXT("existsAfter is present for an on-disk asset"),
            Response->TryGetBoolField(TEXT("existsAfter"), bValue));
        TestTrue(TEXT("an on-disk asset reports existsAfter:true"), bValue);

        bValue = false;
        TestTrue(TEXT("existsOnDisk is present for an on-disk asset"),
            Response->TryGetBoolField(TEXT("existsOnDisk"), bValue));
        TestTrue(TEXT("an on-disk asset reports existsOnDisk:true"), bValue);
    }

    return true;
}
