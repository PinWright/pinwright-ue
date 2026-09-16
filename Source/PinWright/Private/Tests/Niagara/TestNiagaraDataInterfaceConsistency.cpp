// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-niagara-di-count-mismatch-vectorvm-assert-kills-editor.
//
// A UNiagaraSystem left with a compiled data-interface count that differs from its resolved one
// asserts inside the VectorVM (`DataSetIdx < ExecCtx->DataSets.Num()`) on a concurrent worker the
// next time anything ticks it. That is an appError, so it takes the whole editor process down --
// which is why this test asserts the GUARD and never the crash: reproducing the assert would kill
// the automation suite's own host.
//
// What is asserted:
//   1. A system that has never resolved its data interfaces reports Unverified, not Mismatched.
//      A checker that turned "nothing to compare" into a refusal would block every legitimate
//      write, so the false-positive direction is pinned first.
//   2. On a real, saved fixture system the invariant holds: compiled count == resolved count for
//      every script that has a resolved set.
//   3. niagara.add_emitter's response carries the post-write verdict. Before the fix the write
//      path returned a bare success that said nothing about the invariant, so there was no field
//      here to read and no way for a caller to learn the asset had just become fatal-on-tick.
//   4. The refusal's reporting path is well formed: the message names each offending script and
//      both counts. Exercised on a synthetic mismatch list, because obtaining a genuinely corrupt
//      system would arm the very trap this ticket is about.
#include "Misc/AutomationTest.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// The resolved data-interface set niagara.audit_level measures lives in
// FNiagaraScriptRuntimeCompiledData. Same guard NiagaraDataInterfaceConsistency.cpp uses: on an
// engine build without that header there is nothing to seed, and the level-scope cases at the
// bottom of this file report a skip instead of asserting on an unreachable state.
#if __has_include("NiagaraScriptRuntimeCompiledData.h")
#include "NiagaraScriptRuntimeCompiledData.h"
#define PINWRIGHT_TEST_HAS_NIAGARA_RESOLVED_DI 1
#else
#define PINWRIGHT_TEST_HAS_NIAGARA_RESOLVED_DI 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraDataInterfaceConsistencyTest,
    "PinWright.niagara.data_interface_consistency.WritePathReportsVerdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDataInterfaceConsistencyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;

    // ------------------------------------------------------------------
    // 0. Wire spellings the handlers echo.
    // ------------------------------------------------------------------
    TestEqual(TEXT("consistent spelling"),
        FString(DataInterfaceConsistencyToString(EDataInterfaceConsistency::Consistent)), FString(TEXT("consistent")));
    TestEqual(TEXT("mismatched spelling"),
        FString(DataInterfaceConsistencyToString(EDataInterfaceConsistency::Mismatched)), FString(TEXT("mismatched")));
    TestEqual(TEXT("unverified spelling"),
        FString(DataInterfaceConsistencyToString(EDataInterfaceConsistency::Unverified)), FString(TEXT("unverified")));

    // ------------------------------------------------------------------
    // 1. A system with no resolved data interfaces must never be reported as corrupt.
    //    A bare NewObject<UNiagaraSystem> has never been through InitScriptCompiledData, so
    //    there is nothing to compare and the only honest verdict is Unverified.
    // ------------------------------------------------------------------
    if (UNiagaraSystem* BareSystem = NewObject<UNiagaraSystem>(GetTransientPackage()))
    {
        BareSystem->AddToRoot();
        TArray<FDataInterfaceCountMismatch> Mismatches;
        const EDataInterfaceConsistency Verdict = CheckDataInterfaceCounts(*BareSystem, Mismatches);
        TestTrue(TEXT("never-resolved system is not reported as mismatched"),
            Verdict != EDataInterfaceConsistency::Mismatched);
        TestEqual(TEXT("never-resolved system yields no mismatch entries"), Mismatches.Num(), 0);
        BareSystem->RemoveFromRoot();
    }
    else
    {
        AddWarning(TEXT("Could not construct a transient UNiagaraSystem; skipping the never-resolved case."));
    }

    // ------------------------------------------------------------------
    // 2. The invariant on a real, saved fixture system.
    // ------------------------------------------------------------------
    if (UNiagaraSystem* Fixture = LoadObject<UNiagaraSystem>(nullptr, NiagaraEditTestUtils::FixtureSystemAssetPath))
    {
        TArray<FDataInterfaceCountMismatch> Mismatches;
        const EDataInterfaceConsistency Verdict = CheckDataInterfaceCounts(*Fixture, Mismatches);
        if (Verdict == EDataInterfaceConsistency::Unverified)
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-unavailable"),
                FString::Printf(TEXT("no resolved data-interface set on '%s' in this host/engine build"),
                    NiagaraEditTestUtils::FixtureSystemAssetPath));
        }
        else
        {
            TestEqual(TEXT("fixture system compiled/resolved data-interface counts agree"),
                DescribeDataInterfaceMismatches(Mismatches), FString());
            TestTrue(TEXT("fixture system verdict is consistent"),
                Verdict == EDataInterfaceConsistency::Consistent);
        }
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            FString::Printf(TEXT("could not load '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
    }

    // ------------------------------------------------------------------
    // 3. niagara.add_emitter reports the post-write verdict.
    //    Counterfactual: before the fix the response had no dataInterfaceCheck field at all, so
    //    a caller could not tell a verified write from one that had just armed a delayed crash.
    // ------------------------------------------------------------------
    TestTrue(TEXT("niagara.add_emitter is registered"), IsHandlerRegistered(TEXT("niagara.add_emitter")));

    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName SeedEmitterName;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bAuthorable = NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, SeedEmitterName);
    // Assign unconditionally: a partial failure still leaves whatever it managed to build rooted.
    Roots.System = System;
    Roots.Emitter = SourceEmitter;
    if (bAuthorable)
    {
        FString AddedEmitterPath;
        UNiagaraEmitter* AddedEmitter = NiagaraEditTestUtils::NewTransientEmitter(AddedEmitterPath);
        if (AddedEmitter)
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("systemPath"), SystemPath);
            Payload->SetStringField(TEXT("emitterPath"), AddedEmitterPath);
            Payload->SetStringField(TEXT("name"), TEXT("DICheckEmitter"));
            // compile:false / save:false keeps this a pure response-shape assertion. Compiling
            // these synthetic-then-mutated systems is what the fixture header warns crashes
            // inside FNiagaraCompilationGraphDigested::Digest.
            Payload->SetBoolField(TEXT("compile"), false);
            Payload->SetBoolField(TEXT("save"), false);

            FTestResponseCapture Capture;
            if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_emitter"), Payload, Capture))
            {
                FString Verdict;
                const bool bHasVerdict = Capture.Result->TryGetStringField(TEXT("dataInterfaceCheck"), Verdict);
                TestTrue(TEXT("add_emitter response carries the data-interface verdict"), bHasVerdict);
                TestTrue(TEXT("verdict is one of the three wire spellings"),
                    Verdict == TEXT("consistent") || Verdict == TEXT("unverified") || Verdict == TEXT("mismatched"));
                // A write that succeeds must never have been let through on a mismatched system.
                TestNotEqual(TEXT("a successful add_emitter never reports mismatched"),
                    Verdict, FString(TEXT("mismatched")));
            }

            AddedEmitter->RemoveFromRoot();
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-emitter-unavailable"),
                TEXT("NewTransientEmitter returned null; add_emitter response shape not asserted"));
        }
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-authorable-system-unavailable"),
            TEXT("MakeAuthorableSystem failed; add_emitter response shape not asserted"));
    }

    // ------------------------------------------------------------------
    // 4. The refusal message names every offending script and both counts. This is the only
    //    channel the caller gets when the guard fires, so its content is asserted directly
    //    rather than by producing a genuinely fatal system.
    // ------------------------------------------------------------------
    {
        TArray<FDataInterfaceCountMismatch> Synthetic;
        FDataInterfaceCountMismatch& First = Synthetic.AddDefaulted_GetRef();
        First.ScriptPath = TEXT("/Memory/Test.NS_Probe:Emitter0.UpdateScript");
        First.EmitterName = TEXT("Emitter0");
        First.CompiledCount = 0;
        First.ResolvedCount = 2;
        FDataInterfaceCountMismatch& Second = Synthetic.AddDefaulted_GetRef();
        Second.ScriptPath = TEXT("/Memory/Test.NS_Probe:Emitter1.SpawnScript");
        Second.EmitterName = TEXT("Emitter1");
        Second.CompiledCount = 3;
        Second.ResolvedCount = 1;

        const FString Described = DescribeDataInterfaceMismatches(Synthetic);
        TestTrue(TEXT("description names the first script"), Described.Contains(First.ScriptPath));
        TestTrue(TEXT("description names the second script"), Described.Contains(Second.ScriptPath));
        TestTrue(TEXT("description carries the first script's counts"), Described.Contains(TEXT("compiled 0, resolved 2")));
        TestTrue(TEXT("description carries the second script's counts"), Described.Contains(TEXT("compiled 3, resolved 1")));
        TestEqual(TEXT("empty mismatch list describes as an empty string"),
            DescribeDataInterfaceMismatches(TArray<FDataInterfaceCountMismatch>()), FString());
    }

    return true;
}

