// Copyright (c) 2026 Alexander Penkin. MIT License.

// Coverage for the EAssetSaveState save-report contract (B-model-compile-saved-false-opaque).
//
// model.compile save:true intermittently answered saved:false / pendingFlush:true, and the
// response carried nothing a caller could act on: the SAME false is emitted for a deferred edit
// (an editor.save_all fixes it), a failed write (it does not) and a package that can never be
// written (nothing ever will). SaveAssetToDiskReportingPresence had already MEASURED which of
// those it was - it captures file size, timestamp and the pre-save dirty flag - and threw the
// distinction away by collapsing the whole thing to a bool.
//
// Two halves, both of which fail if the fix is reverted:
//
//   * the emitter - AddAssetSaveReport must produce a distinct, correct
//     {saveRequested, saved, pendingFlush, saveState, saveDetail} tuple per state, and must
//     produce the pre-fix three-field shape byte-for-byte when no state is passed;
//   * the producer - SaveAssetToDiskReportingPresence must report a state that agrees with what
//     is actually on disk, driven through the real throttle/force branches rather than mocked.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "State/PluginState.h"
#include "UObject/Package.h"
#include "Utils/AssetUtils.h"

#include "Tests/TestUtils.h"

// Prefixed and file-static: Unity merges this TU with its neighbours, so an unprefixed helper
// collides (see CLAUDE.md > Building).
static FString AssetSaveStateTest_UniquePackagePath()
{
    return FString::Printf(TEXT("/Game/PinWrightTests/AssetSaveState/M_SaveState_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

// A bare dirty UMaterial in its own never-saved package - the shape a create verb leaves behind
// immediately before it asks for a save.
static UMaterial* AssetSaveStateTest_MakeMaterial(const FString& PackagePath)
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
        Material->MarkPackageDirty();
        // Part of the production save path: UEditorAssetSubsystem::SaveLoadedAsset refuses an
        // asset the registry has never heard of (IsARegisteredAsset), so a fixture that skips
        // this would exercise a failure branch instead of the one under test.
        FAssetRegistryModule::AssetCreated(Material);
    }
    return Material;
}

static FString AssetSaveStateTest_PackageFilename(const FString& PackagePath)
{
    FString Filename;
    return FPackageName::TryConvertLongPackageNameToFilename(
               PackagePath, Filename, FPackageName::GetAssetPackageExtension())
        ? Filename
        : FString();
}

// Reads a string field, or "<absent>" so a failure message names what was missing.
static FString AssetSaveStateTest_Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Value;
    return (Object.IsValid() && Object->TryGetStringField(Field, Value)) ? Value : FString(TEXT("<absent>"));
}

// ============================================================================
// The emitter
// ============================================================================

