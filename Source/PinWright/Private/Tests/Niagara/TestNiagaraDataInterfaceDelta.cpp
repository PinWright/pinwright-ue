// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the two halves of the data-interface arming chain:
//   B-niagara-set-parameter-emitter-scope-arms-di-mismatch  (attribution + verdict separation)
//   B-niagara-di-count-mismatch-vectorvm-assert-kills-editor (the tick pre-flight and the
//                                                             live-system repair gate)
//
// NOTHING HERE PUTS A LIVE MISMATCHED SYSTEM IN A WORLD. A system whose compiled data-interface
// count differs from its resolved one asserts inside the VectorVM on a concurrent worker the next
// time anything ticks it, and that is an appError - it would take the automation host down. Every
// seeded mismatch below is held by an unregistered, worldless component, which is exactly the
// negative direction the two new gates have to get right: neither the repair nor the playhead
// refusal may fire on a system nothing is running.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Handlers/Niagara/NiagaraTickPreflight.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if __has_include("NiagaraScriptRuntimeCompiledData.h")
#include "NiagaraScriptRuntimeCompiledData.h"
#define PINWRIGHT_DELTA_TEST_HAS_RESOLVED_DI 1
#else
#define PINWRIGHT_DELTA_TEST_HAS_RESOLVED_DI 0
#endif

// Uniquely named namespace: Unity merges the Niagara test TUs into one translation unit, so a
// helper sharing a name with the sibling seeding probe in TestNiagaraDataInterfaceConsistency.cpp
// would be a redefinition rather than two internal-linkage copies.
namespace NiagaraDataInterfaceDeltaProbe
{
    const TCHAR* const EmitterScope = TEXT("spawnRapidIteration");
    // Synthetic rapid-iteration name. Nothing in the write path parses the shape, so a
    // fixture-owned name keeps this independent of any host's module layout.
    const TCHAR* const ProbeParameter = TEXT("Constants.PinWrightDeltaProbe.Scalar");

    TSharedPtr<FJsonObject> MakeEmitterScopedPayload(const FString& AssetPath, const FString& Emitter, double Value)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetStringField(TEXT("scope"), EmitterScope);
        Params->SetStringField(TEXT("name"), ProbeParameter);
        Params->SetStringField(TEXT("type"), TEXT("float"));
        Params->SetNumberField(TEXT("value"), Value);
        Params->SetBoolField(TEXT("compile"), false);
        Params->SetBoolField(TEXT("save"), false);
        Params->SetStringField(TEXT("emitter"), Emitter);
        return Params;
    }

    // niagara.set_parameter writes EXISTING store entries only - a name the store does not carry
    // is PARAMETER_NOT_FOUND (that is niagara.add_parameter's job) - and MakeAuthorableSystem's
    // fixture carries no rapid-iteration entries of its own. So the probe has to be seeded into
    // the emitter's spawn rapid-iteration store first, exactly as the sibling
    // TestNiagaraSetParameterEmitterScope.cpp does, or the write under test never reaches the
    // handler body the delta is measured around.
    //
    // Returns the store so a caller can read the value back, or null when the fixture did not
    // produce a usable emitter spawn script.
    FNiagaraParameterStore* SeedEmitterSpawnProbe(UNiagaraSystem& System, FName EmitterName, float SeedValue)
    {
        FNiagaraEmitterHandle* Handle = nullptr;
        for (const FNiagaraEmitterHandle& Candidate : System.GetEmitterHandles())
        {
            if (Candidate.GetName() == EmitterName)
            {
                Handle = const_cast<FNiagaraEmitterHandle*>(&Candidate);
                break;
            }
        }
        FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
        UNiagaraScript* SpawnScript = EmitterData ? EmitterData->SpawnScriptProps.Script : nullptr;
        if (!SpawnScript)
        {
            return nullptr;
        }
        FNiagaraParameterStore& Store = SpawnScript->RapidIterationParameters;
        const FNiagaraVariable Probe(FNiagaraTypeDefinition::GetFloatDef(), FName(ProbeParameter));
        Store.SetParameterData(reinterpret_cast<const uint8*>(&SeedValue), Probe, /*bAdd=*/true);
        return &Store;
    }

    float ReadProbe(FNiagaraParameterStore& Store)
    {
        const FNiagaraVariable Probe(FNiagaraTypeDefinition::GetFloatDef(), FName(ProbeParameter));
        return Store.GetParameterValueOrDefault<float>(Probe, 0.0f);
    }

    // The response's dataInterfaceDelta object, or null when the field is absent - which is the
    // pre-fix shape and therefore the thing worth distinguishing from a delta that reads oddly.
    TSharedPtr<FJsonObject> GetDelta(const TSharedPtr<FJsonObject>& Result)
    {
        const TSharedPtr<FJsonObject>* Delta = nullptr;
        if (Result.IsValid() && Result->TryGetObjectField(TEXT("dataInterfaceDelta"), Delta) && Delta)
        {
            return *Delta;
        }
        return nullptr;
    }

