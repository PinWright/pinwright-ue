// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/PropertyUtils.h"

#if __has_include("StateTree.h") && __has_include("StateTreeEditorData.h") && __has_include("StateTreeState.h")
#include "GameplayTagsManager.h"
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "ScopedTransaction.h"
#include "Compat/InstancedStructCompat.h"
#define MCP_STATE_TREE_AUTHORING_AVAILABLE 1
#else
#define MCP_STATE_TREE_AUTHORING_AVAILABLE 0
#endif

#if MCP_STATE_TREE_AUTHORING_AVAILABLE
// The StateTree binding-path type was renamed FStateTreePropertyPath -> FPropertyBindingPath in UE 5.6.
// FStateTreePropertyPath still exists (deprecated) on 5.6+ as a subclass of FPropertyBindingPath, but the
// 5.4/5.5 builds only have FStateTreePropertyPath. Alias to the canonical type for the running engine so the
// binding logic (SetStructID/FromString/ToString and UStateTreeEditorData::AddPropertyBinding) compiles on all.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
using FMcpStateTreeBindingPath = FStateTreePropertyPath;
#else
using FMcpStateTreeBindingPath = FPropertyBindingPath;
#endif
#endif

namespace
{
void SendStateTreeHeadersUnavailable(FHandlerContext& Ctx)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("headersUnavailable"), true);
    Result->SetStringField(TEXT("message"), TEXT("StateTree authoring headers unavailable"));
    Ctx.SendSuccess(Result);
}

#if MCP_STATE_TREE_AUTHORING_AVAILABLE
bool LoadStateTreeEditorData(FHandlerContext& Ctx, UStateTree*& OutStateTree, UStateTreeEditorData*& OutEditorData)
{
    const FString StateTreePath = Ctx.GetString(TEXT("stateTreePath"));
    if (StateTreePath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("stateTreePath is required"));
        return false;
    }

    OutStateTree = LoadObject<UStateTree>(nullptr, *StateTreePath);
    if (!OutStateTree)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("StateTree not found: %s"), *StateTreePath));
        return false;
    }

    OutEditorData = Cast<UStateTreeEditorData>(OutStateTree->EditorData);
    if (!OutEditorData)
    {
        Ctx.SendError(TEXT("INVALID_STATE"), TEXT("StateTree has no EditorData"));
        return false;
    }

    return true;
}

UStateTreeState* FindStateRecursive(UStateTreeState* State, const FString& StateName)
{
    if (!State)
    {
        return nullptr;
    }

    if (State->Name.ToString().Equals(StateName, ESearchCase::IgnoreCase))
    {
        return State;
    }

    for (UStateTreeState* Child : State->Children)
    {
        if (UStateTreeState* Found = FindStateRecursive(Child, StateName))
        {
            return Found;
        }
    }

    return nullptr;
}

UStateTreeState* FindState(UStateTreeEditorData& EditorData, const FString& StateName)
{
    for (UStateTreeState* SubTree : EditorData.SubTrees)
    {
        if (UStateTreeState* Found = FindStateRecursive(SubTree, StateName))
        {
            return Found;
        }
    }

    return nullptr;
}

bool ResolveNodeStruct(FHandlerContext& Ctx, const FString& StructName, const UScriptStruct* BaseStruct, const TCHAR* Kind, UScriptStruct*& OutStruct)
{
    if (StructName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), FString::Printf(TEXT("%s struct is required"), Kind));
        return false;
    }

    OutStruct = ResolveUScriptStruct(StructName);
    if (!OutStruct)
    {
        Ctx.SendError(TEXT("STRUCT_NOT_FOUND"), FString::Printf(TEXT("Could not resolve StateTree %s struct: %s"), Kind, *StructName));
        return false;
    }

    if (!OutStruct->IsChildOf(BaseStruct))
    {
        Ctx.SendError(TEXT("INVALID_STRUCT"), FString::Printf(TEXT("Struct '%s' is not a child of '%s'"), *OutStruct->GetName(), *BaseStruct->GetName()));
        return false;
    }

    return true;
}