// One tuple per state, asserted field by field. The point of the enum is that no two states
// share a tuple, so this test enumerates them and checks the distinctions the wire contract
// promises - including the two that a bool cannot express:
//   * written and alreadyCurrent are BOTH durable (saved:true, no pendingFlush) despite one
//     writing bytes and the other writing none;
//   * deferred is the only not-durable state whose remedy is a flush, and it must NAME the verb.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveReportPerStateFieldsTest,
    "PinWright.assets.AssetSaveReport.EachStateEmitsItsOwnFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSaveReportPerStateFieldsTest::RunTest(const FString& /*Parameters*/)
{
    struct FCase
    {
        EAssetSaveState State;
        bool bSaveRequested;
        bool bDurable;          // what the caller measured, and what `saved` must echo
        const TCHAR* Wire;
    };

    const FCase Cases[] = {
        { EAssetSaveState::NotRequested,   false, false, TEXT("notRequested")   },
        { EAssetSaveState::Written,        true,  true,  TEXT("written")        },
        { EAssetSaveState::AlreadyCurrent, true,  true,  TEXT("alreadyCurrent") },
        { EAssetSaveState::Deferred,       true,  false, TEXT("deferred")       },
        { EAssetSaveState::Failed,         true,  false, TEXT("failed")         },
        { EAssetSaveState::BlockedByPie,   true,  false, TEXT("blockedByPie")   },
        { EAssetSaveState::NotPersistable, true,  false, TEXT("notPersistable") },
    };

    for (const FCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddAssetSaveReport(Result, Case.bSaveRequested, Case.bDurable, Case.State);

        const FString Where = FString::Printf(TEXT("[%s] "), Case.Wire);

        // Save-state reporting keeps the legacy field presence, but a known PIE refusal clears
        // pendingFlush because no flush can run until the external blocker ends.
        TestTrue(*(Where + TEXT("saveRequested echoes the request")),
            Result->GetBoolField(TEXT("saveRequested")) == Case.bSaveRequested);
        TestTrue(*(Where + TEXT("saved is durable-on-disk, nothing else")),
            Result->GetBoolField(TEXT("saved")) == (Case.bSaveRequested && Case.bDurable));
        TestTrue(*(Where + TEXT("pendingFlush is present exactly when a save was requested and is not durable")),
            Result->HasField(TEXT("pendingFlush")) == (Case.bSaveRequested && !Case.bDurable));
        if (Case.bSaveRequested && !Case.bDurable)
        {
            bool bPendingFlush = false;
            TestTrue(*(Where + TEXT("pendingFlush is a boolean")),
                Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush));
            TestTrue(*(Where + TEXT("only blockedByPie clears pendingFlush")),
                bPendingFlush == (Case.State != EAssetSaveState::BlockedByPie));
        }

        // The two new ones.
        TestEqual(*(Where + TEXT("saveState carries the stable wire spelling")),
            AssetSaveStateTest_Str(Result, TEXT("saveState")), FString(Case.Wire));
        TestFalse(*(Where + TEXT("saveDetail is non-empty")),
            AssetSaveStateTest_Str(Result, TEXT("saveDetail")).IsEmpty());
    }

    // written and alreadyCurrent: both durable, and NEITHER asks for a flush. This is the pair a
    // bool cannot separate, so assert the separation exists at all.
    {
        TSharedPtr<FJsonObject> WrittenResult = MakeShared<FJsonObject>();
        AddAssetSaveReport(WrittenResult, true, true, EAssetSaveState::Written);
        TSharedPtr<FJsonObject> CurrentResult = MakeShared<FJsonObject>();
        AddAssetSaveReport(CurrentResult, true, true, EAssetSaveState::AlreadyCurrent);

        TestTrue(TEXT("written is saved:true"), WrittenResult->GetBoolField(TEXT("saved")));
        TestFalse(TEXT("written owes no flush"), WrittenResult->HasField(TEXT("pendingFlush")));
        TestTrue(TEXT("alreadyCurrent is saved:true"), CurrentResult->GetBoolField(TEXT("saved")));
        TestFalse(TEXT("alreadyCurrent owes no flush"), CurrentResult->HasField(TEXT("pendingFlush")));
        TestNotEqual(TEXT("written and alreadyCurrent are distinguishable on the wire"),
            AssetSaveStateTest_Str(WrittenResult, TEXT("saveState")),
            AssetSaveStateTest_Str(CurrentResult, TEXT("saveState")));
    }

    // deferred must NAME the verb that flushes it. A detail string that only says "not saved"
    // leaves the caller exactly where the bool did.
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddAssetSaveReport(Result, true, false, EAssetSaveState::Deferred);
        const FString Detail = AssetSaveStateTest_Str(Result, TEXT("saveDetail"));
        TestTrue(TEXT("deferred asks for a flush"), Result->GetBoolField(TEXT("pendingFlush")));
        TestTrue(*FString::Printf(TEXT("deferred names the flushing verb: '%s'"), *Detail),
            Detail.Contains(TEXT("editor.save_all")) || Detail.Contains(TEXT("asset.save")));
    }

    // failed and notPersistable must NOT be reported as merely pending: each carries its own
    // state, distinct from deferred, and a detail that says a flush is not the remedy.
    {
        TSharedPtr<FJsonObject> FailedResult = MakeShared<FJsonObject>();
        AddAssetSaveReport(FailedResult, true, false, EAssetSaveState::Failed);
        TSharedPtr<FJsonObject> TransientResult = MakeShared<FJsonObject>();
        AddAssetSaveReport(TransientResult, true, false, EAssetSaveState::NotPersistable);

        TestNotEqual(TEXT("failed is not reported as deferred"),
            AssetSaveStateTest_Str(FailedResult, TEXT("saveState")), FString(TEXT("deferred")));
        TestNotEqual(TEXT("notPersistable is not reported as deferred"),
            AssetSaveStateTest_Str(TransientResult, TEXT("saveState")), FString(TEXT("deferred")));

        const FString FailedDetail = AssetSaveStateTest_Str(FailedResult, TEXT("saveDetail"));
        TestTrue(*FString::Printf(TEXT("failed says a flush will not help: '%s'"), *FailedDetail),
            FailedDetail.Contains(TEXT("will not")));
        const FString TransientDetail = AssetSaveStateTest_Str(TransientResult, TEXT("saveDetail"));
        TestTrue(*FString::Printf(TEXT("notPersistable says it can never be persisted: '%s'"), *TransientDetail),
            TransientDetail.Contains(TEXT("never")));
    }

    // blockedByPie is the state the board tickets are about: a PIE session refuses every
    // single-asset save in the editor, and the pre-fix payload (saved:false + pendingFlush, no
    // state) was byte-identical to the throttle case whose documented remedy is force:true.
    // Four things must hold, and each of them failed before the complete fix.
    {
        TSharedPtr<FJsonObject> PieResult = MakeShared<FJsonObject>();
        AddAssetSaveReport(PieResult, /*bSaveRequested=*/true, /*bSavedToDisk=*/false,
            EAssetSaveState::BlockedByPie);
        const FString PieState = AssetSaveStateTest_Str(PieResult, TEXT("saveState"));

        // 1. It is not deferred. Reading it as deferred is the infinite retry loop.
        TestNotEqual(TEXT("blockedByPie is not reported as deferred"),
            PieState, FString(TEXT("deferred")));
        // 2. It is not folded into the generic failure either: the remedy differs (wait, then
        //    re-issue) and so must the state.
        TestNotEqual(TEXT("blockedByPie is not reported as a generic failed"),
            PieState, FString(TEXT("failed")));
        TestEqual(TEXT("blockedByPie carries its own wire spelling"),
            PieState, FString(TEXT("blockedByPie")));

        // 3. It is a hard environmental refusal, not work queued for a flush.
        bool bPendingFlush = true;
        TestTrue(TEXT("blockedByPie explicitly reports pendingFlush"),
            PieResult->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush));
        TestFalse(TEXT("blockedByPie is not pending flush work"), bPendingFlush);

        // 4. The detail must say what unblocks it, and must not point at the remedies that
        //    cannot work. "force" appears only inside the sentence ruling it out.
        const FString PieDetail = AssetSaveStateTest_Str(PieResult, TEXT("saveDetail"));
        TestTrue(*FString::Printf(TEXT("blockedByPie names play mode as the cause: '%s'"), *PieDetail),
            PieDetail.Contains(TEXT("play mode")));
        TestTrue(*FString::Printf(TEXT("blockedByPie rules out retrying: '%s'"), *PieDetail),
            PieDetail.Contains(TEXT("Retrying cannot help")));
        TestTrue(*FString::Printf(TEXT("blockedByPie names the probe for the blocking session: '%s'"), *PieDetail),
            PieDetail.Contains(TEXT("editor.pie_status")));
    }

    // The durability predicate is the single definition `saved` is derived from.
    TestTrue(TEXT("written is durable"), IsAssetSaveStateDurable(EAssetSaveState::Written));
    TestTrue(TEXT("alreadyCurrent is durable"), IsAssetSaveStateDurable(EAssetSaveState::AlreadyCurrent));
    TestFalse(TEXT("deferred is not durable"), IsAssetSaveStateDurable(EAssetSaveState::Deferred));
    TestFalse(TEXT("failed is not durable"), IsAssetSaveStateDurable(EAssetSaveState::Failed));
    TestFalse(TEXT("blockedByPie is not durable"), IsAssetSaveStateDurable(EAssetSaveState::BlockedByPie));
    TestFalse(TEXT("notPersistable is not durable"), IsAssetSaveStateDurable(EAssetSaveState::NotPersistable));
    TestFalse(TEXT("notRequested is not durable"), IsAssetSaveStateDurable(EAssetSaveState::NotRequested));
    return true;
}

