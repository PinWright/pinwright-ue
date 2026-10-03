// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-graph-create-node-target-ignored-for-variableget: every node
// type the wiki lists as target-aware must resolve `target` (VariableGet/Set, Event,
// CustomEvent, Cast), and a missing name must be reported as a missing target rather
// than as a lookup of the empty variable ''.

#include "Misc/AutomationTest.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestUtils.h"
#include "UObject/StrongObjectPtr.h"

namespace TestCreateNodeTargetAwareHelpers
{
    // Creates the node through the create_node handler and returns it, or nullptr (with the
    // failure recorded) when the call failed or the node is not in the event graph.
    template <typename TNode>
    TNode* CreateViaTarget(FAutomationTestBase& Test, UBlueprint* BP, const TCHAR* NodeType, const TCHAR* Target)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("nodeType"), NodeType);
        Payload->SetStringField(TEXT("target"), Target);
        Payload->SetNumberField(TEXT("x"), 16.0);
        Payload->SetNumberField(TEXT("y"), 128.0);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("create_node handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, Capture));
        FString NodeId;
        if (!Test.TestTrue(*FString::Printf(TEXT("%s target:'%s' succeeded (%s %s)"), NodeType, Target, *Capture.ErrorCode, *Capture.Message),
                Capture.bSuccess && Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId)))
        {
            return nullptr;
        }
        UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
        if (!Test.TestNotNull(TEXT("Event graph exists"), EventGraph))
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            if (Node && Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
            {
                TNode* Typed = Cast<TNode>(Node);
                Test.TestNotNull(*FString::Printf(TEXT("%s created the expected node class"), NodeType), Typed);
                return Typed;
            }
        }
        Test.AddError(FString::Printf(TEXT("%s node %s not found in the event graph"), NodeType, *NodeId));
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphCreateNodeTargetAwareTypesTest,
    "PinWright.blueprint.graph.create_node.TargetParamResolvesVariableEventAndCastNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphCreateNodeTargetAwareTypesTest::RunTest(const FString& Parameters)
{
    using namespace TestCreateNodeTargetAwareHelpers;

    TStrongObjectPtr<UBlueprint> Blueprint(CompilerTestUtils::CreateTransientTestBP(TEXT("CreateNodeTargetAware")));
    UBlueprint* BP = Blueprint.Get();
    const FName VarName(TEXT("bTriggerHeld"));
    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    if (!TestNotNull(TEXT("Blueprint created"), BP)
        || !TestTrue(TEXT("Member variable added"), FBlueprintEditorUtils::AddMemberVariable(BP, VarName, BoolPinType)))
    {
        return true;
    }
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    if (UK2Node_VariableGet* Get = CreateViaTarget<UK2Node_VariableGet>(*this, BP, TEXT("VariableGet"), TEXT("bTriggerHeld")))
    {
        TestEqual(TEXT("VariableGet reads the target variable"), Get->VariableReference.GetMemberName(), VarName);
        TestTrue(TEXT("VariableGet is a self member"), Get->VariableReference.IsSelfContext());
    }
    if (UK2Node_VariableSet* Set = CreateViaTarget<UK2Node_VariableSet>(*this, BP, TEXT("VariableSet"), TEXT("bTriggerHeld")))
    {
        TestEqual(TEXT("VariableSet writes the target variable"), Set->VariableReference.GetMemberName(), VarName);
    }
    // Qualified owner that is not this Blueprint's class: an external-member getter.
    if (UK2Node_VariableGet* External = CreateViaTarget<UK2Node_VariableGet>(*this, BP, TEXT("VariableGet"), TEXT("ActorComponent::ComponentTags")))
    {
        TestEqual(TEXT("External getter names the member"), External->VariableReference.GetMemberName(), FName(TEXT("ComponentTags")));
        TestFalse(TEXT("External getter is not a self member"), External->VariableReference.IsSelfContext());
        TestTrue(TEXT("External getter is owned by UActorComponent"),
            External->VariableReference.GetMemberParentClass() == UActorComponent::StaticClass());
    }
    if (UK2Node_Event* Event = CreateViaTarget<UK2Node_Event>(*this, BP, TEXT("Event"), TEXT("ReceiveDestroyed")))
    {
        TestEqual(TEXT("Event overrides the target"), Event->EventReference.GetMemberName(), FName(TEXT("ReceiveDestroyed")));
    }
    if (UK2Node_CustomEvent* Custom = CreateViaTarget<UK2Node_CustomEvent>(*this, BP, TEXT("CustomEvent"), TEXT("OnTargetAwareProbe")))
    {
        TestEqual(TEXT("CustomEvent is named by target"), Custom->CustomFunctionName, FName(TEXT("OnTargetAwareProbe")));
    }
    if (UK2Node_DynamicCast* CastNode = CreateViaTarget<UK2Node_DynamicCast>(*this, BP, TEXT("Cast"), TEXT("Pawn")))
    {
        TestTrue(TEXT("Cast targets the target class"), CastNode->TargetType == APawn::StaticClass());
    }

    // Legacy {variableName, memberClass:<own class>} on a variable added but not yet compiled:
    // a self member, so it must resolve from NewVariables rather than the stale GeneratedClass.
    const FName FreshVarName(TEXT("bFreshUncompiled"));
    if (TestTrue(TEXT("Uncompiled member variable added"), FBlueprintEditorUtils::AddMemberVariable(BP, FreshVarName, BoolPinType))
        && TestNotNull(TEXT("Generated class exists"), BP->GeneratedClass.Get())
        && TestNull(TEXT("Precondition: the compiled class does not hold the new variable"),
            BP->GeneratedClass->FindPropertyByName(FreshVarName)))
    {
        TSharedPtr<FJsonObject> Legacy = MakeShared<FJsonObject>();
        Legacy->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Legacy->SetStringField(TEXT("nodeType"), TEXT("VariableGet"));
        Legacy->SetStringField(TEXT("variableName"), FreshVarName.ToString());
        Legacy->SetStringField(TEXT("memberClass"), BP->GeneratedClass->GetPathName());
        Legacy->SetNumberField(TEXT("x"), 32.0);
        Legacy->SetNumberField(TEXT("y"), 256.0);
        FTestResponseCapture LegacyCapture;
        InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Legacy, LegacyCapture);
        TestTrue(*FString::Printf(TEXT("Legacy variableName+memberClass on an uncompiled variable succeeds (%s %s)"),
            *LegacyCapture.ErrorCode, *LegacyCapture.Message), LegacyCapture.bSuccess);
    }

    // Failure direction: no name at all is a missing target, not Variable ''.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetStringField(TEXT("nodeType"), TEXT("VariableGet"));
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);
    FTestResponseCapture Missing;
    InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, Missing);
    TestFalse(TEXT("VariableGet with no name fails"), Missing.bSuccess);
    TestEqual(TEXT("Missing name is INVALID_ARGUMENT"), Missing.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("Missing-name error names target"), Missing.Message.Contains(TEXT("target")));

    Payload->SetStringField(TEXT("target"), TEXT("NoSuchVariable"));
    FTestResponseCapture Unknown;
    InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, Unknown);
    TestEqual(TEXT("Unknown variable is VARIABLE_NOT_FOUND"), Unknown.ErrorCode, FString(TEXT("VARIABLE_NOT_FOUND")));
    TestTrue(TEXT("Unknown-variable error names the target it read"), Unknown.Message.Contains(TEXT("NoSuchVariable")));

    // An ancestor qualifier must name a property of that ancestor: Actor::bTriggerHeld is a BP-only variable.
    Payload->SetStringField(TEXT("target"), TEXT("Actor::bTriggerHeld"));
    FTestResponseCapture WrongAncestor;
    InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, WrongAncestor);
    TestEqual(TEXT("BP variable qualified by an ancestor is VARIABLE_NOT_FOUND"), WrongAncestor.ErrorCode, FString(TEXT("VARIABLE_NOT_FOUND")));
    return true;
}