#if PINWRIGHT_DELTA_TEST_HAS_RESOLVED_DI
    // Seeds System's spawn script with CompiledCount compiled data-interface entries and
    // ResolvedCount resolved ones, through the same private reference object the checker reads, so
    // the measurement runs for real rather than being stubbed.
    //
    // LastCompileStatus is part of the seed on purpose: a default-constructed
    // FNiagaraVMExecutableData reads NCS_Unknown, which is what UNiagaraScript::InvalidateCompileResults
    // leaves behind, and it is the difference between "this script owns no data interfaces" and
    // "this script's bytecode was thrown away".
    bool Seed(UNiagaraSystem& System, int32 ResolvedCount, int32 CompiledCount, bool bCompiledResultsPresent)
    {
        UNiagaraScript* SpawnScript = System.GetSystemSpawnScript();
        if (!SpawnScript)
        {
            return false;
        }
        FNiagaraVMExecutableData& VMData = SpawnScript->GetVMExecutableData();
        VMData.DataInterfaceInfo.SetNum(CompiledCount);
        VMData.LastCompileStatus = bCompiledResultsPresent
            ? ENiagaraScriptCompileStatus::NCS_UpToDate
            : ENiagaraScriptCompileStatus::NCS_Unknown;

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
        FNiagaraScriptRuntimeCompiledData& Data = Ref->ScriptRuntimeCompiledDataMap.Add(
            FNiagaraScriptDataKey(static_cast<const FNiagaraEmitterHandle*>(nullptr), *SpawnScript));
        Data.ResolvedDataInterfaces.SetNum(ResolvedCount);
        RefProperty->SetObjectPropertyValue_InContainer(&System, Ref);
        return true;
    }

    // A transient system plus an UNREGISTERED component holding it. Discoverable through the same
    // TObjectIterator walks the production sweeps use, and in no world, so nothing ever ticks it.
    // Held by strong pointers rather than root flags: no sweep under test reads IsRooted, and the
    // RAII release covers every exit path of the callers below, including their skip returns.
    struct FHeldSystem
    {
        TStrongObjectPtr<UNiagaraSystem> System;
        TStrongObjectPtr<UNiagaraComponent> Component;
    };

    FHeldSystem MakeHeldSystem(const TCHAR* NamePrefix)
    {
        FHeldSystem Held;
        UNiagaraSystem* System = NewObject<UNiagaraSystem>(
            GetTransientPackage(), FName(*NiagaraEditTestUtils::MakeAssetName(NamePrefix)));
        UNiagaraComponent* Component =
            System ? NewObject<UNiagaraComponent>(GetTransientPackage()) : nullptr;
        if (!System || !Component)
        {
            return Held;
        }
        Component->SetAsset(System);
        Held.System.Reset(System);
        Held.Component.Reset(Component);
        return Held;
    }
#endif
}