// The ~20 call sites that pass no state must keep emitting exactly the three fields they always
// did. A saveState that leaked in with a guessed value would be worse than none: it would be a
// claim nobody measured.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveReportWithoutStateIsUnchangedTest,
    "PinWright.assets.AssetSaveReport.OmittedStateEmitsTheLegacyShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSaveReportWithoutStateIsUnchangedTest::RunTest(const FString& /*Parameters*/)
{
    TSharedPtr<FJsonObject> Saved = MakeShared<FJsonObject>();
    AddAssetSaveReport(Saved, /*bSaveRequested=*/true, /*bSavedToDisk=*/true);
    TestTrue(TEXT("saved:true"), Saved->GetBoolField(TEXT("saved")));
    TestFalse(TEXT("no pendingFlush"), Saved->HasField(TEXT("pendingFlush")));
    TestFalse(TEXT("no saveState when none was supplied"), Saved->HasField(TEXT("saveState")));
    TestFalse(TEXT("no saveDetail when none was supplied"), Saved->HasField(TEXT("saveDetail")));
    TestEqual(TEXT("exactly the two legacy fields"), Saved->Values.Num(), 2);

    TSharedPtr<FJsonObject> Pending = MakeShared<FJsonObject>();
    AddAssetSaveReport(Pending, /*bSaveRequested=*/true, /*bSavedToDisk=*/false);
    TestFalse(TEXT("saved:false"), Pending->GetBoolField(TEXT("saved")));
    TestTrue(TEXT("pendingFlush:true"), Pending->GetBoolField(TEXT("pendingFlush")));
    TestFalse(TEXT("still no saveState"), Pending->HasField(TEXT("saveState")));
    TestEqual(TEXT("exactly the three legacy fields"), Pending->Values.Num(), 3);
    return true;
}