void ApplyPropertiesToEditorNode(FStateTreeEditorNode& EditorNode, const TSharedPtr<FJsonObject>& Properties, TArray<FString>& OutWarnings)
{
    if (!Properties.IsValid())
    {
        return;
    }

    UScriptStruct* NodeStruct = const_cast<UScriptStruct*>(EditorNode.Node.GetScriptStruct());
    UScriptStruct* InstanceStruct = const_cast<UScriptStruct*>(EditorNode.Instance.GetScriptStruct());

    for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Properties->Values)
    {
        FString ApplyError;
        if (NodeStruct)
        {
            if (FProperty* Property = FindPropertyCI(NodeStruct, Pair.Key))
            {
                if (!ApplyJsonValueToProperty(EditorNode.Node.GetMutableMemory(), Property, Pair.Value, ApplyError))
                {
                    OutWarnings.Add(FString::Printf(TEXT("Failed to set node property '%s': %s"), *Pair.Key, *ApplyError));
                }
                continue;
            }
        }

        if (InstanceStruct)
        {
            if (FProperty* Property = FindPropertyCI(InstanceStruct, Pair.Key))
            {
                if (!ApplyJsonValueToProperty(EditorNode.Instance.GetMutableMemory(), Property, Pair.Value, ApplyError))
                {
                    OutWarnings.Add(FString::Printf(TEXT("Failed to set instance property '%s': %s"), *Pair.Key, *ApplyError));
                }
                continue;
            }
        }

        OutWarnings.Add(FString::Printf(TEXT("Unknown StateTree node property '%s'"), *Pair.Key));
    }
}

FStateTreeEditorNode& AddEditorNode(TArray<FStateTreeEditorNode>& Nodes, UScriptStruct& NodeStruct, const FString& NodeName, const TSharedPtr<FJsonObject>& Properties, TArray<FString>& OutWarnings)
{
    FStateTreeEditorNode& EditorNode = Nodes.AddDefaulted_GetRef();
    EditorNode.ID = FGuid::NewGuid();
    EditorNode.Node.InitializeAs(&NodeStruct);

    FStateTreeNodeBase* NodeBase = EditorNode.Node.GetMutablePtr<FStateTreeNodeBase>();
    if (NodeBase)
    {
        if (!NodeName.IsEmpty())
        {
            NodeBase->Name = FName(*NodeName);
        }

        if (const UScriptStruct* InstanceType = Cast<const UScriptStruct>(NodeBase->GetInstanceDataType()))
        {
            EditorNode.Instance.InitializeAs(InstanceType);
        }
    }

    ApplyPropertiesToEditorNode(EditorNode, Properties, OutWarnings);
    return EditorNode;
}

void AddWarnings(TSharedPtr<FJsonObject>& Result, const TArray<FString>& Warnings)
{
    if (Warnings.IsEmpty())
    {
        return;
    }

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }
    Result->SetArrayField(TEXT("warnings"), WarningValues);
}

TSharedPtr<FJsonObject> MakeNodeResult(const FStateTreeEditorNode& EditorNode, const FString& StructName)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), EditorNode.ID.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetStringField(TEXT("struct"), StructName);
    Result->SetStringField(TEXT("name"), EditorNode.GetName().ToString());
    return Result;
}

bool ParseGuid(const FString& Text, FGuid& OutGuid)
{
    return FGuid::Parse(Text, OutGuid) || FGuid::ParseExact(Text, EGuidFormats::DigitsWithHyphens, OutGuid);
}

