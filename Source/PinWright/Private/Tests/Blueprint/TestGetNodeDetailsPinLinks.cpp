// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-node-details-omit-pin-links.
// blueprint.graph.get_node_details and get_node_details_batch built each pin with
// BuildPinJson(Pin, /*bIncludeLinks=*/false), so a wired input pin came back as just its
// defaultValue (which the engine ignores while the pin is connected) and read like the call
// used that literal. Both verbs now carry linkedTo ("<nodeId>:<pinName>", the
// get_pin_details shape) on every connected pin.
//
// Counterfactual: flip bIncludeLinks back to false in BuildNodeDetailsJson and both
// "carries linkedTo" assertions fail, while "still reports defaultValue" stays green.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Dom/JsonObject.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    // Finds pins[] entry `PinName` in a node-details object and checks its linkedTo / defaultValue.
    void AssertWiredInStringPin(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Details,
        const FString& ExpectedLink, const TCHAR* Label)
    {
        if (!Test.TestTrue(*FString::Printf(TEXT("[%s] details present"), Label), Details.IsValid()))
        {
            return;
        }
        const TSharedPtr<FJsonObject> Pin =
            JsonArrayFindObjectByStringField(Details, TEXT("pins"), TEXT("pinName"), TEXT("InString"));
        if (!Test.TestTrue(*FString::Printf(TEXT("[%s] InString pin listed"), Label), Pin.IsValid()))
        {
            return;
        }

        const TArray<TSharedPtr<FJsonValue>>* Linked = nullptr;
        Test.TestTrue(*FString::Printf(TEXT("[%s] wired InString carries linkedTo"), Label),
            Pin->TryGetArrayField(TEXT("linkedTo"), Linked) && Linked && Linked->Num() == 1);
        if (Linked && Linked->Num() == 1)
        {
            Test.TestEqual(*FString::Printf(TEXT("[%s] linkedTo names the source node:pin"), Label),
                (*Linked)[0]->AsString(), ExpectedLink);
        }
        Test.TestTrue(*FString::Printf(TEXT("[%s] still reports defaultValue"), Label),
            Pin->HasField(TEXT("defaultValue")));

        const TSharedPtr<FJsonObject> Unwired =
            JsonArrayFindObjectByStringField(Details, TEXT("pins"), TEXT("pinName"), TEXT("bPrintToScreen"));
        Test.TestTrue(*FString::Printf(TEXT("[%s] unwired pin has no linkedTo"), Label),
            Unwired.IsValid() && !Unwired->HasField(TEXT("linkedTo")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetNodeDetailsWiredInputCarriesLinkedToTest,
    "PinWright.blueprint.graph.get_node_details_links.WiredInputCarriesLinkedTo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetNodeDetailsWiredInputCarriesLinkedToTest::RunTest(const FString& Parameters)
{
    const TStrongObjectPtr<UBlueprint> BlueprintOwner(CompilerTestUtils::CreateTransientTestBP(TEXT("NodeDetailsLinksBP")));
    UBlueprint* BP = BlueprintOwner.Get();
    UEdGraph* Graph = BP ? FBlueprintEditorUtils::FindEventGraph(BP) : nullptr;
    if (!TestNotNull(TEXT("event graph exists"), Graph))
    {
        return true;
    }

    FEdGraphPinType StringType;
    StringType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("LinkSource"), StringType);

    UK2Node_VariableGet* Getter = CompilerTestUtils::SpawnNode<UK2Node_VariableGet>(Graph, 0, 0);
    Getter->VariableReference.SetSelfMember(TEXT("LinkSource"));
    Getter->ReconstructNode();
    UK2Node_CallFunction* Print = CompilerTestUtils::SpawnPrintStringCall(Graph, 320, 0);

    UEdGraphPin* ValuePin = Getter->FindPin(TEXT("LinkSource"), EGPD_Output);
    UEdGraphPin* InStringPin = Print ? Print->FindPin(TEXT("InString"), EGPD_Input) : nullptr;
    if (!TestNotNull(TEXT("getter value pin"), ValuePin) || !TestNotNull(TEXT("PrintString InString pin"), InStringPin))
    {
        return true;
    }
    ValuePin->MakeLinkTo(InStringPin);
    TestFalse(TEXT("fixture: wired InString keeps a non-empty default"), InStringPin->DefaultValue.IsEmpty());

    const FString ExpectedLink = FString::Printf(TEXT("%s:LinkSource"), *Getter->NodeGuid.ToString());
    const FString PrintId = Print->NodeGuid.ToString();

    // Single-node verb.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), Graph->GetName());
        Payload->SetStringField(TEXT("nodeId"), PrintId);
        FTestResponseCapture Capture;
        TestTrue(TEXT("get_node_details registered"),
            InvokeHandlerWithCapture(TEXT("blueprint.graph.get_node_details"), Payload, Capture));
        TestTrue(TEXT("get_node_details succeeds"), Capture.bSuccess);
        AssertWiredInStringPin(*this, Capture.Result, ExpectedLink, TEXT("single"));
    }

    // Batch verb.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), Graph->GetName());
        TArray<TSharedPtr<FJsonValue>> Ids;
        Ids.Add(MakeShared<FJsonValueString>(PrintId));
        Payload->SetArrayField(TEXT("nodeIds"), Ids);
        FTestResponseCapture Capture;
        TestTrue(TEXT("get_node_details_batch registered"),
            InvokeHandlerWithCapture(TEXT("blueprint.graph.get_node_details_batch"), Payload, Capture));
        TestTrue(TEXT("get_node_details_batch succeeds"), Capture.bSuccess);
        const TSharedPtr<FJsonObject> Item =
            JsonArrayFindObjectByStringField(Capture.Result, TEXT("items"), TEXT("nodeId"), PrintId);
        const TSharedPtr<FJsonObject>* Details = nullptr;
        if (TestTrue(TEXT("batch item has details"), Item.IsValid() && Item->TryGetObjectField(TEXT("details"), Details) && Details))
        {
            AssertWiredInStringPin(*this, *Details, ExpectedLink, TEXT("batch"));
        }
    }
    return true;
}
