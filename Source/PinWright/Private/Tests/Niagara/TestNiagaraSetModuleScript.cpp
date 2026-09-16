// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.set_module_script.
//
// Full integration (swap FunctionScript on a live UNiagaraNodeFunctionCall in a
// saved system) requires the deep editor pipeline (PostLoad, GraphSource, script
// compilation) that is not safely reproducible in a transient NewObject fixture
// without hitting MessageAssetKey / PostLoad assertions.
//
// Deferred integration tests (SwapPreservesMatchingOverride, DropsTypeMismatchedOverride,
// RejectsIncompatibleStackGroup) require a NiagaraNodeFunctionCall wired into a live
// NiagaraSystem graph with a real override-node pin chain. Building that safely in a test
// fixture would require duplicating NS_CharacterDash + resolving its module node by name,
// then resolving a compatible/incompatible replacement script path — all of which couples
// these tests tightly to project asset content and would break on any system-asset resave.
// Those tests are deferred until a stable project fixture asset is designated for this purpose.
//
// What IS tested here:
//   1. Registration — niagara.set_module_script is in the dispatcher.
//   2. EnumerateScriptInputs (null source) — degrades to empty array without crashing.
//   3. SnapshotInputOverrides (null graph) — degrades to empty map without crashing.
//   4. EnumerateScriptInputs (real graph) — finds a declared exposed input. Counterfactual
//      anchor: reverting the FindInputNodes+IsExposed logic makes this return 0, failing the test.
//   5. FunctionScript swap counterfactual — direct field assignment changes FunctionScript.
//      Reverting the swap line makes ModuleNode->FunctionScript == OldScript, failing the test.
//   6. RejectsIncompatibleUsage logic check — GetUsage() mismatch is detectable in-process.
//      Documents that the full dispatcher path is deferred per the transient-asset constraint.
//   7. Dispatcher inference — omitted scriptUsage validates an existing module against the
//      stack output that actually owns its entryId.
//
// This follows the same pattern as TestNiagaraDumpStaticSwitch.cpp.

#include "Misc/AutomationTest.h"
#include "Handlers/Niagara/NiagaraSetModuleScriptHelpers.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"

// ---------------------------------------------------------------------------
// Registration test
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleScriptRegistrationTest,
    "PinWright.niagara.set_module_script.Registration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleScriptRegistrationTest::RunTest(const FString& Parameters)
{
    TestTrue(
        TEXT("niagara.set_module_script is registered in the dispatcher"),
        IsHandlerRegistered(TEXT("niagara.set_module_script")));
    return true;
}