void VisitStateNodes(UStateTreeState& State, TFunctionRef<bool(FStateTreeEditorNode&)> Visitor)
{
    if (State.SingleTask.ID.IsValid() && !Visitor(State.SingleTask))
    {
        return;
    }

    for (FStateTreeEditorNode& Node : State.EnterConditions)
    {
        if (!Visitor(Node))
        {
            return;
        }
    }

    for (FStateTreeEditorNode& Node : State.Tasks)
    {
        if (!Visitor(Node))
        {
            return;
        }
    }

    for (FStateTreeTransition& Transition : State.Transitions)
    {
        for (FStateTreeEditorNode& Node : Transition.Conditions)
        {
            if (!Visitor(Node))
            {
                return;
            }
        }
    }

    for (UStateTreeState* Child : State.Children)
    {
        if (Child)
        {
            VisitStateNodes(*Child, Visitor);
        }
    }
}

FStateTreeEditorNode* FindEditorNode(UStateTreeEditorData& EditorData, const FString& NodeToken)
{
    FGuid NodeId;
    const bool bHasGuid = ParseGuid(NodeToken, NodeId);
    FStateTreeEditorNode* FoundNode = nullptr;

    auto MatchNode = [&](FStateTreeEditorNode& Node) -> bool
    {
        if ((bHasGuid && Node.ID == NodeId) || (!bHasGuid && Node.GetName().ToString().Equals(NodeToken, ESearchCase::IgnoreCase)))
        {
            FoundNode = &Node;
            return false;
        }
        return true;
    };

    for (FStateTreeEditorNode& Node : EditorData.Evaluators)
    {
        if (!MatchNode(Node))
        {
            return FoundNode;
        }
    }

    for (FStateTreeEditorNode& Node : EditorData.GlobalTasks)
    {
        if (!MatchNode(Node))
        {
            return FoundNode;
        }
    }

    for (UStateTreeState* SubTree : EditorData.SubTrees)
    {
        if (SubTree)
        {
            VisitStateNodes(*SubTree, MatchNode);
            if (FoundNode)
            {
                return FoundNode;
            }
        }
    }

    return nullptr;
}

bool ParseBindingPath(UStateTreeEditorData& EditorData, const FString& RawPath, const FString& ExplicitStructId, FMcpStateTreeBindingPath& OutPath, FString& OutError)
{
    FString PropertyPath = RawPath;
    FGuid StructId;

    if (!ExplicitStructId.IsEmpty())
    {
        if (!ParseGuid(ExplicitStructId, StructId))
        {
            OutError = FString::Printf(TEXT("Invalid struct id '%s'"), *ExplicitStructId);
            return false;
        }
    }
    else
    {
        FString NodeToken;
        if (RawPath.Split(TEXT(":"), &NodeToken, &PropertyPath))
        {
            if (!ParseGuid(NodeToken, StructId))
            {
                OutError = FString::Printf(TEXT("Invalid node id '%s'"), *NodeToken);
                return false;
            }
        }
        else if (RawPath.Split(TEXT("."), &NodeToken, &PropertyPath))
        {
            if (FStateTreeEditorNode* Node = FindEditorNode(EditorData, NodeToken))
            {
                StructId = Node->ID;
            }
            else if (!ParseGuid(NodeToken, StructId))
            {
                OutError = FString::Printf(TEXT("Could not resolve StateTree node '%s' in binding path '%s'"), *NodeToken, *RawPath);
                return false;
            }
        }
        else
        {
            OutError = FString::Printf(TEXT("Binding path '%s' must include sourceId/targetId or use nodeId:property or nodeName.property"), *RawPath);
            return false;
        }
    }

    OutPath.SetStructID(StructId);
    if (!OutPath.FromString(PropertyPath))
    {
        OutError = FString::Printf(TEXT("Invalid property path '%s'"), *PropertyPath);
        return false;
    }

    return true;
}