// ============================================================================
// 1. The mutation envelope attributes the verdict it reports, and the two verdicts the field used
//    to conflate are separated.
//
// What is asserted, and the counterfactual for each:
//
//  (a) DidWriteArmDataInterfaceMismatch is true only for a real pass turning into a mismatch.
//      Counterfactual: written as `Before != After` it also fires for Unverified -> Mismatched and
//      for Mismatched -> Consistent, and this table fails on the first and third rows.
//
//  (b) A comparison where every compared script has lost its compiled results is Unverified, not
//      Consistent, while a genuine n == n with compiled results present stays Consistent.
//      Counterfactual: revert the `ScriptsWithCompiledResults == 0` branch in
//      NiagaraDataInterfaceConsistency.cpp's ClassifyComparison and the invalidated seed measures
//      Consistent - a vacuous 0-against-0 published as a pass, which is exactly the state an
//      emitter-scoped write leaves behind.
//
//  (c) An emitter-scoped niagara.set_parameter response carries dataInterfaceDelta, its `after`
//      agrees with dataInterfaceCheck, and the system is not left mismatched.
//      Counterfactual: remove the AddDataInterfaceDelta call from NiagaraEdit::MakeMutationResult
//      and the field is absent, which is the pre-fix response - `mismatched` echoed with nothing
//      saying whether the call inherited it or created it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraDataInterfaceDeltaTest,
    "PinWright.niagara.data_interface_delta.MutationReportsBeforeAndAfter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDataInterfaceDeltaTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;
    using namespace NiagaraDataInterfaceDeltaProbe;

    // ---- (a) the attribution predicate -------------------------------------------------------
    TestTrue(TEXT("consistent -> mismatched is armed by the write"),
        DidWriteArmDataInterfaceMismatch(
            EDataInterfaceConsistency::Consistent, EDataInterfaceConsistency::Mismatched));
    TestFalse(TEXT("unverified -> mismatched is not attributable to the write"),
        DidWriteArmDataInterfaceMismatch(
            EDataInterfaceConsistency::Unverified, EDataInterfaceConsistency::Mismatched));
    TestFalse(TEXT("mismatched -> mismatched is inherited, not authored"),
        DidWriteArmDataInterfaceMismatch(
            EDataInterfaceConsistency::Mismatched, EDataInterfaceConsistency::Mismatched));
    TestFalse(TEXT("mismatched -> consistent is a repair, not an arming"),
        DidWriteArmDataInterfaceMismatch(
            EDataInterfaceConsistency::Mismatched, EDataInterfaceConsistency::Consistent));

    // ---- (b) the verdict separation ----------------------------------------------------------
#if PINWRIGHT_DELTA_TEST_HAS_RESOLVED_DI
    {
        const FHeldSystem ComparedHold = MakeHeldSystem(TEXT("NS_DeltaCompared"));
        const FHeldSystem InvalidatedHold = MakeHeldSystem(TEXT("NS_DeltaInvalidated"));
        UNiagaraSystem* ComparedSystem = ComparedHold.System.Get();
        UNiagaraSystem* InvalidatedSystem = InvalidatedHold.System.Get();

        const bool bSeeded = ComparedSystem && InvalidatedSystem
            && Seed(*ComparedSystem, /*ResolvedCount=*/2, /*CompiledCount=*/2, /*bCompiledResultsPresent=*/true)
            && Seed(*InvalidatedSystem, /*ResolvedCount=*/0, /*CompiledCount=*/0, /*bCompiledResultsPresent=*/false);

        if (bSeeded)
        {
            TArray<FDataInterfaceCountMismatch> Mismatches;
            FDataInterfaceComparisonCounts Counts;

            TestTrue(TEXT("n == n with compiled results present is a pass"),
                CheckDataInterfaceCounts(*ComparedSystem, Mismatches, &Counts)
                    == EDataInterfaceConsistency::Consistent);
            TestEqual(TEXT("the pass counts one substantive comparison"),
                Counts.ScriptsWithCompiledResults, 1);

            TestTrue(TEXT("0 == 0 on a script whose compiled results are gone is NOT a pass"),
                CheckDataInterfaceCounts(*InvalidatedSystem, Mismatches, &Counts)
                    == EDataInterfaceConsistency::Unverified);
            TestTrue(TEXT("the vacuous case compared a script"), Counts.ComparedScripts > 0);
            TestEqual(TEXT("but none of them carried compiled results"),
                Counts.ScriptsWithCompiledResults, 0);

            // The gate that decides whether FinalizeNiagaraEdit quiesces and compiles. A component
            // bound to the asset but running nothing must not count, or every emitter-scoped edit
            // of a loaded system pays for a forced compile.
            TestEqual(TEXT("a bound holder that runs nothing is not a live instance"),
                CountLiveSystemInstances(*ComparedSystem), 0);
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-seed-unavailable"),
                TEXT("could not seed a resolved data-interface set on a transient system; the "
                     "consistent-vs-vacuous verdict separation was not asserted"));
        }
    }
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-unavailable"),
        TEXT("this engine build exposes no FNiagaraScriptRuntimeCompiledData, so the "
             "consistent-vs-vacuous verdict separation could not be seeded or asserted"));
