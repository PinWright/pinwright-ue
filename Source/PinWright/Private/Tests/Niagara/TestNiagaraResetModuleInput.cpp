// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.reset_module_input and niagara.clear_module_overrides.
//
// These tests cover:
//   1. Registration — both handlers present in the dispatcher.
//   2. FindStackFunctionOverrideNode on a transient UNiagaraNodeFunctionCall with no
//      called graph returns null (no crash).
//   3. NiagaraStaticSwitch::FindByName returns false for a null-graph node (no crash).
//   4. Integration: resetting a DYNAMIC-INPUT override removes only the override's own
//      nodes and leaves the stage's parameter-map chain intact
//      (B-niagara-reset-module-input-corrupts-stack).
//
// (4) uses the live-asset fixtures in TestNIRFixtures.h rather than a bare
// NewObject<UNiagaraSystem>, which cannot reach the editor pipeline the override-pin
// scaffolding needs — see that header for why.
//
// This follows the same pattern as TestNiagaraSetModuleScript.cpp.

#include "Misc/AutomationTest.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "Handlers/Niagara/NiagaraResetModuleInputHelpers.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

// ---------------------------------------------------------------------------
// Registration tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraResetModuleInputHandlersRegisteredTest,
    "PinWright.niagara.reset_module_input.HandlersRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraResetModuleInputHandlersRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(
        TEXT("niagara.reset_module_input is registered in the dispatcher"),
        IsHandlerRegistered(TEXT("niagara.reset_module_input")));

    TestTrue(
        TEXT("niagara.clear_module_overrides is registered in the dispatcher"),
        IsHandlerRegistered(TEXT("niagara.clear_module_overrides")));

    return true;
}

// ---------------------------------------------------------------------------
// FindStackFunctionOverrideNode — null called graph (no crash, returns null)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraFindStackFunctionOverrideNodeNullTest,
    "PinWright.niagara.reset_module_input.FindOverrideNodeNullGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraFindStackFunctionOverrideNodeNullTest::RunTest(const FString& Parameters)
{
    UNiagaraNodeFunctionCall* Node = NewObject<UNiagaraNodeFunctionCall>(GetTransientPackage());
    TestNotNull(TEXT("Transient UNiagaraNodeFunctionCall constructed"), Node);

    // No called graph, no override-node pin chain — must return null without crashing.
    UNiagaraNodeParameterMapSet* OverrideNode = NiagaraResetModuleInput::FindStackFunctionOverrideNode(*Node);
    TestNull(TEXT("FindStackFunctionOverrideNode returns null for node with no graph"), OverrideNode);

    // NiagaraStaticSwitch::FindByName must return false for a null called-graph (no crash).
    const UNiagaraNodeStaticSwitch* SwitchDecl = nullptr;
    const bool bFound = NiagaraStaticSwitch::FindByName(/*CalledGraph=*/nullptr, FName(TEXT("TestSwitch")), SwitchDecl);
    TestFalse(TEXT("FindByName returns false for null graph"), bFound);
    TestNull(TEXT("FindByName leaves OutDecl null for null graph"), SwitchDecl);
    return true;
}

// ---------------------------------------------------------------------------
// Integration: resetting a dynamic-input override must not consume the stage's stack
// (B-niagara-reset-module-input-corrupts-stack)
// ---------------------------------------------------------------------------

namespace
{
    // Uniquely prefixed so the unity build cannot ODR-collide these with the identically
    // shaped helpers in the sibling Niagara test translation units.
    UEdGraphPin* ResetStackTestMapInputPin(UNiagaraNode& Node)
    {
        TArray<UEdGraphPin*> Pins;
        Node.GetInputPins(Pins);
        return PinWrightNiagara::FindParameterMapPin(Pins);
    }

