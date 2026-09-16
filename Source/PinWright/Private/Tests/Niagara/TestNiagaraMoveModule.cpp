// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.move_module.
//
// The handler does real ParameterMap-chain surgery (DisconnectStackGroup +
// ConnectStackGroup in ApplyModuleMutation's MoveModule branch), but the chain
// reconnect alone does NOT update NodePosY. The editor's stack view and every
// NodePosY-sorted readback (niagara.inspect includeStack, the asset-dump stack
// arrays sorted in NiagaraDumpBuilder::AddGraphStackModules / NiagaraModelBuilder)
// order modules by NodePosY, so a move that only rewires the chain is invisible to
// those readbacks — the symptom B-niagara-move-module-noop reported as a "no-op".
//
// The fix mirrors the engine's own drag-drop reorder (FNiagaraStackGraphUtilities::
// MoveModule ends with RelayoutGraph) but inlines the minimal equivalent — the handler's
// vendored RelayoutModuleNodePositions rewrites the module nodes' NodePosY from the new
// chain order after the reconnect, so the move becomes visible. (A direct RelayoutGraph
// call would not link: that engine helper is not NIAGARAEDITOR_API.)
//
// Counterfactual: revert the relayout call in the move_module handler and the
// NodePosY-order assertion below fails — the three modules keep the identical
// pre-move NodePosY this test forces, so the NodePosY-sorted readback order no longer
// matches the (correctly reordered) ParameterMap chain.
//
// Follows the live-asset fixture pattern of TestNIRGraphLinkCoverage.cpp.

#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphPin.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "NiagaraGraph.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    // Return the parameter-map input pin of a node, reusing the plugin's own param-map pin
    // finder (the engine's FNiagaraStackGraphUtilities::GetParameterMapInputPin is not
    // NIAGARAEDITOR_API, so it cannot be linked cross-module from the plugin/tests).
    UEdGraphPin* ParameterMapInputPin(UNiagaraNode& Node)
    {
        TArray<UEdGraphPin*> Pins;
        Node.GetInputPins(Pins);
        return PinWrightNiagara::FindParameterMapPin(Pins);
    }

    // Walk the ParameterMap input-link chain backward from OutputNode, collecting the
    // module function-call nodes in execution order (top-of-stack first). This is the
    // authoritative stack order Niagara compiles from — independent of NodePosY — and
    // mirrors the handler's private GetOrderedModuleNodes.
    TArray<UNiagaraNodeFunctionCall*> GetChainOrderedModules(UNiagaraNodeOutput& OutputNode)
    {
        TArray<UNiagaraNodeFunctionCall*> Ordered;
        UNiagaraNode* CurrentNode = &OutputNode;
        while (CurrentNode)
        {
            UEdGraphPin* InputPin = ParameterMapInputPin(*CurrentNode);
            if (!InputPin || InputPin->LinkedTo.Num() != 1)
            {
                break;
            }
            UNiagaraNode* PreviousNode = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
            if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(PreviousNode))
            {
                Ordered.Insert(ModuleNode, 0);
            }
            CurrentNode = PreviousNode;
        }
        return Ordered;
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraMoveModuleRegistrationTest,
    "PinWright.niagara.move_module.Registration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraMoveModuleRegistrationTest::RunTest(const FString& Parameters)
{
    TestTrue(
        TEXT("niagara.move_module is registered in the dispatcher"),
        IsHandlerRegistered(TEXT("niagara.move_module")));
    return true;
}

