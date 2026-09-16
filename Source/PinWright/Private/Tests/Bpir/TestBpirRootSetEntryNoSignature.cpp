// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for the anonymous-entry-stub defect: an AnimGraph dumped 24 blocks of
// `entry event UnknownEntry() { }` where it used to carry one
// `# (graph has no decompiled bodies)` marker (ABP_Manny, ThirdPerson_AnimBP,
// ABP_*_PostProcess, AB_IK_RobotArm).
//
// Chain: IsBlueprintEntryNode's class-agnostic backstop (IsEngineCompileRootSetNode)
// admits any impure UK2Node with no input pins, plus anything opting in through
// UK2Node::IsNodeRootSet() — AnimGraph nodes do. Every admitted node then rendered as an
// `entry ... {}` block, and FBpirTextEmitter::EmitEntrySignature has no grammar for them,
// so each one rendered anonymously. The decompiler now drops an entry whose signature is
// the UnknownEntrySignature fallback and which gates no connected exec output, which
// leaves the graph with zero entries and restores the empty-graph marker.
//
// COUNTERFACTUAL: remove the skip in BpirDecompiler.cpp and this graph emits
// `entry event UnknownEntry() {` instead of the marker.
#include "TestBpirRootSetEntryNoSignature.h"
#include "Misc/AutomationTest.h"
#include "BpirGraphTestHelpers.h"

#include "Decompiler/BpirDecompiler.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Utils/AssetDumpBuilder.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirRootSetEntryNoSignature_EmitsMarkerNotAnonymousStub,
    "PinWright.bpir.decompiler.EmptyEntryConsistency.RootSetNodeWithoutSignatureEmitsMarker",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirRootSetEntryNoSignature_EmitsMarkerNotAnonymousStub::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, TEXT("BpirRootSetOnlyFn"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph) { AddError(TEXT("Failed to create function graph")); return false; }
    BP->FunctionGraphs.Add(NewGraph);

    UTestBpirRootSetOnlyNode* Node =
        BpirGraphTestHelpers::AddNodeToGraph<UTestBpirRootSetOnlyNode>(NewGraph);
    if (!Node) { AddError(TEXT("Failed to create root-set node")); return false; }

    // The premise: this node IS admitted as an entry point (shape clause), so the fix has
    // to be in what the decompiler does with it, not in whether it sees it.
    TestTrue(TEXT("Root-set-shaped node is admitted as a Blueprint entry node"),
        BlueprintHandlerUtils::IsBlueprintEntryNode(Node));
    TestEqual(TEXT("Node has no input pins (the root-set shape clause)"),
        Node->Pins.FilterByPredicate([](UEdGraphPin* Pin)
            { return Pin && Pin->Direction == EGPD_Input; }).Num(), 0);

    FBpirDecompiler Decompiler(BP);
    const FBpirDecompileResult Result = Decompiler.DecompileFunction(TEXT("BpirRootSetOnlyFn"));
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(FString::Printf(TEXT("BpirText is empty (text='%s')"), *Result.BpirText),
        Result.BpirText.IsEmpty());
    TestEqual(TEXT("EmptyReason is UnreachableGraph"),
        static_cast<uint8>(Result.EmptyReason),
        static_cast<uint8>(EBpirEmptyReason::UnreachableGraph));

    const FString Output = AssetDumpBuilder::BuildBpirText(BP);
    TestFalse(FString::Printf(TEXT("No anonymous entry stub is emitted (text='%s')"), *Output),
        Output.Contains(TEXT("UnknownEntry")));
    TestTrue(FString::Printf(TEXT("The empty-graph marker is emitted instead (text='%s')"), *Output),
        Output.Contains(TEXT("# (graph has no decompiled bodies)")));

    return true;
}