    // Name of the first non-parameter-map input pin. Discovered rather than hardcoded so a
    // future engine rename of Add_Float's input pins cannot silently turn the nested-override
    // half of the fixture into a no-op (same reasoning as TestNIRDecompiler's depth test).
    FName ResetStackTestFirstValueInputName(UNiagaraNode& Node)
    {
        TArray<UEdGraphPin*> Pins;
        Node.GetInputPins(Pins);
        const UEdGraphPin* MapPin = PinWrightNiagara::FindParameterMapPin(Pins);
        for (const UEdGraphPin* Pin : Pins)
        {
            if (Pin && Pin != MapPin)
            {
                return Pin->PinName;
            }
        }
        return NAME_None;
    }

    // Walk the parameter-map chain backward from a stage's output node. This is the chain
    // Niagara compiles the stage from and the chain every stack resolution walks; a stack that
    // has had nodes deleted out from under it stops early instead of reaching its head
    // UNiagaraNodeInput.
    TArray<UNiagaraNode*> ResetStackTestWalkChain(UNiagaraNodeOutput& OutputNode)
    {
        TArray<UNiagaraNode*> Visited;
        UNiagaraNode* CurrentNode = &OutputNode;
        while (CurrentNode && !Visited.Contains(CurrentNode))
        {
            Visited.Add(CurrentNode);
            UEdGraphPin* InputPin = ResetStackTestMapInputPin(*CurrentNode);
            if (!InputPin || InputPin->LinkedTo.Num() != 1 || !InputPin->LinkedTo[0])
            {
                break;
            }
            CurrentNode = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
        }
        return Visited;
    }

    bool ResetStackTestGraphContains(const UNiagaraGraph* Graph, const UEdGraphNode* Node)
    {
        if (!Graph || !Node)
        {
            return false;
        }
        for (const UEdGraphNode* GraphNode : Graph->Nodes)
        {
            if (GraphNode == Node)
            {
                return true;
            }
        }
        return false;
    }
}