// ---------------------------------------------------------------------------
// Integration: a move reorders the ParameterMap chain AND updates NodePosY so the
// NodePosY-sorted readback reflects the new order (the inline relayout fix).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraMoveModuleRelayoutsNodePosYTest,
    "PinWright.niagara.move_module.RelayoutsNodePosY",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraMoveModuleRelayoutsNodePosYTest::RunTest(const FString& Parameters)
{
    // SpawnRate is a stock CPU module ship-asset that wires cleanly into any usage stack.
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/Modules/Spawn/SpawnRate.SpawnRate"));
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("SpawnRate module not available in this test build; skipping."));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("MoveModuleFixture")));
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

    // Seed three modules into the ParticleUpdate stack. Each appends at the tail, so the
    // resulting chain order is [A, B, C].
    UNiagaraNodeFunctionCall* ModuleA = NIRTestFixtures::AddModuleToStack(System, ENiagaraScriptUsage::ParticleUpdateScript, ModuleScript);
    UNiagaraNodeFunctionCall* ModuleB = NIRTestFixtures::AddModuleToStack(System, ENiagaraScriptUsage::ParticleUpdateScript, ModuleScript);
    UNiagaraNodeFunctionCall* ModuleC = NIRTestFixtures::AddModuleToStack(System, ENiagaraScriptUsage::ParticleUpdateScript, ModuleScript);
    if (!TestNotNull(TEXT("Module A added"), ModuleA) ||
        !TestNotNull(TEXT("Module B added"), ModuleB) ||
        !TestNotNull(TEXT("Module C added"), ModuleC))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    UNiagaraGraph* Graph = ModuleC->GetNiagaraGraph();
    UNiagaraNodeOutput* OutputNode = Graph
        ? Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleUpdateScript, FGuid())
        : nullptr;
    if (!TestNotNull(TEXT("ParticleUpdate output node resolved"), OutputNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // Force a degenerate NodePosY state: all three modules share the same Y. This is the
    // stale state a chain-only reconnect leaves behind. With the bug, the move never
    // touches NodePosY, so the NodePosY-sorted readback can never reflect a reorder.
    ModuleA->NodePosY = 0;
    ModuleB->NodePosY = 0;
    ModuleC->NodePosY = 0;

    const TArray<UNiagaraNodeFunctionCall*> PreOrder = GetChainOrderedModules(*OutputNode);
    if (!TestEqual(TEXT("Three modules in the ParticleUpdate chain before the move"), PreOrder.Num(), 3))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // Move the tail module (chain index 2) to the front (chain index 0).
    UNiagaraNodeFunctionCall* MovedModule = PreOrder.Last();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("MoveModuleFixture"));
    Payload->SetStringField(TEXT("entryId"), MovedModule->NodeGuid.ToString());
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("ParticleUpdateScript"));
    Payload->SetNumberField(TEXT("toIndex"), 0);
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.move_module"), Payload, Capture))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    TestEqual(TEXT("operation is move_module"), Capture.Result->GetStringField(TEXT("operation")), FString(TEXT("move_module")));

    // 1. The ParameterMap chain — the authoritative execution order — actually changed:
    //    the moved module is now first in the chain. (Guards against a real no-op regression.)
    const TArray<UNiagaraNodeFunctionCall*> PostOrder = GetChainOrderedModules(*OutputNode);
    if (!TestEqual(TEXT("Three modules remain in the chain after the move"), PostOrder.Num(), 3))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    TestEqual(TEXT("Moved module is first in the ParameterMap chain after the move"), PostOrder[0], MovedModule);
    TestNotEqual(TEXT("Chain order actually changed (moved module was last before the move)"), PreOrder[0], PostOrder[0]);

    // 2. The fix: the handler's vendored RelayoutModuleNodePositions rewrote NodePosY from
    //    the new chain order, so the NodePosY values are no longer all identical AND the
    //    NodePosY-sorted order matches the chain order. Without that relayout call, all three
    //    NodePosY stay at the forced 0 and both of these assertions fail.
    const bool bNodePosYDistinct =
        !(MovedModule->NodePosY == PostOrder[1]->NodePosY && PostOrder[1]->NodePosY == PostOrder[2]->NodePosY);
    TestTrue(TEXT("NodePosY values are no longer all identical after the move (relayout ran)"), bNodePosYDistinct);

    // The relayout assigns the output-adjacent module the smallest Y and each module further
    // up the chain a larger Y, so the ascending-NodePosY order — exactly how the inspect/dump
    // readback (NiagaraDumpBuilder::AddGraphStackModules) sorts and indexes modules — equals
    // the chain order reversed (output-adjacent module first). The readback index must
    // therefore track the new chain order; with the bug (no relayout) the forced-equal
    // NodePosY leaves this order as the NodeGuid tiebreak, unrelated to the move.
    TArray<UNiagaraNodeFunctionCall*> NodePosYSorted = PostOrder;
    NodePosYSorted.Sort([](const UNiagaraNodeFunctionCall& X, const UNiagaraNodeFunctionCall& Y)
    {
        if (X.NodePosY != Y.NodePosY)
        {
            return X.NodePosY < Y.NodePosY;
        }
        return X.NodeGuid.ToString() < Y.NodeGuid.ToString();
    });

    TArray<UNiagaraNodeFunctionCall*> ExpectedReadbackOrder = PostOrder;
    Algo::Reverse(ExpectedReadbackOrder);

    bool bOrdersAgree = true;
    for (int32 Index = 0; Index < PostOrder.Num(); ++Index)
    {
        if (NodePosYSorted[Index] != ExpectedReadbackOrder[Index])
        {
            bOrdersAgree = false;
            break;
        }
    }
    TestTrue(
        TEXT("NodePosY-sorted readback order tracks the new ParameterMap chain order after the move"),
        bOrdersAgree);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