// ------------------------------------------------------------------------------------------------
// F-niagara-remove-orphan-data-interfaces: the recovery half.
//
// NIAGARA_DATA_INTERFACE_MISMATCH names both counts and no way to act on them. These two verbs are
// the way out: one enumerates the resolved entries with no compiled counterpart, one removes them.
//
// What is asserted, and why each one is the direction that can actually go wrong:
//   1. Both verbs are registered, and the outcome enum's wire spellings are pinned. They are the
//      only channel a caller has for telling a refusal from a no-op from a repair.
//   2. On a system that has never resolved its data interfaces, removal reports Unverified and
//      removes NOTHING. A repair verb that claimed work on an asset it could not read would be
//      worse than one that refuses, and "nothing to remove" is indistinguishable from "could not
//      look" unless this direction is pinned.
//   3. The enumeration's verdict never contradicts CheckDataInterfaceCounts on the same asset.
//      Two verbs disagreeing about whether one system is sound is the failure that would send a
//      caller in circles.
//   4. The outcome and the report agree: a refusal removed nothing and named the blockers; a
//      Removed outcome re-measures Consistent, never Mismatched. The verdict is measured AFTER the
//      write, so it cannot be an echo of the plan.
//   5. Removal is idempotent - a repeat of a landed repair converges to nothing_to_remove rather
//      than erroring or removing again.
//   6. The read verb's response shape, including orphanCount agreeing with orphans[] rather than
//      being reported independently of it.
//
// Everything mutating runs on a TRANSIENT duplicate, never on the shipped fixture asset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraOrphanDataInterfacesTest,
    "PinWright.niagara.orphan_data_interfaces.EnumerateAndRemoveContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraOrphanDataInterfacesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;

    // ------------------------------------------------------------------
    // 1. Registration and the wire vocabulary.
    // ------------------------------------------------------------------
    TestTrue(TEXT("niagara.list_orphan_data_interfaces is registered"),
        IsHandlerRegistered(TEXT("niagara.list_orphan_data_interfaces")));
    TestTrue(TEXT("niagara.remove_orphan_data_interfaces is registered"),
        IsHandlerRegistered(TEXT("niagara.remove_orphan_data_interfaces")));
    // Distinct from the pre-existing parameter-store verb, which targets a different set entirely.
    TestTrue(TEXT("niagara.remove_data_interface is still registered separately"),
        IsHandlerRegistered(TEXT("niagara.remove_data_interface")));

    TestEqual(TEXT("unverified outcome spelling"),
        FString(OrphanRemovalOutcomeToString(EOrphanRemovalOutcome::Unverified)), FString(TEXT("unverified")));
    TestEqual(TEXT("nothing_to_remove outcome spelling"),
        FString(OrphanRemovalOutcomeToString(EOrphanRemovalOutcome::NothingToRemove)), FString(TEXT("nothing_to_remove")));
    TestEqual(TEXT("removed outcome spelling"),
        FString(OrphanRemovalOutcomeToString(EOrphanRemovalOutcome::Removed)), FString(TEXT("removed")));
    TestEqual(TEXT("refused_irreconcilable outcome spelling"),
        FString(OrphanRemovalOutcomeToString(EOrphanRemovalOutcome::RefusedIrreconcilable)), FString(TEXT("refused_irreconcilable")));

    // ------------------------------------------------------------------
    // 2. A system that has never resolved anything: report Unverified, mutate nothing.
    // ------------------------------------------------------------------
    if (UNiagaraSystem* BareSystem = NewObject<UNiagaraSystem>(GetTransientPackage()))
    {
        BareSystem->AddToRoot();

        TArray<FOrphanResolvedDataInterface> Orphans;
        const EDataInterfaceConsistency Verdict = FindOrphanResolvedDataInterfaces(*BareSystem, Orphans);
        TestTrue(TEXT("never-resolved system is not reported as mismatched by the orphan walk"),
            Verdict != EDataInterfaceConsistency::Mismatched);
        TestEqual(TEXT("never-resolved system yields no orphans"), Orphans.Num(), 0);

        FOrphanRemovalReport Report;
        const EOrphanRemovalOutcome Outcome = RemoveOrphanResolvedDataInterfaces(*BareSystem, Report);
        TestTrue(TEXT("never-resolved system is never reported as repaired"),
            Outcome != EOrphanRemovalOutcome::Removed);
        TestEqual(TEXT("never-resolved system had nothing removed"), Report.Removed.Num(), 0);
        TestEqual(TEXT("never-resolved system had no script touched"), Report.ScriptsTouched, 0);
        TestEqual(TEXT("never-resolved system reports no durable prune"), Report.ScriptsPrunedDurably, 0);

        BareSystem->RemoveFromRoot();
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-transient-system-unavailable"),
            TEXT("could not construct a transient UNiagaraSystem; the never-resolved direction was not asserted"));
    }

    // ------------------------------------------------------------------
    // 3-5. The two verdicts agree, and the removal contract holds, on a transient duplicate.
    // ------------------------------------------------------------------
    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName SeedEmitterName;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bAuthorable = NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, SeedEmitterName);
    Roots.System = System;
    Roots.Emitter = SourceEmitter;

    if (bAuthorable && System)
    {
        TArray<FDataInterfaceCountMismatch> Mismatches;
        const EDataInterfaceConsistency CountVerdict = CheckDataInterfaceCounts(*System, Mismatches);

        TArray<FOrphanResolvedDataInterface> Orphans;
        const EDataInterfaceConsistency OrphanVerdict = FindOrphanResolvedDataInterfaces(*System, Orphans);
        // The orphan walk reports the COUNT verdict, deliberately: two verbs disagreeing about
        // whether one asset is sound is what sends a caller in circles.
        TestEqual(TEXT("orphan walk and count check return the same verdict"),
            FString(DataInterfaceConsistencyToString(OrphanVerdict)),
            FString(DataInterfaceConsistencyToString(CountVerdict)));

        // Every reported orphan must identify a real position in a real script - an entry the
        // removal pass could actually address.
        for (const FOrphanResolvedDataInterface& Orphan : Orphans)
        {
            TestTrue(TEXT("orphan names its script"), !Orphan.ScriptPath.IsEmpty());
            TestTrue(TEXT("orphan carries an addressable resolved index"), Orphan.ResolvedIndex >= 0);
        }

        FOrphanRemovalReport Report;
        const EOrphanRemovalOutcome Outcome = RemoveOrphanResolvedDataInterfaces(*System, Report);
        switch (Outcome)
        {
        case EOrphanRemovalOutcome::Removed:
            TestTrue(TEXT("a repair that reports Removed removed something"), Report.Removed.Num() > 0);
            TestTrue(TEXT("a repair that reports Removed touched at least one script"), Report.ScriptsTouched > 0);
            TestTrue(TEXT("scriptsPrunedDurably never exceeds scriptsTouched"),
                Report.ScriptsPrunedDurably <= Report.ScriptsTouched);
            // Measured after the write. A repair that left the system fatal-on-tick must never
            // report success, which is the whole reason VerdictAfter is re-measured rather than
            // inferred from the plan.
            TestTrue(TEXT("a landed repair never leaves the system mismatched"),
                Report.VerdictAfter != EDataInterfaceConsistency::Mismatched);
            {
                // Idempotency: the transport retries on a response-only timeout, so a repeat must
                // converge rather than remove again.
                FOrphanRemovalReport Second;
                const EOrphanRemovalOutcome Repeat = RemoveOrphanResolvedDataInterfaces(*System, Second);
                TestTrue(TEXT("repeating a landed repair does not remove again"),
                    Repeat != EOrphanRemovalOutcome::Removed);
                TestEqual(TEXT("repeating a landed repair removes nothing"), Second.Removed.Num(), 0);
            }
            break;

        case EOrphanRemovalOutcome::RefusedIrreconcilable:
            TestTrue(TEXT("a refusal names the scripts that blocked it"), Report.Irreconcilable.Num() > 0);
            TestEqual(TEXT("a refusal removed nothing"), Report.Removed.Num(), 0);
            TestEqual(TEXT("a refusal touched no script"), Report.ScriptsTouched, 0);
            break;

        case EOrphanRemovalOutcome::NothingToRemove:
            TestEqual(TEXT("nothing_to_remove removed nothing"), Report.Removed.Num(), 0);
            TestEqual(TEXT("nothing_to_remove touched no script"), Report.ScriptsTouched, 0);
            TestEqual(TEXT("nothing_to_remove names no blocked script"), Report.Irreconcilable.Num(), 0);
            break;

        default:
            TestEqual(TEXT("unverified removed nothing"), Report.Removed.Num(), 0);
            TestEqual(TEXT("unverified touched no script"), Report.ScriptsTouched, 0);
            break;
        }

        // ------------------------------------------------------------------
        // 6. The read verb's response shape.
        // ------------------------------------------------------------------
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("systemPath"), SystemPath);

            FTestResponseCapture Capture;
            if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.list_orphan_data_interfaces"), Payload, Capture))
            {
                FString ReportedVerdict;
                TestTrue(TEXT("list response carries the data-interface verdict"),
                    Capture.Result->TryGetStringField(TEXT("dataInterfaceCheck"), ReportedVerdict));
                TestTrue(TEXT("verdict is one of the three wire spellings"),
                    ReportedVerdict == TEXT("consistent") || ReportedVerdict == TEXT("unverified")
                        || ReportedVerdict == TEXT("mismatched"));

                const TArray<TSharedPtr<FJsonValue>>* ListedOrphans = nullptr;
                const bool bHasOrphans = Capture.Result->TryGetArrayField(TEXT("orphans"), ListedOrphans);
                TestTrue(TEXT("list response always carries an orphans array"), bHasOrphans);

                double ReportedCount = -1.0;
                TestTrue(TEXT("list response carries orphanCount"),
                    Capture.Result->TryGetNumberField(TEXT("orphanCount"), ReportedCount));
                if (bHasOrphans && ListedOrphans)
                {
                    // The count is derived from the array, not reported beside it - a count that
                    // could disagree with its own list is a field that cannot fail.
                    TestEqual(TEXT("orphanCount matches the orphans array length"),
                        static_cast<int32>(ReportedCount), ListedOrphans->Num());
                }

                TestTrue(TEXT("list response carries mismatchedScripts for correlation"),
                    Capture.Result->HasField(TEXT("mismatchedScripts")));
            }
        }
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-authorable-system-unavailable"),
            TEXT("MakeAuthorableSystem failed; the removal contract and the list response shape were not asserted"));
    }

    // ------------------------------------------------------------------
    // 7. Failure direction: an asset path that resolves to nothing is an error, never an empty
    //    success. A missing system reported as "zero orphans" would read as a clean bill of health.
    // ------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemPath"), TEXT("/Game/PinWrightTests/NS_DoesNotExist.NS_DoesNotExist"));
        NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.list_orphan_data_interfaces"),
            Payload, TEXT("ASSET_NOT_FOUND"));
    }

    return true;
}