// Counterfactual - what fails without the fix in
// NiagaraResetModuleInput::RemoveOverrideValueNode:
//
// The removal walk used to follow EVERY input pin of every node it reached upstream of the
// override pin and delete all of them. A dynamic input's parameter-map input pin is wired to
// the PREVIOUS STACK NODE's output pin (SetDynamicInputForFunctionInput does exactly that), so
// the walk left the override after one hop and deleted the upstream module call, its override
// map-set node and the stage's head UNiagaraNodeInput. With that walk in place the assertions
// below fail: the post-reset chain no longer contains the upstream module and no longer reaches
// a head UNiagaraNodeInput - precisely the state that made every later stack resolution on that
// stage miss and report INVALID_STACK, with no recovery short of rebuilding the emitter.
//
// The same assertions also fail on a half-fix that bounds the walk but forgets to relink:
// dropping the dynamic input's own override map-set node without reconnecting the previous
// stack node's output to that node's downstream consumers severs the chain at the same place.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraResetModuleInputDynamicInputKeepsStackChainTest,
    "PinWright.niagara.reset_module_input.DynamicInputResetKeepsStackChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraResetModuleInputDynamicInputKeepsStackChainTest::RunTest(const FString& Parameters)
{
    // Both are stock assets of the Niagara engine plugin, not host-project content.
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/Modules/Spawn/SpawnRate.SpawnRate"));
    UNiagaraScript* DynamicInputScript = LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float"));
    if (!ModuleScript || !DynamicInputScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("SpawnRate=%s Add_Float=%s"),
                ModuleScript ? TEXT("present") : TEXT("absent"),
                DynamicInputScript ? TEXT("present") : TEXT("absent")));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("ResetStackFixture")));
    if (!TestNotNull(TEXT("Fixture system created"), System))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        Roots.Emitter = Handle.GetInstance().Emitter.Get();
        break;
    }

    // Two modules so there is a real upstream neighbour for a runaway walk to eat. Each
    // AddModuleToStack appends at the tail, so the chain order is [Upstream, Target].
    UNiagaraNodeFunctionCall* UpstreamModule = NIRTestFixtures::AddModuleToStack(
        System, ENiagaraScriptUsage::ParticleSpawnScript, ModuleScript);
    UNiagaraNodeFunctionCall* TargetModule = NIRTestFixtures::AddModuleToStack(
        System, ENiagaraScriptUsage::ParticleSpawnScript, ModuleScript);
    if (!TestNotNull(TEXT("Upstream module added"), UpstreamModule) ||
        !TestNotNull(TEXT("Target module added"), TargetModule))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // Drive the target module's input from a dynamic input, and give that dynamic input a
    // nested dynamic input of its own so the removal also has to tear down the dynamic input's
    // private override map-set node and heal the chain across it.
    UNiagaraNodeFunctionCall* DynamicInputNode = NIRTestFixtures::SetModuleInputDynamicInput(
        TargetModule, FName(TEXT("SpawnRate")), DynamicInputScript);
    if (!TestNotNull(TEXT("Dynamic-input node created on the target module input"), DynamicInputNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    const FName NestedInputName = ResetStackTestFirstValueInputName(*DynamicInputNode);
    UNiagaraNodeFunctionCall* NestedDynamicInputNode = NestedInputName.IsNone()
        ? nullptr
        : NIRTestFixtures::SetModuleInputDynamicInput(DynamicInputNode, NestedInputName, DynamicInputScript);
    TestNotNull(TEXT("Nested dynamic-input node created on the dynamic input"), NestedDynamicInputNode);

    UNiagaraGraph* Graph = TargetModule->GetNiagaraGraph();
    UNiagaraNodeOutput* OutputNode = Graph
        ? Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSpawnScript, FGuid())
        : nullptr;
    if (!TestNotNull(TEXT("ParticleSpawn output node resolved"), OutputNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    const TArray<UNiagaraNode*> PreChain = ResetStackTestWalkChain(*OutputNode);
    if (!TestTrue(TEXT("Upstream module is in the ParticleSpawn chain before the reset"), PreChain.Contains(UpstreamModule)))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("ResetStackFixture"));
    Payload->SetStringField(TEXT("entryId"), TargetModule->NodeGuid.ToString());
    Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
    // Explicit scriptUsage removes the ambiguity B-niagara-module-input-stack-infer owns: a
    // failure here cannot be blamed on stack inference picking the wrong stage.
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("ParticleSpawnScript"));
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.reset_module_input"), Payload, Capture))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    TestTrue(TEXT("reset_module_input reports the override was reset"), Capture.Result->GetBoolField(TEXT("reset")));

    // The reset did its own job: the dynamic-input chain is gone from the graph.
    TestFalse(TEXT("Dynamic-input node removed from the graph"),
        ResetStackTestGraphContains(Graph, DynamicInputNode));
    if (NestedDynamicInputNode)
    {
        TestFalse(TEXT("Nested dynamic-input node removed from the graph"),
            ResetStackTestGraphContains(Graph, NestedDynamicInputNode));
    }

    // The fix: nothing outside the override was touched, and the map chain is still whole.
    const TArray<UNiagaraNode*> PostChain = ResetStackTestWalkChain(*OutputNode);
    TestTrue(TEXT("Upstream module still in the graph after the reset"),
        ResetStackTestGraphContains(Graph, UpstreamModule));
    TestTrue(TEXT("Upstream module still reachable along the ParticleSpawn chain after the reset"),
        PostChain.Contains(UpstreamModule));
    TestTrue(TEXT("Target module still reachable along the ParticleSpawn chain after the reset"),
        PostChain.Contains(TargetModule));

    bool bChainReachesHeadInputNode = false;
    for (UNiagaraNode* ChainNode : PostChain)
    {
        if (Cast<UNiagaraNodeInput>(ChainNode))
        {
            bChainReachesHeadInputNode = true;
            break;
        }
    }
    TestTrue(TEXT("ParticleSpawn chain still reaches its head UNiagaraNodeInput after the reset"),
        bChainReachesHeadInputNode);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
