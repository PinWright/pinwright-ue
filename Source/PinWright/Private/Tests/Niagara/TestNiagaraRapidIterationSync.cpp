// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the rapid-iteration / override-pin split:
// B-module-input-override-inert-runtime-uses-rapid-iteration-default (set_module_input left the
// shadowing constant standing) and B-niagara-force-compile-resets-rapid-iteration-values (a
// compile rebuilt the system-script stores and discarded authored values).
//
// Both defects come from the same engine fact: a module input's value can live in TWO places at
// once — the graph override pin PinWright writes, and the rapid-iteration constant
// Constants.<Emitter>.<Module>.<Input> a compile generates and the compiled simulation reads.
// The Niagara editor never produces a disagreement (UNiagaraStackFunctionInput::SetLocalValue
// writes the constant on every affected script, and Reset() restores it through the same path);
// PinWright did, on every input whose constant already existed.
//
// The write and the reset are tested together on one fixture on purpose: a write-through that no
// reset undoes is the same silent divergence with the sign flipped, and only a set-then-reset
// sequence on the same input can catch that.

#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraRapidIteration.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"

// Uniquely named namespace: Unity merges test TUs, so an anonymous-namespace helper here would
// ODR-clash with identically-shaped helpers in the sibling Niagara tests.
namespace NiagaraRapidIterationSyncTestLocal
{
    const TCHAR* const SpawnRateModulePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    const TCHAR* const SpawnRateInput = TEXT("SpawnRate");

    // A rapid-iteration name no module owns, used where the test needs an entry whose fate under
    // a rebuild it fully controls.
    const TCHAR* const SyntheticConstant = TEXT("Constants.PinWrightProbe.RapidIterationSync");