#endif

    // ---- (c) the wire shape, from the handler ------------------------------------------------
    TestTrue(TEXT("niagara.set_parameter is registered"), IsHandlerRegistered(TEXT("niagara.set_parameter")));

    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!bSetupOk || !System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            TEXT("could not build an authorable Niagara system fixture; the emitter-scoped "
                 "set_parameter delta was not asserted"));
        return true;
    }

    FNiagaraParameterStore* ProbeStore = SeedEmitterSpawnProbe(*System, EmitterName, 1.0f);
    if (!ProbeStore)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            TEXT("the fixture emitter has no spawn script to seed a rapid-iteration probe into; "
                 "the emitter-scoped set_parameter delta was not asserted"));
        return true;
    }

    FTestResponseCapture Capture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"),
            MakeEmitterScopedPayload(SystemPath, EmitterName.ToString(), 3.0), Capture))
    {
        // The write actually landed in the store, so what follows describes a real mutation
        // rather than a refusal the envelope happened to shape the same way.
        TestEqual(TEXT("the emitter-scoped write reached the rapid-iteration store"),
            ReadProbe(*ProbeStore), 3.0f);

        const TSharedPtr<FJsonObject> Delta = GetDelta(Capture.Result);
        if (TestTrue(TEXT("an emitter-scoped write publishes dataInterfaceDelta"), Delta.IsValid()))
        {
            FString Before;
            FString After;
            TestTrue(TEXT("the delta names the verdict the call found"),
                Delta->TryGetStringField(TEXT("before"), Before) && !Before.IsEmpty());
            TestTrue(TEXT("the delta names the verdict the call left"),
                Delta->TryGetStringField(TEXT("after"), After) && !After.IsEmpty());
            FString EnvelopeVerdict;
            Capture.Result->TryGetStringField(TEXT("dataInterfaceCheck"), EnvelopeVerdict);
            TestEqual(TEXT("after is the same fact dataInterfaceCheck reports"), After, EnvelopeVerdict);
            TestTrue(TEXT("the delta states whether the verdict changed"),
                Delta->HasTypedField<EJson::Boolean>(TEXT("changed")));
            TestTrue(TEXT("the delta attributes the arming"),
                Delta->HasTypedField<EJson::Boolean>(TEXT("armedByThisWrite")));
            TestTrue(TEXT("the delta reports what was done about it"),
                Delta->HasTypedField<EJson::String>(TEXT("repair")));
            // Nothing holds a live instance of a freshly built fixture, so the repair must not
            // have run - that is the branch that keeps the batch workflow cheap.
            FString Repair;
            Delta->TryGetStringField(TEXT("repair"), Repair);
            TestEqual(TEXT("no repair runs on a system nothing is ticking"),
                Repair, FString(TEXT("not_needed")));
        }

        // The system itself, not the response: a write must not leave the asset in the state that
        // asserts inside the VectorVM on its next tick.
        TArray<FDataInterfaceCountMismatch> AfterWrite;
        TestTrue(TEXT("the emitter-scoped write does not leave the system mismatched"),
            CheckDataInterfaceCounts(*System, AfterWrite) != EDataInterfaceConsistency::Mismatched);
    }

    return true;
}