// ============================================================================
// The producer, against a real .uasset
// ============================================================================

// Drives SaveAssetToDiskReportingPresence through its real branches on a real package and
// asserts the reported state matches what is on disk at each step:
//
//   1. never-saved + dirty, forced      -> written,        .uasset appears
//   2. clean, inside the throttle window -> alreadyCurrent, .uasset unchanged, still durable
//   3. dirty, inside the throttle window -> deferred,       .uasset STILL THERE but stale
//   4. transient package                 -> notPersistable, nothing on disk, ever
//
// Step 3 is the case a bare existence probe cannot see - the file from step 1 satisfies it -
// and it is the one that must report a flushable state rather than a failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveStateMatchesDiskTest,
    "PinWright.assets.AssetSaveState.ReportedStateMatchesDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSaveStateMatchesDiskTest::RunTest(const FString& /*Parameters*/)
{
    const FString PackagePath = AssetSaveStateTest_UniquePackagePath();
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    const FString Filename = AssetSaveStateTest_PackageFilename(PackagePath);
    if (!TestFalse(TEXT("the test package path resolves to a filename"), Filename.IsEmpty()))
    {
        return false;
    }

    UMaterial* Material = AssetSaveStateTest_MakeMaterial(PackagePath);
    if (!TestNotNull(TEXT("probe material created"), Material))
    {
        return false;
    }
    UPackage* Package = Material->GetOutermost();

    // Widen the throttle for the duration so steps 2 and 3 are inside one window on any machine
    // rather than depending on how fast the preceding save ran. Restored on the way out because
    // the throttler is process-wide state.
    double& ThrottleSeconds = FPluginState::Get().SaveThrottle().ThrottleSecondsRef();
    const double PreviousThrottleSeconds = ThrottleSeconds;
    ThrottleSeconds = 30.0;
    ON_SCOPE_EXIT { ThrottleSeconds = PreviousThrottleSeconds; };

    IFileManager& Files = IFileManager::Get();
    TestTrue(TEXT("no .uasset on disk before the first save"), Files.FileSize(*Filename) < 0);

    // 1. The create-and-save path: dirty, never written, forced.
    {
        EAssetSaveState State = EAssetSaveState::NotRequested;
        FString OutPackageName;
        int64 OutSize = 0;
        const bool bDurable = SaveAssetToDiskReportingPresence(
            Material, /*bForce=*/true, &OutPackageName, &OutSize, &State);

        TestEqual(TEXT("a fresh forced save reports written"),
            FString(AssetSaveStateToWire(State)), FString(TEXT("written")));
        TestTrue(TEXT("written is durable"), bDurable);
        TestTrue(TEXT("the bool return is exactly the state's durability"),
            bDurable == IsAssetSaveStateDurable(State));
        // The disk is the arbiter, not the returned object.
        TestTrue(TEXT("the .uasset is on disk after a written report"), Files.FileSize(*Filename) > 0);
        TestTrue(TEXT("the reported size is the on-disk size"), OutSize == Files.FileSize(*Filename));
        TestEqual(TEXT("the reported package name is the asset's"), OutPackageName, PackagePath);
    }

    // A reported save clears the dirty flag. Asserted rather than assumed, because step 2's
    // expected state depends on it.
    if (!TestFalse(TEXT("the forced save left the package clean"), Package->IsDirty()))
    {
        return false;
    }

    const int64 SizeAfterFirstSave = Files.FileSize(*Filename);

    // 2. Nothing to write and the throttle window still open: durable, but not by writing.
    {
        EAssetSaveState State = EAssetSaveState::NotRequested;
        const bool bDurable = SaveAssetToDiskReportingPresence(
            Material, /*bForce=*/false, nullptr, nullptr, &State);

        TestEqual(TEXT("a clean throttled save reports alreadyCurrent"),
            FString(AssetSaveStateToWire(State)), FString(TEXT("alreadyCurrent")));
        TestTrue(TEXT("alreadyCurrent is still durable - the disk already matches memory"), bDurable);
        TestTrue(TEXT("the file did not change"), Files.FileSize(*Filename) == SizeAfterFirstSave);
    }

    // 3. The case an existence probe cannot see: dirty again, throttle still open. The .uasset
    //    from step 1 is right there, so "does a file exist" answers yes for an edit that is only
    //    in memory. The state must say deferred - flushable - and NOT durable.
    {
        Material->MarkPackageDirty();
        EAssetSaveState State = EAssetSaveState::NotRequested;
        const bool bDurable = SaveAssetToDiskReportingPresence(
            Material, /*bForce=*/false, nullptr, nullptr, &State);

        TestEqual(TEXT("a throttled save over a dirty package reports deferred"),
            FString(AssetSaveStateToWire(State)), FString(TEXT("deferred")));
        TestFalse(TEXT("deferred is not durable"), bDurable);
        TestTrue(TEXT("the stale .uasset is still on disk, which is why existence alone lies"),
            Files.FileSize(*Filename) > 0);
        TestTrue(TEXT("the package still holds the unsaved edit, so a flush is the remedy"),
            Package->IsDirty());
    }

    // Leave the fixture clean so teardown does not trip over a dirty package.
    SaveAssetToDiskReportingPresence(Material, /*bForce=*/true);
    return true;
}

