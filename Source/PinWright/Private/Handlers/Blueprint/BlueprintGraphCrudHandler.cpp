// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintGraphCrudHandler.cpp - Blueprint graph node/pin CRUD handlers.
// Split from BlueprintGraphHandler.cpp: node create/modify/delete plus pin default
// value mutation. Shared graph helpers live in BlueprintGraphHelpers.h.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintEnumHelpers.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "Compiler/CodeNodeEmitter.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/AssetUtils.h"
#include "Utils/PropertyUtils.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ClassDynamicCast.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Composite.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_DeadClass.h"
#include "K2Node_DelegateSet.h"
#if __has_include("K2Node_GeneratedBoundEvent.h")
#include "K2Node_GeneratedBoundEvent.h"
#define MCP_HAS_K2NODE_GENERATEDBOUNDEVENT 1
#else
#define MCP_HAS_K2NODE_GENERATEDBOUNDEVENT 0
#endif
#include "K2Node_InputActionEvent.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputAxisKeyEvent.h"
#include "K2Node_InputKeyEvent.h"
#include "K2Node_InputTouchEvent.h"
#include "K2Node_InputVectorAxisEvent.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MathExpression.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_Timeline.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "InputAction.h"

using BlueprintGraphHelpers::ResolveBlueprintAndGraph;
using BlueprintGraphHelpers::FindNodeByIdOrName;
using BlueprintGraphHelpers::FindPinByName;

// ---------------------------------------------------------------------------
// create_node-only resolution helpers
// ---------------------------------------------------------------------------

// Common function node name -> (ClassName, FunctionName) mapping
static const TMap<FString, TTuple<FString, FString>>& GetCommonFunctionNodes()
{
    static TMap<FString, TTuple<FString, FString>> Map = {
        {TEXT("PrintString"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("PrintString"))},
        {TEXT("Print"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("PrintString"))},
        {TEXT("PrintText"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("PrintText"))},
        {TEXT("SetActorLocation"), MakeTuple(TEXT("AActor"), TEXT("K2_SetActorLocation"))},
        {TEXT("GetActorLocation"), MakeTuple(TEXT("AActor"), TEXT("K2_GetActorLocation"))},
        {TEXT("SetActorRotation"), MakeTuple(TEXT("AActor"), TEXT("K2_SetActorRotation"))},
        {TEXT("GetActorRotation"), MakeTuple(TEXT("AActor"), TEXT("K2_GetActorRotation"))},
        {TEXT("SetActorTransform"), MakeTuple(TEXT("AActor"), TEXT("K2_SetActorTransform"))},
        {TEXT("GetActorTransform"), MakeTuple(TEXT("AActor"), TEXT("K2_GetActorTransform"))},
        {TEXT("AddActorLocalOffset"), MakeTuple(TEXT("AActor"), TEXT("K2_AddActorLocalOffset"))},
        {TEXT("Delay"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("Delay"))},
        {TEXT("DestroyActor"), MakeTuple(TEXT("AActor"), TEXT("K2_DestroyActor"))},
        {TEXT("SpawnActor"), MakeTuple(TEXT("UGameplayStatics"), TEXT("BeginDeferredActorSpawnFromClass"))},
        {TEXT("GetPlayerPawn"), MakeTuple(TEXT("UGameplayStatics"), TEXT("GetPlayerPawn"))},
        {TEXT("GetPlayerController"), MakeTuple(TEXT("UGameplayStatics"), TEXT("GetPlayerController"))},
        {TEXT("PlaySound"), MakeTuple(TEXT("UGameplayStatics"), TEXT("PlaySound2D"))},
        {TEXT("PlaySound2D"), MakeTuple(TEXT("UGameplayStatics"), TEXT("PlaySound2D"))},
        {TEXT("PlaySoundAtLocation"), MakeTuple(TEXT("UGameplayStatics"), TEXT("PlaySoundAtLocation"))},
        {TEXT("GetWorldDeltaSeconds"), MakeTuple(TEXT("UGameplayStatics"), TEXT("GetWorldDeltaSeconds"))},
        {TEXT("SetTimerByFunctionName"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("K2_SetTimer"))},
        {TEXT("ClearTimer"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("K2_ClearTimer"))},
        {TEXT("IsValid"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("IsValid"))},
        {TEXT("IsValidClass"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("IsValidClass"))},
        {TEXT("Add_IntInt"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("Add_IntInt"))},
        {TEXT("Subtract_IntInt"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("Subtract_IntInt"))},
        {TEXT("Multiply_IntInt"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("Multiply_IntInt"))},
        {TEXT("Divide_IntInt"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("Divide_IntInt"))},
        {TEXT("Add_DoubleDouble"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("Add_DoubleDouble"))},
        {TEXT("Subtract_DoubleDouble"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("Subtract_DoubleDouble"))},
        {TEXT("Multiply_DoubleDouble"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("Multiply_DoubleDouble"))},
        {TEXT("Divide_DoubleDouble"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("Divide_DoubleDouble"))},
        {TEXT("FTrunc"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("FTrunc"))},
        {TEXT("MakeVector"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("MakeVector"))},
        {TEXT("BreakVector"), MakeTuple(TEXT("UKismetMathLibrary"), TEXT("BreakVector"))},
        {TEXT("GetComponentByClass"), MakeTuple(TEXT("AActor"), TEXT("GetComponentByClass"))},
        {TEXT("GetWorldTimerManager"), MakeTuple(TEXT("UKismetSystemLibrary"), TEXT("K2_GetTimerManager"))},
    };
    return Map;
}

static UClass* ResolveCommonFunctionOwnerClass(const FString& ClassName)
{
    if (ClassName == TEXT("UKismetSystemLibrary")) return UKismetSystemLibrary::StaticClass();
    if (ClassName == TEXT("UGameplayStatics")) return UGameplayStatics::StaticClass();
    if (ClassName == TEXT("AActor")) return AActor::StaticClass();
    if (ClassName == TEXT("UKismetMathLibrary")) return UKismetMathLibrary::StaticClass();
    return ResolveUClass(ClassName);
}

static bool ResolveCommonFunctionNodeShortcut(
    const FString& NodeType,
    FString& OutClassName,
    FString& OutFunctionName,
    UFunction*& OutFunction)
{
    OutClassName.Reset();
    OutFunctionName.Reset();
    OutFunction = nullptr;

    const TTuple<FString, FString>* FuncInfo = GetCommonFunctionNodes().Find(NodeType);
    if (!FuncInfo)
    {
        return false;
    }

    OutClassName = FuncInfo->Get<0>();
    OutFunctionName = FuncInfo->Get<1>();
    UClass* Class = ResolveCommonFunctionOwnerClass(OutClassName);
    OutFunction = Class ? Class->FindFunctionByName(*OutFunctionName) : nullptr;
    return true;
}

// User-friendly aliases -> K2Node class names
static const TMap<FString, FString>& GetNodeTypeAliases()
{
    static TMap<FString, FString> Map = {
        {TEXT("Branch"), TEXT("K2Node_IfThenElse")},
        {TEXT("IfThenElse"), TEXT("K2Node_IfThenElse")},
        {TEXT("Sequence"), TEXT("K2Node_ExecutionSequence")},
        {TEXT("ExecutionSequence"), TEXT("K2Node_ExecutionSequence")},
        {TEXT("Select"), TEXT("K2Node_Select")},
        {TEXT("Switch"), TEXT("K2Node_SwitchInteger")},
        {TEXT("SwitchOnInt"), TEXT("K2Node_SwitchInteger")},
        {TEXT("SwitchOnEnum"), TEXT("K2Node_SwitchEnum")},
        {TEXT("SwitchOnString"), TEXT("K2Node_SwitchString")},
        {TEXT("SwitchOnName"), TEXT("K2Node_SwitchName")},
        {TEXT("DoOnce"), TEXT("K2Node_DoOnce")},
        {TEXT("DoN"), TEXT("K2Node_DoN")},
        {TEXT("FlipFlop"), TEXT("K2Node_FlipFlop")},
        {TEXT("Gate"), TEXT("K2Node_Gate")},
        {TEXT("MultiGate"), TEXT("K2Node_MultiGate")},
        {TEXT("ForLoop"), TEXT("K2Node_ForLoop")},
        {TEXT("ForLoopWithBreak"), TEXT("K2Node_ForLoopWithBreak")},
        {TEXT("ForEachLoop"), TEXT("K2Node_ForEachElementInEnum")},
        {TEXT("WhileLoop"), TEXT("K2Node_WhileLoop")},
        {TEXT("MakeArray"), TEXT("K2Node_MakeArray")},
        {TEXT("MakeStruct"), TEXT("K2Node_MakeStruct")},
        {TEXT("BreakStruct"), TEXT("K2Node_BreakStruct")},
        {TEXT("MakeMap"), TEXT("K2Node_MakeMap")},
        {TEXT("MakeSet"), TEXT("K2Node_MakeSet")},
        {TEXT("SpawnActorFromClass"), TEXT("K2Node_SpawnActorFromClass")},
        {TEXT("GetAllActorsOfClass"), TEXT("K2Node_GetAllActorsOfClass")},
        {TEXT("Self"), TEXT("K2Node_Self")},
        {TEXT("GetSelf"), TEXT("K2Node_Self")},
        {TEXT("Timeline"), TEXT("K2Node_Timeline")},
        {TEXT("Knot"), TEXT("K2Node_Knot")},
        {TEXT("Reroute"), TEXT("K2Node_Knot")},
        {TEXT("Comment"), TEXT("EdGraphNode_Comment")},
        {TEXT("Literal"), TEXT("K2Node_Literal")},
    };
    return Map;
}

// Find a UEdGraphNode subclass by name, checking aliases
static UClass* FindNodeClassByName(const FString& TypeName)
{
    FString ResolvedName = TypeName;
    if (const FString* Alias = GetNodeTypeAliases().Find(TypeName))
        ResolvedName = *Alias;

    TArray<FString> NamesToTry;
    NamesToTry.Add(ResolvedName);
    if (ResolvedName.StartsWith(TEXT("U")))
    {
        NamesToTry.Add(ResolvedName.Mid(1));
    }
    if (ResolvedName.StartsWith(TEXT("UK2Node_")))
    {
        NamesToTry.Add(ResolvedName.Mid(1));
    }
    NamesToTry.Add(FString::Printf(TEXT("K2Node_%s"), *ResolvedName));
    NamesToTry.Add(FString::Printf(TEXT("UK2Node_%s"), *ResolvedName));
    if (ResolvedName != TypeName)
    {
        NamesToTry.Add(TypeName);
        if (TypeName.StartsWith(TEXT("UK2Node_")))
        {
            NamesToTry.Add(TypeName.Mid(1));
        }
        NamesToTry.Add(FString::Printf(TEXT("K2Node_%s"), *TypeName));
        NamesToTry.Add(FString::Printf(TEXT("UK2Node_%s"), *TypeName));
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (!It->IsChildOf(UEdGraphNode::StaticClass())) continue;
        FString ClassName = It->GetName();
        for (const FString& NameToMatch : NamesToTry)
        {
            if (ClassName.Equals(NameToMatch, ESearchCase::IgnoreCase))
                return *It;
        }
    }
    return nullptr;
}