// ============================================================================
// 2. The value-identical no-op write, from history entry #10 of
//    B-niagara-di-count-mismatch-vectorvm-assert-kills-editor.
//
// Writing a pin the value it already holds is not a diff, and it was still enough to put a repaired
// system straight back into the editor-killing state - so a fix that only reports the verdict
// leaves an agent who edits, compiles, then edits again holding a primed system behind a green
// validate.
//
// Counterfactual: remove the AddDataInterfaceDelta call from NiagaraEdit::MakeMutationResult and
// `dataInterfaceDelta` is absent from the repeat's response, so the first assertion fails - which
// is the pre-fix envelope exactly. The paired assertion below is the one the fix has to keep
// false: a response may not simultaneously claim it armed the mismatch, report the system still
// mismatched, and report that nothing was done about it. That triple is the state this ticket
// describes a caller being handed, and the live-instance repair in FinalizeNiagaraEdit is what
// closes it when something is ticking the system. The last assertion reads the asset rather than
// the response, so it survives any change to the envelope.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraDataInterfaceDeltaNoOpWriteTest,
    "PinWright.niagara.data_interface_delta.NoOpWriteIsReportedAndNotLeftArmed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDataInterfaceDeltaNoOpWriteTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;
    using namespace NiagaraDataInterfaceDeltaProbe;

    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!bSetupOk || !System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            TEXT("could not build an authorable Niagara system fixture; the value-identical "
                 "no-op write case was not asserted"));
        return true;
    }

    const FString Emitter = EmitterName.ToString();

    // set_parameter writes existing entries only, so the probe has to exist before the first call.
    FNiagaraParameterStore* ProbeStore = SeedEmitterSpawnProbe(*System, EmitterName, 1.0f);
    if (!ProbeStore)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            TEXT("the fixture emitter has no spawn script to seed a rapid-iteration probe into; "
                 "the value-identical no-op write case was not asserted"));
        return true;
    }

    FTestResponseCapture First;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"),
            MakeEmitterScopedPayload(SystemPath, Emitter, 7.0), First))
    {
        return false;
    }
    // Makes the second call a genuine no-op rather than a second first-write: the store already
    // holds exactly the value the repeat is about to write.
    if (!TestEqual(TEXT("the first write landed, so the repeat is value-identical"),
            ReadProbe(*ProbeStore), 7.0f))
    {
        return false;
    }

    // The same arguments again. No content change of any kind - the pin already holds this value.
    FTestResponseCapture Second;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"),
            MakeEmitterScopedPayload(SystemPath, Emitter, 7.0), Second))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Delta = GetDelta(Second.Result);
    if (TestTrue(TEXT("the no-op repeat still publishes dataInterfaceDelta"), Delta.IsValid()))
    {
        FString Before;
        FString After;
        Delta->TryGetStringField(TEXT("before"), Before);
        Delta->TryGetStringField(TEXT("after"), After);
        TestTrue(TEXT("the repeat reports the verdict it found"), !Before.IsEmpty());
        TestTrue(TEXT("the repeat reports the verdict it left"), !After.IsEmpty());
        FString EnvelopeVerdict;
        Second.Result->TryGetStringField(TEXT("dataInterfaceCheck"), EnvelopeVerdict);
        TestEqual(TEXT("after is the same fact dataInterfaceCheck reports"), After, EnvelopeVerdict);

        // The pairing that carries the ticket: a write may not both claim it armed the system and
        // return with the system still armed. Either it did not arm it, or it repaired it.
        bool bArmed = false;
        Delta->TryGetBoolField(TEXT("armedByThisWrite"), bArmed);
        FString Repair;
        Delta->TryGetStringField(TEXT("repair"), Repair);
        TestFalse(TEXT("a write never returns having armed a mismatch it did not act on"),
            bArmed && After == TEXT("mismatched") && Repair == TEXT("not_needed"));
    }

    // Measured off the asset rather than the response, so this holds whatever the envelope says.
    TArray<FDataInterfaceCountMismatch> AfterRepeat;
    TestTrue(TEXT("a value-identical repeat does not leave the system mismatched"),
        CheckDataInterfaceCounts(*System, AfterRepeat) != EDataInterfaceConsistency::Mismatched);

    return true;
}

