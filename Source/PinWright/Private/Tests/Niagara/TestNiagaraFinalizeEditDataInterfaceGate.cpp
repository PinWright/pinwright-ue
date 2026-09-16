// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-niagara-finalize-edit-no-di-gate.
//
// `niagara.add_emitter` and `niagara.remove_emitter` refuse to persist a UNiagaraSystem whose
// compiled data-interface count disagrees with its resolved one, because that system asserts
// inside the VectorVM (`DataSetIdx < ExecCtx->DataSets.Num()`) on a concurrent worker the next
// time anything ticks it - an appError, so the editor process dies minutes later from an
// unrelated caller. The other thirty mutating verbs in the namespace reach disk through
// NiagaraEdit::FinalizeNiagaraEdit, which had no such check: `niagara.add_data_interface` and
// `niagara.remove_data_interface` included, the two verbs whose entire job is to mutate data
// interfaces. They wrote the asset and answered `saved: true` with nothing said.
//
// What is asserted:
//   1. The gate predicate. Only `Mismatched` refuses; `Unverified` must NOT, or every write to a
//      system that has not resolved its data interfaces this session is blocked. The
//      false-positive direction is pinned first because it is the one that breaks working
//      callers rather than the one that lets a bad write through.
//   2. The shared mutation envelope carries `dataInterfaceCheck`, and it is the verdict
//      `CheckDataInterfaceCounts` gives for the same object. Counterfactual: before the fix the
//      field did not exist at all, so this is the assertion that fails on a revert.
//   3. FinalizeNiagaraEdit is where the measurement happens - on the target, at the point the
//      save is decided - not in the envelope builder after the fact.
//   4. The refusal itself: a target carrying a `Mismatched` verdict is NOT written to disk, and
//      the same target with the same asset IS written once the verdict is not a mismatch. The
//      positive control is what makes the refusal mean something rather than proving the fixture
//      was unsavable.
//
// WHY THE REFUSAL IS DRIVEN THROUGH A SEEDED VERDICT AND NOT A CORRUPT SYSTEM. Manufacturing a
// genuinely mismatched UNiagaraSystem is not portable across hosts (parent ticket
// B-niagara-di-count-mismatch-vectorvm-assert-kills-editor, `#5`/`#6`) and would arm the very
// editor-killing assert this gate exists to prevent - a test that takes down its own host
// measures nothing. So case 4 hands FinalizeNiagaraEdit a target with no `System`, which is the
// one shape where it does not re-measure the verdict, and a plain savable asset in a mounted
// package standing in for the write target. The Niagara fixtures cannot serve here: their
// packages are RF_Transient on purpose (see NiagaraEditTestUtils), so every save of one reports
// `notPersistable` and a refusal would be indistinguishable from an asset that could never be
// written.
#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