    UNiagaraEmitter* FirstEmitter(UNiagaraSystem& System)
    {
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            if (UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter)
            {
                return Emitter;
            }
        }
        return nullptr;
    }

    void SeedFloat(FNiagaraParameterStore& Store, const FNiagaraVariable& Variable, float Value)
    {
        Store.SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable, /*bAdd=*/true);
    }

    bool ScopesContain(const TSharedPtr<FJsonObject>& Report, const TCHAR* Scope)
    {
        const TArray<TSharedPtr<FJsonValue>>* Scopes = nullptr;
        if (!Report.IsValid() || !Report->TryGetArrayField(TEXT("updatedScopes"), Scopes) || Scopes == nullptr)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Scopes)
        {
            if (Value.IsValid() && Value->AsString() == Scope)
            {
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// 1. set_module_input pushes its literal into the shadowing rapid-iteration constant.
//
// The fixture reproduces the exact shape the ticket measured: an EmitterUpdate SpawnRate module
// whose constant already exists in the system update store at a value the author never wrote.
// Before the fix the verb wrote only the override pin, the store kept its old number, and the
// response said nothing about either.
//
// Counterfactuals, three of them:
//  - remove the WriteThroughModuleInputConstant call from the literal branch of
//    ApplyModuleMutation (NiagaraEditHandler.cpp) and both stores still read the seeded 7 rather
//    than the written 20, and the response carries no `rapidIteration` object at all;
//  - drop AddEmitterDataStores from CollectRapidIterationStores (NiagaraRapidIteration.cpp) and
//    the write reaches only the system-script mirror, which a compile rebuilds from the
//    emitter-stage store — the emitter-store assertion and the emitterUpdateRapidIteration scope
//    assertion both fail, which is the whole point of the ticket;
//  - remove the RemoveModuleInputConstant call from reset_module_input's override-pin branch and
//    the reset leaves the authored 20 in both stores while the graph reverts to the module
//    default, i.e. the same silent divergence inverted.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputWritesThroughRapidIterationTest,
    "PinWright.niagara.set_module_input.WritesThroughShadowingRapidIterationConstant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputWritesThroughRapidIterationTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraRapidIterationSyncTestLocal;

    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRateModulePath);
    if (!SpawnRateScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("could not load '%s'"), SpawnRateModulePath));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            TEXT("the transient fixture system could not be built"));
        return true;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::EmitterUpdateScript,
        SpawnRateScript);
    UNiagaraEmitter* Emitter = FirstEmitter(*System);
    // The fixture factory roots both the system and its emitter; this owns that pin so both are
    // released on every exit path below, not only the ones calling DestroyFixture.
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    Roots.Emitter = Emitter;

    UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript();
    FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
    UNiagaraScript* EmitterUpdateScript = EmitterData ? EmitterData->EmitterUpdateScriptProps.Script : nullptr;
    if (!TestNotNull(TEXT("SpawnRate module added to the EmitterUpdate stack"), ModuleNode)
        || !TestNotNull(TEXT("fixture emitter resolved"), Emitter)
        || !TestNotNull(TEXT("system update script resolved"), SystemUpdateScript)
        || !TestNotNull(TEXT("emitter update script resolved"), EmitterUpdateScript))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // The constant a compile would generate for this input, built by the production helper so the
    // test cannot agree with a handler that computes a different name.
    const FNiagaraParameterHandle AliasedHandle =
        FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
            FNiagaraParameterHandle::CreateModuleParameterHandle(FName(SpawnRateInput)),
            ModuleNode);
    const FName ConstantName = PinWrightNiagara::MakeRapidIterationConstantName(
        AliasedHandle.GetParameterHandleString(),
        Emitter->GetUniqueEmitterName(),
        ENiagaraScriptUsage::EmitterUpdateScript);
    TestTrue(TEXT("the rapid-iteration constant is namespaced under Constants."),
        ConstantName.ToString().StartsWith(TEXT("Constants.")));

    const FNiagaraVariable Constant(FNiagaraTypeDefinition::GetFloatDef(), ConstantName);
    // Seeded into BOTH stores on purpose. The system-script store is the mirror a compile rebuilds;
    // the emitter-stage store is the source of truth it rebuilds FROM, and is the only half whose
    // value outlives a compile. A write-through that reached only the mirror would pass every
    // assertion below if this seeded one store, and would still be inert after the next compile.
    FNiagaraParameterStore& Store = SystemUpdateScript->RapidIterationParameters;
    FNiagaraParameterStore& EmitterStore = EmitterUpdateScript->RapidIterationParameters;
    constexpr float ShadowValue = 7.0f;
    constexpr float WrittenValue = 20.0f;
    SeedFloat(Store, Constant, ShadowValue);
    SeedFloat(EmitterStore, Constant, ShadowValue);
    if (!TestEqual(TEXT("shadowing constant seeded into the system update store"),
            Store.GetParameterValueOrDefault<float>(Constant, 0.0f), ShadowValue)
        || !TestEqual(TEXT("shadowing constant seeded into the emitter update store"),
            EmitterStore.GetParameterValueOrDefault<float>(Constant, 0.0f), ShadowValue))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
    Payload->SetStringField(TEXT("entryId"), ModuleNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("inputName"), SpawnRateInput);
    Payload->SetNumberField(TEXT("value"), WrittenValue);
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"), Payload, Capture))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // The stores, not the response, are what the simulation reads. Both must move: the mirror so
    // the value is live now, the emitter-stage store so it is still live after the next compile.
    TestEqual(TEXT("the mirror constant now carries the written value"),
        Store.GetParameterValueOrDefault<float>(Constant, 0.0f), WrittenValue);
    TestEqual(TEXT("the emitter-stage constant now carries the written value"),
        EmitterStore.GetParameterValueOrDefault<float>(Constant, 0.0f), WrittenValue);

    const TSharedPtr<FJsonObject>* ConstantReport = nullptr;
    if (TestTrue(TEXT("the response reports the rapid-iteration constant"),
            Capture.Result->TryGetObjectField(TEXT("rapidIteration"), ConstantReport))
        && ConstantReport != nullptr)
    {
        bool bShadowed = false;
        TestTrue(TEXT("`shadowed` is reported"), (*ConstantReport)->TryGetBoolField(TEXT("shadowed"), bShadowed));
        TestTrue(TEXT("the pre-existing constant is reported as a shadow"), bShadowed);
        TestEqual(TEXT("the write-through is reported as an update"),
            (*ConstantReport)->GetStringField(TEXT("action")), FString(TEXT("updated")));
        TestEqual(TEXT("the reported constant is the one that was seeded"),
            (*ConstantReport)->GetStringField(TEXT("parameter")), ConstantName.ToString());
        TestTrue(TEXT("the displaced value is reported"),
            !(*ConstantReport)->GetStringField(TEXT("previousValue")).IsEmpty());
        TestTrue(TEXT("the mirror store is among the reconciled scopes"),
            ScopesContain(*ConstantReport, TEXT("systemUpdateRapidIteration")));
        TestTrue(TEXT("the emitter-stage store is among the reconciled scopes"),
            ScopesContain(*ConstantReport, TEXT("emitterUpdateRapidIteration")));
    }

    // A reset must undo BOTH halves. Removing only the override pin would leave the constant this
    // call just authored standing, so the graph would revert while the runtime kept 20 — the same
    // defect inverted. The constant is removed rather than rewritten, because the next compile
    // reseeds a missing constant from the module's own default pin.
    TSharedPtr<FJsonObject> ResetPayload = MakeShared<FJsonObject>();
    ResetPayload->SetStringField(TEXT("assetPath"), System->GetPathName());
    ResetPayload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
    ResetPayload->SetStringField(TEXT("entryId"), ModuleNode->NodeGuid.ToString());
    ResetPayload->SetStringField(TEXT("inputName"), SpawnRateInput);
    ResetPayload->SetBoolField(TEXT("compile"), false);
    ResetPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture ResetCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.reset_module_input"), ResetPayload, ResetCapture))
    {
        TestEqual(TEXT("the reset removed the constant from the mirror"),
            Store.IndexOf(Constant), INDEX_NONE);
        TestEqual(TEXT("the reset removed the constant from the emitter-stage store"),
            EmitterStore.IndexOf(Constant), INDEX_NONE);

        const TSharedPtr<FJsonObject>* ResetReport = nullptr;
        if (TestTrue(TEXT("the reset response reports the rapid-iteration constant"),
                ResetCapture.Result->TryGetObjectField(TEXT("rapidIteration"), ResetReport))
            && ResetReport != nullptr)
        {
            TestEqual(TEXT("the reset is reported as a removal"),
                (*ResetReport)->GetStringField(TEXT("action")), FString(TEXT("removed")));
            TestEqual(TEXT("the reset names the same constant"),
                (*ResetReport)->GetStringField(TEXT("parameter")), ConstantName.ToString());
            TestTrue(TEXT("the reset names the emitter-stage store it cleared"),
                ScopesContain(*ResetReport, TEXT("emitterUpdateRapidIteration")));
        }
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ============================================================================
// 2. The compile snapshot restores exactly the values a rebuild changed, and re-adds nothing.
//
// The rebuild is simulated rather than compiled here so the assertion is deterministic on every
// host: what a compile does to a system-script store is overwrite surviving entries and drop
// entries its graph traversal no longer reaches, and both are reproduced directly.
//
// Counterfactual: revert FRapidIterationValueSnapshot::MergeBack (NiagaraRapidIteration.cpp) to a
// no-op and the first assertion reads back the rebuild's 1.0 instead of the authored 7.0 and the
// restored count is 0. Reverting instead to a blanket CopyParametersTo fails the last assertion,
// because the dropped parameter comes back — which is what makes the next compile rebuild again.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCompileRapidIterationSnapshotTest,
    "PinWright.niagara.compile.RapidIterationSnapshotMergesAuthoredValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileRapidIterationSnapshotTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraRapidIterationSyncTestLocal;

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            TEXT("the transient fixture system could not be built"));
        return true;
    }
    // The fixture factory roots both the system and its emitter; this owns that pin so both are
    // released on every exit path below, not only the ones calling DestroyFixture.
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    Roots.Emitter = FirstEmitter(*System);

    UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript();
    if (!TestNotNull(TEXT("system update script resolved"), SystemUpdateScript))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FNiagaraParameterStore& Store = SystemUpdateScript->RapidIterationParameters;
    const FNiagaraVariable Authored(FNiagaraTypeDefinition::GetFloatDef(), FName(SyntheticConstant));
    const FNiagaraVariable Dropped(FNiagaraTypeDefinition::GetFloatDef(),
        FName(*(FString(SyntheticConstant) + TEXT("_Dropped"))));
    constexpr float AuthoredValue = 7.0f;
    constexpr float TemplateDefault = 1.0f;
    SeedFloat(Store, Authored, AuthoredValue);
    SeedFloat(Store, Dropped, AuthoredValue);

    PinWrightNiagara::FRapidIterationValueSnapshot Snapshot;
    Snapshot.Capture(*System);
    TestTrue(TEXT("the snapshot captured the seeded entries"), Snapshot.Num() >= 2);

    // What a rebuild does: overwrite what it still reaches, drop what it does not.
    Store.SetParameterData(reinterpret_cast<const uint8*>(&TemplateDefault), Authored, /*bAdd=*/false);
    Store.RemoveParameter(Dropped);
    if (!TestEqual(TEXT("the simulated rebuild replaced the authored value"),
            Store.GetParameterValueOrDefault<float>(Authored, 0.0f), TemplateDefault))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    TestEqual(TEXT("exactly the changed value is restored"), Snapshot.MergeBack(), 1);
    TestEqual(TEXT("the authored value is back"),
        Store.GetParameterValueOrDefault<float>(Authored, 0.0f), AuthoredValue);
    TestEqual(TEXT("a parameter the rebuild dropped is not re-added"),
        Store.IndexOf(Dropped), INDEX_NONE);

    // A second merge over an unchanged store must write nothing, so a compile that reproduced
    // every value reports 0 rather than claiming work it did not do.
    TestEqual(TEXT("merging an unchanged store restores nothing"), Snapshot.MergeBack(), 0);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ============================================================================