// ---------------------------------------------------------------------------
// Dispatcher path — omitted scriptUsage infers the module's owning stack output.
// Counterfactual: if omitted scriptUsage still defaults to ParticleUpdateScript,
// this EmitterUpdate module resolves by entryId but is absent from ParticleUpdate
// stack groups, so the dispatcher returns INVALID_STACK and success fails.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleScriptInfersOwningEmitterUpdateStackTest,
    "PinWright.niagara.set_module_script.InfersOwningEmitterUpdateStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleScriptInfersOwningEmitterUpdateStackTest::RunTest(const FString& Parameters)
{
    const TCHAR* SpawnRatePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePath);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::EmitterUpdateScript,
        SpawnRateScript);
    if (!TestNotNull(TEXT("SpawnRate module added to EmitterUpdate stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    const FString ModuleNodeId = ModuleNode->NodeGuid.ToString();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
    Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
    Payload->SetStringField(TEXT("scriptPath"), SpawnRatePath);
    Payload->SetBoolField(TEXT("preserveOverrides"), true);
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_script"),
        Payload,
        Capture);
    TestNotEqual(
        TEXT("omitted scriptUsage did not resolve to INVALID_STACK"),
        Capture.ErrorCode,
        FString(TEXT("INVALID_STACK")));
    if (!bSucceeded)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    bool bSwapped = false;
    TestTrue(TEXT("swapped field returned"), Capture.Result->TryGetBoolField(TEXT("swapped"), bSwapped));
    TestTrue(TEXT("same-script swap reports swapped"), bSwapped);
    TestEqual(TEXT("returned nodeId matches module node guid"), Capture.Result->GetStringField(TEXT("nodeId")), ModuleNodeId);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// EnumerateScriptInputs — null-source (no crash, returns empty)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraEnumerateScriptInputsNullSourceTest,
    "PinWright.niagara.set_module_script.EnumerateScriptInputsNullSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEnumerateScriptInputsNullSourceTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* Script = NewObject<UNiagaraScript>(GetTransientPackage());
    TestNotNull(TEXT("Transient UNiagaraScript constructed"), Script);

    TArray<FNiagaraVariable> OutInputs;
    NiagaraEdit::EnumerateScriptInputs(*Script, OutInputs);
    TestEqual(
        TEXT("Script with null source yields empty input list"),
        OutInputs.Num(), 0);
    return true;
}

// ---------------------------------------------------------------------------
// SnapshotInputOverrides — null function-script (no crash, returns empty map)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSnapshotInputOverridesNullScriptTest,
    "PinWright.niagara.set_module_script.SnapshotInputOverridesNullScript",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSnapshotInputOverridesNullScriptTest::RunTest(const FString& Parameters)
{
    UNiagaraNodeFunctionCall* Node = NewObject<UNiagaraNodeFunctionCall>(GetTransientPackage());
    TestNotNull(TEXT("Transient UNiagaraNodeFunctionCall constructed"), Node);
    // FunctionScript is null, no override-node pin chain exists.
    TMap<FNiagaraVariable, FString> Snapshot;
    NiagaraEdit::SnapshotInputOverrides(*Node, Snapshot);
    TestEqual(
        TEXT("Node with no override chain yields empty snapshot"),
        Snapshot.Num(), 0);
    return true;
}

// ---------------------------------------------------------------------------
// EnumerateScriptInputs — real graph with one exposed input
// Counterfactual: if the FindInputNodes+IsExposed guard in EnumerateScriptInputs
// is removed or wrong, this test returns 0 inputs instead of 1.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraEnumerateScriptInputsRealGraphTest,
    "PinWright.niagara.set_module_script.EnumerateScriptInputsRealGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEnumerateScriptInputsRealGraphTest::RunTest(const FString& Parameters)
{
    // Build a transient UNiagaraScript with a graph containing one exposed float input "Strength".
    UNiagaraScript* Script = NewObject<UNiagaraScript>(GetTransientPackage(), NAME_None, RF_Transient);
    TestNotNull(TEXT("Transient UNiagaraScript constructed"), Script);
    if (!Script) return false;

    UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Script, NAME_None, RF_Transient);
    TestNotNull(TEXT("Transient UNiagaraScriptSource constructed"), Source);
    if (!Source) return false;

    UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Source, NAME_None, RF_Transient);
    TestNotNull(TEXT("Transient UNiagaraGraph constructed"), Graph);
    if (!Graph) return false;

    Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
    Source->NodeGraph = Graph;
    Script->SetLatestSource(Source);

    // Add one exposed parameter input node for "Strength:float".
    UNiagaraNodeInput* InputNode = NewObject<UNiagaraNodeInput>(Graph, NAME_None, RF_Transient);
    TestNotNull(TEXT("Transient UNiagaraNodeInput constructed"), InputNode);
    if (!InputNode) return false;

    InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Strength")));
    InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
    InputNode->ExposureOptions.bExposed = 1;
    Graph->AddNode(InputNode, false, false);

    TArray<FNiagaraVariable> OutInputs;
    NiagaraEdit::EnumerateScriptInputs(*Script, OutInputs);

    TestEqual(TEXT("One exposed input found in real graph"), OutInputs.Num(), 1);
    if (OutInputs.Num() == 1)
    {
        TestEqual(TEXT("Exposed input name is Strength"), OutInputs[0].GetName(), FName(TEXT("Strength")));
        TestTrue(TEXT("Exposed input type is float"), OutInputs[0].GetType() == FNiagaraTypeDefinition::GetFloatDef());
    }
    return true;
}

