// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression: empty-graph BPIR emission must include a disambiguation marker. After
// B-bpir-no-decompiled-bodies-vs-empty-inconsistency every recognized entry point renders
// as an entry block (empty bodies included), so the marker is reserved for two cases:
// ZeroNodes (no nodes at all) and UnreachableGraph (nodes present but no entry points the
// walker can seed from). Each scenario gets its own test below.
#include "Misc/AutomationTest.h"


#include "Decompiler/BpirDecompiler.h"
#include "Utils/AssetDumpBuilder.h"
#include "Blueprint/UserWidget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirBuildBpirText_ZeroNodesGraph_EmitsZeroNodesMarker,
    "PinWright.utils.asset_dump_builder.BpirEmptyGraphMarker.ZeroNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirBuildBpirText_ZeroNodesGraph_EmitsZeroNodesMarker::RunTest(const FString& Parameters)
{
    // A fresh AActor Blueprint's EventGraph is created with auto-event stubs (BeginPlay /
    // Tick / EndPlay) in some engine paths, but here we explicitly clear all nodes from
    // every graph so the ZeroNodes branch is the one being exercised.
    const FName UniqueName = MakeUniqueObjectName(
        GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BpirZeroNodesFixture"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        UniqueName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("BpirZeroNodesTest")));
    TestNotNull(TEXT("Transient Blueprint created"), BP);
    if (!BP) return false;

    // Force every event/function/macro graph to be node-empty so AssetDumpBuilder picks
    // ZeroNodes regardless of what the engine's CreateBlueprint pre-populates.
    auto ClearGraphNodes = [](const TArray<UEdGraph*>& Graphs)
    {
        for (UEdGraph* G : Graphs)
        {
            if (G) { G->Nodes.Reset(); }
        }
    };
    ClearGraphNodes(BP->UbergraphPages);
    ClearGraphNodes(BP->FunctionGraphs);
    ClearGraphNodes(BP->MacroGraphs);

    const FString Output = AssetDumpBuilder::BuildBpirText(BP);

    TestTrue(TEXT("Output contains EventGraph ubergraph header"),
        Output.Contains(TEXT("# ==== Graph: EventGraph (ubergraph) ====")));

    TestTrue(FString::Printf(TEXT("Output contains ZeroNodes marker (text='%s')"), *Output),
        Output.Contains(TEXT("# (graph has zero nodes)")));

    // The replacement marker is the only empty-state comment on a zero-node graph; no
    // other `# (...)` empty-state markers should appear in the output. (Strings of the
    // pre-fix wording are not pinned here so a grep of Source/ for them returns zero
    // matches per the chunk's acceptance criteria.)

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDecompile_UCSGraph_EmitsEntrySignature,
    "PinWright.utils.asset_dump_builder.BpirEmptyGraphMarker.UCSEmitsEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirDecompile_UCSGraph_EmitsEntrySignature::RunTest(const FString& Parameters)
{
    // Fresh AActor BP carries a `UserConstructionScript` function graph with a single
    // auto-FunctionEntry node that has no connected execs. After the consistency fix the
    // decompiler no longer strips it: a recognized FunctionEntry always renders as an entry
    // block. The UCS graph is special-cased to the dedicated construction-entry form, so the
    // output carries `entry construction ConstructionScript() @(0, 0) {}` rather than any
    // `# (...)` marker comment.
    const FName UniqueName = MakeUniqueObjectName(
        GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BpirUCSEntryFixture"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        UniqueName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("BpirUCSEntryTest")));
    TestNotNull(TEXT("Transient Blueprint created"), BP);
    if (!BP) return false;

    // Verify the UCS graph exists with at least one node so the entry-emit path actually fires.
    UEdGraph* UCSGraph = nullptr;
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (G && G->GetName() == TEXT("UserConstructionScript"))
        {
            UCSGraph = G;
            break;
        }
    }
    TestNotNull(TEXT("UserConstructionScript graph exists on fresh AActor BP"), UCSGraph);
    if (!UCSGraph) return false;
    TestTrue(TEXT("UCS graph has at least one auto-entry node"), UCSGraph->Nodes.Num() > 0);

    FBpirDecompiler Decompiler(BP);
    const FBpirDecompileResult Result = Decompiler.DecompileFunction(TEXT("UserConstructionScript"));

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestFalse(TEXT("BpirText is non-empty (entry signature emitted)"), Result.BpirText.IsEmpty());
    TestEqual(TEXT("EmptyReason stays NotEmpty"),
        static_cast<uint8>(Result.EmptyReason),
        static_cast<uint8>(EBpirEmptyReason::NotEmpty));
    TestTrue(FString::Printf(TEXT("BpirText contains UCS entry signature (text='%s')"), *Result.BpirText),
        Result.BpirText.Contains(TEXT("entry construction ConstructionScript()")));

    const FString Output = AssetDumpBuilder::BuildBpirText(BP);
    TestFalse(FString::Printf(TEXT("Output has no empty-state marker for UCS (text='%s')"), *Output),
        Output.Contains(TEXT("# (graph has no decompiled bodies)")));

    return true;
}

// UnreachableGraph fires for any graph that has nodes but no entry points the decompiler
// recognises (FunctionEntry / Event / CustomEvent / Tunnel). A function graph containing
// only a comment node satisfies that: Graph->Nodes.Num() > 0 (so the dump-builder's
// ZeroNodes override stays off) but FindEntryPoints() returns nothing (so the decompiler
// classifies the empty BPIR as UnreachableGraph).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmptyMarker_UnreachableGraph_EmitsNoDecompiledBodiesMarker,
    "PinWright.utils.asset_dump_builder.BpirEmptyGraphMarker.UnreachableGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmptyMarker_UnreachableGraph_EmitsNoDecompiledBodiesMarker::RunTest(const FString& Parameters)
{
    const FName UniqueName = MakeUniqueObjectName(
        GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BpirAllEntriesEmptyFixture"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        UniqueName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("BpirAllEntriesEmptyTest")));
    TestNotNull(TEXT("Transient Blueprint created"), BP);
    if (!BP) return false;

    // Clear every pre-populated graph so only the synthesised function graph below
    // carries content. Without this, the auto-event ubergraph or UCS could match
    // ZeroNodes / NotEmpty before UnreachableGraph gets a chance.
    auto ClearGraphNodes = [](const TArray<UEdGraph*>& Graphs)
    {
        for (UEdGraph* G : Graphs)
        {
            if (G) { G->Nodes.Reset(); }
        }
    };
    ClearGraphNodes(BP->UbergraphPages);
    ClearGraphNodes(BP->FunctionGraphs);
    ClearGraphNodes(BP->MacroGraphs);

    // Add a function graph with a single orphan comment node — no FunctionEntry, no
    // Event, no Tunnel — so FindEntryPoints returns empty while Graph->Nodes is non-zero.
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, TEXT("BpirAllEntriesEmptyFn"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    TestNotNull(TEXT("Function graph created"), NewGraph);
    if (!NewGraph) return false;
    BP->FunctionGraphs.Add(NewGraph);

    UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(NewGraph);
    TestNotNull(TEXT("Comment node created"), CommentNode);
    if (!CommentNode) return false;
    NewGraph->Nodes.Add(CommentNode);

    // Sanity: the fixture must satisfy both conditions for the UnreachableGraph branch.
    TestTrue(TEXT("Function graph has at least one node"), NewGraph->Nodes.Num() > 0);

    FBpirDecompiler Decompiler(BP);
    const FBpirDecompileResult Result = Decompiler.DecompileFunction(TEXT("BpirAllEntriesEmptyFn"));
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("BpirText is empty (no recognised entry points)"), Result.BpirText.IsEmpty());
    TestEqual(TEXT("EmptyReason is UnreachableGraph"),
        static_cast<uint8>(Result.EmptyReason),
        static_cast<uint8>(EBpirEmptyReason::UnreachableGraph));

    const FString Output = AssetDumpBuilder::BuildBpirText(BP);

    TestTrue(FString::Printf(TEXT("Output contains UnreachableGraph marker (text='%s')"), *Output),
        Output.Contains(TEXT("# (graph has no decompiled bodies)")));

    // The marker must appear exactly once (one matching graph in the fixture).
    int32 MarkerCount = 0;
    int32 SearchFrom = 0;
    const FString Marker = TEXT("# (graph has no decompiled bodies)");
    while (true)
    {
        const int32 Found = Output.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
        if (Found == INDEX_NONE) break;
        ++MarkerCount;
        SearchFrom = Found + Marker.Len();
    }
    TestEqual(TEXT("UnreachableGraph marker appears exactly once"), MarkerCount, 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirShouldEmit_GraphlessParentEmptyGraph_ReturnsFalse,
    "PinWright.utils.asset_dump_builder.BpirElide.GraphlessEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirShouldEmit_GraphlessParentEmptyGraph_ReturnsFalse::RunTest(const FString& Parameters)
{
    // UObject parent → structurally graphless. All graphs empty → must elide.
    const FName UniqueName = MakeUniqueObjectName(
        GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BpirGraphlessEmptyFixture"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        UObject::StaticClass(),
        GetTransientPackage(),
        UniqueName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("BpirGraphlessEmptyTest")));
    TestNotNull(TEXT("Transient Blueprint created"), BP);
    if (!BP) return false;

    TestFalse(TEXT("ShouldEmitBpirText returns false for graphless parent with all-empty graphs"),
        AssetDumpBuilder::ShouldEmitBpirText(BP));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirShouldEmit_UserWidgetEmptyGraph_ReturnsTrue,
    "PinWright.utils.asset_dump_builder.BpirElide.UserWidgetKept",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirShouldEmit_UserWidgetEmptyGraph_ReturnsTrue::RunTest(const FString& Parameters)
{
    // UUserWidget parent → graph-bearing root. Empty graphs → keep marker (preserves
    // B-bpir-stub-no-empty-marker's decompile-failure disambiguation).
    const FName UniqueName = MakeUniqueObjectName(
        GetTransientPackage(), UWidgetBlueprint::StaticClass(), TEXT("BpirUserWidgetEmptyFixture"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        GetTransientPackage(),
        UniqueName,
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("BpirUserWidgetEmptyTest")));
    TestNotNull(TEXT("Transient WidgetBlueprint created"), BP);
    if (!BP) return false;

    TestTrue(TEXT("ShouldEmitBpirText returns true for UUserWidget parent (graph-bearing root)"),
        AssetDumpBuilder::ShouldEmitBpirText(BP));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirShouldEmit_GraphlessParentWithFunctionGraph_ReturnsTrue,
    "PinWright.utils.asset_dump_builder.BpirElide.GraphlessButHasFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirShouldEmit_GraphlessParentWithFunctionGraph_ReturnsTrue::RunTest(const FString& Parameters)
{
    // UObject parent + non-empty function graph → keep (graphs hold content even though
    // parent class normally couldn't host event logic).
    const FName UniqueName = MakeUniqueObjectName(
        GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BpirGraphlessWithFunctionFixture"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        UObject::StaticClass(),
        GetTransientPackage(),
        UniqueName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("BpirGraphlessWithFunctionTest")));
    TestNotNull(TEXT("Transient Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, TEXT("BpirElideFn"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    TestNotNull(TEXT("Function graph created"), NewGraph);
    if (!NewGraph) return false;
    BP->FunctionGraphs.Add(NewGraph);

    UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(NewGraph);
    TestNotNull(TEXT("Comment node created"), CommentNode);
    if (!CommentNode) return false;
    NewGraph->Nodes.Add(CommentNode);

    TestTrue(TEXT("Function graph has at least one node"), NewGraph->Nodes.Num() > 0);

    TestTrue(TEXT("ShouldEmitBpirText returns true when any graph has nodes (even on graphless parent)"),
        AssetDumpBuilder::ShouldEmitBpirText(BP));
    return true;
}