// ---- blueprint.graph.list_graphs ----
REGISTER_RPC_HANDLER("blueprint.graph.list_graphs", "blueprint.graph",
    "Enumerate every UEdGraph in a Blueprint: EventGraph, function graphs, macro graphs, AnimGraph (for ABPs), construction script. Each entry includes the graph name and kind, ready to be passed to other graph.* methods.",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path; alias 'blueprintPath' is also accepted."))
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Unused = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, Unused, false)) return true;

    TArray<TSharedPtr<FJsonValue>> GraphsArray;
    TSet<FString> SeenGraphNames;

    auto AppendGraph = [&](UEdGraph* Graph, const FString& Kind, const FString& ParentName) {
        if (!Graph) return;
        const FString GraphNameValue = Graph->GetName();
        if (SeenGraphNames.Contains(GraphNameValue)) return;
        SeenGraphNames.Add(GraphNameValue);
        TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
        GraphObj->SetStringField(TEXT("name"), GraphNameValue);
        GraphObj->SetStringField(TEXT("kind"), Kind);
        GraphObj->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
        if (!ParentName.IsEmpty())
        {
            GraphObj->SetStringField(TEXT("parentGraphName"), ParentName);
        }
        GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
    };

    for (UEdGraph* Graph : Blueprint->UbergraphPages) AppendGraph(Graph, TEXT("ubergraph"), FString());
    for (UEdGraph* Graph : Blueprint->FunctionGraphs) AppendGraph(Graph, TEXT("function"), FString());
    for (UEdGraph* Graph : Blueprint->MacroGraphs) AppendGraph(Graph, TEXT("macro"), FString());
    for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) AppendGraph(Graph, TEXT("event"), FString());

    // Surface composite subgraphs and math-expression subgraphs not visible in the four
    // top-level lists. CollectAllBlueprintGraphsRecursive returns all graphs in the same
    // canonical order (top-level lists first); graphs already appended above are skipped
    // by SeenGraphNames, so only subgraphs reach AppendGraph here.
    for (UEdGraph* Graph : BlueprintHandlerUtils::CollectAllBlueprintGraphsRecursive(Blueprint))
    {
        if (!Graph) continue;
        const FString GraphNameValue = Graph->GetName();
        if (SeenGraphNames.Contains(GraphNameValue)) continue;

        // Determine parent graph name for agents navigating the hierarchy
        FString ParentName;
        if (UObject* Outer = Graph->GetOuter())
        {
            // The outer of a BoundGraph is the composite node; its outer is the parent graph.
            if (UEdGraphNode* OwnerNode = Cast<UEdGraphNode>(Outer))
            {
                if (UEdGraph* OwnerGraph = OwnerNode->GetGraph())
                {
                    ParentName = OwnerGraph->GetName();
                }
            }
            else if (UEdGraph* OwnerGraph = Cast<UEdGraph>(Outer))
            {
                ParentName = OwnerGraph->GetName();
            }
        }

        AppendGraph(Graph, TEXT("subgraph"), ParentName);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("graphs"), GraphsArray);
    Result->SetNumberField(TEXT("graphCount"), GraphsArray.Num());
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.create_node ----
REGISTER_RPC_HANDLER("blueprint.graph.create_node", "blueprint.graph",
    "Spawn a single graph node in an existing UEdGraph and return its node id. nodeType selects the node class (e.g. 'CallFunction', 'VariableGet', 'Branch'); other params populate fields specific to that class. Coordinates are required to avoid stacking nodes at origin. For bulk authoring prefer blueprint.compile_bpir.",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeType", "string", "Node type or common function name"),
        RPC_PARAM_OPT("graphName", "string", "Graph name (defaults to EventGraph)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("target", "string", "Type-dependent target spec. For CallFunction accepts bare member names or qualified 'Class::Function' / 'Class.Function'; for Timeline accepts the timeline variable name."),
        RPC_PARAM_OPT("variableName", "string", "Variable name for VariableGet/Set nodes"),
        RPC_PARAM_OPT("memberName", "string", "Function name for CallFunction nodes"),
        RPC_PARAM_OPT("memberClass", "classref", "Class for CallFunction/Event nodes"),
        RPC_PARAM_OPT("eventName", "string", "Event name for Event/CustomEvent nodes"),
        RPC_PARAM_OPT("targetClass", "classref", "Target class for Cast nodes"),
        RPC_PARAM_OPT("timelineName", "string", "Legacy Timeline variable name alias; target takes precedence"),
        RPC_PARAM_OPT("inputAxisName", "string", "Axis name for InputAxisEvent nodes"),
        RPC_PARAM_OPT("inputAction", "path", "Full UInputAction object path for EnhancedInputAction nodes")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString NodeType;
    Payload->TryGetStringField(TEXT("nodeType"), NodeType);
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    float X = static_cast<float>(XD), Y = static_cast<float>(YD);

    const bool bIsEnhancedInputAction = NodeType == TEXT("EnhancedInputAction")
        || NodeType == TEXT("K2Node_EnhancedInputAction")
        || NodeType == TEXT("UK2Node_EnhancedInputAction");
    UInputAction* EnhancedInputAction = nullptr;
    UClass* EnhancedInputNodeClass = nullptr;
    FObjectProperty* EnhancedInputActionProperty = nullptr;
    if (bIsEnhancedInputAction)
    {
        FString InputActionPath;
        Payload->TryGetStringField(TEXT("inputAction"), InputActionPath);
        if (InputActionPath.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("inputAction is required"));
            return true;
        }

        FString LoadError;
        UObject* LoadedObject = ResolveUObjectByPath(InputActionPath, LoadError);
        if (!LoadedObject)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), LoadError.IsEmpty()
                ? FString::Printf(TEXT("Input action asset '%s' could not be loaded"), *InputActionPath)
                : LoadError);
            return true;
        }
        EnhancedInputAction = Cast<UInputAction>(LoadedObject);
        if (!EnhancedInputAction)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(
                TEXT("Asset '%s' has type '%s', expected UInputAction"),
                *InputActionPath, *LoadedObject->GetClass()->GetName()));
            return true;
        }

        EnhancedInputNodeClass = FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass();
        EnhancedInputActionProperty = FCodeNodeEmitter::ResolveEnhancedInputActionProperty(EnhancedInputNodeClass);
        if (!EnhancedInputNodeClass || !EnhancedInputActionProperty
            || !EnhancedInputAction->IsA(EnhancedInputActionProperty->PropertyClass))
        {
            Ctx.SendError(TEXT("NODE_TYPE_NOT_FOUND"),
                TEXT("K2Node_EnhancedInputAction or its InputAction property is unavailable"));
            return true;
        }
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("Create Blueprint Node")));
    Blueprint->Modify();
    TargetGraph->Modify();

    // Finalize-and-report helper
    auto FinalizeAndReport = [&](auto& NodeCreator, UEdGraphNode* NewNode) {
        if (NewNode)
        {
            NewNode->NodePosX = X;
            NewNode->NodePosY = Y;
            NodeCreator.Finalize();
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("nodeId"), NewNode->NodeGuid.ToString());
            Result->SetStringField(TEXT("nodeName"), NewNode->GetName());
            AddAssetVerification(Result, Blueprint);
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(TEXT("CREATE_FAILED"),
                TEXT("Failed to create node (unsupported type or internal error)."));
        }
    };

    FString CommonFunctionClassName;
    FString CommonFunctionName;
    UFunction* CommonFunction = nullptr;
    if (ResolveCommonFunctionNodeShortcut(NodeType, CommonFunctionClassName, CommonFunctionName, CommonFunction))
    {
        if (!CommonFunction)
        {
            Ctx.SendError(TEXT("FUNCTION_NOT_FOUND"),
                FString::Printf(TEXT("Could not find function '%s::%s' for node type '%s'"), *CommonFunctionClassName, *CommonFunctionName, *NodeType));
            return true;
        }

        FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*TargetGraph);
        UK2Node_CallFunction* CallFuncNode = NodeCreator.CreateNode(false);
        CallFuncNode->SetFromFunction(CommonFunction);
        FinalizeAndReport(NodeCreator, CallFuncNode);
        return true;
    }

    // Special nodes requiring extra parameters
    if (NodeType == TEXT("VariableGet") || NodeType == TEXT("K2Node_VariableGet"))
    {
        FString VarName;
        Payload->TryGetStringField(TEXT("variableName"), VarName);
        FName VarFName(*VarName);
        bool bFound = false;
        for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
        {
            if (VarDesc.VarName == VarFName) { bFound = true; break; }
        }
        if (!bFound && Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindPropertyByName(VarFName))
            bFound = true;
        if (!bFound)
        {
            Ctx.SendError(TEXT("VARIABLE_NOT_FOUND"),
                FString::Printf(TEXT("Variable '%s' not found"), *VarName));
            return true;
        }
        FGraphNodeCreator<UK2Node_VariableGet> NodeCreator(*TargetGraph);
        UK2Node_VariableGet* VarGet = NodeCreator.CreateNode(false);
        VarGet->VariableReference.SetSelfMember(VarFName);
        FinalizeAndReport(NodeCreator, VarGet);
        return true;
    }

    if (NodeType == TEXT("VariableSet") || NodeType == TEXT("K2Node_VariableSet"))
    {
        FString VarName;
        Payload->TryGetStringField(TEXT("variableName"), VarName);
        FName VarFName(*VarName);
        bool bFound = false;
        for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
        {
            if (VarDesc.VarName == VarFName) { bFound = true; break; }
        }
        if (!bFound && Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindPropertyByName(VarFName))
            bFound = true;
        if (!bFound)
        {
            Ctx.SendError(TEXT("VARIABLE_NOT_FOUND"),
                FString::Printf(TEXT("Variable '%s' not found"), *VarName));
            return true;
        }
        FGraphNodeCreator<UK2Node_VariableSet> NodeCreator(*TargetGraph);
        UK2Node_VariableSet* VarSet = NodeCreator.CreateNode(false);
        VarSet->VariableReference.SetSelfMember(VarFName);
        FinalizeAndReport(NodeCreator, VarSet);
        return true;
    }

    if (NodeType == TEXT("CallFunction") || NodeType == TEXT("K2Node_CallFunction") || NodeType == TEXT("FunctionCall"))
    {
        FString TargetSpec;
        FString MemberName, MemberClass;
        Payload->TryGetStringField(TEXT("target"), TargetSpec);
        Payload->TryGetStringField(TEXT("memberName"), MemberName);
        Payload->TryGetStringField(TEXT("memberClass"), MemberClass);

        const BlueprintHandlerUtils::FBlueprintGraphTargetParts TargetParts = !TargetSpec.TrimStartAndEnd().IsEmpty()
            ? BlueprintHandlerUtils::ParseGraphTargetSpec(TargetSpec)
            : BlueprintHandlerUtils::FBlueprintGraphTargetParts{ MemberClass.TrimStartAndEnd(), MemberName.TrimStartAndEnd() };

        FString ErrorCode;
        FString ErrorMessage;
        UFunction* Func = BlueprintHandlerUtils::ResolveGraphCallableFunction(
            Blueprint,
            TargetParts,
            Blueprint ? Blueprint->GeneratedClass : nullptr,
            ErrorCode,
            ErrorMessage);

        if (Func)
        {
            FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*TargetGraph);
            UK2Node_CallFunction* CallFuncNode = NodeCreator.CreateNode(false);
            CallFuncNode->SetFromFunction(Func);
            FinalizeAndReport(NodeCreator, CallFuncNode);
        }
        else
        {
            Ctx.SendError(
                ErrorCode.IsEmpty() ? TEXT("FUNCTION_NOT_FOUND") : *ErrorCode,
                ErrorMessage.IsEmpty()
                    ? FString::Printf(TEXT("Function '%s' not found"), *TargetParts.MemberName)
                    : ErrorMessage);
        }
        return true;
    }

    if (NodeType == TEXT("Event") || NodeType == TEXT("K2Node_Event"))
    {
        FString EventName, MemberClass;
        Payload->TryGetStringField(TEXT("eventName"), EventName);
        Payload->TryGetStringField(TEXT("memberClass"), MemberClass);
        if (EventName.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("eventName required")); return true; }
        // Class-aware shorthand: only apply the AActor Receive-prefix mapping when
        // the parent is an AActor subclass AND the literal name does not already
        // resolve on the parent. UUserWidget::Tick must stay literal "Tick".
        static const TMap<FString, FString> Aliases = {
            {TEXT("BeginPlay"), TEXT("ReceiveBeginPlay")},
            {TEXT("Tick"), TEXT("ReceiveTick")},
            {TEXT("EndPlay"), TEXT("ReceiveEndPlay")}};
        if (Blueprint && Blueprint->ParentClass)
        {
            const bool bLiteralResolves = Blueprint->ParentClass->FindFunctionByName(FName(*EventName)) != nullptr;
            const bool bIsActorSubclass = Blueprint->ParentClass->IsChildOf(AActor::StaticClass());
            if (!bLiteralResolves && bIsActorSubclass)
            {
                if (const FString* A = Aliases.Find(EventName)) EventName = *A;
            }
        }

        UClass* TargetClass = nullptr;
        UFunction* EventFunc = nullptr;
        if (!MemberClass.IsEmpty())
        {
            TargetClass = ResolveUClass(MemberClass);
            if (TargetClass) EventFunc = TargetClass->FindFunctionByName(*EventName);
        }
        else
        {
            for (UClass* C = Blueprint->ParentClass; C && !EventFunc; C = C->GetSuperClass())
            {
                EventFunc = C->FindFunctionByName(*EventName, EIncludeSuperFlag::ExcludeSuper);
                if (EventFunc) TargetClass = C;
            }
        }
        if (EventFunc && TargetClass)
        {
            FGraphNodeCreator<UK2Node_Event> NodeCreator(*TargetGraph);
            UK2Node_Event* EventNode = NodeCreator.CreateNode(false);
            EventNode->EventReference.SetFromField<UFunction>(EventFunc, false);
            EventNode->bOverrideFunction = true;
            FinalizeAndReport(NodeCreator, EventNode);
        }
        else
        {
            Ctx.SendError(TEXT("EVENT_NOT_FOUND"),
                FString::Printf(TEXT("Event '%s' not found"), *EventName));
        }
        return true;
    }

    if (NodeType == TEXT("CustomEvent") || NodeType == TEXT("K2Node_CustomEvent"))
    {
        FString EventName;
        Payload->TryGetStringField(TEXT("eventName"), EventName);
        FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*TargetGraph);
        UK2Node_CustomEvent* EventNode = NodeCreator.CreateNode(false);
        EventNode->CustomFunctionName = FName(*EventName);
        FinalizeAndReport(NodeCreator, EventNode);
        return true;
    }

    if (NodeType == TEXT("Cast") || NodeType.StartsWith(TEXT("CastTo")))
    {
        FString TargetClassName;
        Payload->TryGetStringField(TEXT("targetClass"), TargetClassName);
        if (TargetClassName.IsEmpty() && NodeType.StartsWith(TEXT("CastTo")))
            TargetClassName = NodeType.Mid(6);
        UClass* TargetClass = ResolveUClass(TargetClassName);
        if (!TargetClass) { Ctx.SendError(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("Class '%s' not found"), *TargetClassName)); return true; }
        FGraphNodeCreator<UK2Node_DynamicCast> NodeCreator(*TargetGraph);
        UK2Node_DynamicCast* CastNode = NodeCreator.CreateNode(false);
        CastNode->TargetType = TargetClass;
        FinalizeAndReport(NodeCreator, CastNode);
        return true;
    }

    if (NodeType == TEXT("InputAxisEvent") || NodeType == TEXT("K2Node_InputAxisEvent"))
    {
        FString InputAxisName;
        Payload->TryGetStringField(TEXT("inputAxisName"), InputAxisName);
        if (InputAxisName.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("inputAxisName required")); return true; }
        FGraphNodeCreator<UK2Node_InputAxisEvent> NodeCreator(*TargetGraph);
        UK2Node_InputAxisEvent* InputNode = NodeCreator.CreateNode(false);
        InputNode->InputAxisName = FName(*InputAxisName);
        FinalizeAndReport(NodeCreator, InputNode);
        return true;
    }

    if (bIsEnhancedInputAction)
    {
        FGraphNodeCreator<UK2Node> NodeCreator(*TargetGraph);
        UK2Node* InputNode = NodeCreator.CreateNode(false, EnhancedInputNodeClass);
        EnhancedInputActionProperty->SetObjectPropertyValue_InContainer(InputNode, EnhancedInputAction);
        FinalizeAndReport(NodeCreator, InputNode);
        return true;
    }

    if (NodeType == TEXT("Timeline") || NodeType == TEXT("K2Node_Timeline") || NodeType == TEXT("UK2Node_Timeline"))
    {
        if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
        {
            Ctx.SendError(TEXT("UNSUPPORTED_BLUEPRINT"),
                TEXT("This Blueprint type does not support timelines."));
            return true;
        }

        FString TargetTimelineName;
        FString LegacyTimelineName;
        Payload->TryGetStringField(TEXT("target"), TargetTimelineName);
        Payload->TryGetStringField(TEXT("timelineName"), LegacyTimelineName);
        TargetTimelineName = TargetTimelineName.TrimStartAndEnd();
        LegacyTimelineName = LegacyTimelineName.TrimStartAndEnd();

        const bool bExplicitTimelineName = !TargetTimelineName.IsEmpty() || !LegacyTimelineName.IsEmpty();
        const FName TimelineFName = bExplicitTimelineName
            ? FName(*(TargetTimelineName.IsEmpty() ? LegacyTimelineName : TargetTimelineName))
            : FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);

        if (bExplicitTimelineName && Blueprint->FindTimelineTemplateByVariableName(TimelineFName))
        {
            Ctx.SendError(TEXT("DUPLICATE_TIMELINE"),
                FString::Printf(TEXT("Timeline '%s' already exists."), *TimelineFName.ToString()));
            return true;
        }

        UTimelineTemplate* TimelineTemplate = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineFName);
        if (!TimelineTemplate)
        {
            Ctx.SendError(TEXT("CREATE_FAILED"),
                FString::Printf(TEXT("Failed to create timeline '%s'."), *TimelineFName.ToString()));
            return true;
        }

        FGraphNodeCreator<UK2Node_Timeline> NodeCreator(*TargetGraph);
        UK2Node_Timeline* TimelineNode = NodeCreator.CreateNode(false);
        TimelineNode->TimelineName = TimelineFName;
        TimelineNode->NodePosX = X;
        TimelineNode->NodePosY = Y;
        NodeCreator.Finalize();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("nodeId"), TimelineNode->NodeGuid.ToString());
        Result->SetStringField(TEXT("nodeName"), TimelineNode->GetName());
        Result->SetStringField(TEXT("nodeClass"), UK2Node_Timeline::StaticClass()->GetName());
        Result->SetStringField(TEXT("timelineName"), TimelineFName.ToString());
        Result->SetStringField(TEXT("timelineTemplatePath"), TimelineTemplate->GetPathName());
        AddAssetVerification(Result, Blueprint);
        Ctx.SendSuccess(Result);
        return true;
    }

    // Dynamic fallback: find any node class by name
    UClass* NodeClass = FindNodeClassByName(NodeType);
    if (NodeClass)
    {
        UEdGraphNode* NewNode = NewObject<UEdGraphNode>(TargetGraph, NodeClass);
        if (NewNode)
        {
            TargetGraph->AddNode(NewNode, false, false);
            NewNode->CreateNewGuid();
            NewNode->PostPlacedNewNode();
            NewNode->AllocateDefaultPins();
            NewNode->NodePosX = X;
            NewNode->NodePosY = Y;
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("nodeId"), NewNode->NodeGuid.ToString());
            Result->SetStringField(TEXT("nodeName"), NewNode->GetName());
            Result->SetStringField(TEXT("nodeClass"), NodeClass->GetName());
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to instantiate node."));
        }
    }
    else
    {
        Ctx.SendError(TEXT("NODE_TYPE_NOT_FOUND"),
            FString::Printf(TEXT("Node type '%s' not found. Use list_node_types to see available types."), *NodeType));
    }
    return true;
}

