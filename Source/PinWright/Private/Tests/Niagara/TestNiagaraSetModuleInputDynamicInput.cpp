// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red regression test for F-niagara-dynamic-input-authoring:
// niagara.set_module_input cannot assign a dynamic-input chain to a module input.
//
// niagara.set_module_input recognizes exactly two value shapes: a { link|parameter: "..." }
// object (linked-attribute bind) and a literal (bool / number / vector / color). A value of
// the form { "dynamicInput": "<ScriptAssetPath>" } carries none of the link keys, so
// TryGetLinkedParameterRequest returns false and it falls through to InferNiagaraInputType,
// whose JsonValueToPinDefaultString has no dynamic-input case and returns an empty string —
// the fresh-pin path then rejects the call with UNSUPPORTED_INPUT_VALUE
// (NiagaraEditHandler.cpp: ApplyModuleMutation SetModuleInput branch, ~:757-905).
//
// Dynamic inputs are how idiomatic Niagara content parameterizes a scalar module input
// (curves over life, random ranges, multiply chains), so the correct behavior is: assigning a
// float dynamic-input script to a float module input SUCCEEDS and wires the override pin to a
// dynamic-input function-call node driven by that script. This test asserts that correct
// behavior; it FAILS on current source because the value shape is rejected outright.
//
// Fixture combo (SpawnRate module's float "SpawnRate" input + the Add_Float float dynamic
// input) is the same type-compatible pairing the NIR override-chain tests build directly via
// NIRTestFixtures::SetModuleInputDynamicInput — here it is driven through the PRODUCTION
// niagara.set_module_input RPC instead, which is the capability the ticket says is missing.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

namespace
{
    // Resolve the graph pin carrying PinId on the module node's owning Niagara graph.
    // Uniquely named (not FindOverridePin, which the sibling TestNiagaraSetModuleInput.cpp
    // already defines in an anonymous namespace) so the two do not ODR-collide when the
    // unity build merges the translation units.
    UEdGraphPin* ResolvePinByGuidForDynInput(UNiagaraNodeFunctionCall* ModuleNode, const FString& PinId)
    {
        if (!ModuleNode || PinId.IsEmpty())
        {
            return nullptr;
        }
        if (UNiagaraGraph* Graph = ModuleNode->GetNiagaraGraph())
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin && Pin->PinId.ToString() == PinId)
                    {
                        return Pin;
                    }
                }
            }
        }
        return nullptr;
    }

    // True when the override pin is driven by a dynamic-input node — i.e. it is wired upstream
    // to a UNiagaraNodeFunctionCall whose FunctionScript is the requested dynamic-input script.
    // This is the structural signature of a real dynamic-input chain (as opposed to a literal
    // default or a parameter link); it mirrors the wiring NIRTestFixtures::SetModuleInputDynamicInput
    // establishes via FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput.
    bool PinReadsDynamicInputScript(UEdGraphPin* OverridePin, UNiagaraScript* DynamicInputScript)
    {
        if (!OverridePin || !DynamicInputScript || OverridePin->LinkedTo.Num() == 0)
        {
            return false;
        }
        for (UEdGraphPin* UpstreamPin : OverridePin->LinkedTo)
        {
            UEdGraphNode* UpstreamNode = UpstreamPin ? UpstreamPin->GetOwningNode() : nullptr;
            if (UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(UpstreamNode))
            {
                if (FunctionNode->FunctionScript == DynamicInputScript)
                {
                    return true;
                }
            }
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// Assigning a float dynamic-input script to a float module input succeeds and wires a
// dynamic-input chain (F-niagara-dynamic-input-authoring).
//
// Expected-green behavior: set_module_input accepts { dynamicInput: "<script>" }, does NOT
// reject it as UNSUPPORTED_INPUT_VALUE, and the override pin ends up driven by a
// dynamic-input function-call node running the requested script.
//
// Pre-fix (current source): the { dynamicInput: ... } object matches no link key and has no
// vector/color spelling, so InferNiagaraInputType returns false and ApplyModuleMutation
// returns UNSUPPORTED_INPUT_VALUE. InvokeExpectSuccess's bSuccess assertion fails and the
// UNSUPPORTED_INPUT_VALUE assertion fails — this test goes Result={Fail}, reproducing the
// defect. It flips green only once the handler learns the dynamic-input value mode.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputAssignsDynamicInputChainTest,
    "PinWright.niagara.set_module_input.AssignsDynamicInputChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputAssignsDynamicInputChainTest::RunTest(const FString& Parameters)
{
    const TCHAR* SpawnRatePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePath);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
    {
        return false;
    }

    // Add_Float is a stock float DynamicInput-usage script shipped with the Niagara plugin —
    // the same one the NIR override-chain tests load. A missing required fixture is a FAILURE.
    const TCHAR* DynamicInputPath = TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float");
    UNiagaraScript* DynamicInputScript = LoadObject<UNiagaraScript>(nullptr, DynamicInputPath);
    if (!TestNotNull(TEXT("Add_Float dynamic-input script loads"), DynamicInputScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    // SpawnRate is an EmitterUpdate-stage module whose "SpawnRate" input is a float — the
    // scalar-input shape a float dynamic input is meant to drive.
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

    // Drive the production RPC with a dynamic-input value shape: { dynamicInput: "<script>" }.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
    Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
    Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
    TSharedPtr<FJsonObject> DynamicInputValue = MakeShared<FJsonObject>();
    DynamicInputValue->SetStringField(TEXT("dynamicInput"), DynamicInputPath);
    Payload->SetObjectField(TEXT("value"), DynamicInputValue);
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("EmitterUpdateScript"));
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        Payload,
        Capture);
    // The defect symptom: the dynamic-input value shape is rejected as UNSUPPORTED_INPUT_VALUE
    // because it falls through to the literal-only InferNiagaraInputType path.
    TestNotEqual(
        TEXT("dynamic-input value not rejected as UNSUPPORTED_INPUT_VALUE"),
        Capture.ErrorCode,
        FString(TEXT("UNSUPPORTED_INPUT_VALUE")));
    if (!bSucceeded)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // Load-bearing structural assertion (reached only in the green state): the override pin is
    // actually driven by a dynamic-input node running Add_Float — a real chain, not a literal.
    const FString PinId = Capture.Result->GetStringField(TEXT("pinId"));
    TestFalse(TEXT("pinId returned for the dynamic-input override"), PinId.IsEmpty());
    UEdGraphPin* OverridePin = ResolvePinByGuidForDynInput(ModuleNode, PinId);
    TestNotNull(TEXT("override pin resolved by id"), OverridePin);
    TestTrue(
        TEXT("override pin is driven by the Add_Float dynamic-input node (real chain, not a literal)"),
        PinReadsDynamicInputScript(OverridePin, DynamicInputScript));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
