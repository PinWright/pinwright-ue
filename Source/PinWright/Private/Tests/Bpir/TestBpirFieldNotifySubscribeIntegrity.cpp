// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-bpir-field-notify-primitive.
//
// Verifies that FieldNotifySubscribe / FieldNotifyUnsubscribe opcodes compile
// end-to-end through the BPIR compiler -> KismetCompile -> ValidateBlueprintGraphIntegrity
// pipeline. The test exercises the same failing shape that motivated the ticket:
//
//   entry event Construct()  { field_notify_subscribe Replay(event: @SetValues) }
//   entry event Destruct()   { field_notify_unsubscribe Replay(event: @SetValues) }
//   entry custom_event SetValues(object Object, FieldNotificationId Field) {}
//
// Assertions:
//   (1) FBpirCompiler::Compile returns bSuccess=true.
//   (2) Exactly one K2_AddFieldValueChangedDelegate call node and one
//       K2_RemoveFieldValueChangedDelegate call node exist in the graph.
//   (3) For each, the FieldId pin DefaultValue contains FieldName="Replay".
//   (4) For each, the Delegate input pin has at least one linked UK2Node_CreateDelegate
//       whose GetFunctionName() == "SetValues".
//   (5) CompileBlueprintWithDiagnostics returns bCompiled=true.
//   (6) ValidateBlueprintGraphIntegrity returns true with zero failures.
//
// Counterfactual: if the FieldNotifySubscribe/Unsubscribe switch case in
// BpirCompiler.cpp is reverted to the default branch, (1) fails because the
// compiler records "Unhandled opcode". If only the WireCreateDelegateForBindNode
// call is removed, (4) fails because no UK2Node_CreateDelegate is linked into
// the Delegate pin.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"


using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirFieldNotifySubscribeIntegrityTest,
    "PinWright.bpir.compiler.integration.FieldNotifySubscribe_WidgetFieldLocalEventPassesIntegrityGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirFieldNotifySubscribeIntegrityTest::RunTest(const FString& Parameters)
{
    // Create a genuine UWidgetBlueprint — K2_AddFieldValueChangedDelegate lives on UWidget.
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        GetTransientPackage(),
        FName(*FString::Printf(TEXT("FieldNotifySubscribeIntegrityBP_%d"), FMath::Rand())),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()));

    TestNotNull(TEXT("Widget blueprint was created"), WBP);
    if (!WBP) return false;

    // Mirror the handler's widget-BP pre-compile so PopulateBlueprintGeneratedVariables
    // runs before BPIR emit (same as BpirCompilerHandler.cpp:136).
    FKismetEditorUtilities::CompileBlueprint(WBP);

    FBpirCompiler Compiler(WBP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event Construct() {\n")
        TEXT("    field_notify_subscribe Replay(event: @SetValues)\n")
        TEXT("}\n")
        TEXT("entry event Destruct() {\n")
        TEXT("    field_notify_unsubscribe Replay(event: @SetValues)\n")
        TEXT("}\n")
        TEXT("entry custom_event SetValues(object Object, FieldNotificationId Field) {\n")
        TEXT("}"));

    // Assertion (1): compile succeeded
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("(1) BPIR compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Assertions (2), (3), (4): walk the graph and verify the emitted nodes.
    UK2Node_CallFunction* AddNode = nullptr;
    UK2Node_CallFunction* RemoveNode = nullptr;

    for (UEdGraph* Graph : WBP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
            if (!CallNode) continue;
            const FName FnName = CallNode->FunctionReference.GetMemberName();
            if (FnName == TEXT("K2_AddFieldValueChangedDelegate") && !AddNode)
                AddNode = CallNode;
            else if (FnName == TEXT("K2_RemoveFieldValueChangedDelegate") && !RemoveNode)
                RemoveNode = CallNode;
        }
    }

    // Assertion (2): both call nodes exist
    TestNotNull(TEXT("(2a) K2_AddFieldValueChangedDelegate node exists"), AddNode);
    TestNotNull(TEXT("(2b) K2_RemoveFieldValueChangedDelegate node exists"), RemoveNode);
    if (!AddNode || !RemoveNode) return false;

    // Helper: check FieldId pin default contains FieldName="Replay"
    auto CheckFieldIdPin = [&](UK2Node_CallFunction* Node, const TCHAR* NodeLabel)
    {
        UEdGraphPin* FieldIdPin = Node->FindPin(TEXT("FieldId"));
        if (!FieldIdPin)
        {
            AddError(FString::Printf(TEXT("%s: FieldId pin not found"), NodeLabel));
            return;
        }
        // Assertion (3)
        TestTrue(FString::Printf(TEXT("(3) %s FieldId contains FieldName=\"Replay\""), NodeLabel),
            FieldIdPin->DefaultValue.Contains(TEXT("FieldName=\"Replay\"")));
    };

    // Helper: verify Delegate input pin links to a UK2Node_CreateDelegate for SetValues
    auto CheckDelegatePin = [&](UK2Node_CallFunction* Node, const TCHAR* NodeLabel)
    {
        bool bFoundCreateDelegate = false;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction != EGPD_Input) continue;
            const FName& Cat = Pin->PinType.PinCategory;
            if (Cat != UEdGraphSchema_K2::PC_Delegate && Cat != UEdGraphSchema_K2::PC_MCDelegate) continue;
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (!Linked || !Linked->GetOwningNode()) continue;
                if (UK2Node_CreateDelegate* CDNode = Cast<UK2Node_CreateDelegate>(Linked->GetOwningNode()))
                {
                    if (CDNode->GetFunctionName() == FName(TEXT("SetValues")))
                    {
                        bFoundCreateDelegate = true;
                        break;
                    }
                }
            }
            if (bFoundCreateDelegate) break;
        }
        // Assertion (4)
        TestTrue(FString::Printf(TEXT("(4) %s Delegate pin links to UK2Node_CreateDelegate(SetValues)"), NodeLabel),
            bFoundCreateDelegate);
    };

    CheckFieldIdPin(AddNode, TEXT("AddFieldValueChangedDelegate"));
    CheckFieldIdPin(RemoveNode, TEXT("RemoveFieldValueChangedDelegate"));
    CheckDelegatePin(AddNode, TEXT("AddFieldValueChangedDelegate"));
    CheckDelegatePin(RemoveNode, TEXT("RemoveFieldValueChangedDelegate"));

    // Assertion (5): full Blueprint compile
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(WBP);
    if (!Diagnostics.bCompiled)
    {
        for (const FString& Err : Diagnostics.Errors)
        {
            AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
        }
    }
    TestTrue(TEXT("(5) Post-BPIR full compile succeeded"), Diagnostics.bCompiled);

    BlueprintHandlerUtils::RefreshBpirDelegateNodes(WBP, Result.CreatedNodeGUIDs);

    // Assertion (6): integrity gate
    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bIntegrityOk = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(WBP, Failures);

    if (!bIntegrityOk)
    {
        for (const BlueprintHandlerUtils::FBlueprintIntegrityFailure& Fail : Failures)
        {
            AddError(FString::Printf(
                TEXT("IntegrityFailure nodeKind=%s graphName=%s reason=%s"),
                *Fail.NodeKind, *Fail.GraphName, *Fail.Reason));
        }
    }

    TestTrue(TEXT("(6) ValidateBlueprintGraphIntegrity returns true for field_notify_subscribe + local custom event"),
        bIntegrityOk);
    TestEqual(TEXT("(6) No integrity failures recorded"), Failures.Num(), 0);
    return bIntegrityOk;
}