// ---- blueprint.graph.delete_node ----
REGISTER_RPC_HANDLER("blueprint.graph.delete_node", "blueprint.graph",
    "Delete a node from a blueprint graph",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID or name"),
        RPC_PARAM_OPT("graphName", "string", "Graph name"),
        RPC_PARAM_DEF("cleanupNewOrphans", "boolean", "Delete nodes that become orphaned by this deletion (default: true)", "true")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    FString NodeId = Ctx.GetString(TEXT("nodeId"));
    const bool bCleanupNewOrphans = Ctx.GetBool(TEXT("cleanupNewOrphans"), true);

    const FScopedTransaction Transaction(FText::FromString(TEXT("Delete Blueprint Node")));
    Blueprint->Modify();
    TargetGraph->Modify();

    UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
    if (TargetNode)
    {
        const TSet<FGuid> OrphansBefore =
            BlueprintHandlerUtils::SnapshotBlueprintOrphanGuids(Blueprint, true);

        // Capture the effective function name BEFORE RemoveNode invalidates the node.
        TSet<FName> RemovedFunctionNames;
        const FName EffectiveName = BlueprintHandlerUtils::GetEffectiveFunctionNameForRemoval(TargetNode);
        if (EffectiveName != NAME_None)
        {
            RemovedFunctionNames.Add(EffectiveName);
        }

        FBlueprintEditorUtils::RemoveNode(Blueprint, TargetNode, true);

        const int32 CascadedCreateDelegatesRemoved =
            BlueprintHandlerUtils::CascadeRemoveStaleCreateDelegates(Blueprint, RemovedFunctionNames);

        const BlueprintHandlerUtils::FBlueprintOrphanDeltaCleanupResult OrphanCleanup =
            BlueprintHandlerUtils::CleanupNewBlueprintOrphans(Blueprint, OrphansBefore, bCleanupNewOrphans, true);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("cascadedCreateDelegatesRemoved"), CascadedCreateDelegatesRemoved);
        BlueprintHandlerUtils::AddOrphanDeltaCleanupResultToJson(OrphanCleanup, Result);
        AddAssetVerification(Result, Blueprint);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found."));
    }
    return true;
}