bool ResolveTrigger(FHandlerContext& Ctx, const FString& TriggerString, EStateTreeTransitionTrigger& OutTrigger)
{
    if (TriggerString.Equals(TEXT("OnStateCompleted"), ESearchCase::IgnoreCase))
    {
        OutTrigger = EStateTreeTransitionTrigger::OnStateCompleted;
        return true;
    }
    if (TriggerString.Equals(TEXT("OnStateSucceeded"), ESearchCase::IgnoreCase))
    {
        OutTrigger = EStateTreeTransitionTrigger::OnStateSucceeded;
        return true;
    }
    if (TriggerString.Equals(TEXT("OnStateFailed"), ESearchCase::IgnoreCase))
    {
        OutTrigger = EStateTreeTransitionTrigger::OnStateFailed;
        return true;
    }
    if (TriggerString.Equals(TEXT("OnTick"), ESearchCase::IgnoreCase))
    {
        OutTrigger = EStateTreeTransitionTrigger::OnTick;
        return true;
    }
    if (TriggerString.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase))
    {
        OutTrigger = EStateTreeTransitionTrigger::OnEvent;
        return true;
    }

    Ctx.SendError(TEXT("INVALID_PARAMS"), FString::Printf(TEXT("Unsupported transition trigger '%s'"), *TriggerString));
    return false;
}

FStateTreeTransition* FindTransition(UStateTreeState& SourceState, const UStateTreeState* TargetState, const FString& TargetStateName, const FString& TransitionId)
{
    FGuid ParsedTransitionId;
    const bool bHasTransitionId = !TransitionId.IsEmpty() && ParseGuid(TransitionId, ParsedTransitionId);

    for (FStateTreeTransition& Transition : SourceState.Transitions)
    {
        if (bHasTransitionId && Transition.ID == ParsedTransitionId)
        {
            return &Transition;
        }

        if (TargetState && Transition.State.ID == TargetState->ID)
        {
            return &Transition;
        }

        if (!TargetStateName.IsEmpty() && Transition.State.Name.ToString().Equals(TargetStateName, ESearchCase::IgnoreCase))
        {
            return &Transition;
        }
    }

    return nullptr;
}

bool ResolveTransition(FHandlerContext& Ctx, UStateTreeEditorData& EditorData, UStateTreeState*& OutSourceState, FStateTreeTransition*& OutTransition)
{
    FString FromState = Ctx.GetString(TEXT("fromState"));
    if (FromState.IsEmpty())
    {
        FromState = Ctx.GetString(TEXT("stateName"));
    }
    const FString ToState = Ctx.GetString(TEXT("toState"));
    const FString TransitionId = Ctx.GetString(TEXT("transitionId"));

    if (FromState.IsEmpty() || (ToState.IsEmpty() && TransitionId.IsEmpty()))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("fromState and either toState or transitionId are required"));
        return false;
    }

    OutSourceState = FindState(EditorData, FromState);
    if (!OutSourceState)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Source state '%s' not found"), *FromState));
        return false;
    }

    UStateTreeState* TargetState = ToState.IsEmpty() ? nullptr : FindState(EditorData, ToState);
    if (!ToState.IsEmpty() && !TargetState)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Target state '%s' not found"), *ToState));
        return false;
    }

    OutTransition = FindTransition(*OutSourceState, TargetState, ToState, TransitionId);
    if (!OutTransition)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Matching transition not found"));
        return false;
    }

    return true;
}

FString GetStructParam(FHandlerContext& Ctx, const TCHAR* Primary, const TCHAR* Alias)
{
    FString Value = Ctx.GetString(Primary);
    if (Value.IsEmpty())
    {
        Value = Ctx.GetString(Alias);
    }
    return Value;
}

// Returns the value the five state_tree.* mutators publish as `saved`, so it has to be
// a measurement. It used to be `bSave ? McpSafeAssetSave(StateTree) : false` — and that
// helper returned the literal true for any non-null asset, so `save:true` produced
// `saved:true` for a mutation that only ever marked the package dirty. The deferral
// itself stays (it dodges the documented bulkdata-corruption vector); what changes is
// that a still-unwritten asset now reports saved:false and the caller can flush it with
// asset.save before relying on it.
bool FinishStateTreeMutation(UStateTree* StateTree, bool bSave)
{
    StateTree->LastCompiledEditorDataHash = 0;
    StateTree->MarkPackageDirty();
    if (!bSave)
    {
        return false;
    }
    McpSafeAssetSave(StateTree);
    return IsAssetPersistedToDisk(StateTree);
}
#endif
}