// ------------------------------------------------------------------------------------------------
// E-niagara-validate-no-data-interface-check: the READ half.
//
// niagara.validate never called CheckDataInterfaceCounts, so a system already carrying the
// resolved-vs-compiled mismatch came back `valid: true` with an issue list that said nothing about
// it -- and then took the editor down on its next tick. Validate's verdict is exactly where an
// author looks to ask whether an asset is sound, so the check now runs there too.
//
// What is asserted, and why each is the direction that can actually go wrong:
//   1. The response carries `dataInterfaceCheck` at all. That field did not exist before, which is
//      the counterfactual: there was no way to learn whether the check had run.
//   2. The published verdict AGREES with CheckDataInterfaceCounts run against the same object. A
//      constant would satisfy (1) and mean nothing.
//   3. Each verdict has exactly one legal reporting shape, read from the top-level errors /
//      warnings arrays rather than from the issue's own severity field, because `valid` is derived
//      from `errors`. Mismatched is an ERROR -- at `basic` as well as `strict`, so the loop below
//      runs both levels. Unverified is a WARNING: an empty mismatch list on an unverified system
//      is not evidence there are none, so silence would read as a pass, while an error there would
//      fail every system nothing has compiled this session.
//   4. The verdict is never fabricated for an asset kind the check cannot cover.
//
// As with the tests above, the Mismatched branch is not manufactured: producing a genuinely
// corrupt system would arm the appError that kills the automation host. The switch is exhaustive,
// so whichever verdict the host lands on is asserted rather than stepped over.
namespace NiagaraValidateDiIssueProbe
{
    // Which top-level array validate filed a code under: "error", "warning", or empty when the
    // code is absent from both. Those arrays are what a caller branches on and what `valid` is
    // computed from, so they -- not the issue object's own severity string -- are the contract.
    static FString SeverityOf(const TSharedPtr<FJsonObject>& Result, const TCHAR* Code)
    {
        if (!Result.IsValid())
        {
            return FString();
        }

        for (const TCHAR* ArrayName : { TEXT("errors"), TEXT("warnings") })
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!Result->TryGetArrayField(ArrayName, Values) || !Values)
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                FString IssueCode;
                if (Value.IsValid() && Value->Type == EJson::Object
                    && Value->AsObject()->TryGetStringField(TEXT("code"), IssueCode)
                    && IssueCode.Equals(Code, ESearchCase::IgnoreCase))
                {
                    return FString(ArrayName).Equals(TEXT("errors")) ? TEXT("error") : TEXT("warning");
                }
            }
        }
        return FString();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraValidateDataInterfaceConsistencyTest,
    "PinWright.niagara.validate.DataInterfaceConsistencyIsChecked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateDataInterfaceConsistencyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;

    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName SeedEmitterName;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bAuthorable = NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, SeedEmitterName);
    // Assign unconditionally: a partial failure still leaves whatever it managed to build rooted.
    Roots.System = System;
    Roots.Emitter = SourceEmitter;

    if (!bAuthorable || !System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-authorable-system-unavailable"),
            TEXT("MakeAuthorableSystem failed; validate's data-interface reporting was not asserted"));
        return true;
    }

    // The verdict for this exact object, read straight from the checker. Everything below is
    // measured against it rather than against a hard-coded expectation, so the test asserts the
    // same contract on a host whose engine build exposes no resolved set as on one that does.
    TArray<FDataInterfaceCountMismatch> Mismatches;
    const EDataInterfaceConsistency Expected = CheckDataInterfaceCounts(*System, Mismatches);
    const FString ExpectedSpelling(DataInterfaceConsistencyToString(Expected));

    // Both levels, because the mismatch is an error at every level rather than one of the three
    // codes `level: strict` escalates: a system that cannot be ticked is not acceptable under any
    // reading of the asset.
    for (const TCHAR* Level : { TEXT("basic"), TEXT("strict") })
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemPath"), SystemPath);
        Payload->SetStringField(TEXT("level"), Level);

        FTestResponseCapture Capture;
        if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.validate"), Payload, Capture))
        {
            continue;
        }

        FString Reported;
        TestTrue(FString::Printf(TEXT("validate(%s) carries the data-interface verdict"), Level),
            Capture.Result->TryGetStringField(TEXT("dataInterfaceCheck"), Reported));
        TestEqual(FString::Printf(TEXT("validate(%s) verdict matches CheckDataInterfaceCounts"), Level),
            Reported, ExpectedSpelling);

        const FString MismatchSeverity =
            NiagaraValidateDiIssueProbe::SeverityOf(Capture.Result, TEXT("NIAGARA_DATA_INTERFACE_MISMATCH"));
        const FString UnverifiedSeverity =
            NiagaraValidateDiIssueProbe::SeverityOf(Capture.Result, TEXT("NIAGARA_DATA_INTERFACE_UNVERIFIED"));

        switch (Expected)
        {
        case EDataInterfaceConsistency::Mismatched:
            // The ticket's whole point. A warning here would leave `valid` true on a system that
            // kills the editor process the next time anything ticks it.
            TestEqual(FString::Printf(TEXT("validate(%s) reports a data-interface mismatch as an error"), Level),
                MismatchSeverity, FString(TEXT("error")));
            TestFalse(FString::Printf(TEXT("validate(%s) is not valid on a mismatched system"), Level),
                Capture.Result->GetBoolField(TEXT("valid")));
            break;

        case EDataInterfaceConsistency::Unverified:
            TestEqual(FString::Printf(TEXT("validate(%s) reports an unverified check as a warning"), Level),
                UnverifiedSeverity, FString(TEXT("warning")));
            // Never the other way round: a check that could not run must not name a mismatch.
            TestEqual(FString::Printf(TEXT("validate(%s) claims no mismatch it could not measure"), Level),
                MismatchSeverity, FString());
            break;

        default:
            TestEqual(FString::Printf(TEXT("validate(%s) raises no data-interface issue on a consistent system"), Level),
                MismatchSeverity + UnverifiedSeverity, FString());
            break;
        }
    }

    // 4. CheckDataInterfaceCounts takes a UNiagaraSystem. An emitter asset has no system-level
    //    resolved set, so a verdict on one would be invented rather than measured.
    {
        FString EmitterPath;
        UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(EmitterPath);
        if (Emitter)
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("assetPath"), EmitterPath);

            FTestResponseCapture Capture;
            if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.validate"), Payload, Capture))
            {
                TestFalse(TEXT("validate on an emitter publishes no data-interface verdict"),
                    Capture.Result->HasField(TEXT("dataInterfaceCheck")));
            }
            Emitter->RemoveFromRoot();
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-emitter-unavailable"),
                TEXT("NewTransientEmitter returned null; the emitter no-verdict direction was not asserted"));
        }
    }

    return true;
}

