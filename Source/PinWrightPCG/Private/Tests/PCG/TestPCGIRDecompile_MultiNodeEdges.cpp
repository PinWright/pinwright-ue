// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-pcg-decompile-ir.
//
// Builds a transient PCG graph with two Density Filter settings nodes
// and one edge from the first node's output pin to the second node's
// input pin. Asserts that both `node N1` and `node N2` lines appear
// and exactly one `connect N1.<src> -> N2.<dst>` line appears with
// the arrow pointing from source to destination.
//
// Counterfactual: if the edge walk iterates input pins instead of
// output pins, every edge is emitted from both endpoints and the
// connect line count doubles; if `Edge->InputPin` / `OutputPin`
// roles are inverted, the arrow points the wrong way (`N2 -> N1`)
// and the directional assertion fails.
#if WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "PCGIR/PCGIRDecompiler.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Elements/PCGDensityFilter.h"
#include "UObject/Package.h"


namespace
{
    int32 CountOccurrences(const FString& Text, const FString& Needle)
    {
        int32 Count = 0;
        int32 Index = 0;
        while ((Index = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, Index)) != INDEX_NONE)
        {
            ++Count;
            Index += Needle.Len();
        }
        return Count;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGIRDecompileMultiNodeEdges_EmitsConnects,
    "PinWright.pcgir.decompile.MultiNodeEdges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGIRDecompileMultiNodeEdges_EmitsConnects::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage());
    TestNotNull(TEXT("Transient UPCGGraph created"), Graph);
    if (!Graph) return false;

    UPCGSettings* DefaultA = nullptr;
    UPCGNode* NodeA = Graph->AddNodeOfType(UPCGDensityFilterSettings::StaticClass(), DefaultA);
    UPCGSettings* DefaultB = nullptr;
    UPCGNode* NodeB = Graph->AddNodeOfType(UPCGDensityFilterSettings::StaticClass(), DefaultB);
    TestNotNull(TEXT("First density filter node created"), NodeA);
    TestNotNull(TEXT("Second density filter node created"), NodeB);
    if (!NodeA || !NodeB) return false;

    // Density filter has one default point input and one default point
    // output. Wire output of A to input of B by pin label.
    FName OutLabel = NAME_None;
    FName InLabel = NAME_None;
    if (NodeA->GetOutputPins().Num() > 0 && NodeA->GetOutputPins()[0])
    {
        OutLabel = NodeA->GetOutputPins()[0]->Properties.Label;
    }
    if (NodeB->GetInputPins().Num() > 0 && NodeB->GetInputPins()[0])
    {
        InLabel = NodeB->GetInputPins()[0]->Properties.Label;
    }
    TestFalse(TEXT("Resolved source output pin label"), OutLabel.IsNone());
    TestFalse(TEXT("Resolved dest input pin label"), InLabel.IsNone());

    Graph->AddEdge(NodeA, OutLabel, NodeB, InLabel);

    const FPCGIRDecompileResult Result = FPCGIRDecompiler::DecompileGraph(Graph);
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString& Text = Result.PCGIRText;

    TestTrue(FString::Printf(TEXT("Text contains 'node N1' (text='%s')"), *Text),
        Text.Contains(TEXT("node N1")));
    TestTrue(FString::Printf(TEXT("Text contains 'node N2' (text='%s')"), *Text),
        Text.Contains(TEXT("node N2")));

    const int32 ConnectCount = CountOccurrences(Text, TEXT("connect "));
    TestEqual(FString::Printf(TEXT("Exactly one connect line emitted (text='%s')"), *Text),
        ConnectCount, 1);

    // Direction: arrow goes from N1 (source/upstream) to N2 (dest/downstream).
    const FString ExpectedArrow = FString::Printf(
        TEXT("connect N1.%s -> N2.%s"), *OutLabel.ToString(), *InLabel.ToString());
    TestTrue(FString::Printf(TEXT("Connect line has correct direction '%s' (text='%s')"),
        *ExpectedArrow, *Text), Text.Contains(ExpectedArrow));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")