REGISTER_RPC_HANDLER("state_tree.add_evaluator", "state_tree",
    "Add a native StateTree evaluator node by UScriptStruct name or path.",
    RPC_PARAMS(
        RPC_PARAM_REQ("stateTreePath", "path", "Path to the StateTree asset"),
        RPC_PARAM_REQ("evaluatorClass", "classref", "UScriptStruct name or path for an FStateTreeEvaluatorBase child"),
        RPC_PARAM_OPT("name", "string", "Optional node name"),
        RPC_PARAM_OPT("properties", "object", "Optional node or instance properties"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
#if MCP_STATE_TREE_AUTHORING_AVAILABLE
    UStateTree* StateTree = nullptr;
    UStateTreeEditorData* EditorData = nullptr;
    if (!LoadStateTreeEditorData(Ctx, StateTree, EditorData))
    {
        return true;
    }

    UScriptStruct* EvaluatorStruct = nullptr;
    if (!ResolveNodeStruct(Ctx, GetStructParam(Ctx, TEXT("evaluatorClass"), TEXT("evaluatorStruct")), FStateTreeEvaluatorBase::StaticStruct(), TEXT("evaluator"), EvaluatorStruct))
    {
        return true;
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), false);
    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: state_tree.add_evaluator")));
    StateTree->Modify();
    EditorData->Modify();
    TArray<FString> Warnings;
    FStateTreeEditorNode& EditorNode = AddEditorNode(EditorData->Evaluators, *EvaluatorStruct, Ctx.GetString(TEXT("name")), Ctx.GetObject(TEXT("properties")), Warnings);
    const bool bSaved = FinishStateTreeMutation(StateTree, bSave);

    TSharedPtr<FJsonObject> Result = MakeNodeResult(EditorNode, EvaluatorStruct->GetName());
    Result->SetNumberField(TEXT("evaluatorCount"), EditorData->Evaluators.Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("message"), TEXT("StateTree evaluator added"));
    AddWarnings(Result, Warnings);
    AddAssetVerification(Result, StateTree);
    Ctx.SendSuccess(Result);
#else
    SendStateTreeHeadersUnavailable(Ctx);
#endif
    return true;
}

REGISTER_RPC_HANDLER("state_tree.add_task", "state_tree",
    "Add a native StateTree task node to a state by UScriptStruct name or path.",
    RPC_PARAMS(
        RPC_PARAM_REQ("stateTreePath", "path", "Path to the StateTree asset"),
        RPC_PARAM_REQ("stateName", "string", "State that receives the task"),
        RPC_PARAM_REQ("taskClass", "classref", "UScriptStruct name or path for an FStateTreeTaskBase child"),
        RPC_PARAM_OPT("name", "string", "Optional node name"),
        RPC_PARAM_OPT("properties", "object", "Optional node or instance properties"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
#if MCP_STATE_TREE_AUTHORING_AVAILABLE
    UStateTree* StateTree = nullptr;
    UStateTreeEditorData* EditorData = nullptr;
    if (!LoadStateTreeEditorData(Ctx, StateTree, EditorData))
    {
        return true;
    }

    const FString StateName = Ctx.GetString(TEXT("stateName"));
    UStateTreeState* State = FindState(*EditorData, StateName);
    if (!State)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("State '%s' not found"), *StateName));
        return true;
    }

    UScriptStruct* TaskStruct = nullptr;
    if (!ResolveNodeStruct(Ctx, GetStructParam(Ctx, TEXT("taskClass"), TEXT("taskStruct")), FStateTreeTaskBase::StaticStruct(), TEXT("task"), TaskStruct))
    {
        return true;
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), false);
    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: state_tree.add_task")));
    StateTree->Modify();
    EditorData->Modify();
    State->Modify();
    TArray<FString> Warnings;
    FStateTreeEditorNode& EditorNode = AddEditorNode(State->Tasks, *TaskStruct, Ctx.GetString(TEXT("name")), Ctx.GetObject(TEXT("properties")), Warnings);
    const bool bSaved = FinishStateTreeMutation(StateTree, bSave);

    TSharedPtr<FJsonObject> Result = MakeNodeResult(EditorNode, TaskStruct->GetName());
    Result->SetStringField(TEXT("stateName"), StateName);
    Result->SetNumberField(TEXT("taskCount"), State->Tasks.Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("message"), TEXT("StateTree task added"));
    AddWarnings(Result, Warnings);
    AddAssetVerification(Result, StateTree);
    Ctx.SendSuccess(Result);
