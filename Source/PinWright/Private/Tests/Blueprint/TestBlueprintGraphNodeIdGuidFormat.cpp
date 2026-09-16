// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBlueprintGraphNodeIdGuidFormat.cpp - Regression test for the producer/consumer
// GUID-format mismatch (board ticket B-node-id-dashed-guid-rejected).
//
// blueprint.decompile orphan/cast warnings print nodeId in the dashed 36-char
// EGuidFormats::DigitsWithHyphens form, while the shared node resolver historically
// string-compared the undashed 32-char FGuid default. A dashed nodeId copied verbatim
// from a warning therefore failed to resolve (NODE_NOT_FOUND). The fix routes GUID
// matching through BlueprintGraphHelpers::NodeGuidMatchesId, which accepts either form.
// This test builds its blueprint fixture entirely in-code (no content load) and would
// fail if NodeGuidMatchesId / FindNodeByIdOrName reverted to an undashed-only compare.

#include "Misc/AutomationTest.h"

#include "Handlers/Blueprint/BlueprintGraphHelpers.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphNodeIdDashedGuidResolvesTest,
    "PinWright.blueprint.graph.NodeIdDashedGuidResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphNodeIdDashedGuidResolvesTest::RunTest(const FString& Parameters)
{
    // --- Pure helper: NodeGuidMatchesId accepts both textual GUID formats -------------
    // A synthesized GUID matches its own undashed (Digits) and dashed (DigitsWithHyphens)
    // renderings; an unrelated string does not. This is the root-cause assertion.
    const FGuid Synth = FGuid::NewGuid();
    const FString SynthUndashed = Synth.ToString(EGuidFormats::Digits);
    const FString SynthDashed = Synth.ToString(EGuidFormats::DigitsWithHyphens);

    TestNotEqual(TEXT("Digits vs DigitsWithHyphens render differently (36 vs 32 chars)"),
        SynthDashed, SynthUndashed);
    TestTrue(TEXT("NodeGuidMatchesId matches the undashed default form"),
        BlueprintGraphHelpers::NodeGuidMatchesId(Synth, SynthUndashed));
    TestTrue(TEXT("NodeGuidMatchesId matches the dashed warning form"),
        BlueprintGraphHelpers::NodeGuidMatchesId(Synth, SynthDashed));
    TestTrue(TEXT("NodeGuidMatchesId matches the lowercase dashed form"),
        BlueprintGraphHelpers::NodeGuidMatchesId(Synth, SynthDashed.ToLower()));
    TestFalse(TEXT("NodeGuidMatchesId rejects an unrelated GUID string"),
        BlueprintGraphHelpers::NodeGuidMatchesId(Synth,
            FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens)));
    TestFalse(TEXT("NodeGuidMatchesId rejects a non-GUID name string"),
        BlueprintGraphHelpers::NodeGuidMatchesId(Synth, TEXT("K2Node_CallFunction_0")));
    TestFalse(TEXT("NodeGuidMatchesId rejects an empty id"),
        BlueprintGraphHelpers::NodeGuidMatchesId(Synth, FString()));

    // --- Resolver end-to-end: FindNodeByIdOrName resolves a dashed nodeId --------------
    // Build a throwaway Actor blueprint + event graph + one node entirely in-code so the
    // test carries no content dependency.
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        FName(*FString::Printf(TEXT("BP_NodeIdGuidFmt_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits))),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("CreateBlueprint produced a blueprint"), Blueprint))
    {
        return false;
    }

    UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0
        ? Blueprint->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("Blueprint has an event graph (UbergraphPages[0])"), EventGraph))
    {
        return false;
    }

    // The resolver reads only NodeGuid and GetName(), so a bare node with a fresh GUID
    // added to the graph is a sufficient fixture.
    UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(EventGraph);
    if (!TestNotNull(TEXT("Created a UK2Node_CallFunction fixture node"), Node))
    {
        return false;
    }
    Node->CreateNewGuid();
    EventGraph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    if (!TestTrue(TEXT("Fixture node has a valid GUID"), Node->NodeGuid.IsValid()))
    {
        return false;
    }

    const FString Undashed = Node->NodeGuid.ToString(EGuidFormats::Digits);
    const FString Dashed = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

    // The bug: a dashed nodeId (exactly what a decompile warning prints) must resolve.
    TestEqual(TEXT("FindNodeByIdOrName resolves the dashed warning-form nodeId"),
        BlueprintGraphHelpers::FindNodeByIdOrName(EventGraph, Dashed),
        static_cast<UEdGraphNode*>(Node));
    // No regression: the undashed default form still resolves.
    TestEqual(TEXT("FindNodeByIdOrName still resolves the undashed nodeId"),
        BlueprintGraphHelpers::FindNodeByIdOrName(EventGraph, Undashed),
        static_cast<UEdGraphNode*>(Node));
    // The name-match fallback is preserved.
    TestEqual(TEXT("FindNodeByIdOrName still resolves by UObject name"),
        BlueprintGraphHelpers::FindNodeByIdOrName(EventGraph, Node->GetName()),
        static_cast<UEdGraphNode*>(Node));
    // No false positives on a string that is neither this node's GUID nor its name.
    TestNull(TEXT("FindNodeByIdOrName returns null for an unrelated id"),
        BlueprintGraphHelpers::FindNodeByIdOrName(EventGraph,
            FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens)));

    return true;
}