// Prefixed and file-static: Unity merges this TU with its neighbours, so an unprefixed helper
// collides (see CLAUDE.md > Building).
static FString NiagaraDIGateTest_UniquePackagePath()
{
    return FString::Printf(TEXT("/Game/PinWrightTests/NiagaraDIGate/DA_DIGate_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

// A dirty, never-written asset in a mounted package - the shape a mutation verb hands to the
// save. Deliberately a UDataTable and not a Niagara asset: this stands in only for "something
// FinalizeNiagaraEdit can actually persist", and a Niagara system in a non-transient dirty
// package invites the autosaver into FNiagaraCompilationGraphDigested::Digest.
static UDataTable* NiagaraDIGateTest_MakeSavableAsset(const FString& PackagePath)
{
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }
    UDataTable* Asset = NewObject<UDataTable>(
        Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
        RF_Public | RF_Standalone);
    if (Asset)
    {
        Asset->RowStruct = FTableRowBase::StaticStruct();
        Asset->MarkPackageDirty();
        // The production save path refuses an asset the registry has never heard of, so a fixture
        // that skips this would exercise a failure branch instead of the one under test.
        FAssetRegistryModule::AssetCreated(Asset);
    }
    return Asset;
}

static FString NiagaraDIGateTest_PackageFilename(const FString& PackagePath)
{
    FString Filename;
    return FPackageName::TryConvertLongPackageNameToFilename(
               PackagePath, Filename, FPackageName::GetAssetPackageExtension())
        ? Filename
        : FString();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraFinalizeEditDataInterfaceGateTest,
    "PinWright.niagara.finalize_edit.DataInterfaceGateOnEveryMutationVerb",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraFinalizeEditDataInterfaceGateTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;

    // ------------------------------------------------------------------
    // 1. The gate predicate.
    // ------------------------------------------------------------------
    TestTrue(TEXT("a consistent system may be persisted"),
        NiagaraEdit::MayPersistAfterDataInterfaceCheck(EDataInterfaceConsistency::Consistent));
    TestTrue(TEXT("an unverified system may be persisted - 'could not look' is not 'corrupt'"),
        NiagaraEdit::MayPersistAfterDataInterfaceCheck(EDataInterfaceConsistency::Unverified));
    TestFalse(TEXT("a mismatched system must not be persisted"),
        NiagaraEdit::MayPersistAfterDataInterfaceCheck(EDataInterfaceConsistency::Mismatched));

    // ------------------------------------------------------------------
    // 2 & 3. The envelope carries the verdict, and FinalizeNiagaraEdit is what measured it.
    //
    // niagara.add_data_interface is the verb the ticket names: it mutates a system's data
    // interfaces and reported nothing about their consistency. compile:false / save:false keeps
    // this a response-shape assertion - compiling these synthetic systems is what
    // NiagaraEditTestUtils warns crashes inside FNiagaraCompilationGraphDigested::Digest.
    // ------------------------------------------------------------------
    TestTrue(TEXT("niagara.add_data_interface is registered"),
        IsHandlerRegistered(TEXT("niagara.add_data_interface")));

    FString SystemPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemPath);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    if (System)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SystemPath);
        Payload->SetStringField(TEXT("scope"), TEXT("user"));
        Payload->SetStringField(TEXT("parameterName"), TEXT("DIGateProbe"));
        Payload->SetStringField(TEXT("dataInterfaceClass"), TEXT("/Script/Niagara.NiagaraDataInterfaceCurve"));
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_data_interface"), Payload, Capture))
        {
            FString Reported;
            const bool bHasVerdict = Capture.Result->TryGetStringField(TEXT("dataInterfaceCheck"), Reported);
            TestTrue(TEXT("the mutation envelope carries dataInterfaceCheck"), bHasVerdict);

            // The verdict must be the checker's, not a constant that happens to look like one.
            // Nothing mutates the system between the handler's measurement and this one.
            TArray<FDataInterfaceCountMismatch> Mismatches;
            const EDataInterfaceConsistency Measured = CheckDataInterfaceCounts(*System, Mismatches);
            TestEqual(TEXT("the reported verdict is the checker's verdict for the same object"),
                Reported, FString(DataInterfaceConsistencyToString(Measured)));

            bool bSaved = true;
            Capture.Result->TryGetBoolField(TEXT("saved"), bSaved);
            if (Reported == TEXT("mismatched"))
            {
                TestFalse(TEXT("a mismatched system is never reported as saved"), bSaved);
                TestTrue(TEXT("a mismatched verdict names the offending scripts"),
                    Capture.Result->HasField(TEXT("mismatchedScripts")));
            }
            else
            {
                // The list is the mismatch's evidence; publishing an empty one beside a pass
                // would make the two states read alike.
                TestFalse(TEXT("no mismatch list on a verdict that is not a mismatch"),
                    Capture.Result->HasField(TEXT("mismatchedScripts")));
            }

            // 3. The measurement lands on the target inside FinalizeNiagaraEdit, which is what
            //    lets the save decision and the response draw on the same fact.
            FNiagaraResolvedTarget Target;
            Target.Asset = System;
            Target.System = System;
            Target.AssetPath = SystemPath;
            Target.AssetKind = TEXT("NiagaraSystem");
            TestTrue(TEXT("a freshly resolved target starts unverified"),
                Target.DataInterfaceVerdict == EDataInterfaceConsistency::Unverified);

            bool bCompiled = false;
            bool bFinalizeSaved = false;
            NiagaraEdit::FinalizeNiagaraEdit(Target, FNiagaraEditOptions{false, false}, bCompiled, bFinalizeSaved);
            TestTrue(TEXT("finalize records the checker's verdict on the target"),
                Target.DataInterfaceVerdict == Measured);
            TestEqual(TEXT("finalize records the checker's mismatch list on the target"),
                DescribeDataInterfaceMismatches(Target.DataInterfaceMismatches),
                DescribeDataInterfaceMismatches(Mismatches));
        }
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            TEXT("NewTransientSystem returned null; the envelope and finalize assertions were not exercised."));
    }

    // ------------------------------------------------------------------
    // 4. The refusal, against a real write.
    //
    // Counterfactual: before the fix the write ran on
    // `Options.bSave && Target.Asset && bCompileIsPersistable` alone, so the mismatched case
    // below put the .uasset on disk and reported saved:true.
    // ------------------------------------------------------------------
    {
        const FString PackagePath = NiagaraDIGateTest_UniquePackagePath();
        ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

        const FString Filename = NiagaraDIGateTest_PackageFilename(PackagePath);
        UDataTable* SaveProbe = Filename.IsEmpty() ? nullptr : NiagaraDIGateTest_MakeSavableAsset(PackagePath);
        if (SaveProbe)
        {
            IFileManager& Files = IFileManager::Get();
            TestTrue(TEXT("nothing on disk before either save"), Files.FileSize(*Filename) < 0);

            FNiagaraResolvedTarget Refused;
            Refused.Asset = SaveProbe;
            Refused.AssetPath = PackagePath;
            Refused.DataInterfaceVerdict = EDataInterfaceConsistency::Mismatched;

            bool bCompiled = false;
            bool bSaved = false;
            NiagaraEdit::FinalizeNiagaraEdit(Refused, FNiagaraEditOptions{false, true}, bCompiled, bSaved);
            TestFalse(TEXT("a mismatched verdict refuses the save"), bSaved);
            TestTrue(TEXT("a refused save wrote no .uasset"), Files.FileSize(*Filename) < 0);
            TestTrue(TEXT("the refusal did not silently clear the verdict"),
                Refused.DataInterfaceVerdict == EDataInterfaceConsistency::Mismatched);

            // Positive control on the SAME asset: the refusal above was the gate, not a fixture
            // that could never have been written.
            FNiagaraResolvedTarget Allowed;
            Allowed.Asset = SaveProbe;
            Allowed.AssetPath = PackagePath;
            Allowed.DataInterfaceVerdict = EDataInterfaceConsistency::Consistent;

            bool bControlCompiled = false;
            bool bControlSaved = false;
            NiagaraEdit::FinalizeNiagaraEdit(Allowed, FNiagaraEditOptions{false, true}, bControlCompiled, bControlSaved);
            TestTrue(TEXT("the same asset saves once the verdict is not a mismatch"), bControlSaved);
            TestTrue(TEXT("the permitted save wrote the .uasset"), Files.FileSize(*Filename) > 0);
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-di-gate-save-probe-unavailable"),
                TEXT("could not create the savable probe asset; the save refusal was not exercised."));
        }
    }

    return true;
}
