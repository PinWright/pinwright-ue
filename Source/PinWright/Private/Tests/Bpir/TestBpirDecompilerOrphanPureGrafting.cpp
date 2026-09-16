// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "BpirGraphTestHelpers.h"

#include "Decompiler/BpirDecompiler.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Package.h"

// Regression cover for B-decompile-orphan-pure-nodes-grafted.
//
// The decompiler's orphan-pure injection pass (BpirDecompiler.cpp) hoists unwired
// pure nodes into the first event entry's body so a standalone side-effect orphan
// (e.g. `%ss = subsystem<X>()`) survives a decompile -> recompile round-trip.
//
// The bug: that pass hoisted EVERY pure orphan, including ones that are the data
// feeders of an entry-less exec subgraph (an auto-play Timeline's VLerp /
// GetWorldLocation producers). Grafting those into an unrelated reachable event's
// body is misleading and non-round-trippable, and — unlike the dropped exec orphans
// — they got no warning.
//
// Fix contract verified here:
//   * A pure orphan WITH a downstream consumer (it feeds another node) must NOT be
//     grafted into a body, and must instead be flagged by the orphan-warning sweep.
//   * A pure orphan with NO consumer (genuinely standalone) must STILL be hoisted and
//     must NOT be warned — preserving the subsystem-getter round-trip behavior.
namespace
{
    // Spawn a pure UK2Node_CallFunction targeting a UKismetSystemLibrary member.
    UK2Node_CallFunction* AddPureCall(UEdGraph* Graph, FName FunctionName)
    {
        UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->FunctionReference.SetExternalMember(FunctionName, UKismetSystemLibrary::StaticClass());
        Node->ReconstructNode();
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDecompilerOrphanPureNodeNotGraftedTest,
    "PinWright.bpir.decompiler.OrphanPureNodeNotGrafted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirDecompilerOrphanPureNodeNotGraftedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // Reachable entry with a body — the block the orphans would (wrongly) graft into.
    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    UK2Node_CallFunction* ReachablePrint = BpirGraphTestHelpers::AddPrintStringNode(EventGraph);
    BpirGraphTestHelpers::WireExec(BeginPlayNode, ReachablePrint);

    // Entry-less subgraph: a pure feeder whose output drives an orphan exec consumer
    // (mirrors an auto-play Timeline's VLerp feeding `Set Relative Location`). The
    // consumer is never wired to any entry, so the whole subgraph is orphaned.
    UK2Node_CallFunction* Feeder =
        AddPureCall(EventGraph, GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralString));
    Feeder->NodePosX = 100; Feeder->NodePosY = 100;

    UK2Node_CallFunction* OrphanConsumer = BpirGraphTestHelpers::AddPrintStringNode(EventGraph);
    OrphanConsumer->NodePosX = 400; OrphanConsumer->NodePosY = 100;

    UEdGraphPin* FeederOut = Feeder->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    UEdGraphPin* ConsumerIn = OrphanConsumer->FindPin(TEXT("InString"), EGPD_Input);
    if (!FeederOut || !ConsumerIn)
    {
        AddError(TEXT("Could not find feeder ReturnValue / consumer InString pins to wire"));
        return false;
    }
    FeederOut->MakeLinkTo(ConsumerIn);

    // Genuinely standalone pure orphan: no output links. Must keep round-tripping
    // (hoisted into the body, never warned).
    UK2Node_CallFunction* Standalone =
        AddPureCall(EventGraph, GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralInt));
    Standalone->NodePosX = 700; Standalone->NodePosY = 100;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

    const FString OrphanPrefix = TEXT("Orphaned node not reachable from any entry point");
    TArray<FString> OrphanWarnings;
    for (const FBpirWarning& Warning : Result.Warnings)
    {
        if (Warning.Text.Contains(OrphanPrefix))
        {
            OrphanWarnings.Add(Warning.Text);
        }
    }

    const FString FeederGuid = Feeder->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
    const FString StandaloneGuid = Standalone->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

    const bool bFeederWarned = OrphanWarnings.ContainsByPredicate(
        [&](const FString& W) { return W.Contains(FeederGuid); });
    const bool bStandaloneWarned = OrphanWarnings.ContainsByPredicate(
        [&](const FString& W) { return W.Contains(StandaloneGuid); });

    // The feeder (pure orphan with a consumer) must be warned, not silently grafted.
    // Pre-fix this fails: the feeder was injected into the body and pure nodes were
    // never warned.
    TestTrue(TEXT("Pure orphan WITH a downstream consumer is warned (not grafted into a body)"),
        bFeederWarned);

    // The standalone pure orphan must stay hoisted and unwarned (subsystem-getter
    // round-trip behavior). This guards against an over-broad fix that warns every
    // pure orphan.
    TestFalse(TEXT("Standalone pure orphan (no consumer) is still hoisted, not warned"),
        bStandaloneWarned);

    // Direct symptom check: the feeder must not appear as a binding anywhere in the
    // emitted body. `MakeLiteralString` is the function token the emitter would render
    // for a grafted feeder line; the orphan warning uses the spaced display title
    // ("Make Literal String") and lives in Warnings, not BpirText, so this only
    // catches a body graft.
    TestFalse(TEXT("Feeder is absent from the decompiled body (no graft)"),
        Result.BpirText.Contains(TEXT("MakeLiteralString")));

    return true;
}
