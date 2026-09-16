// Copyright (c) 2026 Alexander Penkin. MIT License.

// Asserts EmitPureNode emits literal `self` for K2Node_CreateDelegate with an unwired Self pin.

#include "Misc/AutomationTest.h"

#include "BpirGraphTestHelpers.h"
#include "Decompiler/BpirTextEmitter.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#if defined(__has_include) && (__has_include("BlueprintGraph/K2Node_CreateDelegate.h") || __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h") || __has_include("K2Node_CreateDelegate.h"))
#if __has_include("BlueprintGraph/K2Node_CreateDelegate.h")
#include "BlueprintGraph/K2Node_CreateDelegate.h"
#elif __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h")
#include "BlueprintGraph/Classes/K2Node_CreateDelegate.h"
#else
#include "K2Node_CreateDelegate.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCreateDelegateImplicitSelfEmitsSelfToken,
    "PinWright.Bpir.CreateDelegate.ImplicitSelfEmitsSelfToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCreateDelegateImplicitSelfEmitsSelfToken::RunTest(const FString& Parameters)
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

    // Create a K2Node_CreateDelegate with Self pin left unwired. This is the
    // implicit-self case: engine binds the delegate to the BP being compiled.
    UK2Node_CreateDelegate* CDNode =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CreateDelegate>(EventGraph);
    if (!CDNode)
    {
        AddError(TEXT("Failed to add UK2Node_CreateDelegate to event graph"));
        return false;
    }

    // Optional function-name hint so the emitter has something to print after
    // the self token; the test does not depend on the function existing on the
    // BP class — only on the emitter shape.
    CDNode->SetFunction(FName(TEXT("SomeHandler")));

    // Drive production EmitPureNode directly. ResolvePin returns the pin's
    // DefaultValue so any wired-pin fallback can be observed; for the unwired
    // Self pin the typed branch must short-circuit to literal `self` before
    // ResolvePin is consulted.
    FBpirTextEmitter Emitter;
    auto ResolvePin = [](UEdGraphPin* Pin) -> FString
    {
        if (!Pin) return FString();
        return Pin->DefaultValue;
    };

    const FString Output = Emitter.EmitPureNode(CDNode, TEXT("n0"), ResolvePin);

    AddInfo(FString::Printf(TEXT("EmitPureNode output: %s"), *Output));

    TestTrue(TEXT("starts with 'call Create_Event('"),
        Output.Contains(TEXT("call Create_Event(")));
    TestTrue(TEXT("self token is 'self', not '?'"),
        Output.Contains(TEXT("self: self")));
    TestFalse(TEXT("no '?' fallback"),
        Output.Contains(TEXT("self: ?")));
    return true;
}

#endif