// ---- blueprint.graph.replace_node ----
REGISTER_RPC_HANDLER("blueprint.graph.replace_node", "blueprint.graph",
    "Substitute one node for another in place, transferring matched pin connections and defaults. Mirrors UE's internal Convert-Event-To-Function pattern: spawns a node of newNodeType using a unified 'target' string (bare member name, or qualified 'Class::Member' / 'Class.Member', or a class name for casts), per-pin MovePinLinks across (name,direction)-matched pins, copies unconnected pin defaults, then RemoveNode + orphan/delegate cascade cleanup. Splits matching struct sub-pins on the replacement so split-pin wires survive. Whole operation runs in one transaction for clean Ctrl+Z. Refuses graph terminators (FunctionEntry/Result/Tunnel), composites, math expressions, knots, async-task nodes, bound events (component/actor/generated/input), macro instances, CallParentFunction, dead/deprecated classes.",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID (GUID) or unique name of the node to replace"),
        RPC_PARAM_REQ("newNodeType", "string", "Replacement node type — same vocabulary as create_node"),
        RPC_PARAM_OPT("target", "string", "Type-dependent target: bare member name, qualified 'Class::Member' or 'Class.Member', or class name for casts. Optional for Branch/Sequence/Select/MakeArray."),
        RPC_PARAM_OPT("graphName", "string", "Graph name — when omitted with a GUID nodeId, all blueprint graphs are scanned for the unique match"),
        RPC_PARAM_OPT("pinRemap", "object", "Optional object mapping old pin names to replacement pin names before automatic matching"),
        RPC_PARAM_DEF("allowOrphanPlaceholders", "boolean", "When the new node lacks a matching pin, create an orphan placeholder (red wire) instead of erroring (default: false)", "false")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString NodeId, NewNodeType, TargetSpec, GraphNameIn;
    Payload->TryGetStringField(TEXT("nodeId"), NodeId);
    Payload->TryGetStringField(TEXT("newNodeType"), NewNodeType);
    Payload->TryGetStringField(TEXT("target"), TargetSpec);
    Payload->TryGetStringField(TEXT("graphName"), GraphNameIn);
    if (NodeId.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("nodeId required")); return true; }
    if (NewNodeType.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("newNodeType required")); return true; }

    const bool bAllowOrphanPlaceholders = Ctx.GetBool(TEXT("allowOrphanPlaceholders"), false);
    TMap<FName, FName> PinRemap;
    bool bRequestHasPinRemap = false;
    if (Payload->HasField(TEXT("pinRemap")))
    {
        if (!Payload->HasTypedField<EJson::Object>(TEXT("pinRemap")))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("pinRemap must be an object mapping old pin names to new pin names"));
            return true;
        }

        const TSharedPtr<FJsonObject>* PinRemapObject = nullptr;
        if (Payload->TryGetObjectField(TEXT("pinRemap"), PinRemapObject) && PinRemapObject && PinRemapObject->IsValid())
        {
            bRequestHasPinRemap = true;
            for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : (*PinRemapObject)->Values)
            {
                if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
                {
                    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("pinRemap values must be strings"));
                    return true;
                }

                const FString NewPinName = Pair.Value->AsString();
                if (!Pair.Key.IsEmpty() && !NewPinName.IsEmpty())
                {
                    PinRemap.Add(FName(*Pair.Key), FName(*NewPinName));
                }
            }
        }
    }

    const BlueprintHandlerUtils::FBlueprintGraphTargetParts TargetParts =
        BlueprintHandlerUtils::ParseGraphTargetSpec(TargetSpec);

    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;

    if (!GraphNameIn.IsEmpty())
    {
        // Back-compat path — explicit graph scope.
        if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;
    }
    else
    {
        // Implicit cross-graph lookup: load BP from assetPath, then scan all graphs
        // for a node whose GUID (or name) matches nodeId.
        UEdGraph* UnusedGraph = nullptr;
        if (!ResolveBlueprintAndGraph(Ctx, Blueprint, UnusedGraph, /*bGraphRequired=*/false)) return true;
    }

    UEdGraphNode* OldNode = nullptr;
    if (TargetGraph)
    {
        OldNode = FindNodeByIdOrName(TargetGraph, NodeId);
    }
    else
    {
        TArray<UEdGraph*> AllGraphs;
        Blueprint->GetAllGraphs(AllGraphs);
        TArray<TPair<UEdGraph*, UEdGraphNode*>> Matches;
        for (UEdGraph* G : AllGraphs)
        {
            if (!G) continue;
            for (UEdGraphNode* Node : G->Nodes)
            {
                if (!Node) continue;
                if (BlueprintGraphHelpers::NodeGuidMatchesId(Node->NodeGuid, NodeId) ||
                    Node->GetName().Equals(NodeId, ESearchCase::IgnoreCase))
                {
                    Matches.Add({G, Node});
                }
            }
        }
        if (Matches.Num() == 0)
        {
            Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found."));
            return true;
        }
        if (Matches.Num() > 1)
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("code"), TEXT("INVALID_ARGUMENT"));
            Err->SetStringField(TEXT("message"), TEXT("Ambiguous nodeId — matches in multiple graphs; pass graphName to disambiguate."));
            TArray<TSharedPtr<FJsonValue>> Ambig;
            for (const auto& M : Matches) Ambig.Add(MakeShared<FJsonValueString>(M.Key->GetName()));
            Err->SetArrayField(TEXT("ambiguousGraphs"), Ambig);
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), Err->GetStringField(TEXT("message")));
            return true;
        }
        TargetGraph = Matches[0].Key;
        OldNode = Matches[0].Value;
    }

    if (!OldNode) { Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found.")); return true; }

    // Refusal list — node kinds whose replacement is unsafe (graph terminators,
    // composite owners of BoundGraphs, async/latent state, context-bound events,
    // input-driven events, macro instances, deprecated/dead classes).
    if (OldNode->IsA<UK2Node_FunctionEntry>() ||
        OldNode->IsA<UK2Node_FunctionResult>() ||
        OldNode->IsA<UK2Node_Composite>() ||
        OldNode->IsA<UK2Node_MathExpression>() ||
        OldNode->IsA<UK2Node_Knot>() ||
        OldNode->IsA<UK2Node_CallParentFunction>() ||
        OldNode->IsA<UK2Node_ComponentBoundEvent>() ||
        OldNode->IsA<UK2Node_BaseAsyncTask>() ||
        OldNode->IsA<UK2Node_AsyncAction>())
    {
        Ctx.SendError(TEXT("REPLACE_REFUSED"),
            FString::Printf(TEXT("Refusing to replace %s — class is in refusal list (graph terminators, composites, async tasks, bound events, parent calls)."),
                *OldNode->GetClass()->GetName()));
        return true;
    }
    // Catch bare tunnels (macro entry/exit) but allow them only when wrapped by composites
    // (already refused above).
    if (OldNode->IsA<UK2Node_Tunnel>() && !OldNode->IsA<UK2Node_Composite>())
    {
        Ctx.SendError(TEXT("REPLACE_REFUSED"),
            TEXT("Cannot replace macro tunnel nodes."));
        return true;
    }
    // Extra refusals: bound-event variants, input events, macro instances,
    // deprecated delegate-set, dead-class placeholder.
    if (OldNode->IsA<UK2Node_ActorBoundEvent>() ||
#if MCP_HAS_K2NODE_GENERATEDBOUNDEVENT
        OldNode->IsA<UK2Node_GeneratedBoundEvent>() ||
#endif
        OldNode->IsA<UK2Node_InputActionEvent>() ||
        OldNode->IsA<UK2Node_InputKeyEvent>() ||
        OldNode->IsA<UK2Node_InputAxisEvent>() ||
        OldNode->IsA<UK2Node_InputAxisKeyEvent>() ||
        OldNode->IsA<UK2Node_InputVectorAxisEvent>() ||
        OldNode->IsA<UK2Node_InputTouchEvent>() ||
        OldNode->IsA<UK2Node_MacroInstance>() ||
        OldNode->IsA<UK2Node_DelegateSet>() ||
        OldNode->IsA<UK2Node_DeadClass>())
    {
        Ctx.SendError(TEXT("REPLACE_REFUSED"),
            FString::Printf(TEXT("Refusing to replace %s — class is in refusal list (bound/input events, macro instances, deprecated/dead classes)."),
                *OldNode->GetClass()->GetName()));
        return true;
    }

    if (!OldNode->CanUserDeleteNode())
    {
        Ctx.SendError(TEXT("REPLACE_REFUSED"),
            FString::Printf(TEXT("Refusing to replace %s — node cannot be deleted by users."), *OldNode->GetClass()->GetName()));
        return true;
    }

    auto MapNewNodeTypeToClass = [](const FString& InType) -> UClass*
    {
        if (InType == TEXT("VariableGet") || InType == TEXT("K2Node_VariableGet")) return UK2Node_VariableGet::StaticClass();
        if (InType == TEXT("VariableSet") || InType == TEXT("K2Node_VariableSet")) return UK2Node_VariableSet::StaticClass();
        if (InType == TEXT("CallFunction") || InType == TEXT("K2Node_CallFunction") || InType == TEXT("FunctionCall")) return UK2Node_CallFunction::StaticClass();
        if (InType == TEXT("Event") || InType == TEXT("K2Node_Event")) return UK2Node_Event::StaticClass();
        if (InType == TEXT("CustomEvent") || InType == TEXT("K2Node_CustomEvent")) return UK2Node_CustomEvent::StaticClass();
        if (InType == TEXT("DynamicCast") || InType == TEXT("K2Node_DynamicCast") || InType == TEXT("Cast") || InType.StartsWith(TEXT("CastTo"))) return UK2Node_DynamicCast::StaticClass();
        if (InType == TEXT("ClassDynamicCast") || InType == TEXT("K2Node_ClassDynamicCast")) return UK2Node_ClassDynamicCast::StaticClass();
        if (InType == TEXT("CreateDelegate") || InType == TEXT("K2Node_CreateDelegate")) return UK2Node_CreateDelegate::StaticClass();
        if (InType == TEXT("Branch") || InType == TEXT("IfThenElse") || InType == TEXT("K2Node_IfThenElse")) return UK2Node_IfThenElse::StaticClass();
        if (InType == TEXT("Sequence") || InType == TEXT("ExecutionSequence") || InType == TEXT("K2Node_ExecutionSequence")) return UK2Node_ExecutionSequence::StaticClass();
        if (InType == TEXT("Select") || InType == TEXT("K2Node_Select")) return UK2Node_Select::StaticClass();
        if (InType == TEXT("MakeArray") || InType == TEXT("K2Node_MakeArray")) return UK2Node_MakeArray::StaticClass();
        return nullptr;
    };

    auto IsGenericReplaceClassAllowed = [](const UClass* CandidateClass) -> bool
    {
        if (!CandidateClass) return false;
        static const TSet<FString> AllowedClassNames = {
            TEXT("K2Node_IfThenElse"),
            TEXT("K2Node_Knot"),
            TEXT("K2Node_Self"),
            TEXT("K2Node_Copy"),
            TEXT("K2Node_GetArrayItem"),
            TEXT("K2Node_AssignmentStatement"),
            TEXT("K2Node_PureAssignmentStatement"),
            TEXT("K2Node_FormatText"),
            TEXT("K2Node_TemporaryVariable"),
            TEXT("K2Node_EaseFunction"),
            TEXT("K2Node_EnumEquality"),
            TEXT("K2Node_EnumInequality"),
            TEXT("K2Node_MakeContainer"),
            TEXT("K2Node_MakeSet"),
            TEXT("K2Node_MakeMap")
        };
        return AllowedClassNames.Contains(CandidateClass->GetName());
    };

    auto IsExplicitReplaceClass = [&](const UClass* CandidateClass) -> bool
    {
        return CandidateClass &&
            (CandidateClass == UK2Node_VariableGet::StaticClass() ||
             CandidateClass == UK2Node_VariableSet::StaticClass() ||
             CandidateClass == UK2Node_CallFunction::StaticClass() ||
             CandidateClass == UK2Node_Event::StaticClass() ||
             CandidateClass == UK2Node_CustomEvent::StaticClass() ||
             CandidateClass == UK2Node_DynamicCast::StaticClass() ||
             CandidateClass == UK2Node_ClassDynamicCast::StaticClass() ||
             CandidateClass == UK2Node_CreateDelegate::StaticClass() ||
             CandidateClass == UK2Node_IfThenElse::StaticClass() ||
             CandidateClass == UK2Node_ExecutionSequence::StaticClass() ||
             CandidateClass == UK2Node_Select::StaticClass() ||
             CandidateClass == UK2Node_MakeArray::StaticClass());
    };

    FString CommonFunctionClassName;
    FString CommonFunctionName;
    UFunction* CommonFunction = nullptr;
    const bool bCommonFunctionShortcut = ResolveCommonFunctionNodeShortcut(
        NewNodeType,
        CommonFunctionClassName,
        CommonFunctionName,
        CommonFunction);
    UClass* ResolvedNewClass = bCommonFunctionShortcut ? UK2Node_CallFunction::StaticClass() : MapNewNodeTypeToClass(NewNodeType);
    FString FactoryPath = TEXT("explicit");
    bool bGenericFactoryPath = false;
    bool bTargetIgnored = false;

    if (!ResolvedNewClass)
    {
        ResolvedNewClass = FindNodeClassByName(NewNodeType);
        if (!ResolvedNewClass)
        {
            Ctx.SendError(TEXT("NODE_TYPE_NOT_FOUND"),
                FString::Printf(TEXT("Replacement node type '%s' is not supported by replace_node."), *NewNodeType));
            return true;
        }

        if (!ResolvedNewClass->IsChildOf(UK2Node::StaticClass()))
        {
            Ctx.SendError(TEXT("UnsupportedNodeClass"),
                FString::Printf(TEXT("Replacement class '%s' is not a UK2Node."), *ResolvedNewClass->GetName()));
            return true;
        }

        if (ResolvedNewClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) ||
            ResolvedNewClass->IsChildOf(UK2Node_DeadClass::StaticClass()))
        {
            Ctx.SendError(TEXT("REPLACE_REFUSED"),
                FString::Printf(TEXT("Replacement class '%s' is abstract, deprecated, dead, or superseded."), *ResolvedNewClass->GetName()));
            return true;
        }

        if (IsExplicitReplaceClass(ResolvedNewClass) && !IsGenericReplaceClassAllowed(ResolvedNewClass))
        {
            Ctx.SendError(TEXT("UnsupportedNodeClass"),
                FString::Printf(TEXT("Replacement class '%s' requires explicit replace_node target configuration; use the explicit branch token contract."), *ResolvedNewClass->GetName()));
            return true;
        }

        if (!IsGenericReplaceClassAllowed(ResolvedNewClass))
        {
            Ctx.SendError(TEXT("UnsupportedNodeClass"),
                FString::Printf(TEXT("Replacement class '%s' is not in replace_node generic allow-list."), *ResolvedNewClass->GetName()));
            return true;
        }

        FactoryPath = TEXT("generic");
        bGenericFactoryPath = true;
        bTargetIgnored = !TargetSpec.IsEmpty();
    }

    const FString ResolvedClassName = ResolvedNewClass ? ResolvedNewClass->GetName() : FString();

    auto GetOldVariableOwnerClass = [&]() -> UClass*
    {
        if (auto* OldVar = Cast<UK2Node_Variable>(OldNode))
        {
            return OldVar->VariableReference.GetMemberParentClass(OldVar->GetBlueprintClassFromNode());
        }
        return nullptr;
    };

    auto IsBlueprintSelfVariableOwner = [&](UClass* OwnerClass) -> bool
    {
        UClass* SelfClass = Blueprint->SkeletonGeneratedClass
            ? Blueprint->SkeletonGeneratedClass->GetAuthoritativeClass()
            : Blueprint->GeneratedClass.Get();
        UClass* AuthoritativeOwnerClass = OwnerClass ? OwnerClass->GetAuthoritativeClass() : nullptr;
        return SelfClass && AuthoritativeOwnerClass &&
            (SelfClass == AuthoritativeOwnerClass || SelfClass->IsChildOf(AuthoritativeOwnerClass));
    };

    // No-op short-circuit — same class + matching config means there's nothing to do.
    // Bail before opening the transaction so we don't show a redundant undo entry.
    if (ResolvedNewClass)
    {
        if (OldNode->GetClass() == ResolvedNewClass)
        {
            bool bNoop = false;
            if (auto* OldCF = Cast<UK2Node_CallFunction>(OldNode))
            {
                const FName CurMember = OldCF->FunctionReference.GetMemberName();
                const FString DesiredMemberName = bCommonFunctionShortcut
                    ? CommonFunctionName
                    : TargetParts.MemberName;
                bNoop = DesiredMemberName.IsEmpty()
                    ? TargetSpec.IsEmpty()
                    : CurMember == FName(*DesiredMemberName);
            }
            else if (auto* OldVar = Cast<UK2Node_Variable>(OldNode))
            {
                const FName CurMember = OldVar->VariableReference.GetMemberName();
                bNoop = TargetSpec.IsEmpty();
                if (!bNoop && !TargetParts.MemberName.IsEmpty() &&
                    CurMember == FName(*TargetParts.MemberName))
                {
                    if (TargetParts.ClassName.IsEmpty())
                    {
                        // Bare same-name requests preserve the existing context.
                        bNoop = true;
                    }
                    else
                    {
                        UClass* RequestedOwnerClass = ResolveUClass(TargetParts.ClassName);
                        if (RequestedOwnerClass)
                        {
                            const bool bOldSelfContext = OldVar->VariableReference.IsSelfContext();
                            if (IsBlueprintSelfVariableOwner(RequestedOwnerClass))
                            {
                                bNoop = bOldSelfContext;
                            }
                            else if (!bOldSelfContext)
                            {
                                UClass* OldOwnerClass = GetOldVariableOwnerClass();
                                UClass* OldAuthoritativeOwnerClass = OldOwnerClass
                                    ? OldOwnerClass->GetAuthoritativeClass()
                                    : nullptr;
                                UClass* RequestedAuthoritativeOwnerClass = RequestedOwnerClass->GetAuthoritativeClass();
                                bNoop = OldAuthoritativeOwnerClass && RequestedAuthoritativeOwnerClass &&
                                    OldAuthoritativeOwnerClass == RequestedAuthoritativeOwnerClass;
                            }
                        }
                    }
                }
            }
            else if (auto* OldEv = Cast<UK2Node_Event>(OldNode))
            {
                const FName CurEv = OldEv->EventReference.GetMemberName();
                bNoop = TargetSpec.IsEmpty() ||
                    (TargetParts.MemberName.IsEmpty() ? false : CurEv == FName(*TargetParts.MemberName));
            }
            else if (auto* OldCE = Cast<UK2Node_CustomEvent>(OldNode))
            {
                bNoop = TargetSpec.IsEmpty() ||
                    OldCE->CustomFunctionName == FName(*TargetParts.MemberName);
            }
            else if (auto* OldDC = Cast<UK2Node_DynamicCast>(OldNode))
            {
                UClass* Resolved = TargetSpec.IsEmpty() ? nullptr : ResolveUClass(TargetSpec);
                bNoop = TargetSpec.IsEmpty() || OldDC->TargetType == Resolved;
            }
            else
            {
                // No-config types (Branch, Sequence, Select, MakeArray, etc.) — same class is a no-op.
                bNoop = true;
            }
            if (bNoop)
            {
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("noop"), true);
                Result->SetStringField(TEXT("nodeId"), OldNode->NodeGuid.ToString());
                Result->SetStringField(TEXT("factoryPath"), FactoryPath);
                Result->SetStringField(TEXT("resolvedClass"), ResolvedClassName);
                if (bTargetIgnored)
                {
                    Result->SetBoolField(TEXT("targetIgnored"), true);
                }
                Ctx.SendSuccess(Result);
                return true;
            }
        }
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Replace Blueprint Node")));
    bool bSuccess = false;
    ON_SCOPE_EXIT { if (!bSuccess) Transaction.Cancel(); };

    Blueprint->Modify();
    TargetGraph->Modify();
    OldNode->Modify();
    // Modify every neighbor whose LinkedTo will change so the transaction captures
    // the pre-replacement wire state on those nodes too.
    for (UEdGraphPin* OldPin : OldNode->Pins)
    {
        if (!OldPin) continue;
        for (UEdGraphPin* Linked : OldPin->LinkedTo)
        {
            if (Linked && Linked->GetOwningNode()) Linked->GetOwningNode()->Modify();
        }
    }

    const TSet<FGuid> OrphansBefore =
        BlueprintHandlerUtils::SnapshotBlueprintOrphanGuids(Blueprint, true);

    // Capture function name BEFORE removal so the delegate cascade can match it.
    TSet<FName> RemovedFunctionNames;
    const FName EffectiveOldName = BlueprintHandlerUtils::GetEffectiveFunctionNameForRemoval(OldNode);
    if (EffectiveOldName != NAME_None) RemovedFunctionNames.Add(EffectiveOldName);

    const bool bOldWasEventLike =
        OldNode->IsA<UK2Node_Event>() || OldNode->IsA<UK2Node_CustomEvent>();
    const int32 OldX = OldNode->NodePosX;
    const int32 OldY = OldNode->NodePosY;
    const FString OldComment = OldNode->NodeComment;
    const bool bOldCommentPinned = OldNode->bCommentBubblePinned;
    const bool bOldCommentVisible = OldNode->bCommentBubbleVisible;
    const FString OldNodeName = OldNode->GetName();

    // Refresh the old pin set so the per-pin transfer below sees the current shape.
    TargetGraph->GetSchema()->ReconstructNode(*OldNode);

    // Spawn replacement, configured per class. Mirrors the create_node factory pattern
    // — keep these branches in lockstep with that handler when adding new node types.
    UEdGraphNode* NewNode = nullptr;
    auto FailReplace = [&](const TCHAR* Code, const FString& Message)
    {
        Ctx.SendError(Code, Message);
    };

    auto ShouldUseSelfVariableContext = [&](UClass* OwnerClass) -> bool
    {
        if (TargetParts.ClassName.IsEmpty())
        {
            if (const auto* OldVar = Cast<UK2Node_Variable>(OldNode))
            {
                return OldVar->VariableReference.IsSelfContext();
            }
            return OwnerClass == nullptr;
        }

        return IsBlueprintSelfVariableOwner(OwnerClass);
    };

    if (bCommonFunctionShortcut)
    {
        if (!CommonFunction)
        {
            FailReplace(TEXT("FUNCTION_NOT_FOUND"),
                FString::Printf(TEXT("Could not find function '%s::%s' for node type '%s'"), *CommonFunctionClassName, *CommonFunctionName, *NewNodeType));
            return true;
        }

        FGraphNodeCreator<UK2Node_CallFunction> Creator(*TargetGraph);
        UK2Node_CallFunction* CallNode = Creator.CreateNode(false);
        CallNode->SetFromFunction(CommonFunction);
        Creator.Finalize();
        NewNode = CallNode;
    }
    else if (NewNodeType == TEXT("VariableGet") || NewNodeType == TEXT("K2Node_VariableGet"))
    {
        UClass* OwnerClass = nullptr;
        FName VarFName;
        FString ErrCode, ErrMsg;
        if (!BlueprintHandlerUtils::ResolveGraphVariableTarget(Blueprint, TargetParts, GetOldVariableOwnerClass(), OwnerClass, VarFName, ErrCode, ErrMsg)) { FailReplace(*ErrCode, ErrMsg); return true; }
        if (!BlueprintHandlerUtils::DoesGraphVariableExist(Blueprint, OwnerClass, VarFName))
        {
            FailReplace(TEXT("VARIABLE_NOT_FOUND"), FString::Printf(TEXT("Variable '%s' not found"), *VarFName.ToString()));
            return true;
        }
        FGraphNodeCreator<UK2Node_VariableGet> Creator(*TargetGraph);
        UK2Node_VariableGet* VarGet = Creator.CreateNode(false);
        if (ShouldUseSelfVariableContext(OwnerClass)) VarGet->VariableReference.SetSelfMember(VarFName);
        else VarGet->VariableReference.SetExternalMember(VarFName, OwnerClass);
        Creator.Finalize();
        TargetGraph->GetSchema()->ReconstructNode(*VarGet);
        NewNode = VarGet;
    }
    else if (NewNodeType == TEXT("VariableSet") || NewNodeType == TEXT("K2Node_VariableSet"))
    {
        UClass* OwnerClass = nullptr;
        FName VarFName;
        FString ErrCode, ErrMsg;
        if (!BlueprintHandlerUtils::ResolveGraphVariableTarget(Blueprint, TargetParts, GetOldVariableOwnerClass(), OwnerClass, VarFName, ErrCode, ErrMsg)) { FailReplace(*ErrCode, ErrMsg); return true; }
        if (!BlueprintHandlerUtils::DoesGraphVariableExist(Blueprint, OwnerClass, VarFName))
        {
            FailReplace(TEXT("VARIABLE_NOT_FOUND"), FString::Printf(TEXT("Variable '%s' not found"), *VarFName.ToString()));
            return true;
        }
        FGraphNodeCreator<UK2Node_VariableSet> Creator(*TargetGraph);
        UK2Node_VariableSet* VarSet = Creator.CreateNode(false);
        if (ShouldUseSelfVariableContext(OwnerClass)) VarSet->VariableReference.SetSelfMember(VarFName);
        else VarSet->VariableReference.SetExternalMember(VarFName, OwnerClass);
        Creator.Finalize();
        TargetGraph->GetSchema()->ReconstructNode(*VarSet);
        NewNode = VarSet;
    }
    else if (NewNodeType == TEXT("CallFunction") || NewNodeType == TEXT("K2Node_CallFunction") || NewNodeType == TEXT("FunctionCall"))
    {
        UClass* PreferredClass = nullptr;
        if (auto* OldCF = Cast<UK2Node_CallFunction>(OldNode))
        {
            PreferredClass = OldCF->FunctionReference.GetMemberParentClass(OldCF->GetBlueprintClassFromNode());
        }
        FString ErrCode, ErrMsg;
        UFunction* Func = BlueprintHandlerUtils::ResolveGraphCallableFunction(
            Blueprint,
            TargetParts,
            PreferredClass,
            ErrCode,
            ErrMsg);
        if (!Func)
        {
            FailReplace(ErrCode.IsEmpty() ? TEXT("FUNCTION_NOT_FOUND") : *ErrCode,
                ErrMsg.IsEmpty()
                    ? FString::Printf(TEXT("Function '%s' not found"), *TargetParts.MemberName)
                    : ErrMsg);
            return true;
        }
        if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Func->HasMetaData(TEXT("DeprecatedFunction")))
        {
            FailReplace(TEXT("FUNCTION_NOT_FOUND"),
                FString::Printf(TEXT("Function '%s' is not BlueprintCallable or is deprecated"), *TargetParts.MemberName));
            return true;
        }
        FGraphNodeCreator<UK2Node_CallFunction> Creator(*TargetGraph);
        UK2Node_CallFunction* CallNode = Creator.CreateNode(false);
        CallNode->SetFromFunction(Func);
        Creator.Finalize();
        NewNode = CallNode;
    }
    else if (NewNodeType == TEXT("Event") || NewNodeType == TEXT("K2Node_Event"))
    {
        const FString& EventName = TargetParts.MemberName;
        const FString& MemberClass = TargetParts.ClassName;
        if (EventName.IsEmpty()) { FailReplace(TEXT("INVALID_ARGUMENT"), TEXT("target required (event name, optionally qualified as 'Class::Event')")); return true; }
        UClass* TargetClass = nullptr;
        UFunction* EventFunc = nullptr;
        if (!MemberClass.IsEmpty())
        {
            TargetClass = ResolveUClass(MemberClass);
            if (TargetClass) EventFunc = TargetClass->FindFunctionByName(*EventName);
        }
        else
        {
            for (UClass* C = Blueprint->ParentClass; C && !EventFunc; C = C->GetSuperClass())
            {
                EventFunc = C->FindFunctionByName(*EventName, EIncludeSuperFlag::ExcludeSuper);
                if (EventFunc) TargetClass = C;
            }
        }
        if (!EventFunc || !TargetClass)
        {
            FailReplace(TEXT("EVENT_NOT_FOUND"), FString::Printf(TEXT("Event '%s' not found"), *EventName));
            return true;
        }
        FGraphNodeCreator<UK2Node_Event> Creator(*TargetGraph);
        UK2Node_Event* EventNode = Creator.CreateNode(false);
        EventNode->EventReference.SetFromField<UFunction>(EventFunc, false);
        EventNode->bOverrideFunction = true;
        Creator.Finalize();
        NewNode = EventNode;
    }
    else if (NewNodeType == TEXT("CustomEvent") || NewNodeType == TEXT("K2Node_CustomEvent"))
    {
        // CustomEvent always lives on the BP — TargetParts.ClassName is ignored;
        // accept either qualified or bare forms.
        const FString& EventName = TargetParts.MemberName;
        if (EventName.IsEmpty()) { FailReplace(TEXT("INVALID_ARGUMENT"), TEXT("target required (custom event name)")); return true; }
        // Reject if a parent-class function with the same name already exists — would
        // collide with an override slot rather than create a new custom event.
        if (Blueprint->ParentClass && Blueprint->ParentClass->FindFunctionByName(*EventName))
        {
            FailReplace(TEXT("EVENT_NOT_FOUND"),
                FString::Printf(TEXT("Custom event name '%s' collides with a parent-class function"), *EventName));
            return true;
        }
        FGraphNodeCreator<UK2Node_CustomEvent> Creator(*TargetGraph);
        UK2Node_CustomEvent* EventNode = Creator.CreateNode(false);
        EventNode->CustomFunctionName = FName(*EventName);
        // Carry forward UserDefinedPins if the old node also exposed them — preserves
        // custom event parameter signatures across the swap. Deep-copy each
        // FUserPinInfo so the new node owns independent entries (the array stores
        // TSharedPtr; a shallow copy would share state with the soon-to-be-deleted
        // old node).
        if (UK2Node_EditablePinBase* OldEditable = Cast<UK2Node_EditablePinBase>(OldNode))
        {
            EventNode->UserDefinedPins.Reset();
            for (const TSharedPtr<FUserPinInfo>& Src : OldEditable->UserDefinedPins)
            {
                if (Src.IsValid()) EventNode->UserDefinedPins.Add(MakeShared<FUserPinInfo>(*Src));
            }
        }
        Creator.Finalize();
        NewNode = EventNode;
    }
    else if (NewNodeType == TEXT("Cast") ||
             NewNodeType == TEXT("DynamicCast") ||
             NewNodeType == TEXT("K2Node_DynamicCast") ||
             NewNodeType == TEXT("ClassDynamicCast") ||
             NewNodeType == TEXT("K2Node_ClassDynamicCast") ||
             NewNodeType.StartsWith(TEXT("CastTo")))
    {
        // For casts the unified target string is the class name; no '::' split applies.
        FString TargetClassName = TargetSpec;
        if (TargetClassName.IsEmpty() && NewNodeType.StartsWith(TEXT("CastTo")))
            TargetClassName = NewNodeType.Mid(6);
        if (TargetClassName.IsEmpty()) { FailReplace(TEXT("INVALID_ARGUMENT"), TEXT("target required (class name)")); return true; }
        UClass* TargetClass = ResolveUClass(TargetClassName);
        if (!TargetClass)
        {
            FailReplace(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("Class '%s' not found"), *TargetClassName));
            return true;
        }
        if (TargetClass->HasAnyClassFlags(CLASS_NewerVersionExists))
        {
            FailReplace(TEXT("CLASS_NOT_FOUND"),
                FString::Printf(TEXT("Class '%s' has a newer version — refuse to bind a stale class to a cast"), *TargetClassName));
            return true;
        }
        const bool bClassCast =
            NewNodeType == TEXT("ClassDynamicCast") || NewNodeType == TEXT("K2Node_ClassDynamicCast");
        if (bClassCast)
        {
            FGraphNodeCreator<UK2Node_ClassDynamicCast> Creator(*TargetGraph);
            UK2Node_ClassDynamicCast* CastNode = Creator.CreateNode(false);
            CastNode->TargetType = TargetClass;
            Creator.Finalize();
            NewNode = CastNode;
        }
        else
        {
            FGraphNodeCreator<UK2Node_DynamicCast> Creator(*TargetGraph);
            UK2Node_DynamicCast* CastNode = Creator.CreateNode(false);
            CastNode->TargetType = TargetClass;
            Creator.Finalize();
            NewNode = CastNode;
        }
    }
    else if (NewNodeType == TEXT("CreateDelegate") || NewNodeType == TEXT("K2Node_CreateDelegate"))
    {
        const FString& MemberName = TargetParts.MemberName;
        const FString& MemberClass = TargetParts.ClassName;
        if (MemberName.IsEmpty()) { FailReplace(TEXT("INVALID_ARGUMENT"), TEXT("target required (function name, optionally qualified as 'Class::Function')")); return true; }
        UClass* ScopeClass = MemberClass.IsEmpty() ? Blueprint->SkeletonGeneratedClass.Get() : ResolveUClass(MemberClass);
        if (!ScopeClass) { FailReplace(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("Class '%s' not found"), *MemberClass)); return true; }
        UFunction* Func = ScopeClass->FindFunctionByName(*MemberName);
        if (!Func) { FailReplace(TEXT("FUNCTION_NOT_FOUND"), FString::Printf(TEXT("Function '%s' not found on '%s'"), *MemberName, *ScopeClass->GetName())); return true; }
        FGraphNodeCreator<UK2Node_CreateDelegate> Creator(*TargetGraph);
        UK2Node_CreateDelegate* DelegateNode = Creator.CreateNode(false);
        const FName MemberFName(*MemberName);
        FGuid SelectedFunctionGuid;
        DelegateNode->SelectedFunctionName = MemberFName;
        FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(ScopeClass, MemberFName, SelectedFunctionGuid);
        DelegateNode->SelectedFunctionGuid = SelectedFunctionGuid;
        Creator.Finalize();
        DelegateNode->HandleAnyChange(false);
        NewNode = DelegateNode;
    }
    else if (NewNodeType == TEXT("Branch") || NewNodeType == TEXT("IfThenElse") || NewNodeType == TEXT("K2Node_IfThenElse"))
    {
        FGraphNodeCreator<UK2Node_IfThenElse> Creator(*TargetGraph);
        UK2Node_IfThenElse* Node = Creator.CreateNode(false);
        Creator.Finalize();
        NewNode = Node;
    }
    else if (NewNodeType == TEXT("Sequence") || NewNodeType == TEXT("ExecutionSequence") || NewNodeType == TEXT("K2Node_ExecutionSequence"))
    {
        FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*TargetGraph);
        UK2Node_ExecutionSequence* Node = Creator.CreateNode(false);
        Creator.Finalize();
        NewNode = Node;
    }
    else if (NewNodeType == TEXT("Select") || NewNodeType == TEXT("K2Node_Select"))
    {
        FGraphNodeCreator<UK2Node_Select> Creator(*TargetGraph);
        UK2Node_Select* Node = Creator.CreateNode(false);
        Creator.Finalize();
        NewNode = Node;
    }
    else if (NewNodeType == TEXT("MakeArray") || NewNodeType == TEXT("K2Node_MakeArray"))
    {
        FGraphNodeCreator<UK2Node_MakeArray> Creator(*TargetGraph);
        UK2Node_MakeArray* Node = Creator.CreateNode(false);
        Creator.Finalize();
        NewNode = Node;
    }
    else if (bGenericFactoryPath)
    {
        if (bTargetIgnored)
        {
            UE_LOG(LogTemp, Verbose, TEXT("blueprint.graph.replace_node: ignoring target '%s' for generic replacement class '%s'"),
                *TargetSpec, *ResolvedClassName);
        }

        FGraphNodeCreator<UEdGraphNode> Creator(*TargetGraph);
        NewNode = Creator.CreateNode(false, ResolvedNewClass);
        Creator.Finalize();
    }

    if (!NewNode)
    {
        Ctx.SendError(TEXT("REPLACE_FAILED"), TEXT("Failed to construct replacement node."));
        return true;
    }

    if (bGenericFactoryPath && !NewNode->CanUserDeleteNode())
    {
        FBlueprintEditorUtils::RemoveNode(Blueprint, NewNode, true);
        Ctx.SendError(TEXT("REPLACE_REFUSED"),
            FString::Printf(TEXT("Refusing to replace with %s — spawned node cannot be deleted by users."), *ResolvedClassName));
        return true;
    }

    // Preserve position is always-on in v1.1 (caller can move via set_node_property after).
    NewNode->NodePosX = OldX;
    NewNode->NodePosY = OldY;
    NewNode->NodeComment = OldComment;
    NewNode->bCommentBubblePinned = bOldCommentPinned;
    NewNode->bCommentBubbleVisible = bOldCommentVisible;
    // Carry enabled/disabled / breakpoint-friendly state and advanced-pin display.
    NewNode->SetEnabledState(OldNode->GetDesiredEnabledState(), OldNode->HasUserSetTheEnabledState());
    NewNode->AdvancedPinDisplay = OldNode->AdvancedPinDisplay;

    // Split struct sub-pins on the new node to mirror the old node's split state.
    // Old node was already reconstructed; iterate its parent pins and call SplitPin
    // on the matching new pin so the per-pin loop below sees the flattened sub-pin set.
    int32 SubPinsSplit = 0;
    TArray<FName> SplitParentPinNames;
    if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(TargetGraph->GetSchema()))
    {
        for (UEdGraphPin* OldPin : OldNode->Pins)
        {
            if (!OldPin || OldPin->ParentPin != nullptr) continue;
            if (OldPin->SubPins.Num() == 0) continue;
            UEdGraphPin* NewParent = NewNode->FindPin(OldPin->PinName, OldPin->Direction);
            if (!NewParent || NewParent->SubPins.Num() > 0) continue;
            K2Schema->SplitPin(NewParent, /*bNotify=*/false);
            if (NewParent->SubPins.Num() > 0)
            {
                SplitParentPinNames.AddUnique(OldPin->PinName);
                ++SubPinsSplit;
            }
        }
    }

    struct FSplitSubPinTransferRecord
    {
        FName PinName;
        TArray<UEdGraphPin*> OriginalLinks;
    };

    int32 ConnectionsRewired = 0;
    int32 DefaultsTransferred = 0;
    int32 PinRemapApplied = 0;
    TArray<TSharedPtr<FJsonValue>> DroppedConnections;
    TArray<TSharedPtr<FJsonValue>> OrphanPlaceholders;
    TArray<TSharedPtr<FJsonValue>> PinRemapUnmatched;
    TArray<TSharedPtr<FJsonObject>> UnmatchedWiredPinDetails;
    TArray<FString> UnmatchedWiredPinNames;
    TArray<FSplitSubPinTransferRecord> SplitSubPinTransfers;

    const bool bBothCallFunction = OldNode->IsA<UK2Node_CallFunction>() && NewNode->IsA<UK2Node_CallFunction>();

    const UEdGraphSchema* Schema = TargetGraph->GetSchema();

    auto MakePinIssue = [](const UEdGraphPin* Pin, const TCHAR* Reason, const int32 LinkedCountOverride = INDEX_NONE) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        if (Pin)
        {
            Entry->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
            Entry->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input"));
            Entry->SetNumberField(TEXT("linkedCount"), LinkedCountOverride == INDEX_NONE ? Pin->LinkedTo.Num() : LinkedCountOverride);
        }
        Entry->SetStringField(TEXT("reason"), Reason);
        return Entry;
    };

    auto RecordDropped = [&](const UEdGraphPin* Pin, const TCHAR* Reason)
    {
        if (!Pin) return;
        TSharedPtr<FJsonObject> Entry = MakePinIssue(Pin, Reason);
        DroppedConnections.Add(MakeShared<FJsonValueObject>(Entry));
    };

    auto RecordPinRemapUnmatched = [&](const UEdGraphPin* Pin, const FName& RequestedPinName, const TCHAR* Reason)
    {
        TSharedPtr<FJsonObject> Entry = MakePinIssue(Pin, Reason);
        Entry->SetStringField(TEXT("requestedPinName"), RequestedPinName.ToString());
        PinRemapUnmatched.Add(MakeShared<FJsonValueObject>(Entry));
    };

    auto GatherMissingTransferredLinks = [&](UEdGraphPin* NewPin, const TArray<UEdGraphPin*>& OriginalLinks)
    {
        TArray<UEdGraphPin*> MissingLinks;
        for (UEdGraphPin* OriginalLink : OriginalLinks)
        {
            if (!OriginalLink)
            {
                continue;
            }

            const bool bLinkedDirectly = NewPin && NewPin->LinkedTo.Contains(OriginalLink);
            const bool bConnectedViaConversionNode =
                NewPin && Schema && NewPin->LinkedTo.Num() > 0 &&
                Schema->CanCreateConnection(NewPin, OriginalLink).Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE;
            if (!bLinkedDirectly && !bConnectedViaConversionNode)
            {
                MissingLinks.Add(OriginalLink);
            }
        }

        return MissingLinks;
    };

    auto ResolveMissingOrphanedLinks = [&](UEdGraphPin* NewPin, TArray<UEdGraphPin*>& MissingLinks)
    {
        if (!NewPin || !Schema || MissingLinks.Num() == 0)
        {
            return;
        }

        TArray<UEdGraphPin*> ResolvedOrphanLinks;
        for (UEdGraphPin* MissingLink : MissingLinks)
        {
            if (!MissingLink)
            {
                continue;
            }

            if (Schema->TryCreateConnection(NewPin, MissingLink) || Schema->TryCreateConnection(MissingLink, NewPin))
            {
                ResolvedOrphanLinks.AddUnique(MissingLink);
                continue;
            }

            if (!MissingLink->bOrphanedPin)
            {
                continue;
            }

            UEdGraphNode* LinkedNode = MissingLink->GetOwningNodeUnchecked();
            if (!LinkedNode)
            {
                continue;
            }

            UEdGraphPin* ReplacementLink = LinkedNode->FindPinByPredicate(
                [MissingLink](const UEdGraphPin* CandidatePin)
                {
                    return CandidatePin &&
                        !CandidatePin->bOrphanedPin &&
                        CandidatePin->PinName == MissingLink->PinName &&
                        CandidatePin->Direction == MissingLink->Direction;
                });
            if (ReplacementLink && Schema->TryCreateConnection(ReplacementLink, NewPin))
            {
                ResolvedOrphanLinks.AddUnique(MissingLink);
            }
        }

        for (UEdGraphPin* ResolvedOrphanLink : ResolvedOrphanLinks)
        {
            if (!ResolvedOrphanLink)
            {
                continue;
            }

            if (ResolvedOrphanLink->LinkedTo.Num() == 0)
            {
                if (UEdGraphNode* OwningNode = ResolvedOrphanLink->GetOwningNodeUnchecked())
                {
                    OwningNode->RemovePin(ResolvedOrphanLink);
                }
            }
            MissingLinks.Remove(ResolvedOrphanLink);
        }
    };

    auto CreateOrphanPlaceholder = [&](UEdGraphPin* OldPin, const TArray<UEdGraphPin*>& LinksToPreserve, const TCHAR* Reason) -> bool
    {
        if (!OldPin || LinksToPreserve.Num() == 0)
        {
            return false;
        }

        UEdGraphPin* Placeholder = NewNode->CreatePin(OldPin->Direction, OldPin->PinType, OldPin->PinName);
        if (!Placeholder)
        {
            return false;
        }

        Placeholder->bOrphanedPin = true;
        Placeholder->bNotConnectable = true;
        Placeholder->SetSavePinIfOrphaned(true);

        int32 PreservedLinkCount = 0;
        for (UEdGraphPin* LinkToPreserve : LinksToPreserve)
        {
            if (!LinkToPreserve || Placeholder->LinkedTo.Contains(LinkToPreserve))
            {
                continue;
            }

            Placeholder->MakeLinkTo(LinkToPreserve);
            if (Placeholder->LinkedTo.Contains(LinkToPreserve))
            {
                ++PreservedLinkCount;
            }
        }

        if (PreservedLinkCount == 0)
        {
            NewNode->RemovePin(Placeholder);
            return false;
        }

        TSharedPtr<FJsonObject> Entry = MakePinIssue(OldPin, Reason, PreservedLinkCount);
        OrphanPlaceholders.Add(MakeShared<FJsonValueObject>(Entry));
        return true;
    };

    auto ArePinTypesEquivalent = [](const UEdGraphPin* OldPin, const UEdGraphPin* NewPin) -> bool
    {
        if (!OldPin || !NewPin) return false;
        return OldPin->PinType.PinCategory == NewPin->PinType.PinCategory &&
            OldPin->PinType.PinSubCategory == NewPin->PinType.PinSubCategory &&
            OldPin->PinType.PinSubCategoryObject == NewPin->PinType.PinSubCategoryObject &&
            OldPin->PinType.ContainerType == NewPin->PinType.ContainerType;
    };

    auto CanMovePinLinksTo = [&](UEdGraphPin* OldPin, UEdGraphPin* NewPin) -> bool
    {
        if (!OldPin || !NewPin) return false;
        if (OldPin->Direction != NewPin->Direction) return false;
        if (OldPin->LinkedTo.Num() == 0)
        {
            return ArePinTypesEquivalent(OldPin, NewPin);
        }
        if (!Schema) return true;

        for (UEdGraphPin* LinkedPin : OldPin->LinkedTo)
        {
            if (!LinkedPin) continue;
            const FPinConnectionResponse Response = Schema->CanCreateConnection(NewPin, LinkedPin);
            if (Response.Response == CONNECT_RESPONSE_DISALLOW)
            {
                return false;
            }
        }
        return true;
    };

    TMap<UEdGraphPin*, UEdGraphPin*> PinMatches;
    TSet<UEdGraphPin*> ClaimedNewPins;

    for (UEdGraphPin* OldPin : OldNode->Pins)
    {
        if (!OldPin) continue;
        if (OldPin->PinName == UEdGraphSchema_K2::PN_Self && !bBothCallFunction) continue;

        const FName* RemappedPinName = PinRemap.Find(OldPin->PinName);
        if (RemappedPinName)
        {
            UEdGraphPin* RemappedPin = FindPinByName(NewNode, RemappedPinName->ToString());
            if (!RemappedPin)
            {
                RecordPinRemapUnmatched(OldPin, *RemappedPinName, TEXT("MISSING_NEW_PIN"));
                continue;
            }
            if (ClaimedNewPins.Contains(RemappedPin) || !CanMovePinLinksTo(OldPin, RemappedPin))
            {
                RecordPinRemapUnmatched(OldPin, *RemappedPinName, TEXT("TYPE_INCOMPATIBLE"));
                continue;
            }

            PinMatches.Add(OldPin, RemappedPin);
            ClaimedNewPins.Add(RemappedPin);
            ++PinRemapApplied;
            continue;
        }

        UEdGraphPin* ExactPin = NewNode->FindPin(OldPin->PinName, OldPin->Direction);
        if (ExactPin && !ClaimedNewPins.Contains(ExactPin) && CanMovePinLinksTo(OldPin, ExactPin))
        {
            PinMatches.Add(OldPin, ExactPin);
            ClaimedNewPins.Add(ExactPin);
            continue;
        }

    }

    for (UEdGraphPin* OldPin : OldNode->Pins)
    {
        if (!OldPin) continue;
        if (OldPin->PinName == UEdGraphSchema_K2::PN_Self && !bBothCallFunction) continue;
        if (PinMatches.Contains(OldPin)) continue;
        if (OldPin->LinkedTo.Num() > 0 && !bAllowOrphanPlaceholders)
        {
            UnmatchedWiredPinNames.Add(OldPin->PinName.ToString());
            UnmatchedWiredPinDetails.Add(MakePinIssue(OldPin, TEXT("NO_MATCH")));
        }
    }

    if (UnmatchedWiredPinDetails.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> UnmatchedPinsJson;
        for (const TSharedPtr<FJsonObject>& PinDetail : UnmatchedWiredPinDetails)
        {
            if (PinDetail.IsValid())
            {
                UnmatchedPinsJson.Add(MakeShared<FJsonValueObject>(PinDetail));
            }
        }

        TSharedPtr<FJsonObject> ErrorPayload = MakeShared<FJsonObject>();
        ErrorPayload->SetStringField(TEXT("message"),
            FString::Printf(TEXT("Replacement would leave wired pins unmatched: %s"),
                *FString::Join(UnmatchedWiredPinNames, TEXT(", "))));
        ErrorPayload->SetArrayField(TEXT("unmatchedPins"), UnmatchedPinsJson);

        FBlueprintEditorUtils::RemoveNode(Blueprint, NewNode, true);
        Ctx.SendError(TEXT("PIN_REMAP_INVALID"),
            BlueprintHandlerUtils::JsonValueToString(MakeShared<FJsonValueObject>(ErrorPayload)));
        return true;
    }

    for (UEdGraphPin* OldPin : OldNode->Pins)
    {
        if (!OldPin) continue;
        if (OldPin->PinName == UEdGraphSchema_K2::PN_Self && !bBothCallFunction) continue;

        if (OldPin->bOrphanedPin)
        {
            if (bAllowOrphanPlaceholders && OldPin->LinkedTo.Num() > 0)
            {
                if (!CreateOrphanPlaceholder(OldPin, OldPin->LinkedTo, TEXT("PRE_EXISTING_ORPHAN")))
                {
                    RecordDropped(OldPin, TEXT("PRE_EXISTING_ORPHAN"));
                }
            }
            continue;
        }

        UEdGraphPin** MatchedPinPtr = PinMatches.Find(OldPin);
        UEdGraphPin* NewPin = MatchedPinPtr ? *MatchedPinPtr : nullptr;
        if (!NewPin)
        {
            if (bAllowOrphanPlaceholders && OldPin->LinkedTo.Num() > 0)
            {
                if (!CreateOrphanPlaceholder(OldPin, OldPin->LinkedTo, TEXT("NO_MATCH")))
                {
                    RecordDropped(OldPin, TEXT("NO_MATCH"));
                }
            }
            continue;
        }

        if (!OldPin->bNotConnectable && OldPin->LinkedTo.Num() > 0 && Schema)
        {
            const int32 LinkedCount = OldPin->LinkedTo.Num();
            const TArray<UEdGraphPin*> OriginalLinks = OldPin->LinkedTo;
            const FPinConnectionResponse Resp = Schema->MovePinLinks(*OldPin, *NewPin, false, true);

            if (OldPin->ParentPin != nullptr && NewPin->ParentPin != nullptr && OriginalLinks.Num() > 0)
            {
                FSplitSubPinTransferRecord& TransferRecord = SplitSubPinTransfers.AddDefaulted_GetRef();
                TransferRecord.PinName = NewPin->PinName;
                TransferRecord.OriginalLinks = OriginalLinks;
            }

            TArray<UEdGraphPin*> MissingLinks = GatherMissingTransferredLinks(NewPin, OriginalLinks);
            ResolveMissingOrphanedLinks(NewPin, MissingLinks);

            const bool bMovedSafely = MissingLinks.Num() == 0;
            const bool bRecoveredAsPlaceholder =
                MissingLinks.Num() > 0 && CreateOrphanPlaceholder(OldPin, MissingLinks, TEXT("INCOMPATIBLE"));

            if (bMovedSafely || bRecoveredAsPlaceholder)
            {
                ConnectionsRewired += LinkedCount;
                if (UK2Node* K2NewNode = Cast<UK2Node>(NewNode))
                {
                    K2NewNode->NotifyPinConnectionListChanged(NewPin);
                }
            }
            else
            {
                RecordDropped(OldPin, TEXT("INCOMPATIBLE"));
            }
        }

        if (NewPin->LinkedTo.Num() == 0 && !OldPin->bNotConnectable)
        {
            const bool bDefaultDiffers =
                NewPin->DefaultValue != OldPin->DefaultValue ||
                NewPin->DefaultObject != OldPin->DefaultObject ||
                !NewPin->DefaultTextValue.EqualTo(OldPin->DefaultTextValue);
            if (bDefaultDiffers)
            {
                NewPin->DefaultValue = OldPin->DefaultValue;
                NewPin->DefaultObject = OldPin->DefaultObject;
                NewPin->DefaultTextValue = OldPin->DefaultTextValue;
                NewNode->PinDefaultValueChanged(NewPin);
                ++DefaultsTransferred;
            }
        }
    }

    // Remove old node — bDontRecompile=true to avoid an O(N) per-replace compile.
    FBlueprintEditorUtils::RemoveNode(Blueprint, OldNode, true);

    // Cascade-clean CreateDelegate nodes that referenced the old event/function.
    const int32 CascadedCreateDelegatesRemoved =
        BlueprintHandlerUtils::CascadeRemoveStaleCreateDelegates(Blueprint, RemovedFunctionNames);

    // Scrub stale UFunctions when the old node was an event source — same reason
    // delete_node does it on the equivalent path.
    if (bOldWasEventLike && RemovedFunctionNames.Num() > 0)
    {
        BlueprintHandlerUtils::ScrubStaleUFunctionsFromClass(Blueprint, RemovedFunctionNames);
    }

    // Sweep newly orphaned neighbors (always-on in v1.1 — caller no longer toggles).
    TSet<FGuid> OrphansBeforeWithReplacement = OrphansBefore;
    if (NewNode->NodeGuid.IsValid())
    {
        OrphansBeforeWithReplacement.Add(NewNode->NodeGuid);
    }
    const BlueprintHandlerUtils::FBlueprintOrphanDeltaCleanupResult OrphanCleanup =
        BlueprintHandlerUtils::CleanupNewBlueprintOrphans(Blueprint, OrphansBeforeWithReplacement, true, true);

    // Refresh any CreateDelegate node (new or existing) whose SelectedFunctionGuid may
    // now be stale relative to the post-replacement class.
    if (NewNode->IsA<UK2Node_CreateDelegate>())
    {
        BlueprintHandlerUtils::RefreshBpirDelegateNodes(Blueprint, TArray<FGuid>{ NewNode->NodeGuid });
    }

    // Node-class swap is structural — dependent BPs and the skeleton class must
    // rebuild. MarkBlueprintAsModified alone leaves stale skeletons behind.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (SplitParentPinNames.Num() > 0)
    {
        if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(TargetGraph->GetSchema()))
        {
            for (const FName& SplitParentPinName : SplitParentPinNames)
            {
                UEdGraphPin* NewParent = FindPinByName(NewNode, SplitParentPinName.ToString());
                if (!NewParent || NewParent->SubPins.Num() > 0)
                {
                    continue;
                }

                K2Schema->SplitPin(NewParent, /*bNotify=*/false);
            }
        }
    }

    if (Schema && SplitSubPinTransfers.Num() > 0)
    {
        for (const FSplitSubPinTransferRecord& TransferRecord : SplitSubPinTransfers)
        {
            UEdGraphPin* LiveSubPin = FindPinByName(NewNode, TransferRecord.PinName.ToString());
            if (!LiveSubPin)
            {
                continue;
            }

            TArray<UEdGraphPin*> MissingLinks = GatherMissingTransferredLinks(LiveSubPin, TransferRecord.OriginalLinks);
            if (MissingLinks.Num() > 0)
            {
                UEdGraphPin* LiveParentPin = LiveSubPin->ParentPin;
                if (LiveParentPin)
                {
                    for (int32 MissingLinkIndex = MissingLinks.Num() - 1; MissingLinkIndex >= 0; --MissingLinkIndex)
                    {
                        UEdGraphPin* MissingLink = MissingLinks[MissingLinkIndex];
                        if (!MissingLink || !LiveParentPin->LinkedTo.Contains(MissingLink))
                        {
                            continue;
                        }

                        LiveParentPin->BreakLinkTo(MissingLink);
                        const bool bMovedToLiveSubPin =
                            Schema->TryCreateConnection(LiveSubPin, MissingLink) ||
                            Schema->TryCreateConnection(MissingLink, LiveSubPin);
                        if (bMovedToLiveSubPin)
                        {
                            MissingLinks.RemoveAt(MissingLinkIndex);
                            continue;
                        }

                        LiveParentPin->MakeLinkTo(MissingLink);
                    }
                }

                ResolveMissingOrphanedLinks(LiveSubPin, MissingLinks);
            }

            if (UK2Node* K2NewNode = Cast<UK2Node>(NewNode))
            {
                K2NewNode->NotifyPinConnectionListChanged(LiveSubPin);
            }
        }
    }

    const FString NewNodeId = NewNode->NodeGuid.ToString();
    const FString NewNodeName = NewNode->GetName();
    const FString NewNodeClassName = NewNode->GetClass()->GetName();

    UE_LOG(LogTemp, Verbose, TEXT("blueprint.graph.replace_node: %s -> %s (rewired=%d dropped=%d defaults=%d subPinsSplit=%d)"),
        *NodeId, *NewNode->NodeGuid.ToString(), ConnectionsRewired, DroppedConnections.Num(), DefaultsTransferred, SubPinsSplit);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("oldNodeId"), NodeId);
    Result->SetStringField(TEXT("oldNodeName"), OldNodeName);
    Result->SetStringField(TEXT("newNodeId"), NewNodeId);
    Result->SetStringField(TEXT("newNodeName"), NewNodeName);
    Result->SetStringField(TEXT("newNodeType"), NewNodeClassName);
    Result->SetStringField(TEXT("factoryPath"), FactoryPath);
    Result->SetStringField(TEXT("resolvedClass"), ResolvedClassName);
    Result->SetNumberField(TEXT("connectionsRewired"), ConnectionsRewired);
    Result->SetArrayField(TEXT("connectionsDropped"), DroppedConnections);
    Result->SetNumberField(TEXT("defaultsTransferred"), DefaultsTransferred);
    Result->SetNumberField(TEXT("subPinsSplit"), SubPinsSplit);
    Result->SetArrayField(TEXT("orphanPlaceholdersCreated"), OrphanPlaceholders);
    if (bRequestHasPinRemap && PinRemap.Num() > 0)
    {
        Result->SetNumberField(TEXT("pinRemapApplied"), PinRemapApplied);
    }
    if (PinRemapUnmatched.Num() > 0)
    {
        Result->SetArrayField(TEXT("pinRemapUnmatched"), PinRemapUnmatched);
    }
    if (bTargetIgnored)
    {
        Result->SetBoolField(TEXT("targetIgnored"), true);
    }
    Result->SetNumberField(TEXT("cascadedCreateDelegatesRemoved"), CascadedCreateDelegatesRemoved);
    BlueprintHandlerUtils::AddOrphanDeltaCleanupResultToJson(OrphanCleanup, Result);
    AddAssetVerification(Result, Blueprint);

    bSuccess = true;
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.create_reroute_node ----
REGISTER_RPC_HANDLER("blueprint.graph.create_reroute_node", "blueprint.graph",
    "Create a reroute (knot) node",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Graph name"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    float X = static_cast<float>(XD), Y = static_cast<float>(YD);

    const FScopedTransaction Transaction(FText::FromString(TEXT("Create Reroute Node")));
    Blueprint->Modify();
    TargetGraph->Modify();

    FGraphNodeCreator<UK2Node_Knot> NodeCreator(*TargetGraph);
    UK2Node_Knot* RerouteNode = NodeCreator.CreateNode(false);
    RerouteNode->NodePosX = X;
    RerouteNode->NodePosY = Y;
    NodeCreator.Finalize();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), RerouteNode->NodeGuid.ToString());
    Result->SetStringField(TEXT("nodeName"), RerouteNode->GetName());
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.set_node_property ----
REGISTER_RPC_HANDLER("blueprint.graph.set_node_property", "blueprint.graph",
    "Set one supported presentation property on a Blueprint graph node. This is a fixed whitelist, not a reflected property writer.",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID or name"),
        RPC_PARAM_REQ("propertyName", "string", "Supported names: Comment/NodeComment, X/NodePosX, Y/NodePosY, bCommentBubbleVisible, bCommentBubblePinned"),
        RPC_PARAM_REQ("value", "string", "Value to set"),
        RPC_PARAM_OPT("graphName", "string", "Graph name")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString NodeId, PropertyName, Value;
    Payload->TryGetStringField(TEXT("nodeId"), NodeId);
    Payload->TryGetStringField(TEXT("propertyName"), PropertyName);
    Payload->TryGetStringField(TEXT("value"), Value);

    const FScopedTransaction Transaction(FText::FromString(TEXT("Set Blueprint Node Property")));
    Blueprint->Modify();
    TargetGraph->Modify();

    UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
    if (!TargetNode) { Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found.")); return true; }

    TargetNode->Modify();
    bool bHandled = false;

    if (PropertyName.Equals(TEXT("Comment"), ESearchCase::IgnoreCase) || PropertyName.Equals(TEXT("NodeComment"), ESearchCase::IgnoreCase))
    {
        TargetNode->NodeComment = Value;
        bHandled = true;
    }
    else if (PropertyName.Equals(TEXT("X"), ESearchCase::IgnoreCase) || PropertyName.Equals(TEXT("NodePosX"), ESearchCase::IgnoreCase))
    {
        double NumValue = 0.0;
        if (!Payload->TryGetNumberField(TEXT("value"), NumValue)) NumValue = FCString::Atod(*Value);
        TargetNode->NodePosX = static_cast<float>(NumValue);
        bHandled = true;
    }
    else if (PropertyName.Equals(TEXT("Y"), ESearchCase::IgnoreCase) || PropertyName.Equals(TEXT("NodePosY"), ESearchCase::IgnoreCase))
    {
        double NumValue = 0.0;
        if (!Payload->TryGetNumberField(TEXT("value"), NumValue)) NumValue = FCString::Atod(*Value);
        TargetNode->NodePosY = static_cast<float>(NumValue);
        bHandled = true;
    }
    else if (PropertyName.Equals(TEXT("bCommentBubbleVisible"), ESearchCase::IgnoreCase))
    {
        TargetNode->bCommentBubbleVisible = Value.ToBool();
        bHandled = true;
    }
    else if (PropertyName.Equals(TEXT("bCommentBubblePinned"), ESearchCase::IgnoreCase))
    {
        TargetNode->bCommentBubblePinned = Value.ToBool();
        bHandled = true;
    }

    if (bHandled)
    {
        TargetGraph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("nodeId"), TargetNode->NodeGuid.ToString());
        Result->SetStringField(TEXT("nodeName"), TargetNode->GetName());
        AddAssetVerification(Result, Blueprint);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_SUPPORTED"),
            FString::Printf(TEXT("Unsupported node property '%s'"), *PropertyName));
    }
    return true;
}