#else
    SendStateTreeHeadersUnavailable(Ctx);
#endif
    return true;
}

REGISTER_RPC_HANDLER("state_tree.add_condition", "state_tree",
    "Add a native StateTree condition node to a state's enter conditions or to a transition.",
    RPC_PARAMS(
        RPC_PARAM_REQ("stateTreePath", "path", "Path to the StateTree asset"),
        RPC_PARAM_REQ("stateName", "string", "State for enter condition target, or source state for transition target"),
        RPC_PARAM_REQ("conditionClass", "classref", "UScriptStruct name or path for an FStateTreeConditionBase child"),
        RPC_PARAM_DEF("target", "string", "enter or transition", "enter"),
        RPC_PARAM_OPT("toState", "string", "Transition target state when target=transition"),
        RPC_PARAM_OPT("transitionId", "string", "Transition ID when target=transition"),
        RPC_PARAM_OPT("name", "string", "Optional node name"),
        RPC_PARAM_OPT("properties", "object", "Optional node or instance properties"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
#if MCP_STATE_TREE_AUTHORING_AVAILABLE
    UStateTree* StateTree = nullptr;
    UStateTreeEditorData* EditorData = nullptr;
    if (!LoadStateTreeEditorData(Ctx, StateTree, EditorData))
    {
        return true;
    }

    UScriptStruct* ConditionStruct = nullptr;
    if (!ResolveNodeStruct(Ctx, GetStructParam(Ctx, TEXT("conditionClass"), TEXT("conditionStruct")), FStateTreeConditionBase::StaticStruct(), TEXT("condition"), ConditionStruct))
    {
        return true;
    }

    const FString Target = Ctx.GetString(TEXT("target"), TEXT("enter"));
    TArray<FString> Warnings;
    FStateTreeEditorNode* EditorNode = nullptr;
    int32 ConditionCount = 0;
    const bool bSave = Ctx.GetBool(TEXT("save"), false);
    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: state_tree.add_condition")));

    if (Target.Equals(TEXT("transition"), ESearchCase::IgnoreCase))
    {
        UStateTreeState* SourceState = nullptr;
        FStateTreeTransition* Transition = nullptr;
        if (!ResolveTransition(Ctx, *EditorData, SourceState, Transition))
        {
            return true;
        }

        StateTree->Modify();
        EditorData->Modify();
        SourceState->Modify();
        EditorNode = &AddEditorNode(Transition->Conditions, *ConditionStruct, Ctx.GetString(TEXT("name")), Ctx.GetObject(TEXT("properties")), Warnings);
        ConditionCount = Transition->Conditions.Num();
    }
    else if (Target.Equals(TEXT("enter"), ESearchCase::IgnoreCase))
    {
        const FString StateName = Ctx.GetString(TEXT("stateName"));
        UStateTreeState* State = FindState(*EditorData, StateName);
        if (!State)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("State '%s' not found"), *StateName));
            return true;
        }

        StateTree->Modify();
        EditorData->Modify();
        State->Modify();
        EditorNode = &AddEditorNode(State->EnterConditions, *ConditionStruct, Ctx.GetString(TEXT("name")), Ctx.GetObject(TEXT("properties")), Warnings);
        ConditionCount = State->EnterConditions.Num();
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), FString::Printf(TEXT("Unsupported condition target '%s'"), *Target));
        return true;
    }

    const bool bSaved = FinishStateTreeMutation(StateTree, bSave);

    TSharedPtr<FJsonObject> Result = MakeNodeResult(*EditorNode, ConditionStruct->GetName());
    Result->SetStringField(TEXT("target"), Target);
    Result->SetNumberField(TEXT("conditionCount"), ConditionCount);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("message"), TEXT("StateTree condition added"));
    AddWarnings(Result, Warnings);
    AddAssetVerification(Result, StateTree);
    Ctx.SendSuccess(Result);