// ------------------------------------------------------------------------------------------------
// F-niagara-audit-level-data-interfaces: the LEVEL-SCOPE half.
//
// Every surface above takes a `systemPath` the caller already suspects. niagara.audit_level answers
// the question a capture session actually has -- is anything LOADED here fatal on its next tick --
// by walking every live UNiagaraComponent rather than a sequence's bindings. That scope is
// load-bearing: scrubbing calls ReRenderLevelViewports -> RequestRealTimeFrames(1), which promotes
// the next frame to LEVELTICK_ViewportsOnly and reaches ExecuteSimulations over EVERY system in the
// world, while a Sequencer binding calls SetForceSolo(true) and pulls the bound systems OUT of that
// batch. A binding-scoped pre-flight would inspect close to exactly the wrong set.
//
// What is asserted, and why each is a direction that can actually go wrong:
//   1. Registration, and that a malformed argument is an RPC ERROR rather than a finding. A bad
//      `scope` folded into the report would be an argument mistake wearing a content defect's
//      costume (docs/rpc-design.md section 18).
//   2. A clean level PASSES. Run at the default scope -- the editor world -- which none of the
//      seeded subjects below can reach, because each is held by an unregistered component with no
//      world. `pass` is also re-derived from the response's own numbers so it cannot be a constant,
//      and the per-system buckets are asserted to sum.
//   3. A seeded MISMATCH is found, keyed by system, with both counts in the same mismatchedScripts
//      shape the write path and niagara.list_orphan_data_interfaces already publish.
//   4. An UNVERIFIABLE system produces an UNRUNNABLE row, never a clean one -- and failOn:"none"
//      cannot rescue it. That is the term failOn must not reach, and the whole reason the audit
//      framework exists: an empty result on a system nothing compared is not evidence of
//      cleanliness.
//
// SEEDING IS SAFE, deliberately. The mismatch is REAL -- written through the same private
// ScriptRuntimeCompiledDataForEditor reference CheckDataInterfaceCounts reads, so the measurement
// runs for real rather than being stubbed -- but the corrupt system's only holder is an
// unregistered, worldless UNiagaraComponent. A mismatched system is fatal only when something TICKS
// it, and an unregistered component has no FNiagaraSystemInstance and is in no world's simulation.
// Registering one in the editor world would arm the very appError this verb exists to prevent and
// take the automation host down with it.
namespace NiagaraAuditLevelProbe
{
#if PINWRIGHT_TEST_HAS_NIAGARA_RESOLVED_DI
    // Gives System a resolved data-interface set of ResolvedCount entries for its system spawn
    // script, and a compiled DataInterfaceInfo list of CompiledCount entries on that same script.
    //
    // Equal counts with CompiledCount > 0 is the Consistent subject: a real comparison of a real
    // set. Equal counts at 0/0 is NOT -- a bare NewObject script's FNiagaraVMExecutableData is
    // default-constructed, so its LastCompileStatus is NCS_Unknown and IsValid() is false, which is
    // exactly the "compiled results were thrown away" shape CheckDataInterfaceCounts refuses to
    // call a pass. Unequal counts is the Mismatched subject; no reference object at all is
    // Unverified.
    static bool SeedResolvedDataInterfaces(UNiagaraSystem& System, int32 ResolvedCount, int32 CompiledCount = 0)
    {
        UNiagaraScript* SpawnScript = System.GetSystemSpawnScript();
        if (!SpawnScript)
        {
            return false;
        }
        FNiagaraVMExecutableData& VMData = SpawnScript->GetVMExecutableData();
        VMData.DataInterfaceInfo.SetNum(CompiledCount);
        // Only a script the engine considers compiled licenses an equality as a pass; seeding the
        // status is what makes the clean subject clean rather than merely empty.
        VMData.LastCompileStatus = CompiledCount > 0
            ? ENiagaraScriptCompileStatus::NCS_UpToDate
            : ENiagaraScriptCompileStatus::NCS_Unknown;
        // UNiagaraSystem::ScriptRuntimeCompiledDataForEditor is private; the same reflective hop
        // the checker makes is what lets the seed land where the checker will read it.
        const FObjectPropertyBase* RefProperty = CastField<FObjectPropertyBase>(
            System.GetClass()->FindPropertyByName(TEXT("ScriptRuntimeCompiledDataForEditor")));
        if (!RefProperty || !RefProperty->PropertyClass)
        {
            return false;
        }
        UNiagaraScriptRuntimeCompiledDataEditorReference* Ref =
            Cast<UNiagaraScriptRuntimeCompiledDataEditorReference>(
                NewObject<UObject>(&System, RefProperty->PropertyClass));
        if (!Ref)
        {
            return false;
        }
        // Same key the checker looks the script up by: a null emitter handle for the system-level
        // scripts (UNiagaraSystem::ForEachScriptWithOwningContext).
        FNiagaraScriptRuntimeCompiledData& Data = Ref->ScriptRuntimeCompiledDataMap.Add(
            FNiagaraScriptDataKey(static_cast<const FNiagaraEmitterHandle*>(nullptr), *SpawnScript));
        Data.ResolvedDataInterfaces.SetNum(ResolvedCount);
        RefProperty->SetObjectPropertyValue_InContainer(&System, Ref);
        return true;
    }