// ---- blueprint.graph.reconstruct_node ----
REGISTER_RPC_HANDLER("blueprint.graph.reconstruct_node", "blueprint.graph",
    "Reconstruct one K2 node in memory after a shape-changing reflected property write, refresh the graph, and mark the Blueprint modified.",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID or name"),
        RPC_PARAM_OPT("graphName", "string", "Graph name")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const FString NodeId = Ctx.GetString(TEXT("nodeId"));
    UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
    if (!TargetNode)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found."));
        return true;
    }

    UK2Node* K2Node = Cast<UK2Node>(TargetNode);
    if (!K2Node)
    {
        Ctx.SendError(TEXT("INVALID_NODE_TYPE"), FString::Printf(
            TEXT("Node '%s' is not a UK2Node and cannot be reconstructed"), *TargetNode->GetName()));
        return true;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("Reconstruct Blueprint Node")));
    Blueprint->Modify();
    TargetGraph->Modify();
    K2Node->Modify();

    const FGuid OriginalGuid = K2Node->NodeGuid;
    K2Node->ReconstructNode();
    TargetGraph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), K2Node->NodeGuid.ToString());
    Result->SetStringField(TEXT("nodeName"), K2Node->GetName());
    Result->SetStringField(TEXT("nodeClass"), K2Node->GetClass()->GetName());
    Result->SetBoolField(TEXT("guidPreserved"), K2Node->NodeGuid == OriginalGuid);
    Result->SetNumberField(TEXT("pinCount"), K2Node->Pins.Num());
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// Helper: apply a single pin default value and return a result JSON object.
// Used by both the singular and batch set_pin_default_value(s) handlers.
// ---------------------------------------------------------------------------
static bool ApplyPersistedTextPinDefaultValue(
    UEdGraphNode* TargetNode,
    UEdGraphPin* Pin,
    const FString& Value,
    FString& OutError)
{
    if (!TargetNode || !Pin)
    {
        OutError = TEXT("Invalid text pin");
        return false;
    }

    FText NewText;
    if (!CoerceStringToPersistedFText(Value, &Pin->DefaultTextValue, NewText, OutError))
    {
        return false;
    }

    Pin->DefaultTextValue = MoveTemp(NewText);
    Pin->DefaultValue.Empty();
    Pin->DefaultObject = nullptr;
    TargetNode->PinDefaultValueChanged(Pin);
    return true;
}

