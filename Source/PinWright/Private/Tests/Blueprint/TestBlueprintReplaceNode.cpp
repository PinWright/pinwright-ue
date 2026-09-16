// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FormatText.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"


namespace
{
    constexpr const TCHAR* ReplaceNodeMethod = TEXT("blueprint.graph.replace_node");

    UBlueprint* CreateReplaceNodeTestBlueprint(const FString& Prefix)
    {
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            FName(*FString::Printf(TEXT("%s_%s"), *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits))),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
    }

    UEdGraph* GetEventGraph(UBlueprint* Blueprint)
    {
        return Blueprint && Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
    }

    UEdGraph* AddFunctionGraph(UBlueprint* Blueprint, const FName GraphName)
    {
        UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
            Blueprint,
            GraphName,
            UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);
        return FunctionGraph;
    }

    FEdGraphPinType MakePinType(const FName Category)
    {
        FEdGraphPinType PinType;
        PinType.PinCategory = Category;
        return PinType;
    }

    template<typename T>
    T* AddConfiguredNode(UEdGraph* Graph, TFunctionRef<void(T*)> Configure, const int32 X = 0, const int32 Y = 0)
    {
        T* Node = NewObject<T>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Configure(Node);
        Node->AllocateDefaultPins();
        Node->NodePosX = X;
        Node->NodePosY = Y;
        Graph->AddNode(Node, true, false);
        return Node;
    }

    template<typename T>
    T* AddNode(UEdGraph* Graph, const int32 X = 0, const int32 Y = 0)
    {
        return AddConfiguredNode<T>(Graph, [](T*) {}, X, Y);
    }

    UK2Node_CallFunction* AddCallFunctionNode(UEdGraph* Graph, UClass* OwnerClass, const FName FunctionName)
    {
        return AddConfiguredNode<UK2Node_CallFunction>(
            Graph,
            [OwnerClass, FunctionName](UK2Node_CallFunction* Node)
            {
                Node->FunctionReference.SetExternalMember(FunctionName, OwnerClass);
            });
    }

    UK2Node_VariableGet* AddVariableGetNode(UEdGraph* Graph, const FName VarName)
    {
        return AddConfiguredNode<UK2Node_VariableGet>(
            Graph,
            [VarName](UK2Node_VariableGet* Node)
            {
                Node->VariableReference.SetSelfMember(VarName);
            });
    }

    UK2Node_VariableSet* AddVariableSetNode(UEdGraph* Graph, const FName VarName)
    {
        return AddConfiguredNode<UK2Node_VariableSet>(
            Graph,
            [VarName](UK2Node_VariableSet* Node)
            {
                Node->VariableReference.SetSelfMember(VarName);
            });
    }

    UK2Node_DynamicCast* AddDynamicCastNode(UEdGraph* Graph, UClass* TargetClass)
    {
        return AddConfiguredNode<UK2Node_DynamicCast>(
            Graph,
            [TargetClass](UK2Node_DynamicCast* Node)
            {
                Node->TargetType = TargetClass;
            });
    }

    UK2Node_MacroInstance* AddForEachMacroNode(UEdGraph* Graph)
    {
        UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Graph->AddNode(Node, true, false);

        UBlueprint* MacroLibrary = LoadObject<UBlueprint>(
            nullptr,
            TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
        if (!MacroLibrary)
        {
            return nullptr;
        }

        for (UEdGraph* MacroGraph : MacroLibrary->MacroGraphs)
        {
            if (MacroGraph && MacroGraph->GetFName() == TEXT("ForEachLoop"))
            {
                Node->SetMacroGraph(MacroGraph);
                Node->AllocateDefaultPins();
                Node->ReconstructNode();
                return Node;
            }
        }
        return nullptr;
    }

    UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FString& NodeId)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
                {
                    return Node;
                }
            }
        }
        return nullptr;
    }

    template<typename T>
    T* FindNodeOfTypeInGraph(UEdGraph* Graph)
    {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (T* Typed = Cast<T>(Node))
            {
                return Typed;
            }
        }
        return nullptr;
    }

    bool IsNodeStillPresent(UEdGraph* Graph, const UEdGraphNode* Node)
    {
        return Graph && Node && Graph->Nodes.ContainsByPredicate(
            [Node](const UEdGraphNode* Candidate)
            {
                return Candidate == Node;
            });
    }

    TSharedPtr<FJsonObject> MakeReplacePayload(
        UBlueprint* Blueprint,
        UEdGraphNode* OldNode,
        const FString& NewNodeType,
        const FString& Target = FString(),
        const FString& GraphName = FString(),
        const bool bAllowOrphanPlaceholders = false,
        const TSharedPtr<FJsonObject>& PinRemap = nullptr)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
        Payload->SetStringField(TEXT("nodeId"), OldNode->NodeGuid.ToString());
        Payload->SetStringField(TEXT("newNodeType"), NewNodeType);
        if (!Target.IsEmpty())
        {
            Payload->SetStringField(TEXT("target"), Target);
        }
        if (!GraphName.IsEmpty())
        {
            Payload->SetStringField(TEXT("graphName"), GraphName);
        }
        if (bAllowOrphanPlaceholders)
        {
            Payload->SetBoolField(TEXT("allowOrphanPlaceholders"), true);
        }
        if (PinRemap.IsValid())
        {
            Payload->SetObjectField(TEXT("pinRemap"), PinRemap);
        }
        return Payload;
    }

    bool InvokeReplaceNode(
        UBlueprint* Blueprint,
        UEdGraphNode* OldNode,
        const FString& NewNodeType,
        FTestResponseCapture& Capture,
        const FString& Target = FString(),
        const FString& GraphName = FString(),
        const bool bAllowOrphanPlaceholders = false,
        const TSharedPtr<FJsonObject>& PinRemap = nullptr)
    {
        return InvokeHandlerWithCapture(
            ReplaceNodeMethod,
            MakeReplacePayload(Blueprint, OldNode, NewNodeType, Target, GraphName, bAllowOrphanPlaceholders, PinRemap),
            Capture);
    }

    bool AssertSuccessCommon(
        FAutomationTestBase& Test,
        const FTestResponseCapture& Capture,
        const FString& ExpectedFactoryPath,
        const FString& ResolvedClassSubstring)
    {
        Test.TestTrue(TEXT("replace_node reports success"), Capture.bSuccess);
        if (!Capture.Result.IsValid())
        {
            Test.AddError(TEXT("replace_node returned no result JSON"));
            return false;
        }

        FString FactoryPath;
        Test.TestTrue(TEXT("factoryPath field exists"), Capture.Result->TryGetStringField(TEXT("factoryPath"), FactoryPath));
        Test.TestEqual(TEXT("factoryPath matches"), FactoryPath, ExpectedFactoryPath);

        FString ResolvedClass;
        Test.TestTrue(TEXT("resolvedClass field exists"), Capture.Result->TryGetStringField(TEXT("resolvedClass"), ResolvedClass));
        Test.TestTrue(TEXT("resolvedClass names replacement class"), ResolvedClass.Contains(ResolvedClassSubstring));

        Test.TestTrue(TEXT("connectionsRewired field exists"), Capture.Result->HasField(TEXT("connectionsRewired")));
        Test.TestTrue(TEXT("connectionsDropped field exists"), Capture.Result->HasField(TEXT("connectionsDropped")));
        Test.TestTrue(TEXT("defaultsTransferred field exists"), Capture.Result->HasField(TEXT("defaultsTransferred")));
        Test.TestTrue(TEXT("subPinsSplit field exists"), Capture.Result->HasField(TEXT("subPinsSplit")));
        return true;
    }

    UEdGraphNode* AssertSuccessAndFindNewNode(
        FAutomationTestBase& Test,
        UBlueprint* Blueprint,
        const FTestResponseCapture& Capture,
        const FString& ExpectedFactoryPath,
        const FString& ResolvedClassSubstring)
    {
        if (!AssertSuccessCommon(Test, Capture, ExpectedFactoryPath, ResolvedClassSubstring))
        {
            return nullptr;
        }

        FString NewNodeId;
        Test.TestTrue(TEXT("newNodeId field exists"), Capture.Result->TryGetStringField(TEXT("newNodeId"), NewNodeId));
        UEdGraphNode* NewNode = FindNodeByGuid(Blueprint, NewNodeId);
        Test.TestNotNull(TEXT("replacement node exists in graph"), NewNode);
        return NewNode;
    }

    void AssertErrorAndOldNodePresent(
        FAutomationTestBase& Test,
        const FTestResponseCapture& Capture,
        const FString& ExpectedErrorCode,
        UEdGraph* Graph,
        UEdGraphNode* OldNode)
    {
        Test.TestTrue(TEXT("replace_node handler sent a response"), Capture.bWasCalled);
        Test.TestFalse(TEXT("replace_node reports failure"), Capture.bSuccess);
        Test.TestEqual(TEXT("error code matches"), Capture.ErrorCode, ExpectedErrorCode);
        Test.TestTrue(TEXT("old node remains present"), IsNodeStillPresent(Graph, OldNode));
    }

    bool AssertPinIssueArrayContains(
        FAutomationTestBase& Test,
        const FTestResponseCapture& Capture,
        const TCHAR* ArrayField,
        const TCHAR* ExpectedPinName,
        const TCHAR* ExpectedReason,
        const int32 ExpectedLinkedCount)
    {
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!Test.TestTrue(
            FString::Printf(TEXT("%s field exists"), ArrayField),
            Capture.Result.IsValid() && Capture.Result->TryGetArrayField(ArrayField, Entries)))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (!EntryValue.IsValid() || !EntryValue->TryGetObject(Entry) || !Entry || !Entry->IsValid())
            {
                continue;
            }

            FString PinName;
            if (!(*Entry)->TryGetStringField(TEXT("pinName"), PinName) || PinName != ExpectedPinName)
            {
                continue;
            }

            FString Reason;
            double LinkedCount = -1.0;
            Test.TestTrue(TEXT("pin issue reason exists"), (*Entry)->TryGetStringField(TEXT("reason"), Reason));
            Test.TestEqual(TEXT("pin issue reason matches"), Reason, FString(ExpectedReason));
            Test.TestTrue(TEXT("pin issue linkedCount exists"), (*Entry)->TryGetNumberField(TEXT("linkedCount"), LinkedCount));
            Test.TestEqual(TEXT("pin issue linkedCount matches"), static_cast<int32>(LinkedCount), ExpectedLinkedCount);
            return true;
        }

        Test.AddError(FString::Printf(TEXT("%s did not contain pin '%s'"), ArrayField, ExpectedPinName));
        return false;
    }

    UK2Node_IfThenElse* AddBranchWithLinkedCondition(UBlueprint* Blueprint, UEdGraph* Graph)
    {
        FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("ReplaceNodeCondition"), MakePinType(UEdGraphSchema_K2::PC_Boolean));
        UK2Node_VariableGet* ConditionSource = AddVariableGetNode(Graph, TEXT("ReplaceNodeCondition"));
        UK2Node_IfThenElse* Branch = AddNode<UK2Node_IfThenElse>(Graph);

        UEdGraphPin* ValuePin = ConditionSource->FindPin(TEXT("ReplaceNodeCondition"), EGPD_Output);
        UEdGraphPin* ConditionPin = Branch->FindPin(UEdGraphSchema_K2::PN_Condition, EGPD_Input);
        if (ValuePin && ConditionPin)
        {
            ValuePin->MakeLinkTo(ConditionPin);
        }
        return Branch;
    }
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeVariableGet_To_VariableSet_SameVariableTest,
    "PinWright.blueprint.graph.replace_node.VariableGet_To_VariableSet_SameVariable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeVariableGet_To_VariableSet_SameVariableTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceVarGetSet"));
    const TStrongObjectPtr<UBlueprint> BlueprintOwner(Blueprint);
    UEdGraph* Graph = GetEventGraph(Blueprint);
    TestNotNull(TEXT("event graph exists"), Graph);

    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("ReplaceNodeBool"), MakePinType(UEdGraphSchema_K2::PC_Boolean));
    UK2Node_VariableGet* OldNode = AddVariableGetNode(Graph, TEXT("ReplaceNodeBool"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("VariableSet"), Capture, TEXT("ReplaceNodeBool")));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_VariableSet"));
    UK2Node_VariableSet* NewSet = Cast<UK2Node_VariableSet>(NewNode);
    TestNotNull(TEXT("new node is VariableSet"), NewSet);
    TestTrue(TEXT("VariableSet remains self-context"), NewSet && NewSet->VariableReference.IsSelfContext());
    UEdGraphPin* SelfPin = NewSet ? NewSet->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input) : nullptr;
    TestNotNull(TEXT("VariableSet has a self pin"), SelfPin);
    TestTrue(TEXT("self-context VariableSet hides its self pin"), SelfPin && SelfPin->bHidden);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeVariableGet_SelfMember_RemainsSelfBoundTest,
    "PinWright.blueprint.graph.replace_node.VariableGet_SelfMember_RemainsSelfBound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeVariableGet_SelfMember_RemainsSelfBoundTest::RunTest(const FString& Parameters)
{
    const TStrongObjectPtr<UBlueprint> BlueprintOwner(CreateReplaceNodeTestBlueprint(TEXT("ReplaceVarGetSelf")));
    UBlueprint* Blueprint = BlueprintOwner.Get();
    UEdGraph* Graph = GetEventGraph(Blueprint);
    TestNotNull(TEXT("event graph exists"), Graph);

    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("OldValue"), MakePinType(UEdGraphSchema_K2::PC_Boolean));
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("NewValue"), MakePinType(UEdGraphSchema_K2::PC_Boolean));
    UK2Node_VariableGet* OldNode = AddVariableGetNode(Graph, TEXT("OldValue"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("VariableGet"), Capture, TEXT("NewValue")));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_VariableGet"));
    UK2Node_VariableGet* NewGet = Cast<UK2Node_VariableGet>(NewNode);
    TestNotNull(TEXT("new node is VariableGet"), NewGet);
    TestTrue(TEXT("replacement reads NewValue"),
        NewGet && NewGet->VariableReference.GetMemberName() == TEXT("NewValue"));
    TestTrue(TEXT("replacement remains self-context"), NewGet && NewGet->VariableReference.IsSelfContext());
    UEdGraphPin* SelfPin = NewGet ? NewGet->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input) : nullptr;
    TestNotNull(TEXT("replacement has a self pin"), SelfPin);
    TestTrue(TEXT("self-context replacement hides its self pin"), SelfPin && SelfPin->bHidden);
    TestTrue(TEXT("self-context replacement needs no explicit self wire"), SelfPin && SelfPin->LinkedTo.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeVariableGet_QualifiedOtherClass_RemainsExternalTest,
    "PinWright.blueprint.graph.replace_node.VariableGet_QualifiedOtherClass_RemainsExternal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeVariableGet_QualifiedOtherClass_RemainsExternalTest::RunTest(const FString& Parameters)
{
    const TStrongObjectPtr<UBlueprint> BlueprintOwner(CreateReplaceNodeTestBlueprint(TEXT("ReplaceVarGetExternal")));
    UBlueprint* Blueprint = BlueprintOwner.Get();
    UEdGraph* Graph = GetEventGraph(Blueprint);
    TestNotNull(TEXT("event graph exists"), Graph);

    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("BaseEyeHeight"), MakePinType(UEdGraphSchema_K2::PC_Boolean));
    UK2Node_VariableGet* OldNode = AddVariableGetNode(Graph, TEXT("BaseEyeHeight"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("VariableGet"), Capture, TEXT("Pawn::BaseEyeHeight")));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_VariableGet"));
    UK2Node_VariableGet* NewGet = Cast<UK2Node_VariableGet>(NewNode);
    TestNotNull(TEXT("new node is VariableGet"), NewGet);
    TestFalse(TEXT("unrelated owner remains external context"),
        !NewGet || NewGet->VariableReference.IsSelfContext());
    TestEqual(TEXT("external owner is Pawn"),
        NewGet ? NewGet->VariableReference.GetMemberParentClass() : nullptr,
        APawn::StaticClass());
    UEdGraphPin* SelfPin = NewGet ? NewGet->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input) : nullptr;
    TestNotNull(TEXT("external replacement has a target pin"), SelfPin);
    TestFalse(TEXT("external replacement exposes its target pin"), !SelfPin || SelfPin->bHidden);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeEvent_To_CustomEvent_PreservesUserDefinedPinsTest,
    "PinWright.blueprint.graph.replace_node.Event_To_CustomEvent_PreservesUserDefinedPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeEvent_To_CustomEvent_PreservesUserDefinedPinsTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceCustomEventPins"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    TestNotNull(TEXT("event graph exists"), Graph);

    UK2Node_CustomEvent* OldNode = AddConfiguredNode<UK2Node_CustomEvent>(
        Graph,
        [](UK2Node_CustomEvent* Node)
        {
            Node->CustomFunctionName = TEXT("OldEventWithPins");
        });
    OldNode->CreateUserDefinedPin(TEXT("Payload"), MakePinType(UEdGraphSchema_K2::PC_String), EGPD_Output);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("CustomEvent"), Capture, TEXT("NewEventWithPins")));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_CustomEvent"));
    UK2Node_CustomEvent* NewEvent = Cast<UK2Node_CustomEvent>(NewNode);
    TestNotNull(TEXT("new node is CustomEvent"), NewEvent);
    TestNotNull(TEXT("user-defined pin preserved"), NewEvent ? NewEvent->FindPin(TEXT("Payload"), EGPD_Output) : nullptr);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeCallFunction_RetargetSameClass_InfersFromOldNodeTest,
    "PinWright.blueprint.graph.replace_node.CallFunction_RetargetSameClass_InfersFromOldNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeCallFunction_RetargetSameClass_InfersFromOldNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceCallInfer"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_CallFunction* OldNode = AddCallFunctionNode(
        Graph,
        UKismetSystemLibrary::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("CallFunction"), Capture, TEXT("PrintText")));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_CallFunction"));
    UK2Node_CallFunction* NewCall = Cast<UK2Node_CallFunction>(NewNode);
    TestEqual(TEXT("function retargeted within inferred old class"),
        NewCall ? NewCall->FunctionReference.GetMemberName() : NAME_None,
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeCallFunction_QualifiedTarget_ChangesClassTest,
    "PinWright.blueprint.graph.replace_node.CallFunction_QualifiedTarget_ChangesClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeCallFunction_QualifiedTarget_ChangesClassTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceCallQualified"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_CallFunction* OldNode = AddCallFunctionNode(
        Graph,
        UKismetSystemLibrary::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("CallFunction"), Capture, TEXT("GameplayStatics::GetPlatformName")));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_CallFunction"));
    UK2Node_CallFunction* NewCall = Cast<UK2Node_CallFunction>(NewNode);
    TestEqual(TEXT("function retargeted to qualified class"),
        NewCall ? NewCall->FunctionReference.GetMemberName() : NAME_None,
        GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetPlatformName));
    UClass* NewMemberParent = NewCall ? NewCall->FunctionReference.GetMemberParentClass(NewCall->GetBlueprintClassFromNode()) : nullptr;
    TestEqual(TEXT("function parent retargeted to qualified class"), NewMemberParent, UGameplayStatics::StaticClass());
    TestFalse(TEXT("function parent no longer uses original class"), NewMemberParent == UKismetSystemLibrary::StaticClass());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeDynamicCast_Retarget_DifferentClassTest,
    "PinWright.blueprint.graph.replace_node.DynamicCast_Retarget_DifferentClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeDynamicCast_Retarget_DifferentClassTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceCast"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_DynamicCast* OldNode = AddDynamicCastNode(Graph, AActor::StaticClass());

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("DynamicCast"), Capture, TEXT("Pawn")));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_DynamicCast"));
    UK2Node_DynamicCast* NewCast = Cast<UK2Node_DynamicCast>(NewNode);
    TestEqual(TEXT("cast target changed"), NewCast ? NewCast->TargetType.Get() : nullptr, APawn::StaticClass());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeGeneric_FormatText_SpawnsCleanlyTest,
    "PinWright.blueprint.graph.replace_node.Generic_FormatText_SpawnsCleanly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeGeneric_FormatText_SpawnsCleanlyTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceGenericFormat"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddNode<UK2Node_IfThenElse>(Graph);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("UK2Node_FormatText"), Capture));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("generic"), TEXT("K2Node_FormatText"));
    TestNotNull(TEXT("new node is FormatText"), Cast<UK2Node_FormatText>(NewNode));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeGeneric_Knot_SpawnsCleanlyTest,
    "PinWright.blueprint.graph.replace_node.Generic_Knot_SpawnsCleanly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeGeneric_Knot_SpawnsCleanlyTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceGenericKnot"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddNode<UK2Node_IfThenElse>(Graph);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("UK2Node_Knot"), Capture));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("generic"), TEXT("K2Node_Knot"));
    TestNotNull(TEXT("new node is Knot"), Cast<UK2Node_Knot>(NewNode));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeGeneric_CallFunction_Refused_RoutedToExplicitHintTest,
    "PinWright.blueprint.graph.replace_node.Generic_CallFunction_Refused_RoutedToExplicitHint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeGeneric_CallFunction_Refused_RoutedToExplicitHintTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceGenericCallRefused"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddNode<UK2Node_IfThenElse>(Graph);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("UK2Node_CallFunction"), Capture));

    AssertErrorAndOldNodePresent(*this, Capture, TEXT("UnsupportedNodeClass"), Graph, OldNode);
    TestTrue(TEXT("error message routes caller to explicit contract"),
        Capture.Message.Contains(TEXT("explicit"), ESearchCase::IgnoreCase));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeGeneric_AbstractClass_RefusedWithReplaceRefusedTest,
    "PinWright.blueprint.graph.replace_node.Generic_AbstractClass_RefusedWithReplaceRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeGeneric_AbstractClass_RefusedWithReplaceRefusedTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceGenericAbstract"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddNode<UK2Node_IfThenElse>(Graph);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("UK2Node"), Capture));

    AssertErrorAndOldNodePresent(*this, Capture, TEXT("REPLACE_REFUSED"), Graph, OldNode);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeGeneric_TargetIgnoredFlagTest,
    "PinWright.blueprint.graph.replace_node.Generic_TargetIgnoredFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeGeneric_TargetIgnoredFlagTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceGenericTargetIgnored"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddNode<UK2Node_IfThenElse>(Graph);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("UK2Node_Self"), Capture, TEXT("Ignored.Target")));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("generic"), TEXT("K2Node_Self"));
    TestNotNull(TEXT("new node is Self"), Cast<UK2Node_Self>(NewNode));
    TestTrue(TEXT("targetIgnored flag returned"), Capture.Result.IsValid() && Capture.Result->GetBoolField(TEXT("targetIgnored")));
    TestFalse(TEXT("old node removed after generic replace"), IsNodeStillPresent(Graph, OldNode));
    TestTrue(TEXT("replacement node remains in original graph"), NewNode && IsNodeStillPresent(Graph, NewNode));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeBranch_To_Select_DropsConditionPin_RecordedTest,
    "PinWright.blueprint.graph.replace_node.Branch_To_Select_DropsConditionPin_Recorded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeBranch_To_Select_DropsConditionPin_RecordedTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceBranchSelectDrop"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddBranchWithLinkedCondition(Blueprint, Graph);
    UEdGraphPin* OldConditionPin = OldNode ? OldNode->FindPin(UEdGraphSchema_K2::PN_Condition, EGPD_Input) : nullptr;
    TestTrue(TEXT("condition fixture has a real linked wire"), OldConditionPin && OldConditionPin->LinkedTo.Num() == 1);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("Select"), Capture, FString(), FString(), true));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_Select"));
    UK2Node_Select* SelectNode = Cast<UK2Node_Select>(NewNode);
    TestNotNull(TEXT("new node is Select"), SelectNode);
    TestFalse(TEXT("old branch removed after placeholder replace"), IsNodeStillPresent(Graph, OldNode));
    TestTrue(TEXT("replacement select remains in graph"), NewNode && IsNodeStillPresent(Graph, NewNode));
    const TArray<TSharedPtr<FJsonValue>>* Dropped = nullptr;
    TestTrue(TEXT("connectionsDropped is returned for compatibility"),
        Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("connectionsDropped"), Dropped));
    TestEqual(TEXT("condition wire is preserved as placeholder instead of dropped"), Dropped ? Dropped->Num() : -1, 0);
    AssertPinIssueArrayContains(*this, Capture, TEXT("orphanPlaceholdersCreated"), TEXT("Condition"), TEXT("NO_MATCH"), 1);
    const UEdGraphPin* Placeholder = NewNode ? NewNode->FindPin(UEdGraphSchema_K2::PN_Condition, EGPD_Input) : nullptr;
    TestTrue(TEXT("replacement records orphaned condition pin"), Placeholder && Placeholder->bOrphanedPin && Placeholder->LinkedTo.Num() == 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeBranch_To_Select_WithPinRemap_ConditionToIndexTest,
    "PinWright.blueprint.graph.replace_node.Branch_To_Select_WithPinRemap_ConditionToIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeBranch_To_Select_WithPinRemap_ConditionToIndexTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceBranchSelectRemap"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddBranchWithLinkedCondition(Blueprint, Graph);

    TSharedPtr<FJsonObject> PinRemap = MakeShared<FJsonObject>();
    PinRemap->SetStringField(TEXT("Condition"), TEXT("Index"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("Select"), Capture, FString(), FString(), false, PinRemap));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_Select"));
    UK2Node_Select* SelectNode = Cast<UK2Node_Select>(NewNode);
    TestNotNull(TEXT("new node is Select"), SelectNode);
    TestTrue(TEXT("pinRemapApplied field returned"),
        Capture.Result.IsValid() && Capture.Result->GetIntegerField(TEXT("pinRemapApplied")) >= 1);
    TestFalse(TEXT("pinRemapUnmatched omitted for fully matched remap"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("pinRemapUnmatched")));
    TestTrue(TEXT("Condition wire moved to Select index"),
        SelectNode && SelectNode->GetIndexPin() && SelectNode->GetIndexPin()->LinkedTo.Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeBranch_To_Select_DefaultStrict_HardErrorsTest,
    "PinWright.blueprint.graph.replace_node.Branch_To_Select_DefaultStrict_HardErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeBranch_To_Select_DefaultStrict_HardErrorsTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceBranchSelectStrict"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddBranchWithLinkedCondition(Blueprint, Graph);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("Select"), Capture));

    AssertErrorAndOldNodePresent(*this, Capture, TEXT("PIN_REMAP_INVALID"), Graph, OldNode);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeBranch_To_Select_OrphanPlaceholders_CreatesRedWiresTest,
    "PinWright.blueprint.graph.replace_node.Branch_To_Select_OrphanPlaceholders_CreatesRedWires",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeBranch_To_Select_OrphanPlaceholders_CreatesRedWiresTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceBranchSelectOrphans"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddBranchWithLinkedCondition(Blueprint, Graph);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("Select"), Capture, FString(), FString(), true));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_Select"));
    const UEdGraphPin* Placeholder = NewNode ? NewNode->FindPin(UEdGraphSchema_K2::PN_Condition, EGPD_Input) : nullptr;
    TestTrue(TEXT("allowOrphanPlaceholders creates orphan condition pin"),
        Placeholder && Placeholder->bOrphanedPin && Placeholder->LinkedTo.Num() > 0);
    TestTrue(TEXT("orphanPlaceholdersCreated field returned"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("orphanPlaceholdersCreated")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeSplitStructPin_TransfersSubWiresTest,
    "PinWright.blueprint.graph.replace_node.SplitStructPin_TransfersSubWires",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeSplitStructPin_TransfersSubWiresTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceSplitStruct"));
    UEdGraph* Graph = GetEventGraph(Blueprint);

    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("ReplaceNodeX"), MakePinType(UEdGraphSchema_K2::PC_Double));
    UK2Node_VariableGet* XSource = AddVariableGetNode(Graph, TEXT("ReplaceNodeX"));
    UK2Node_CallFunction* OldNode = AddCallFunctionNode(
        Graph,
        AActor::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(AActor, K2_SetActorLocation));

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    UEdGraphPin* NewLocation = OldNode->FindPin(TEXT("NewLocation"), EGPD_Input);
    if (NewLocation)
    {
        Schema->SplitPin(NewLocation, false);
    }

    UEdGraphPin* SourcePin = XSource->FindPin(TEXT("ReplaceNodeX"), EGPD_Output);
    UEdGraphPin* SplitX = OldNode->FindPin(TEXT("NewLocation_X"), EGPD_Input);
    if (SourcePin && SplitX)
    {
        SourcePin->MakeLinkTo(SplitX);
    }

    TSharedPtr<FJsonObject> PinRemap = MakeShared<FJsonObject>();
    PinRemap->SetStringField(TEXT("NewLocation_X"), TEXT("NewLocation_X"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("CallFunction"), Capture, TEXT("AActor::K2_SetActorLocationAndRotation"), FString(), false, PinRemap));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_CallFunction"));
    TestTrue(TEXT("split struct pins were created on replacement"),
        Capture.Result.IsValid() && Capture.Result->GetIntegerField(TEXT("subPinsSplit")) >= 1);
    TestTrue(TEXT("split subpin remap was applied"),
        Capture.Result.IsValid() && Capture.Result->GetIntegerField(TEXT("pinRemapApplied")) >= 1);
    TestFalse(TEXT("split subpin remap had no unmatched entries"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("pinRemapUnmatched")));
    TestTrue(TEXT("split X wire moved to replacement subpin"),
        NewNode && NewNode->FindPin(TEXT("NewLocation_X"), EGPD_Input) && NewNode->FindPin(TEXT("NewLocation_X"), EGPD_Input)->LinkedTo.Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeCrossGraphLookup_NodeIdInFunctionGraph_FindsWithoutGraphNameTest,
    "PinWright.blueprint.graph.replace_node.CrossGraphLookup_NodeIdInFunctionGraph_FindsWithoutGraphName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeCrossGraphLookup_NodeIdInFunctionGraph_FindsWithoutGraphNameTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceCrossGraph"));
    UEdGraph* FunctionGraph = AddFunctionGraph(Blueprint, TEXT("ReplaceNodeFunction"));
    UK2Node_IfThenElse* OldNode = AddNode<UK2Node_IfThenElse>(FunctionGraph);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("Sequence"), Capture));

    UEdGraphNode* NewNode = AssertSuccessAndFindNewNode(*this, Blueprint, Capture, TEXT("explicit"), TEXT("K2Node_ExecutionSequence"));
    TestNotNull(TEXT("new node is Sequence"), Cast<UK2Node_ExecutionSequence>(NewNode));
    TestFalse(TEXT("old node removed from function graph"), IsNodeStillPresent(FunctionGraph, OldNode));
    TestTrue(TEXT("replacement node remains in function graph"), NewNode && IsNodeStillPresent(FunctionGraph, NewNode));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeAmbiguous_NodeName_ErrorsTest,
    "PinWright.blueprint.graph.replace_node.Ambiguous_NodeName_Errors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeAmbiguous_NodeName_ErrorsTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceAmbiguousName"));
    UEdGraph* EventGraph = GetEventGraph(Blueprint);
    UEdGraph* FunctionGraph = AddFunctionGraph(Blueprint, TEXT("ReplaceNodeAmbiguousFunction"));
    UK2Node_IfThenElse* EventNode = AddNode<UK2Node_IfThenElse>(EventGraph);
    UK2Node_IfThenElse* FunctionNode = AddNode<UK2Node_IfThenElse>(FunctionGraph);

    EventNode->Rename(TEXT("ReplaceNodeSharedName"), EventGraph, REN_DontCreateRedirectors);
    FunctionNode->Rename(TEXT("ReplaceNodeSharedName"), FunctionGraph, REN_DontCreateRedirectors);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Payload->SetStringField(TEXT("nodeId"), TEXT("ReplaceNodeSharedName"));
    Payload->SetStringField(TEXT("newNodeType"), TEXT("Sequence"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeHandlerWithCapture(ReplaceNodeMethod, Payload, Capture));

    AssertErrorAndOldNodePresent(*this, Capture, TEXT("INVALID_ARGUMENT"), EventGraph, EventNode);
    TestTrue(TEXT("other ambiguous node also remains present"), IsNodeStillPresent(FunctionGraph, FunctionNode));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeNoOp_SameClassSameTarget_ShortCircuitsTest,
    "PinWright.blueprint.graph.replace_node.NoOp_SameClassSameTarget_ShortCircuits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeNoOp_SameClassSameTarget_ShortCircuitsTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceNoop"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddNode<UK2Node_IfThenElse>(Graph);
    const int32 NodeCountBefore = Graph ? Graph->Nodes.Num() : 0;

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("Branch"), Capture));

    TestTrue(TEXT("replace_node reports success"), Capture.bSuccess);
    TestTrue(TEXT("noop flag returned"), Capture.Result.IsValid() && Capture.Result->GetBoolField(TEXT("noop")));
    FString NodeId;
    TestTrue(TEXT("noop returns original nodeId"), Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
    TestEqual(TEXT("noop nodeId matches original node"), NodeId, OldNode->NodeGuid.ToString());
    FString FactoryPath;
    TestTrue(TEXT("noop returns factoryPath"), Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("factoryPath"), FactoryPath));
    TestEqual(TEXT("noop factoryPath is explicit"), FactoryPath, FString(TEXT("explicit")));
    FString ResolvedClass;
    TestTrue(TEXT("noop returns resolvedClass"), Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("resolvedClass"), ResolvedClass));
    TestTrue(TEXT("noop resolvedClass names branch class"), ResolvedClass.Contains(TEXT("K2Node_IfThenElse")));
    TestFalse(TEXT("noop does not return newNodeId"), Capture.Result.IsValid() && Capture.Result->HasField(TEXT("newNodeId")));
    TestTrue(TEXT("old branch remains after noop"), IsNodeStillPresent(Graph, OldNode));
    TestEqual(TEXT("noop keeps graph node count unchanged"), Graph ? Graph->Nodes.Num() : 0, NodeCountBefore);
    int32 MatchingNodeCount = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node == OldNode)
        {
            ++MatchingNodeCount;
        }
    }
    TestEqual(TEXT("graph still contains only the original node once"), MatchingNodeCount, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeStructurallyModified_BumpsStampTest,
    "PinWright.blueprint.graph.replace_node.StructurallyModified_BumpsStamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeStructurallyModified_BumpsStampTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceStructuralDirty"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_IfThenElse* OldNode = AddNode<UK2Node_IfThenElse>(Graph);
    Blueprint->Status = BS_UpToDate;

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, OldNode, TEXT("Sequence"), Capture));

    AssertSuccessCommon(*this, Capture, TEXT("explicit"), TEXT("K2Node_ExecutionSequence"));
    TestTrue(TEXT("Blueprint marked structurally dirty"), Blueprint->Status == BS_Dirty || Blueprint->Status == BS_Unknown);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeRefused_FunctionEntry_ReturnsReplaceRefusedTest,
    "PinWright.blueprint.graph.replace_node.Refused_FunctionEntry_ReturnsReplaceRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeRefused_FunctionEntry_ReturnsReplaceRefusedTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceRefuseEntry"));
    UEdGraph* FunctionGraph = AddFunctionGraph(Blueprint, TEXT("ReplaceNodeEntryFunction"));
    UK2Node_FunctionEntry* EntryNode = FindNodeOfTypeInGraph<UK2Node_FunctionEntry>(FunctionGraph);
    TestNotNull(TEXT("function entry exists"), EntryNode);

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, EntryNode, TEXT("Branch"), Capture));

    AssertErrorAndOldNodePresent(*this, Capture, TEXT("REPLACE_REFUSED"), FunctionGraph, EntryNode);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeRefused_NonDeletable_CanUserDeleteNodeGateTest,
    "PinWright.blueprint.graph.replace_node.Refused_NonDeletable_CanUserDeleteNodeGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeRefused_NonDeletable_CanUserDeleteNodeGateTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceRefuseNonDeletable"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UAnimGraphNode_Root* NonDeletableNode = NewObject<UAnimGraphNode_Root>(Graph);
    NonDeletableNode->CreateNewGuid();
    Graph->AddNode(NonDeletableNode, true, false);
    TestFalse(TEXT("fixture node is not user-deletable"), NonDeletableNode->CanUserDeleteNode());

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, NonDeletableNode, TEXT("Branch"), Capture));

    AssertErrorAndOldNodePresent(*this, Capture, TEXT("REPLACE_REFUSED"), Graph, NonDeletableNode);
    TestTrue(TEXT("refusal came from CanUserDeleteNode gate"),
        Capture.Message.Contains(TEXT("cannot be deleted by users")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReplaceNodeRefused_MacroInstance_ReturnsReplaceRefusedTest,
    "PinWright.blueprint.graph.replace_node.Refused_MacroInstance_ReturnsReplaceRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReplaceNodeRefused_MacroInstance_ReturnsReplaceRefusedTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CreateReplaceNodeTestBlueprint(TEXT("ReplaceRefuseMacro"));
    UEdGraph* Graph = GetEventGraph(Blueprint);
    UK2Node_MacroInstance* MacroNode = AddForEachMacroNode(Graph);
    if (!TestNotNull(TEXT("ForEach macro fixture available"), MacroNode))
    {
        return true;
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("replace_node handler found"),
        InvokeReplaceNode(Blueprint, MacroNode, TEXT("Branch"), Capture));

    AssertErrorAndOldNodePresent(*this, Capture, TEXT("REPLACE_REFUSED"), Graph, MacroNode);
    return true;
}