// 3. niagara.compile runs the snapshot and reports honestly whether it ran.
//
// The mechanism is proven above; this is the wiring, end to end through the handler on a real
// compile of a real (transient) system. The contract asserted is the one a caller branches on:
// `rapidIterationMerge` always names what happened, and the count exists only when the merge
// actually ran — a `wait:false` call must publish null rather than a 0 the docs define as "the
// compile reproduced every value", because with rapid-iteration parameters baked out the rebuild
// is deferred past the request and a merge there would precede the thing it undoes.
//
// Counterfactual: remove the Capture/MergeBack calls and the two fields from
// NiagaraCompileHandler.cpp and the first assertion fails — the response carries no such key.
// Gate the merge on bRequested alone again and the wait:false call reports a number instead of
// null, failing the second half.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraCompileReportsRapidIterationPreservedTest,
    "PinWright.niagara.compile.ReportsRapidIterationPreserved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCompileReportsRapidIterationPreservedTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            TEXT("the transient fixture system could not be built"));
        return true;
    }
    // The fixture factory roots both the system and its emitter; this owns that pin so both are
    // released on every exit path below, not only the ones calling DestroyFixture.
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    Roots.Emitter = NiagaraRapidIterationSyncTestLocal::FirstEmitter(*System);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetBoolField(TEXT("force"), true);
    Payload->SetBoolField(TEXT("wait"), true);
    Payload->SetNumberField(TEXT("timeoutSeconds"), 30.0);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.compile"), Payload, Capture))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FString Merge;
    TestTrue(TEXT("the compile response names what the merge did"),
        Capture.Result->TryGetStringField(TEXT("rapidIterationMerge"), Merge));
    double Preserved = -1.0;
    const bool bHasCount = Capture.Result->TryGetNumberField(TEXT("rapidIterationPreserved"), Preserved);
    if (Merge == TEXT("merged"))
    {
        TestTrue(TEXT("a merge that ran publishes its count"), bHasCount);
        TestTrue(TEXT("the reported count is not negative"), Preserved >= 0.0);
    }
    else
    {
        TestFalse(TEXT("a merge that did not run publishes no count"), bHasCount);
    }

    // The wait:false form must never claim a count: the rebuild can be deferred past the request,
    // so there is nothing measured to report.
    TSharedPtr<FJsonObject> NoWaitPayload = MakeShared<FJsonObject>();
    NoWaitPayload->SetStringField(TEXT("assetPath"), System->GetPathName());
    NoWaitPayload->SetBoolField(TEXT("force"), false);
    NoWaitPayload->SetBoolField(TEXT("wait"), false);

    FTestResponseCapture NoWaitCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.compile"), NoWaitPayload, NoWaitCapture))
    {
        double NoWaitPreserved = -1.0;
        TestFalse(TEXT("a wait:false compile publishes no preserved count"),
            NoWaitCapture.Result->TryGetNumberField(TEXT("rapidIterationPreserved"), NoWaitPreserved));
        FString NoWaitMerge;
        TestTrue(TEXT("a wait:false compile still names why the merge did not run"),
            NoWaitCapture.Result->TryGetStringField(TEXT("rapidIterationMerge"), NoWaitMerge));
        TestNotEqual(TEXT("a wait:false compile does not report a merge"),
            NoWaitMerge, FString(TEXT("merged")));
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
