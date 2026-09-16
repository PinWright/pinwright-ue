// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-direct-delegate-outputdelegate-loss.
//
// When a UK2Node_CustomEvent.OutputDelegate pin is wired directly to a
// UK2Node_AddDelegate.Delegate pin (no UK2Node_CreateDelegate in between),
// the decompiler used to emit "Delegate: $OutputDelegate" — losing the handler
// identity. The fix matches UK2Node_CustomEvent during EventArg recovery so the
// decompiler emits "event: @HandlerName" instead.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

// ============================================================================
// Bpir.Decompiler.DirectOutputDelegateEmitsEventAt
//
// Build a graph where CustomEvent.OutputDelegate is linked directly to
// AddDelegate.Delegate (no CreateDelegate in between). Decompile and assert
// that "event: @ProbeHandler" appears and "$OutputDelegate" does not.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDirectOutputDelegateDecompilesAsEventAt,
    "PinWright.Bpir.Decompiler.DirectOutputDelegateEmitsEventAt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirDirectOutputDelegateDecompilesAsEventAt::RunTest(const FString& Parameters)
{
    // Step 1: create BP with OnProbeEvent MCDelegate variable and compile so the
    // skeleton class has the property available for UK2Node_AddDelegate to reference.
    UBlueprint* BP = CreateTransientTestBP(TEXT("DirectOutputDelegateBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnProbeEvent"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    // Step 2: use FBpirCompiler to emit the standard wiring (CreateDelegate + AddDelegate)
    // so we get a properly-initialised UK2Node_AddDelegate whose DelegateReference points
    // at OnProbeEvent. Then we re-wire the Delegate input manually.
    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(
        TEXT("entry custom_event ProbeHandler() {\n")
        TEXT("}\n")
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnProbeEvent(target: self, event: @ProbeHandler)\n")
        TEXT("}"));

    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }
    TestTrue(TEXT("BPIR compile succeeded"), CompileResult.bSuccess);

    // Step 3: locate the nodes we need to rewire.
    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("EventGraph exists"), EventGraph);
    if (!EventGraph) return false;

    UK2Node_AddDelegate* AddDelegateNode = nullptr;
    UK2Node_CreateDelegate* CreateDelegateNode = nullptr;
    UK2Node_CustomEvent* CustomEventNode = nullptr;

    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (!AddDelegateNode)    AddDelegateNode    = Cast<UK2Node_AddDelegate>(Node);
        if (!CreateDelegateNode) CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node);
        if (!CustomEventNode)
        {
            if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
            {
                if (CE->GetFunctionName() == TEXT("ProbeHandler"))
                {
                    CustomEventNode = CE;
                }
            }
        }
    }

    TestNotNull(TEXT("AddDelegate node found"), AddDelegateNode);
    TestNotNull(TEXT("CreateDelegate node found"), CreateDelegateNode);
    TestNotNull(TEXT("CustomEvent ProbeHandler found"), CustomEventNode);
    if (!AddDelegateNode || !CreateDelegateNode || !CustomEventNode) return false;

    // Step 4: find the Delegate input pin on AddDelegate.
    UEdGraphPin* AddDelegatePin = AddDelegateNode->GetDelegatePin();
    TestNotNull(TEXT("AddDelegate.Delegate pin found"), AddDelegatePin);
    if (!AddDelegatePin) return false;

    // Step 5: find the OutputDelegate output pin on the CustomEvent.
    UEdGraphPin* OutputDelegatePin = CustomEventNode->FindPin(
        UK2Node_Event::DelegateOutputName, EGPD_Output);
    TestNotNull(TEXT("CustomEvent.OutputDelegate pin found"), OutputDelegatePin);
    if (!OutputDelegatePin) return false;

    // Step 6: disconnect the existing CreateDelegate → AddDelegate.Delegate link,
    // then wire CustomEvent.OutputDelegate → AddDelegate.Delegate directly.
    AddDelegatePin->BreakAllPinLinks(/*bNotifyNodes=*/false);
    OutputDelegatePin->MakeLinkTo(AddDelegatePin);

    // Step 7: decompile and assert the fix.
    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

    // Primary assertion: handler name emitted via event: @ProbeHandler.
    TestTrue(
        TEXT("Decompiled output contains 'event: @ProbeHandler'"),
        Result.BpirText.Contains(TEXT("event: @ProbeHandler")));

    // Regression guard: the raw pin reference must not appear.
    TestFalse(
        TEXT("Decompiled output does not contain '$OutputDelegate'"),
        Result.BpirText.Contains(TEXT("$OutputDelegate")));
    return true;
}
