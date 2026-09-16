// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for B-bpir-no-decompiled-bodies-vs-empty-inconsistency. The empty-graph
// representation is now canonical: any recognized entry point (FunctionEntry / Event /
// CustomEvent / Tunnel) renders as an `entry ... {}` block even when its body is empty,
// and the `# (graph has no decompiled bodies)` marker is reserved for graphs that have
// nodes but expose no entry point the walker can seed from (EBpirEmptyReason::UnreachableGraph).
#include "Misc/AutomationTest.h"
#include "BpirGraphTestHelpers.h"

#include "Decompiler/BpirDecompiler.h"
#include "Utils/AssetDumpBuilder.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"

// An empty BeginPlay event stub (no connected exec pins) must render its entry signature,
// never the empty-graph marker. Before the fix a single empty auto-event collapsed the
// graph to `# (graph has no decompiled bodies)`, which was indistinguishable from a graph
// the walker never reached.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmptyEntryConsistency_EventGraphWithAutoEntry_EmitsEntryNotMarker,
    "PinWright.bpir.decompiler.EmptyEntryConsistency.EventGraphEmitsEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmptyEntryConsistency_EventGraphWithAutoEntry_EmitsEntryNotMarker::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // A BeginPlay event with no downstream exec wiring — the empty auto-stub case.
    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    if (!BeginPlayNode) { AddError(TEXT("Failed to create BeginPlay node")); return false; }

    // Sanity: the stub must genuinely have no connected exec output (no body to decompile).
    bool bHasConnectedExec = false;
    for (UEdGraphPin* Pin : BeginPlayNode->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            && Pin->LinkedTo.Num() > 0)
        {
            bHasConnectedExec = true;
            break;
        }
    }
    TestFalse(TEXT("BeginPlay stub has no connected exec output"), bHasConnectedExec);

    const FString Output = AssetDumpBuilder::BuildBpirText(BP);

    TestTrue(FString::Printf(TEXT("Output emits BeginPlay entry signature (text='%s')"), *Output),
        Output.Contains(TEXT("entry event BeginPlay()")));
    TestFalse(FString::Printf(TEXT("Output has no empty-graph marker for the event graph (text='%s')"), *Output),
        Output.Contains(TEXT("# (graph has no decompiled bodies)")));

    return true;
}

// Companion: a graph with nodes but zero recognized entry points (only a comment node) has
// no entry block to render, so it consistently falls back to the same marker form. This
// pins the one remaining case the marker is reserved for, ensuring it is not reused for
// graphs that do expose entry points.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmptyEntryConsistency_GraphWithOnlyComments_StillEmitsMarker,
    "PinWright.bpir.decompiler.EmptyEntryConsistency.CommentOnlyEmitsMarker",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmptyEntryConsistency_GraphWithOnlyComments_StillEmitsMarker::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    // Add a function graph holding only a comment node: Graph->Nodes.Num() > 0 (so the
    // dump-builder's ZeroNodes override stays off) but FindEntryPoints() returns nothing.
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, TEXT("BpirCommentOnlyFn"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph) { AddError(TEXT("Failed to create function graph")); return false; }
    BP->FunctionGraphs.Add(NewGraph);

    UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(NewGraph);
    if (!CommentNode) { AddError(TEXT("Failed to create comment node")); return false; }
    NewGraph->Nodes.Add(CommentNode);
    TestTrue(TEXT("Function graph has at least one node"), NewGraph->Nodes.Num() > 0);

    FBpirDecompiler Decompiler(BP);
    const FBpirDecompileResult Result = Decompiler.DecompileFunction(TEXT("BpirCommentOnlyFn"));
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("BpirText is empty (no recognised entry points)"), Result.BpirText.IsEmpty());
    TestEqual(TEXT("EmptyReason is UnreachableGraph"),
        static_cast<uint8>(Result.EmptyReason),
        static_cast<uint8>(EBpirEmptyReason::UnreachableGraph));

    const FString Output = AssetDumpBuilder::BuildBpirText(BP);
    TestTrue(FString::Printf(TEXT("Comment-only graph still emits the marker (text='%s')"), *Output),
        Output.Contains(TEXT("# (graph has no decompiled bodies)")));

    return true;
}