// A transient-package object can never reach disk, so it must report notPersistable rather than
// the same not-durable false a flushable edit reports. Reverting the mapping folds this back
// into an unactionable saved:false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveStateTransientIsNotPersistableTest,
    "PinWright.assets.AssetSaveState.TransientPackageReportsNotPersistable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSaveStateTransientIsNotPersistableTest::RunTest(const FString& /*Parameters*/)
{
    UMaterial* Transient = NewObject<UMaterial>(
        GetTransientPackage(),
        FName(*FString::Printf(TEXT("M_Transient_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))),
        RF_Transient);
    if (!TestNotNull(TEXT("transient probe material created"), Transient))
    {
        return false;
    }

    EAssetSaveState State = EAssetSaveState::NotRequested;
    int64 OutSize = 0;
    const bool bDurable =
        SaveAssetToDiskReportingPresence(Transient, /*bForce=*/true, nullptr, &OutSize, &State);

    TestEqual(TEXT("a transient asset reports notPersistable"),
        FString(AssetSaveStateToWire(State)), FString(TEXT("notPersistable")));
    TestFalse(TEXT("notPersistable is never durable"), bDurable);
    TestTrue(TEXT("nothing was written, so no size is reported"), OutSize == 0);

    // And the wire report built from it does not read as a flushable pending edit.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, /*bSaveRequested=*/true, bDurable, State);
    TestNotEqual(TEXT("a transient asset is not reported as deferred"),
        AssetSaveStateTest_Str(Result, TEXT("saveState")), FString(TEXT("deferred")));
    return true;
}