    // A transient system plus an UNREGISTERED component holding it, so the sweep can discover it
    // through the same TObjectIterator walk it uses on the real level without anything ever
    // ticking it. Both are rooted; the caller unroots them.
    static UNiagaraSystem* MakeHeldSystem(const TCHAR* NamePrefix, UNiagaraComponent*& OutComponent)
    {
        OutComponent = nullptr;
        UNiagaraSystem* System = NewObject<UNiagaraSystem>(
            GetTransientPackage(), FName(*NiagaraEditTestUtils::MakeAssetName(NamePrefix)));
        if (!System)
        {
            return nullptr;
        }
        System->AddToRoot();

        UNiagaraComponent* Component = NewObject<UNiagaraComponent>(GetTransientPackage());
        if (!Component)
        {
            System->RemoveFromRoot();
            return nullptr;
        }
        Component->AddToRoot();
        Component->SetAsset(System);
        OutComponent = Component;
        return System;
    }

    // The row niagara.audit_level published for one system path, or null when the sweep never
    // reported it. A seeded subject missing from the response is the failure this looks for.
    static TSharedPtr<FJsonObject> FindSystemRow(const TSharedPtr<FJsonObject>& Result,
                                                 const FString& SystemPath)
    {
        const TArray<TSharedPtr<FJsonValue>>* Systems = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("systems"), Systems) || !Systems)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Systems)
        {
            FString Path;
            if (Value.IsValid() && Value->Type == EJson::Object
                && Value->AsObject()->TryGetStringField(TEXT("systemPath"), Path)
                && Path == SystemPath)
            {
                return Value->AsObject();
            }
        }
        return nullptr;
    }

    // The finding filed against one system path, or null when none was.
    static TSharedPtr<FJsonObject> FindFinding(const TSharedPtr<FJsonObject>& Result,
                                               const FString& SystemPath)
    {
        const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("findings"), Findings) || !Findings)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Findings)
        {
            FString Path;
            if (Value.IsValid() && Value->Type == EJson::Object
                && Value->AsObject()->TryGetStringField(TEXT("systemPath"), Path)
                && Path == SystemPath)
            {
                return Value->AsObject();
            }
        }
        return nullptr;
    }