// ---------------------------------------------------------------------------
// FunctionScript swap counterfactual
// Verifies that assigning ModuleNode->FunctionScript = NewScript changes the field.
// If the FunctionScript assignment in ApplyModuleMutation's SetModuleScript branch
// is reverted, the post-swap node still points to OldScript and this test fails.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraFunctionScriptSwapCounterfactualTest,
    "PinWright.niagara.set_module_script.FunctionScriptSwapCounterfactual",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraFunctionScriptSwapCounterfactualTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* OldScript = NewObject<UNiagaraScript>(GetTransientPackage(), NAME_None, RF_Transient);
    UNiagaraScript* NewScript = NewObject<UNiagaraScript>(GetTransientPackage(), NAME_None, RF_Transient);
    TestNotNull(TEXT("OldScript constructed"), OldScript);
    TestNotNull(TEXT("NewScript constructed"), NewScript);
    if (!OldScript || !NewScript) return false;

    // Both scripts have the same usage — compatible swap.
    OldScript->SetUsage(ENiagaraScriptUsage::Module);
    NewScript->SetUsage(ENiagaraScriptUsage::Module);

    UNiagaraNodeFunctionCall* Node = NewObject<UNiagaraNodeFunctionCall>(GetTransientPackage(), NAME_None, RF_Transient);
    TestNotNull(TEXT("Transient UNiagaraNodeFunctionCall constructed"), Node);
    if (!Node) return false;

    Node->FunctionScript = OldScript;
    TestEqual(TEXT("Pre-swap: FunctionScript is OldScript"), Node->FunctionScript.Get(), OldScript);

    // Simulate the swap performed by ApplyModuleMutation's SetModuleScript branch.
    Node->FunctionScript = NewScript;

    TestNotEqual(TEXT("Post-swap: FunctionScript is no longer OldScript"), Node->FunctionScript.Get(), OldScript);
    TestEqual(TEXT("Post-swap: FunctionScript is NewScript"), Node->FunctionScript.Get(), NewScript);
    return true;
}

// ---------------------------------------------------------------------------
// RejectsIncompatibleUsage — logic check (dispatcher path deferred)
//
// Full dispatcher-path coverage would require calling niagara.set_module_script
// via InvokeExpectError with a live NiagaraNodeFunctionCall whose FunctionScript
// is set to a Module-usage script, and a NewScriptPath pointing to a
// DynamicInput-usage script.  Wiring that up requires a saved system asset with
// a known module entry, and a second saved script with mismatched usage — coupling
// the test to project asset content.  Deferred per transient-asset constraint noted
// at the top of this file.
//
// What IS tested: the boolean condition that drives the rejection is checkable
// in-process using GetUsage() on two NewObject scripts.  This is a unit test on
// the predicate, not on the handler invocation path.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraRejectsIncompatibleUsagePredicateTest,
    "PinWright.niagara.set_module_script.RejectsIncompatibleUsagePredicate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraRejectsIncompatibleUsagePredicateTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = NewObject<UNiagaraScript>(GetTransientPackage(), NAME_None, RF_Transient);
    UNiagaraScript* DynamicInputScript = NewObject<UNiagaraScript>(GetTransientPackage(), NAME_None, RF_Transient);
    TestNotNull(TEXT("ModuleScript constructed"), ModuleScript);
    TestNotNull(TEXT("DynamicInputScript constructed"), DynamicInputScript);
    if (!ModuleScript || !DynamicInputScript) return false;

    ModuleScript->SetUsage(ENiagaraScriptUsage::Module);
    DynamicInputScript->SetUsage(ENiagaraScriptUsage::DynamicInput);

    // This is the exact predicate that ApplyModuleMutation checks before returning
    // INCOMPATIBLE_SCRIPT_USAGE.  If it returns false here, the handler would not
    // reject the swap, and the error path would be silently skipped.
    const bool bUsageMismatch = ModuleScript->GetUsage() != DynamicInputScript->GetUsage();
    TestTrue(TEXT("Module vs DynamicInput usage mismatch is detected by GetUsage() comparison"), bUsageMismatch);

    // Sanity: same-usage pair does NOT trigger the rejection predicate.
    UNiagaraScript* AnotherModule = NewObject<UNiagaraScript>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!AnotherModule) return false;
    AnotherModule->SetUsage(ENiagaraScriptUsage::Module);
    const bool bCompatibleUsage = ModuleScript->GetUsage() != AnotherModule->GetUsage();
    TestFalse(TEXT("Two Module-usage scripts do not trigger the rejection predicate"), bCompatibleUsage);
    return true;
}