// ============================================================================
// 3. The sequencer playhead pre-flight.
//
// The positive direction cannot be exercised here: taking it needs a LIVE component of a mismatched
// system in the editor world, and the next tick of that is the appError this whole chain exists to
// prevent. So what is asserted is the reporting path on a synthetic list (the same approach the
// sibling consistency test takes for DescribeDataInterfaceMismatches) plus the liveness definition
// both new gates share, on a real seeded subject.
//
// Counterfactuals, stated per assertion because they are not equally strong:
//
//  * `CountLiveSystemInstances == 0` is the LOAD-BEARING one. Drop the
//    `GetSystemInstanceController().IsValid()` test from it and the bound-but-unregistered probe
//    component counts as live, so it returns 1 and this fails. That function is what decides
//    whether FinalizeNiagaraEdit quiesces and compiles, so the broken version would fire a forced
//    compile on every emitter-scoped edit of any loaded system - the cost the batch workflow
//    exists to avoid.
//  * `CountLiveSystemInstances == KillSystemInstances` pins the two sweeps to ONE definition of
//    live. They disagreeing is how the repair would quiesce a system it had counted as idle, or
//    skip one it had counted as live; the same broken filter above breaks this equality too.
//  * `FindTickUnsafeNiagaraSystems` not naming the subject asserts the COMPOSITION only, and that
//    is a weaker claim, stated rather than dressed up: this probe is excluded by the world filter
//    and by the controller filter independently, so dropping either one alone still excludes it.
//    What fails here is a sweep that drops its liveness scoping altogether - which is the shape
//    that would refuse every scrub in a level merely REFERENCING such an asset.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraTickPreflightTest,
    "PinWright.niagara.tick_preflight.PlayheadGateIsScopedToLiveInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraTickPreflightTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;
    using namespace NiagaraDataInterfaceDeltaProbe;

    TestTrue(TEXT("sequencer.set_playhead is registered"), IsHandlerRegistered(TEXT("sequencer.set_playhead")));

    // The refusal text has to name the system, how many live components hold it and both counts -
    // a refusal a caller cannot act on is a refusal they will work around.
    {
        FTickUnsafeNiagaraSystem Row;
        Row.SystemPath = TEXT("/Game/Probe/NS_TickUnsafeProbe.NS_TickUnsafeProbe");
        Row.LiveComponents = 3;
        FDataInterfaceCountMismatch& Mismatch = Row.Mismatches.AddDefaulted_GetRef();
        Mismatch.ScriptPath = TEXT("/Game/Probe/NS_TickUnsafeProbe.NS_TickUnsafeProbe:Probe.UpdateScript");
        Mismatch.EmitterName = TEXT("Probe");
        Mismatch.CompiledCount = 0;
        Mismatch.ResolvedCount = 2;

        const FString Described = DescribeTickUnsafeNiagaraSystems({ Row });
        TestTrue(TEXT("the refusal names the system"), Described.Contains(Row.SystemPath));
        TestTrue(TEXT("the refusal names the live component count"), Described.Contains(TEXT("3 live component")));
        TestTrue(TEXT("the refusal names the offending script"), Described.Contains(Mismatch.ScriptPath));
        TestTrue(TEXT("the refusal names both counts"), Described.Contains(TEXT("compiled 0, resolved 2")));
    }

#if PINWRIGHT_DELTA_TEST_HAS_RESOLVED_DI
    const FHeldSystem MismatchHold = MakeHeldSystem(TEXT("NS_PreflightMismatch"));
    UNiagaraSystem* MismatchSystem = MismatchHold.System.Get();
    const bool bSeeded = MismatchSystem
        && Seed(*MismatchSystem, /*ResolvedCount=*/2, /*CompiledCount=*/0, /*bCompiledResultsPresent=*/true);

    if (bSeeded)
    {
        TArray<FDataInterfaceCountMismatch> Probe;
        TestTrue(TEXT("the seed really is a mismatch"),
            CheckDataInterfaceCounts(*MismatchSystem, Probe) == EDataInterfaceConsistency::Mismatched);

        // The subject is discoverable: it is a real UNiagaraComponent bound to the system, so a
        // counter that keyed on the binding rather than on a running instance would find it. That
        // is what makes the next assertion a measurement rather than a walk over nothing.
        TestEqual(TEXT("an unregistered holder is bound but runs no instance"),
            CountLiveSystemInstances(*MismatchSystem), 0);

        // One definition of "live" across both sweeps. KillSystemInstances counts what it actually
        // stopped, so on a subject with nothing running the two must agree at 0; if they could
        // disagree, the repair would quiesce a system it had counted as idle or skip one it had
        // counted as live. Safe to call here precisely because nothing is running.
        TestEqual(TEXT("counting and killing agree on what is live"),
            CountLiveSystemInstances(*MismatchSystem), KillSystemInstances(*MismatchSystem));

        TArray<FTickUnsafeNiagaraSystem> Unsafe;
        FindTickUnsafeNiagaraSystems(Unsafe);
        const bool bNamed = Unsafe.ContainsByPredicate(
            [MismatchSystem](const FTickUnsafeNiagaraSystem& Row)
            {
                return Row.SystemPath == MismatchSystem->GetPathName();
            });
        TestFalse(TEXT("a mismatched system nothing is running does not block the playhead"), bNamed);

        // DISARM before the hold is released: dropping the last reference only makes the system
        // collectable, and an armed system nobody can see is the hazard the pre-flight exists to find.
        Seed(*MismatchSystem, /*ResolvedCount=*/0, /*CompiledCount=*/0, /*bCompiledResultsPresent=*/true);
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-seed-unavailable"),
            TEXT("could not seed a mismatched transient system; the pre-flight's live-instance "
                 "scoping was not asserted"));
    }
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-unavailable"),
        TEXT("this engine build exposes no FNiagaraScriptRuntimeCompiledData, so the pre-flight's "
             "live-instance scoping could not be seeded or asserted"));
#endif

    return true;
}