#endif // PINWRIGHT_TEST_HAS_NIAGARA_RESOLVED_DI
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraAuditLevelTest,
    "PinWright.niagara.audit_level.LevelScopeDataInterfaceSweep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAuditLevelTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("niagara.audit_level is registered"),
        IsHandlerRegistered(TEXT("niagara.audit_level")));

    // ------------------------------------------------------------------
    // 1. A malformed argument is an RPC error, never a finding. Folding one into the report would
    //    hand a caller a content verdict against content that is fine.
    // ------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> BadScope = MakeShared<FJsonObject>();
        BadScope->SetStringField(TEXT("scope"), TEXT("sequence"));
        NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.audit_level"),
            BadScope, TEXT("INVALID_ARGUMENT"));

        TSharedPtr<FJsonObject> BadDetail = MakeShared<FJsonObject>();
        BadDetail->SetStringField(TEXT("detail"), TEXT("everything"));
        NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.audit_level"),
            BadDetail, TEXT("INVALID_ARGUMENT"));

        TSharedPtr<FJsonObject> BadFailOn = MakeShared<FJsonObject>();
        BadFailOn->SetStringField(TEXT("failOn"), TEXT("sometimes"));
        NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.audit_level"),
            BadFailOn, TEXT("INVALID_ARGUMENT"));

        // A typo in `checks` that silently ran nothing is indistinguishable from a level that
        // passed every check, which is the single most dangerous way for a lint to fail.
        TSharedPtr<FJsonObject> BadCheck = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> CheckIds;
        CheckIds.Add(MakeShared<FJsonValueString>(TEXT("data_interface_count")));
        BadCheck->SetArrayField(TEXT("checks"), CheckIds);
        NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.audit_level"),
            BadCheck, TEXT("AUDIT_UNKNOWN_CHECK"));
    }

    // ------------------------------------------------------------------
    // 2. A clean level passes, at the default scope: the editor world. Every seeded subject below
    //    is worldless, so nothing this test creates can reach this sweep.
    // ------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.audit_level"),
                MakeShared<FJsonObject>(), Capture))
        {
            TestEqual(TEXT("the default scope is the editor world"),
                Capture.Result->GetStringField(TEXT("scope")), FString(TEXT("world")));
            TestTrue(TEXT("the response publishes the pass rule it derived pass from"),
                !Capture.Result->GetStringField(TEXT("passRule")).IsEmpty());

            const TSharedPtr<FJsonObject>* Summary = nullptr;
            if (Capture.Result->TryGetObjectField(TEXT("summary"), Summary) && Summary)
            {
                int32 Systems = -1, Consistent = -1, Mismatched = -1, Unverified = -1;
                int32 Errors = -1, Unrunnable = -1;
                (*Summary)->TryGetNumberField(TEXT("systems"), Systems);
                (*Summary)->TryGetNumberField(TEXT("consistent"), Consistent);
                (*Summary)->TryGetNumberField(TEXT("mismatched"), Mismatched);
                (*Summary)->TryGetNumberField(TEXT("unverified"), Unverified);
                (*Summary)->TryGetNumberField(TEXT("errors"), Errors);
                (*Summary)->TryGetNumberField(TEXT("unrunnable"), Unrunnable);
                const bool bTruncated = (*Summary)->GetBoolField(TEXT("truncated"));
                const bool bPass = Capture.Result->GetBoolField(TEXT("pass"));

                // The buckets sum to the subjects examined. A system that falls out of every
                // bucket is exactly how a check silently stops running.
                TestEqual(TEXT("consistent + mismatched + unverified == systems"),
                    Consistent + Mismatched + Unverified, Systems);
                // pass is DERIVED, not decided: re-deriving it from the response's own numbers is
                // what stops a constant from satisfying every other assertion here.
                TestTrue(TEXT("pass is the shared verdict over the reported numbers"),
                    bPass == (Errors == 0 && Unrunnable == 0 && !bTruncated));

                // Every mismatched system is one Error and every unverified one is one Unrunnable:
                // the two buckets ARE the two verdict terms, which is what makes the implication
                // below a statement about THIS verb rather than about the shared rule alone.
                TestEqual(TEXT("every mismatched system is one error"), Errors, Mismatched);
                TestEqual(TEXT("every unverified system is one unrunnable row"),
                    Unrunnable, Unverified);
                // "A clean level passes", stated as an implication over the numbers this sweep
                // actually produced, so it holds on a host whose level is dirty rather than being
                // stepped over there. It fails the moment a sweep with nothing mismatched and
                // nothing unverified reports pass:false.
                TestTrue(TEXT("a sweep with nothing mismatched and nothing unverified passes"),
                    !(Mismatched == 0 && Unverified == 0) || bPass);
            }
            else
            {
                AddError(TEXT("niagara.audit_level published no summary object."));
            }
        }
    }