#else
    SendStateTreeHeadersUnavailable(Ctx);
#endif
    return true;
}

REGISTER_RPC_HANDLER("state_tree.set_transition_trigger", "state_tree",
    "Set an existing StateTree transition trigger and optional OnEvent tag/payload.",
    RPC_PARAMS(
        RPC_PARAM_REQ("stateTreePath", "path", "Path to the StateTree asset"),
        RPC_PARAM_REQ("fromState", "string", "Source state name"),
        RPC_PARAM_OPT("toState", "string", "Target state name"),
        RPC_PARAM_OPT("transitionId", "string", "Transition ID"),
        RPC_PARAM_REQ("trigger", "string", "OnTick, OnEvent, OnStateCompleted, OnStateSucceeded, or OnStateFailed"),
        RPC_PARAM_OPT("gameplayEventTag", "string", "Required event tag for OnEvent"),
        RPC_PARAM_OPT("payloadStruct", "classref", "Optional event payload UScriptStruct"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
#if MCP_STATE_TREE_AUTHORING_AVAILABLE
    UStateTree* StateTree = nullptr;
    UStateTreeEditorData* EditorData = nullptr;
    if (!LoadStateTreeEditorData(Ctx, StateTree, EditorData))
    {
        return true;
    }

    EStateTreeTransitionTrigger Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
    const FString TriggerString = Ctx.GetString(TEXT("trigger"));
    if (!ResolveTrigger(Ctx, TriggerString, Trigger))
    {
        return true;
    }

    UStateTreeState* SourceState = nullptr;
    FStateTreeTransition* Transition = nullptr;
    if (!ResolveTransition(Ctx, *EditorData, SourceState, Transition))
    {
        return true;
    }

    FGameplayTag RequiredEventTag;
    const UScriptStruct* RequiredPayloadStruct = nullptr;
    if (Trigger == EStateTreeTransitionTrigger::OnEvent)
    {
        const FString TagString = Ctx.GetString(TEXT("gameplayEventTag"));
        if (!TagString.IsEmpty())
        {
            RequiredEventTag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagString), false);
            if (!RequiredEventTag.IsValid())
            {
                Ctx.SendError(TEXT("INVALID_PARAMS"), FString::Printf(TEXT("Gameplay tag '%s' is not registered"), *TagString));
                return true;
            }
        }

        const FString PayloadStructName = Ctx.GetString(TEXT("payloadStruct"));
        if (!PayloadStructName.IsEmpty())
        {
            RequiredPayloadStruct = ResolveUScriptStruct(PayloadStructName);
            if (!RequiredPayloadStruct)
            {
                Ctx.SendError(TEXT("STRUCT_NOT_FOUND"), FString::Printf(TEXT("Could not resolve event payload struct: %s"), *PayloadStructName));
                return true;
            }
        }
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), false);
    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: state_tree.set_transition_trigger")));
    StateTree->Modify();
    EditorData->Modify();
    SourceState->Modify();

    Transition->Trigger = Trigger;
    // FStateTreeTransition::RequiredEvent (FStateTreeEventDesc) was added in UE 5.5. On 5.4 the transition's
    // gameplay event is a bare FGameplayTag EventTag and there is no per-transition payload-struct field, so the
    // resolved payload struct cannot be persisted on a 5.4 transition (5.4 carries the payload on the runtime event).
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    Transition->EventTag = FGameplayTag();
    if (Trigger == EStateTreeTransitionTrigger::OnEvent)
    {
        Transition->EventTag = RequiredEventTag;
    }
