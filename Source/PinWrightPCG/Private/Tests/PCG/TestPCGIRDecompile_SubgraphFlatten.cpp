// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-pcg-decompile-ir.
//
// Builds a parent graph that references a child graph via a
// UPCGSubgraphSettings node, then decompiles only the parent. Asserts
// the parent text contains the PCGSubgraphSettings class path token
// AND the child graph's path emitted as a `subgraph = "..."` property
// in the parent node's property block, but does NOT contain any node
// identifier from the child graph (the parent should flatten on
// decompile, not recurse).
//
// Counterfactual: if the decompiler recurses into
// UPCGBaseSubgraphSettings::GetSubgraph() and emits its inner nodes,
// the assertion that the child's unique marker is absent fails.
#if WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "PCGIR/PCGIRDecompiler.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "Elements/PCGDensityFilter.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGIRDecompileSubgraph_EmitsFlatNodeWithGraphPath,
    "PinWright.pcgir.decompile.SubgraphFlatten",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGIRDecompileSubgraph_EmitsFlatNodeWithGraphPath::RunTest(const FString& Parameters)
{
    // Child graph with a distinct node we can search for in the parent
    // decompile output. A DensityFilter class path token in the child is
    // expected NOT to appear in the parent text — flatten-on-decompile.
    UPCGGraph* ChildGraph = NewObject<UPCGGraph>(GetTransientPackage(),
        UPCGGraph::StaticClass(), TEXT("PCGIR_TestChildGraph"));
    TestNotNull(TEXT("Transient child UPCGGraph created"), ChildGraph);
    if (!ChildGraph) return false;

    UPCGSettings* ChildSettings = nullptr;
    UPCGNode* ChildNode = ChildGraph->AddNodeOfType(UPCGDensityFilterSettings::StaticClass(), ChildSettings);
    TestNotNull(TEXT("Child graph density filter node created"), ChildNode);
    if (!ChildNode) return false;

    // Parent graph references the child via a subgraph settings node.
    UPCGGraph* ParentGraph = NewObject<UPCGGraph>(GetTransientPackage(),
        UPCGGraph::StaticClass(), TEXT("PCGIR_TestParentGraph"));
    TestNotNull(TEXT("Transient parent UPCGGraph created"), ParentGraph);
    if (!ParentGraph) return false;

    UPCGSettings* SubgraphSettingsOut = nullptr;
    UPCGNode* SubgraphNode = ParentGraph->AddNodeOfType(UPCGSubgraphSettings::StaticClass(), SubgraphSettingsOut);
    TestNotNull(TEXT("Parent graph subgraph node created"), SubgraphNode);
    if (!SubgraphNode) return false;

    if (UPCGBaseSubgraphSettings* SubgraphSettings = Cast<UPCGBaseSubgraphSettings>(SubgraphNode->GetSettings()))
    {
        SubgraphSettings->SetSubgraph(ChildGraph);
    }

    const FPCGIRDecompileResult Result = FPCGIRDecompiler::DecompileGraph(ParentGraph);
    TestTrue(TEXT("Parent decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString& Text = Result.PCGIRText;
    const FString ChildPath = ChildGraph->GetPathName();

    TestTrue(FString::Printf(TEXT("Parent text contains PCGSubgraphSettings class path (text='%s')"), *Text),
        Text.Contains(TEXT("PCGSubgraphSettings")));
    TestTrue(FString::Printf(TEXT("Parent text contains 'subgraph = ' property emitted by special handler (text='%s')"), *Text),
        Text.Contains(TEXT("subgraph = ")));
    TestTrue(FString::Printf(TEXT("Parent text contains child graph path '%s'"), *ChildPath),
        Text.Contains(ChildPath));
    TestFalse(FString::Printf(TEXT("Parent text does NOT contain child's PCGDensityFilterSettings class path — flatten policy holds (text='%s')"), *Text),
        Text.Contains(TEXT("PCGDensityFilterSettings")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")
