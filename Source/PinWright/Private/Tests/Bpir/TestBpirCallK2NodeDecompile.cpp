// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirCallK2NodeDecompile.cpp - regression test for the residual scope of
// B-bpir-decompiler-emitter-coverage-gap: EmitGenericNode must append a
// `node_props { ... }` block built from the CDO diff so non-default UPROPERTY
// values on K2Node-fallback emits round-trip through the BPIR parser.
//
// Counterfactual: if the BuildSparsePropertyDiffJson call inside EmitGenericNode
// is reverted (or guarded out), the `node_props {` assertion below fails because
// no block is emitted at all — the output is the bare `call K2Node_<Type>(args)`
// form that history #3 mistakenly described as already complete.

#include "Misc/AutomationTest.h"

#include "BpirGraphTestHelpers.h"
#include "Decompiler/BpirTextEmitter.h"
#include "Engine/Blueprint.h"
#include "K2Node_InputKey.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCallK2NodeDecompileEmitsNodeProps,
    "PinWright.Bpir.CallK2Node.DecompileEmitsNodeProps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCallK2NodeDecompileEmitsNodeProps::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP)
    {
        AddError(TEXT("Failed to create transient Blueprint"));
        return false;
    }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph)
    {
        AddError(TEXT("Test Blueprint has no event graph"));
        return false;
    }

    // UK2Node_InputKey is the cleanest target with a non-default UPROPERTY that
    // survives BuildSparsePropertyDiffJson's CPF_Edit | CPF_BlueprintVisible filter:
    // bExecuteWhenPaused is declared `UPROPERTY(EditAnywhere, Category="Input")`
    // and zero-initialised by GENERATED_UCLASS_BODY. Flipping it to true gives a
    // single property in the diff so the assertions can target a specific name.
    UK2Node_InputKey* InputKeyNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_InputKey>(EventGraph);

    InputKeyNode->bExecuteWhenPaused = true;

    // EmitGenericNode is the public production entry point for the K2Node
    // fallback path. Calling it directly avoids the higher-level Decompile()
    // dispatcher (which routes UK2Node_InputKey to the entry-signature emitter
    // because IsBlueprintEntryNode covers it) and exercises the path we changed.
    FBpirTextEmitter Emitter;
    auto ResolvePin = [](UEdGraphPin* Pin) -> FString
    {
        if (!Pin) return FString();
        return Pin->DefaultValue;
    };

    const FString Output = Emitter.EmitGenericNode(
        InputKeyNode,
        /*ResultName=*/FString(),
        /*LabelMap=*/FBpirLabelMap{},
        ResolvePin);

    AddInfo(FString::Printf(TEXT("EmitGenericNode output: %s"), *Output));

    TestTrue(TEXT("Output contains 'call K2Node_InputKey('"),
        Output.Contains(TEXT("call K2Node_InputKey(")));
    TestTrue(TEXT("Output contains 'node_props {'"),
        Output.Contains(TEXT("node_props {")));

    // Verify the modified property name shows up inside the node_props block —
    // not just somewhere in the line — by slicing on the keyword.
    const int32 BlockStart = Output.Find(TEXT("node_props {"));
    const int32 BlockEnd = (BlockStart != INDEX_NONE) ? Output.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BlockStart) : INDEX_NONE;
    if (BlockStart != INDEX_NONE && BlockEnd != INDEX_NONE)
    {
        const FString Block = Output.Mid(BlockStart, BlockEnd - BlockStart + 1);
        TestTrue(TEXT("node_props block mentions bExecuteWhenPaused"),
            Block.Contains(TEXT("bExecuteWhenPaused")));
    }
    else
    {
        AddError(TEXT("Could not slice node_props block from output"));
    }
    return true;
}
