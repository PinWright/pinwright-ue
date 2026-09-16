// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-pcg-decompile-ir.
//
// Builds a transient PCG graph containing one Density Filter settings
// node positioned at (10, 20), decompiles, and asserts the resulting
// text contains a `node N` line with the settings class path and the
// `@(10, 20)` position suffix.
//
// Counterfactual: if FormatPositionSuffix is dropped from the node
// header emission, the position annotation is missing — the
// `@(10, 20)` substring assertion fails.
#if WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "PCGIR/PCGIRDecompiler.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "Elements/PCGDensityFilter.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGIRDecompileSingleNode_EmitsClassPathAndPosition,
    "PinWright.pcgir.decompile.SingleNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGIRDecompileSingleNode_EmitsClassPathAndPosition::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage());
    TestNotNull(TEXT("Transient UPCGGraph created"), Graph);
    if (!Graph) return false;

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(UPCGDensityFilterSettings::StaticClass(), DefaultSettings);
    TestNotNull(TEXT("Added density filter node"), Node);
    if (!Node) return false;

    Node->PositionX = 10;
    Node->PositionY = 20;

    const FPCGIRDecompileResult Result = FPCGIRDecompiler::DecompileGraph(Graph);
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString& Text = Result.PCGIRText;
    TestTrue(FString::Printf(TEXT("Text contains a 'node N1 = ' line (text='%s')"), *Text),
        Text.Contains(TEXT("node N1 = ")));
    TestTrue(TEXT("Text contains '/Script/' qualifying the settings class path"),
        Text.Contains(TEXT("/Script/")));
    TestTrue(FString::Printf(TEXT("Text contains '@(10, 20)' position suffix (text='%s')"), *Text),
        Text.Contains(TEXT("@(10, 20)")));
    TestFalse(TEXT("No 'connect ' lines emitted for single-node graph"),
        Text.Contains(TEXT("connect ")));
    // Negative case for the empty-body marker: PCGIRDecompiler only emits it when
    // the body is genuinely empty. Without this, deleting that guard (emitting the
    // marker unconditionally) leaves the whole PCG suite green while destroying the
    // marker's only purpose - telling an intentionally empty body apart from a
    // failed extraction.
    TestFalse(FString::Printf(TEXT("No '# no nodes' marker on a populated graph (text='%s')"), *Text),
        Text.Contains(TEXT("# no nodes")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")