static TSharedPtr<FJsonObject> ApplyPinDefaultValueCore(
    UEdGraph* TargetGraph,
    const FString& NodeId,
    const FString& PinName,
    const FString& Value)
{
    TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("nodeId"), NodeId);
    Item->SetStringField(TEXT("pinName"), PinName);

    UEdGraphNode* TargetNode = FindNodeByIdOrName(TargetGraph, NodeId);
    if (!TargetNode)
    {
        Item->SetBoolField(TEXT("success"), false);
        Item->SetStringField(TEXT("error"), TEXT("NODE_NOT_FOUND"));
        Item->SetStringField(TEXT("message"), TEXT("Node not found."));
        return Item;
    }

    Item->SetStringField(TEXT("nodeName"), TargetNode->GetName());

    UEdGraphPin* Pin = FindPinByName(TargetNode, PinName);
    if (!Pin)
    {
        Item->SetBoolField(TEXT("success"), false);
        Item->SetStringField(TEXT("error"), TEXT("PIN_NOT_FOUND"));
        Item->SetStringField(TEXT("message"), TEXT("Pin not found."));
        return Item;
    }
    if (Pin->Direction != EGPD_Input)
    {
        Item->SetBoolField(TEXT("success"), false);
        Item->SetStringField(TEXT("error"), TEXT("INVALID_PIN_DIRECTION"));
        Item->SetStringField(TEXT("message"), TEXT("Can only set default values on input pins."));
        return Item;
    }

    const UEdGraphSchema* Schema = TargetGraph->GetSchema();
    if (!Schema)
    {
        Item->SetBoolField(TEXT("success"), false);
        Item->SetStringField(TEXT("error"), TEXT("SCHEMA_NOT_FOUND"));
        Item->SetStringField(TEXT("message"), TEXT("Could not resolve graph schema."));
        return Item;
    }

    TargetNode->Modify();

    const FString PreviousDefaultValue = Pin->DefaultValue;
    const FString PreviousDefaultTextValue = Pin->DefaultTextValue.ToString();
    FString AppliedValue = Value;

    if (Pin->PinType.PinSubCategoryObject.IsValid() &&
        Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()) != nullptr)
    {
        FString ErrorCode;
        FString ErrorMessage;
        if (!BlueprintEnumHelpers::TryApplyEnumPinDefaultValue(Schema, Pin, Value, AppliedValue, ErrorCode, ErrorMessage))
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetStringField(TEXT("error"),
                ErrorCode.IsEmpty() ? TEXT("VERIFICATION_FAILED") : ErrorCode);
            Item->SetStringField(TEXT("message"),
                ErrorMessage.IsEmpty() ? TEXT("Failed to apply enum default value.") : ErrorMessage);
            return Item;
        }
    }
    else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
    {
        FString TextError;
        if (!ApplyPersistedTextPinDefaultValue(TargetNode, Pin, Value, TextError))
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetStringField(TEXT("error"), TEXT("INVALID_TEXT_LOCALIZATION_IDENTITY"));
            Item->SetStringField(TEXT("message"),
                FString::Printf(TEXT("Pin '%s' requires an FText namespace and key: %s"),
                    *PinName, *TextError));
            return Item;
        }
    }
    else
    {
        // Object-reference pins (object/class/softobject/softclass/interface) store
        // their resolved value in Pin->DefaultObject, leaving the string DefaultValue
        // empty. Detect those so verification reads the correct slot instead of always
        // reporting VERIFICATION_FAILED for a write that actually succeeded.
        const FName PinCategory = Pin->PinType.PinCategory;
        const bool bIsObjectPin =
            PinCategory == UEdGraphSchema_K2::PC_Object ||
            PinCategory == UEdGraphSchema_K2::PC_Class ||
            PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
            PinCategory == UEdGraphSchema_K2::PC_SoftClass ||
            PinCategory == UEdGraphSchema_K2::PC_Interface;

        const UObject* PreviousDefaultObject = Pin->DefaultObject;

        Schema->TrySetDefaultValue(*Pin, Value);

        // Object-reference pins resolve their value into Pin->DefaultObject and report it
        // via the object's path; all other pins report Pin->DefaultValue. Compute the
        // match/no-change checks and the value to echo on failure for whichever slot
        // applies, then share one verification-failure emission.
        bool bMatchesRequested;
        bool bNoStateChange;
        FString ActualValue;
        if (bIsObjectPin)
        {
            // Mirror the inspection chain (BlueprintGraphHelpers / inspection handler):
            // the object pin's value is Pin->DefaultObject->GetPathName(). TrySetDefaultValue
            // also accepts a path string in DefaultValue for soft references, so accept a
            // match in either slot.
            ActualValue = Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString();
            bMatchesRequested =
                (!ActualValue.IsEmpty() && ActualValue.Equals(Value, ESearchCase::CaseSensitive)) ||
                Pin->DefaultValue.Equals(Value, ESearchCase::CaseSensitive);
            bNoStateChange =
                Pin->DefaultObject == PreviousDefaultObject &&
                Pin->DefaultValue.Equals(PreviousDefaultValue, ESearchCase::CaseSensitive);
        }
        else
        {
            ActualValue = Pin->DefaultValue;
            bMatchesRequested = Pin->DefaultValue.Equals(Value, ESearchCase::CaseSensitive) ||
                Pin->DefaultTextValue.ToString().Equals(Value, ESearchCase::CaseSensitive);
            bNoStateChange = Pin->DefaultValue.Equals(PreviousDefaultValue, ESearchCase::CaseSensitive) &&
                Pin->DefaultTextValue.ToString().Equals(PreviousDefaultTextValue, ESearchCase::CaseSensitive);
        }

        if (!bMatchesRequested && bNoStateChange)
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetStringField(TEXT("error"), TEXT("VERIFICATION_FAILED"));
            Item->SetStringField(TEXT("message"),
                FString::Printf(
                    TEXT("Requested pin default '%s' but pin %sremained '%s'"),
                    *Value,
                    bIsObjectPin ? TEXT("object ") : TEXT(""),
                    *ActualValue));
            return Item;
        }
        if (bIsObjectPin && !ActualValue.IsEmpty())
        {
            Item->SetStringField(TEXT("defaultObjectPath"), ActualValue);
        }
    }

    Item->SetBoolField(TEXT("success"), true);
    Item->SetStringField(TEXT("requestedValue"), Value);
    Item->SetStringField(TEXT("value"), AppliedValue);
    Item->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
    if (!Pin->DefaultTextValue.IsEmptyOrWhitespace())
    {
        Item->SetStringField(TEXT("defaultTextValue"), Pin->DefaultTextValue.ToString());
    }
    return Item;
}