#if PINWRIGHT_TEST_HAS_NIAGARA_RESOLVED_DI
    // ------------------------------------------------------------------
    // 3-4. Seeded subjects, reached through the real TObjectIterator walk at scope:"all".
    // ------------------------------------------------------------------
    {
        UNiagaraComponent* CleanComponent = nullptr;
        UNiagaraComponent* MismatchComponent = nullptr;
        UNiagaraComponent* UnverifiedComponent = nullptr;
        UNiagaraSystem* CleanSystem =
            NiagaraAuditLevelProbe::MakeHeldSystem(TEXT("NS_AuditClean"), CleanComponent);
        UNiagaraSystem* MismatchSystem =
            NiagaraAuditLevelProbe::MakeHeldSystem(TEXT("NS_AuditMismatch"), MismatchComponent);
        // Seeded with no reference object at all, so nothing can be compared for it.
        UNiagaraSystem* UnverifiedSystem =
            NiagaraAuditLevelProbe::MakeHeldSystem(TEXT("NS_AuditUnverified"), UnverifiedComponent);

        const bool bSeeded = CleanSystem && MismatchSystem && UnverifiedSystem
            && NiagaraAuditLevelProbe::SeedResolvedDataInterfaces(*CleanSystem, /*ResolvedCount=*/1, /*CompiledCount=*/1)
            && NiagaraAuditLevelProbe::SeedResolvedDataInterfaces(*MismatchSystem, /*ResolvedCount=*/1);

        if (bSeeded)
        {
            const FString CleanPath = CleanSystem->GetPathName();
            const FString MismatchPath = MismatchSystem->GetPathName();
            const FString UnverifiedPath = UnverifiedSystem->GetPathName();

            // The seeds are what the checker actually measures, asserted straight from the
            // checker: if this drifts, everything below is measuring the wrong thing.
            {
                TArray<PinWrightNiagara::FDataInterfaceCountMismatch> Probe;
                TestTrue(TEXT("the clean seed measures Consistent"),
                    PinWrightNiagara::CheckDataInterfaceCounts(*CleanSystem, Probe)
                        == PinWrightNiagara::EDataInterfaceConsistency::Consistent);
                TestTrue(TEXT("the mismatch seed measures Mismatched"),
                    PinWrightNiagara::CheckDataInterfaceCounts(*MismatchSystem, Probe)
                        == PinWrightNiagara::EDataInterfaceConsistency::Mismatched);
                TestTrue(TEXT("the unverified seed measures Unverified"),
                    PinWrightNiagara::CheckDataInterfaceCounts(*UnverifiedSystem, Probe)
                        == PinWrightNiagara::EDataInterfaceConsistency::Unverified);
            }

            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("scope"), TEXT("all"));
            Payload->SetStringField(TEXT("detail"), TEXT("orphans"));

            FTestResponseCapture Capture;
            if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.audit_level"),
                    Payload, Capture))
            {
                TestFalse(TEXT("a sweep that found a mismatched system does not pass"),
                    Capture.Result->GetBoolField(TEXT("pass")));

                // ---- the seeded mismatch ----
                const TSharedPtr<FJsonObject> MismatchRow =
                    NiagaraAuditLevelProbe::FindSystemRow(Capture.Result, MismatchPath);
                if (MismatchRow.IsValid())
                {
                    TestEqual(TEXT("the seeded mismatch reports the mismatched verdict"),
                        MismatchRow->GetStringField(TEXT("dataInterfaceCheck")),
                        FString(TEXT("mismatched")));
                    TestEqual(TEXT("the seeded mismatch is a flagged row"),
                        MismatchRow->GetStringField(TEXT("status")), FString(TEXT("flagged")));

                    const TArray<TSharedPtr<FJsonValue>>* Scripts = nullptr;
                    if (MismatchRow->TryGetArrayField(TEXT("mismatchedScripts"), Scripts)
                        && Scripts && Scripts->Num() > 0)
                    {
                        // The same per-script spelling niagara.add_emitter's refusal payload,
                        // NiagaraEdit::MakeMutationResult and list_orphan_data_interfaces publish.
                        const TSharedPtr<FJsonObject> Entry = (*Scripts)[0]->AsObject();
                        int32 Compiled = -1;
                        int32 Resolved = -1;
                        Entry->TryGetNumberField(TEXT("compiledDataInterfaces"), Compiled);
                        Entry->TryGetNumberField(TEXT("resolvedDataInterfaces"), Resolved);
                        TestEqual(TEXT("mismatchedScripts carries the compiled count"), Compiled, 0);
                        TestEqual(TEXT("mismatchedScripts carries the resolved count"), Resolved, 1);
                        TestTrue(TEXT("mismatchedScripts names the offending script"),
                            Entry->GetStringField(TEXT("scriptPath")).Contains(TEXT("SystemSpawnScript")));
                    }
                    else
                    {
                        AddError(TEXT("A flagged niagara.audit_level row carried no mismatchedScripts."));
                    }

                    // detail:"orphans" plans the repair from the same call.
                    int32 OrphanCount = -1;
                    TestTrue(TEXT("detail:orphans publishes orphanCount on the row"),
                        MismatchRow->TryGetNumberField(TEXT("orphanCount"), OrphanCount));
                    TestTrue(TEXT("the seeded leftover resolved entry is named as an orphan"),
                        OrphanCount >= 1);

                    // The component that holds it, because the caller needs something to detach.
                    const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
                    TestTrue(TEXT("the row names the components holding the system"),
                        MismatchRow->TryGetArrayField(TEXT("components"), Components)
                            && Components && Components->Num() >= 1);
                }
                else
                {
                    AddError(FString::Printf(
                        TEXT("niagara.audit_level did not report the seeded mismatched system '%s'."),
                        *MismatchPath));
                }

                const TSharedPtr<FJsonObject> MismatchFinding =
                    NiagaraAuditLevelProbe::FindFinding(Capture.Result, MismatchPath);
                if (MismatchFinding.IsValid())
                {
                    TestEqual(TEXT("a mismatch is a flagged finding"),
                        MismatchFinding->GetStringField(TEXT("status")), FString(TEXT("flagged")));
                    TestEqual(TEXT("a mismatch is an error"),
                        MismatchFinding->GetStringField(TEXT("severity")), FString(TEXT("error")));
                    TestEqual(TEXT("a mismatch keeps the namespace's one code for the fault"),
                        MismatchFinding->GetStringField(TEXT("code")),
                        FString(TEXT("NIAGARA_DATA_INTERFACE_MISMATCH")));
                }
                else
                {
                    AddError(TEXT("niagara.audit_level filed no finding for the seeded mismatch."));
                }

                // ---- the unverifiable system: UNRUNNABLE, never clean ----
                const TSharedPtr<FJsonObject> UnverifiedRow =
                    NiagaraAuditLevelProbe::FindSystemRow(Capture.Result, UnverifiedPath);
                if (UnverifiedRow.IsValid())
                {
                    TestEqual(TEXT("an unverifiable system reports the unverified verdict"),
                        UnverifiedRow->GetStringField(TEXT("dataInterfaceCheck")),
                        FString(TEXT("unverified")));
                    // The whole point of the ticket: not "clean". Nothing was compared, so an
                    // empty mismatch list beside it is not evidence the system is sound.
                    TestEqual(TEXT("an unverifiable system is an unrunnable row, not a clean one"),
                        UnverifiedRow->GetStringField(TEXT("status")), FString(TEXT("unrunnable")));
                }
                else
                {
                    AddError(FString::Printf(
                        TEXT("niagara.audit_level did not report the seeded unverifiable system '%s'."),
                        *UnverifiedPath));
                }

                const TSharedPtr<FJsonObject> UnverifiedFinding =
                    NiagaraAuditLevelProbe::FindFinding(Capture.Result, UnverifiedPath);
                if (UnverifiedFinding.IsValid())
                {
                    TestEqual(TEXT("an unverifiable system files an unrunnable finding"),
                        UnverifiedFinding->GetStringField(TEXT("status")), FString(TEXT("unrunnable")));
                    TestEqual(TEXT("an unverifiable system is not reported as a mismatch"),
                        UnverifiedFinding->GetStringField(TEXT("code")),
                        FString(TEXT("NIAGARA_DATA_INTERFACE_UNVERIFIED")));
                }
                else
                {
                    AddError(TEXT("niagara.audit_level filed no finding for the seeded unverifiable system."));
                }

                // ---- the clean seed: a clean row and no finding at all ----
                const TSharedPtr<FJsonObject> CleanRow =
                    NiagaraAuditLevelProbe::FindSystemRow(Capture.Result, CleanPath);
                if (CleanRow.IsValid())
                {
                    TestEqual(TEXT("a consistent system reports the consistent verdict"),
                        CleanRow->GetStringField(TEXT("dataInterfaceCheck")),
                        FString(TEXT("consistent")));
                    TestEqual(TEXT("a consistent system is a clean row"),
                        CleanRow->GetStringField(TEXT("status")), FString(TEXT("clean")));
                    TestFalse(TEXT("a consistent system files no finding"),
                        NiagaraAuditLevelProbe::FindFinding(Capture.Result, CleanPath).IsValid());
                }
                else
                {
                    AddError(FString::Printf(
                        TEXT("niagara.audit_level did not report the seeded consistent system '%s'."),
                        *CleanPath));
                }
            }

            // ------------------------------------------------------------------
            // failOn:"none" moves the SEVERITY bar and nothing else. It must not be able to turn
            // "we could not measure this" into a pass -- that fold is the defect the shared audit
            // framework exists to make impossible.
            // ------------------------------------------------------------------
            {
                TSharedPtr<FJsonObject> LenientPayload = MakeShared<FJsonObject>();
                LenientPayload->SetStringField(TEXT("scope"), TEXT("all"));
                LenientPayload->SetStringField(TEXT("failOn"), TEXT("none"));

                FTestResponseCapture LenientCapture;
                if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.audit_level"),
                        LenientPayload, LenientCapture))
                {
                    const TSharedPtr<FJsonObject>* LenientSummary = nullptr;
                    int32 Unrunnable = -1;
                    if (LenientCapture.Result->TryGetObjectField(TEXT("summary"), LenientSummary)
                        && LenientSummary)
                    {
                        (*LenientSummary)->TryGetNumberField(TEXT("unrunnable"), Unrunnable);
                    }
                    TestTrue(TEXT("the unverifiable seed is counted as unrunnable"), Unrunnable >= 1);
                    TestFalse(TEXT("failOn:none cannot turn an unrunnable system into a pass"),
                        LenientCapture.Result->GetBoolField(TEXT("pass")));
                }
            }
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-seed-unavailable"),
                TEXT("could not seed a resolved data-interface set on a transient system; the "
                     "level-scope sweep's flagged / unrunnable / clean directions were not asserted"));
        }

        // DISARM before unrooting. The seed is harmless while its only holder is an unregistered
        // component, but RemoveFromRoot only makes the system collectable - it stays in the object
        // graph until the next GC, and an armed system nobody can see is exactly the hazard this
        // verb exists to find. Putting the counts back in agreement costs one call.
        if (MismatchSystem)
        {
            NiagaraAuditLevelProbe::SeedResolvedDataInterfaces(*MismatchSystem, /*ResolvedCount=*/0);
        }
        // Nothing here was ever registered with a world, so dropping the roots is all that is
        // needed; GC reclaims the components and the systems together.
        for (UNiagaraComponent* Component : { CleanComponent, MismatchComponent, UnverifiedComponent })
        {
            if (Component)
            {
                Component->RemoveFromRoot();
            }
        }
        for (UNiagaraSystem* System : { CleanSystem, MismatchSystem, UnverifiedSystem })
        {
            if (System)
            {
                System->RemoveFromRoot();
            }
        }
    }
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-unavailable"),
        TEXT("this engine build exposes no FNiagaraScriptRuntimeCompiledData, so no subject can be "
             "seeded and the level-scope sweep's verdict directions were not asserted"));
#endif

    return true;
}