#else
    Transition->RequiredEvent = FStateTreeEventDesc();
    if (Trigger == EStateTreeTransitionTrigger::OnEvent)
    {
        Transition->RequiredEvent.Tag = RequiredEventTag;
        Transition->RequiredEvent.PayloadStruct = RequiredPayloadStruct;
    }
#endif

    const bool bSaved = FinishStateTreeMutation(StateTree, bSave);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("transitionId"), Transition->ID.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetStringField(TEXT("trigger"), TriggerString);
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    Result->SetStringField(TEXT("gameplayEventTag"), Transition->EventTag.ToString());
#else
    Result->SetStringField(TEXT("gameplayEventTag"), Transition->RequiredEvent.Tag.ToString());
#endif
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("message"), TEXT("StateTree transition trigger updated"));
    AddAssetVerification(Result, StateTree);
    Ctx.SendSuccess(Result);
#else
    SendStateTreeHeadersUnavailable(Ctx);
#endif
    return true;
}

REGISTER_RPC_HANDLER("state_tree.bind_property", "state_tree",
    "Add an editor property binding between two StateTree node property paths.",
    RPC_PARAMS(
        RPC_PARAM_REQ("stateTreePath", "path", "Path to the StateTree asset"),
        RPC_PARAM_REQ("sourcePath", "string", "Source path as nodeId:property, nodeName.property, or property with sourceId"),
        RPC_PARAM_REQ("targetPath", "string", "Target path as nodeId:property, nodeName.property, or property with targetId"),
        RPC_PARAM_OPT("sourceId", "string", "Optional source node ID"),
        RPC_PARAM_OPT("targetId", "string", "Optional target node ID"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset after mutation", "false")
    ))
{
#if MCP_STATE_TREE_AUTHORING_AVAILABLE
    UStateTree* StateTree = nullptr;
    UStateTreeEditorData* EditorData = nullptr;
    if (!LoadStateTreeEditorData(Ctx, StateTree, EditorData))
    {
        return true;
    }

    FMcpStateTreeBindingPath SourcePath;
    FMcpStateTreeBindingPath TargetPath;
    FString Error;
    if (!ParseBindingPath(*EditorData, Ctx.GetString(TEXT("sourcePath")), Ctx.GetString(TEXT("sourceId")), SourcePath, Error)
        || !ParseBindingPath(*EditorData, Ctx.GetString(TEXT("targetPath")), Ctx.GetString(TEXT("targetId")), TargetPath, Error))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), Error);
        return true;
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), false);
    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: state_tree.bind_property")));
    StateTree->Modify();
    EditorData->Modify();
    EditorData->AddPropertyBinding(SourcePath, TargetPath);
    const bool bSaved = FinishStateTreeMutation(StateTree, bSave);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sourcePath"), SourcePath.ToString());
    Result->SetStringField(TEXT("targetPath"), TargetPath.ToString());
    // FStateTreeEditorPropertyBindings::GetNumBindings() was added in UE 5.6.
    // On 5.4/5.5 use GetBindings().Num(), the underlying array accessor.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    const int32 BindingCount = EditorData->GetPropertyEditorBindings() ? EditorData->GetPropertyEditorBindings()->GetBindings().Num() : 0;
#else
    const int32 BindingCount = EditorData->GetPropertyEditorBindings() ? EditorData->GetPropertyEditorBindings()->GetNumBindings() : 0;
#endif
    Result->SetNumberField(TEXT("bindingCount"), BindingCount);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("message"), TEXT("StateTree property binding added"));
    AddAssetVerification(Result, StateTree);
    Ctx.SendSuccess(Result);
#else
    SendStateTreeHeadersUnavailable(Ctx);
#endif
    return true;
}