// ---- blueprint.graph.set_pin_default_value ----
REGISTER_RPC_HANDLER("blueprint.graph.set_pin_default_value", "blueprint.graph",
    "Set the default value on a node's input pin",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID or name"),
        RPC_PARAM_REQ("pinName", "string", "Pin name"),
        RPC_PARAM_REQ("value", "string", "Default value to set"),
        RPC_PARAM_OPT("graphName", "string", "Graph name")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString NodeId, PinName, Value;
    Payload->TryGetStringField(TEXT("nodeId"), NodeId);
    Payload->TryGetStringField(TEXT("pinName"), PinName);
    Payload->TryGetStringField(TEXT("value"), Value);

    if (NodeId.IsEmpty() || PinName.IsEmpty() || Value.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("nodeId, pinName, and value are required."));
        return true;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("Set Pin Default Value")));
    Blueprint->Modify();
    TargetGraph->Modify();

    TSharedPtr<FJsonObject> ItemResult = ApplyPinDefaultValueCore(TargetGraph, NodeId, PinName, Value);

    bool bSuccess = false;
    ItemResult->TryGetBoolField(TEXT("success"), bSuccess);
    if (!bSuccess)
    {
        FString ErrorCode, ErrorMessage;
        ItemResult->TryGetStringField(TEXT("error"), ErrorCode);
        ItemResult->TryGetStringField(TEXT("message"), ErrorMessage);
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    // Strip internal fields and reuse the result object directly
    ItemResult->RemoveField(TEXT("success"));
    ItemResult->RemoveField(TEXT("error"));
    ItemResult->RemoveField(TEXT("message"));
    AddAssetVerification(ItemResult, Blueprint);
    Ctx.SendSuccess(ItemResult);
    return true;
}

