// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-pcg-decompile-ir.
//
// Verifies that PCGIR decompile of a graph with no nodes emits a
// `# no nodes` marker inside the body, mirroring MGIR's
// `# no expression graph` disambiguation. UPCGGraph auto-creates
// Input and Output terminal nodes in its constructor, so reaching
// the empty-body branch in the decompiler requires nulling both
// terminals via reflection — the test does that explicitly.
//
// Counterfactual: if the empty-body marker is removed, an empty
// graph is indistinguishable from a load failure in the text
// output; this test fails because `# no nodes` is absent.
#if WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "PCGIR/PCGIRDecompiler.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"


namespace
{
    // Sets a TObjectPtr UPROPERTY on the graph to null via reflection.
    // UPCGGraph keeps InputNode/OutputNode private; the decompiler reads
    // them through GetInputNode/GetOutputNode, which return the same slots
    // that the UPROPERTY mirrors. Returns false if the property is missing —
    // catches a future UE rename that would otherwise leave terminals
    // populated and make this test exercise something other than the
    // empty-body branch.
    bool NullObjectProperty(UPCGGraph* Graph, const TCHAR* PropertyName)
    {
        if (!Graph) return false;
        FObjectProperty* Property = FindFProperty<FObjectProperty>(Graph->GetClass(), PropertyName);
        if (!Property) return false;
        Property->SetObjectPropertyValue_InContainer(Graph, nullptr);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGIRDecompileEmptyGraph_EmitsMarker,
    "PinWright.pcgir.decompile.EmptyGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGIRDecompileEmptyGraph_EmitsMarker::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage());
    TestNotNull(TEXT("Transient UPCGGraph created"), Graph);
    if (!Graph) return false;

    TestTrue(TEXT("Found InputNode UPROPERTY"), NullObjectProperty(Graph, TEXT("InputNode")));
    TestTrue(TEXT("Found OutputNode UPROPERTY"), NullObjectProperty(Graph, TEXT("OutputNode")));

    const FPCGIRDecompileResult Result = FPCGIRDecompiler::DecompileGraph(Graph);
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString& Text = Result.PCGIRText;
    TestTrue(FString::Printf(TEXT("Text contains 'entry pcg' (text='%s')"), *Text),
        Text.Contains(TEXT("entry pcg")));
    TestTrue(FString::Printf(TEXT("Text contains '# no nodes' (text='%s')"), *Text),
        Text.Contains(TEXT("# no nodes")));
    TestFalse(TEXT("No 'node N' lines emitted for empty graph"),
        Text.Contains(TEXT("node N")));
    TestFalse(TEXT("No 'connect ' lines emitted for empty graph"),
        Text.Contains(TEXT("connect ")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")