// ---- blueprint.graph.set_pin_default_values (batch) ----
REGISTER_RPC_HANDLER("blueprint.graph.set_pin_default_values", "blueprint.graph",
    "Batch set default values on multiple node pins",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("updates", "array", "Array of {nodeId, pinName, value} objects"),
        RPC_PARAM_OPT("graphName", "string", "Graph name")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, TargetGraph)) return true;
    if (!TargetGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not resolve target graph."));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Updates = Ctx.GetArray(TEXT("updates"));
    if (!Updates || Updates->Num() == 0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("'updates' array is required and must not be empty."));
        return true;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("Batch Set Pin Default Values")));
    Blueprint->Modify();
    TargetGraph->Modify();

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 SuccessCount = 0;
    int32 FailureCount = 0;

    for (const TSharedPtr<FJsonValue>& UpdateValue : *Updates)
    {
        if (!UpdateValue.IsValid() || UpdateValue->Type != EJson::Object || !UpdateValue->AsObject().IsValid())
        {
            TSharedPtr<FJsonObject> ErrItem = MakeShared<FJsonObject>();
            ErrItem->SetStringField(TEXT("nodeId"), TEXT(""));
            ErrItem->SetStringField(TEXT("pinName"), TEXT(""));
            ErrItem->SetBoolField(TEXT("success"), false);
            ErrItem->SetStringField(TEXT("error"), TEXT("INVALID_ARGUMENT"));
            ErrItem->SetStringField(TEXT("message"), TEXT("Each update must be a JSON object with nodeId, pinName, value."));
            Results.Add(MakeShared<FJsonValueObject>(ErrItem));
            ++FailureCount;
            continue;
        }

        const TSharedPtr<FJsonObject>& UpdateObj = UpdateValue->AsObject();
        FString NodeId, PinName, Value;
        UpdateObj->TryGetStringField(TEXT("nodeId"), NodeId);
        UpdateObj->TryGetStringField(TEXT("pinName"), PinName);
        UpdateObj->TryGetStringField(TEXT("value"), Value);

        if (NodeId.IsEmpty() || PinName.IsEmpty() || Value.IsEmpty())
        {
            TSharedPtr<FJsonObject> ErrItem = MakeShared<FJsonObject>();
            ErrItem->SetStringField(TEXT("nodeId"), NodeId);
            ErrItem->SetStringField(TEXT("pinName"), PinName);
            ErrItem->SetBoolField(TEXT("success"), false);
            ErrItem->SetStringField(TEXT("error"), TEXT("INVALID_ARGUMENT"));
            ErrItem->SetStringField(TEXT("message"), TEXT("nodeId, pinName, and value are required."));
            Results.Add(MakeShared<FJsonValueObject>(ErrItem));
            ++FailureCount;
            continue;
        }

        TSharedPtr<FJsonObject> ItemResult = ApplyPinDefaultValueCore(TargetGraph, NodeId, PinName, Value);

        bool bItemSuccess = false;
        ItemResult->TryGetBoolField(TEXT("success"), bItemSuccess);
        if (bItemSuccess)
        {
            ++SuccessCount;
        }
        else
        {
            ++FailureCount;
        }
        Results.Add(MakeShared<FJsonValueObject>(ItemResult));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"), Results);
    Result->SetNumberField(TEXT("totalUpdates"), Updates->Num());
    Result->SetNumberField(TEXT("successCount"), SuccessCount);
    Result->SetNumberField(TEXT("failureCount"), FailureCount);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
