// Copyright (c) 2026 Alexander Penkin. MIT License.

// CodeNodeEmitter.cpp - K2 node creation and visual layout engine for Blueprint code compilation

#include "Compiler/CodeNodeEmitter.h"
#include "Compiler/BpirSharedConstants.h"

#include "Utils/ClassUtils.h"
#include "Utils/GuardedLoad.h"
#include "Utils/PropertyUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_Message.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Self.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_MakeArray.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Select.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_InputKey.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_FormatText.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_AsyncAction.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_SpawnActorFromClass.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"
#include "Compat/EngineVersionCompat.h"
#include "UObject/UnrealType.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node.h"
#include "InputAction.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogCodeNodeEmitter, Log, All);

// ------------------------------------------------------------------------------------------------
// Construction
// ------------------------------------------------------------------------------------------------

FCodeNodeEmitter::FCodeNodeEmitter(UBlueprint* InBlueprint, UEdGraph* InGraph)
    : Blueprint(InBlueprint)
    , Graph(InGraph)
    , CurrentNodeX(0)
    , CurrentBaseY(0)
    , CurrentPureNodeYOffset(80)
    , NodeCount(0)
    , CachedStandardMacros(nullptr)
{
}

void FCodeNodeEmitter::SetGraph(UEdGraph* InGraph)
{
    Graph = InGraph;
    // Reset layout state when switching to a new graph
    CurrentNodeX = 0;
    CurrentBaseY = 0;
    CurrentPureNodeYOffset = 80;
}

// ------------------------------------------------------------------------------------------------
// Layout
// ------------------------------------------------------------------------------------------------

void FCodeNodeEmitter::ResetPlacementForChain(UEdGraphPin* StartPin, int32 YOffset)
{
    if (!StartPin || !StartPin->GetOwningNode())
    {
        return;
    }
    UEdGraphNode* OwnerNode = StartPin->GetOwningNode();
    CurrentNodeX = OwnerNode->NodePosX + 300;
    CurrentBaseY = OwnerNode->NodePosY + YOffset;
    CurrentPureNodeYOffset = 80;
}

void FCodeNodeEmitter::PlaceNode(UEdGraphNode* Node, UEdGraphPin* LastExecPin, int32 XOff, int32 YOff)
{
    if (!Node)
    {
        return;
    }

    // Determine whether the node has any exec pins (making it impure)
    bool bHasExecPin = false;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            bHasExecPin = true;
            break;
        }
    }

    if (!bHasExecPin)
    {
        // Pure nodes stack vertically to the left of the current chain position
        Node->NodePosX = CurrentNodeX - 300 + XOff;
        Node->NodePosY = CurrentBaseY + CurrentPureNodeYOffset + YOff;
        CurrentPureNodeYOffset += 130;
    }
    else
    {
        // Impure nodes advance the horizontal cursor
        Node->NodePosX = CurrentNodeX + XOff;
        Node->NodePosY = CurrentBaseY + YOff;
        CurrentNodeX += 450;
        CurrentPureNodeYOffset = 80;
    }

    // Wire the incoming exec connection if a source pin was provided
    if (LastExecPin && Graph)
    {
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        if (Schema)
        {
            // Find the node's exec-input pin (direction: EGPD_Input, category: exec)
            UEdGraphPin* ExecInputPin = nullptr;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin
                    && Pin->Direction == EGPD_Input
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    ExecInputPin = Pin;
                    break;
                }
            }
            if (ExecInputPin)
            {
                if (!Schema->TryCreateConnection(LastExecPin, ExecInputPin))
                {
                    UE_LOG(LogCodeNodeEmitter, Warning, TEXT("PlaceNode: TryCreateConnection failed wiring exec '%s' -> '%s'"),
                        *LastExecPin->PinName.ToString(), *ExecInputPin->PinName.ToString());
                }
            }
        }
    }
}

int32 FCodeNodeEmitter::FindFreeYPositionForEventNode()
{
    if (!Graph)
    {
        return 0;
    }

    int32 MaxY = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodePosY > MaxY)
        {
            MaxY = Node->NodePosY;
        }
    }
    return MaxY + 450;
}

// ------------------------------------------------------------------------------------------------
// Private helpers
// ------------------------------------------------------------------------------------------------

void FCodeNodeEmitter::InitializeNode(UEdGraphNode* Node)
{
    if (!Node || !Graph)
    {
        return;
    }
    Node->CreateNewGuid();
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    CreatedNodeGUIDs.Add(Node->NodeGuid);
    NodeCount++;
}

UEdGraphPin* FCodeNodeEmitter::FindExecPin(UEdGraphNode* Node, FName PinName)
{
    return FindExecPin(Node, PinName.ToString());
}

UEdGraphPin* FCodeNodeEmitter::FindExecPin(UEdGraphNode* Node, const FString& PinName)
{
    if (!Node)
    {
        return nullptr;
    }

    // First pass: match by name
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin
            && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
        {
            return Pin;
        }
    }

    // Second pass: return any output exec pin ("then" is the typical name in Unreal)
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin
            && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return Pin;
        }
    }

    return nullptr;
}

UBlueprint* FCodeNodeEmitter::GetStandardMacrosLibrary()
{
    if (!CachedStandardMacros)
    {
        CachedStandardMacros = LoadObject<UBlueprint>(
            nullptr,
            TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));

        if (!CachedStandardMacros)
        {
            UE_LOG(LogCodeNodeEmitter, Warning, TEXT("Failed to load StandardMacros library"));
        }
    }
    return CachedStandardMacros;
}

UEdGraph* BpirCompilerMacroUtils::FindMacroGraphByName(const TArray<UEdGraph*>& MacroGraphs, FName MacroName, const UEdGraph* ExcludedGraph)
{
    for (UEdGraph* MacroGraph : MacroGraphs)
    {
        if (MacroGraph && MacroGraph != ExcludedGraph && MacroGraph->GetFName() == MacroName)
        {
            return MacroGraph;
        }
    }

    const FString NormalizedName = MacroName.ToString().Replace(TEXT(" "), TEXT(""));
    for (UEdGraph* MacroGraph : MacroGraphs)
    {
        if (!MacroGraph || MacroGraph == ExcludedGraph)
        {
            continue;
        }

        const FString GraphName = MacroGraph->GetFName().ToString().Replace(TEXT(" "), TEXT(""));
        if (GraphName.Equals(NormalizedName, ESearchCase::IgnoreCase))
        {
            UE_LOG(LogCodeNodeEmitter, Verbose,
                TEXT("Resolved macro '%s' via fuzzy match to '%s'"),
                *MacroName.ToString(), *MacroGraph->GetFName().ToString());
            return MacroGraph;
        }
    }

    return nullptr;
}

UEdGraph* BpirCompilerMacroUtils::FindMacroGraphByName(UBlueprint* Blueprint, FName MacroName, const UEdGraph* ExcludedGraph)
{
    if (!Blueprint)
    {
        return nullptr;
    }

    return FindMacroGraphByName(Blueprint->MacroGraphs, MacroName, ExcludedGraph);
}

UEdGraph* FCodeNodeEmitter::FindMacroGraph(UBlueprint* MacroLib, FName MacroName)
{
    return BpirCompilerMacroUtils::FindMacroGraphByName(MacroLib, MacroName);
}

// ------------------------------------------------------------------------------------------------
// Node creation
// ------------------------------------------------------------------------------------------------

UK2Node_CallFunction* FCodeNodeEmitter::CreateCallFunctionNode(UFunction* Function, UEdGraphPin*& InOutExecPin, bool bAsInterfaceMessage)
{
    if (!Function || !Graph)
    {
        UE_LOG(LogCodeNodeEmitter, Warning, TEXT("CreateCallFunctionNode: null Function or Graph"));
        return nullptr;
    }

    // Functions with ArrayParm metadata (Array_Get, Array_Add, Array_Contains, etc.) need
    // UK2Node_CallArrayFunction so wildcard-to-element type propagates via PropagateArrayTypeInfo.
    // A generic UK2Node_CallFunction leaves the wildcard unresolved, which breaks any
    // downstream cast<>/accessor wiring on the result pin.
    //
    // UK2Node_Message is the interface-message node ("call if the object implements it"):
    // it derives from UK2Node_CallFunction but overrides CreateSelfPin to make the self pin
    // a plain object pin (K2Node_Message.cpp:88-93), so an object reference can be wired
    // straight in and the dispatch check happens at runtime. An interface function is never
    // an ArrayParm, so the two branches cannot collide.
    UK2Node_CallFunction* Node =
        bAsInterfaceMessage
        ? NewObject<UK2Node_Message>(Graph)
        : (Function->HasMetaData(TEXT("ArrayParm"))
            ? NewObject<UK2Node_CallArrayFunction>(Graph)
            : NewObject<UK2Node_CallFunction>(Graph));
    Node->SetFromFunction(Function);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_CallFunction* FCodeNodeEmitter::CreateCallParentFunctionNode(
    UFunction* Function,
    UEdGraphPin*& InOutExecPin)
{
    if (!Function || !Graph)
    {
        UE_LOG(LogCodeNodeEmitter, Warning, TEXT("CreateCallParentFunctionNode: null Function or Graph"));
        return nullptr;
    }

    UK2Node_CallParentFunction* Node = NewObject<UK2Node_CallParentFunction>(Graph);
    Node->SetFromFunction(Function);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_VariableGet* FCodeNodeEmitter::CreateVariableGetNode(FName VarName)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }

    UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
    Node->VariableReference.SetSelfMember(VarName);
    InitializeNode(Node);
    // Pure node — no exec pin to pass; PlaceNode uses nullptr to skip wiring
    PlaceNode(Node, nullptr);
    return Node;
}

UK2Node_VariableSet* FCodeNodeEmitter::CreateVariableSetNode(FName VarName, UEdGraphPin*& InOutExecPin)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }

    UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
    Node->VariableReference.SetSelfMember(VarName);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_VariableGet* FCodeNodeEmitter::CreateExternalVariableGetNode(FName VarName, UClass* TargetClass)
{
    if (!Graph || !Blueprint || !TargetClass)
    {
        return nullptr;
    }

    UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
    Node->VariableReference.SetExternalMember(VarName, TargetClass);
    InitializeNode(Node);
    PlaceNode(Node, nullptr);
    return Node;
}

UK2Node_VariableSet* FCodeNodeEmitter::CreateExternalVariableSetNode(FName VarName, UClass* TargetClass, UEdGraphPin*& InOutExecPin)
{
    if (!Graph || !Blueprint || !TargetClass)
    {
        return nullptr;
    }

    UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
    Node->VariableReference.SetExternalMember(VarName, TargetClass);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_Self* FCodeNodeEmitter::CreateSelfNode()
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_Self* Node = NewObject<UK2Node_Self>(Graph);
    InitializeNode(Node);
    PlaceNode(Node, nullptr);
    return Node;
}

UK2Node_IfThenElse* FCodeNodeEmitter::CreateBranchNode(UEdGraphPin*& InOutExecPin)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_IfThenElse* Node = NewObject<UK2Node_IfThenElse>(Graph);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    // Advance cursor but leave InOutExecPin pointing at "then" (true branch) by convention
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_MacroInstance* FCodeNodeEmitter::CreateMacroNode(FName MacroName, UEdGraphPin*& InOutExecPin)
{
    if (!Graph)
    {
        return nullptr;
    }

    UBlueprint* MacroLib = GetStandardMacrosLibrary();
    UEdGraph* MacroGraph = MacroLib ? FindMacroGraph(MacroLib, MacroName) : nullptr;
    if (!MacroGraph && Blueprint)
    {
        MacroGraph = BpirCompilerMacroUtils::FindMacroGraphByName(Blueprint, MacroName, Graph);
    }

    if (!MacroGraph)
    {
        UE_LOG(LogCodeNodeEmitter, Warning,
            TEXT("CreateMacroNode: could not find macro '%s' in StandardMacros or target Blueprint MacroGraphs"),
            *MacroName.ToString());
        return nullptr;
    }

    UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
    Node->SetMacroGraph(MacroGraph);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_DynamicCast* FCodeNodeEmitter::CreateCastNode(UClass* TargetClass, UEdGraphPin*& InOutExecPin)
{
    if (!Graph || !TargetClass)
    {
        return nullptr;
    }

    UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Graph);
    Node->TargetType = TargetClass;
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_SwitchEnum* FCodeNodeEmitter::CreateSwitchEnumNode(UEnum* Enum, UEdGraphPin*& InOutExecPin)
{
    if (!Graph || !Enum)
    {
        return nullptr;
    }

    UK2Node_SwitchEnum* Node = NewObject<UK2Node_SwitchEnum>(Graph);
    Node->Enum = Enum;
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    // Leave InOutExecPin at the first case output pin; callers rewire per-case as needed
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_SwitchName* FCodeNodeEmitter::CreateSwitchNameNode(UEdGraphPin*& InOutExecPin)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_SwitchName* Node = NewObject<UK2Node_SwitchName>(Graph);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_SwitchInteger* FCodeNodeEmitter::CreateSwitchIntegerNode(UEdGraphPin*& InOutExecPin)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_SwitchInteger* Node = NewObject<UK2Node_SwitchInteger>(Graph);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_SwitchString* FCodeNodeEmitter::CreateSwitchStringNode(UEdGraphPin*& InOutExecPin)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_SwitchString* Node = NewObject<UK2Node_SwitchString>(Graph);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    return Node;
}

UK2Node_MakeStruct* FCodeNodeEmitter::CreateMakeStructNode(UScriptStruct* Struct)
{
    if (!Graph || !Struct)
    {
        return nullptr;
    }

    UK2Node_MakeStruct* Node = NewObject<UK2Node_MakeStruct>(Graph);
    Node->StructType = Struct;
    InitializeNode(Node);
    // Pure node — no exec pins; pass nullptr to skip exec wiring
    PlaceNode(Node, nullptr);
    return Node;
}

UK2Node_MakeArray* FCodeNodeEmitter::CreateMakeArrayNode(int32 NumInputs)
{
    if (!Graph || NumInputs < 0)
    {
        return nullptr;
    }

    UK2Node_MakeArray* Node = NewObject<UK2Node_MakeArray>(Graph);
    InitializeNode(Node);

    // AllocateDefaultPins creates some initial input pins; add any extras needed
    int32 CurrentInputCount = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->Direction == EGPD_Input)
        {
            CurrentInputCount++;
        }
    }
    while (CurrentInputCount < NumInputs)
    {
        Node->AddInputPin();
        CurrentInputCount++;
    }

    // Pure node — no exec pins; pass nullptr to skip exec wiring
    PlaceNode(Node, nullptr);
    return Node;
}

UK2Node_ExecutionSequence* FCodeNodeEmitter::CreateSequenceNode(int32 NumOutputs, UEdGraphPin*& InOutExecPin)
{
    if (!Graph || NumOutputs <= 0)
    {
        return nullptr;
    }

    UK2Node_ExecutionSequence* Node = NewObject<UK2Node_ExecutionSequence>(Graph);
    InitializeNode(Node);

    // AllocateDefaultPins creates one output; add extras
    const int32 DefaultOutputs = 1;
    for (int32 i = DefaultOutputs; i < NumOutputs; i++)
    {
        Node->AddInputPin();
    }

    PlaceNode(Node, InOutExecPin);
    // Point to the first sequence output ("then_0")
    InOutExecPin = FindExecPin(Node, FName(TEXT("then_0")));
    if (!InOutExecPin)
    {
        InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);
    }
    return Node;
}

UK2Node_Timeline* FCodeNodeEmitter::CreateTimelineNode(const FString& Name, UEdGraphPin*& InOutExecPin)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }

    UK2Node_Timeline* Node = NewObject<UK2Node_Timeline>(Graph);
    Node->TimelineName = FName(*Name);
    // Register the timeline in the Blueprint's timeline list so it survives compilation
    FBlueprintEditorUtils::AddNewTimeline(Blueprint, Node->TimelineName);
    InitializeNode(Node);
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, FName(TEXT("Update")));
    return Node;
}

UK2Node_CustomEvent* FCodeNodeEmitter::CreateCustomEventNode(FName EventName)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
    Node->CustomFunctionName = EventName;
    InitializeNode(Node);

    // Place at a free vertical slot so event roots do not overlap
    Node->NodePosX = 0;
    Node->NodePosY = FindFreeYPositionForEventNode();
    CurrentBaseY = Node->NodePosY;
    CurrentNodeX = 300;
    CurrentPureNodeYOffset = 80;
    return Node;
}

UK2Node_Event* FCodeNodeEmitter::CreateEventNode(FName EventName)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }

    // Use GeneratedClass if available, fall back to SkeletonGeneratedClass for uncompiled BPs
    UClass* EventClass = Blueprint->GeneratedClass
        ? Blueprint->GeneratedClass
        : Blueprint->SkeletonGeneratedClass;

    if (!EventClass)
    {
        UE_LOG(LogCodeNodeEmitter, Warning, TEXT("CreateEventNode: no GeneratedClass or SkeletonGeneratedClass for '%s'"), *EventName.ToString());
        return nullptr;
    }

    // Reuse an existing override node if one already exists for this event
    UK2Node_Event* ExistingNode = FBlueprintEditorUtils::FindOverrideForFunction(
        Blueprint, EventClass, EventName);

    if (ExistingNode)
    {
        return ExistingNode;
    }

    // Hard-fail if the parent class has no UFunction by this name. Without this
    // gate CreateEventNode silently produces a phantom event referencing a
    // non-existent member (e.g. "ReceiveTick" on a UUserWidget whose tick is
    // literally "Tick"). The caller (FBpirCompiler::SetupBuiltinEvent) propagates
    // nullptr as a compile failure, which triggers atomic rollback.
    if (EventClass->FindFunctionByName(EventName) == nullptr)
    {
        UE_LOG(LogCodeNodeEmitter, Warning,
            TEXT("CreateEventNode: parent class '%s' has no UFunction '%s' — refusing to create phantom event"),
            *EventClass->GetName(), *EventName.ToString());
        return nullptr;
    }

    UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
    Node->EventReference.SetExternalMember(EventName, EventClass);
    Node->bOverrideFunction = true;
    InitializeNode(Node);

    Node->NodePosX = 0;
    Node->NodePosY = FindFreeYPositionForEventNode();
    CurrentBaseY = Node->NodePosY;
    CurrentNodeX = 300;
    CurrentPureNodeYOffset = 80;
    return Node;
}

UK2Node_FunctionEntry* FCodeNodeEmitter::GetOrCreateFunctionEntry(UEdGraph* FuncGraph)
{
    if (!FuncGraph)
    {
        return nullptr;
    }

    // Function graphs always have exactly one entry node created by the framework
    for (UEdGraphNode* Node : FuncGraph->Nodes)
    {
        if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            return Entry;
        }
    }

    // Fallback: create one if the graph was built without the usual editor scaffolding
    UK2Node_FunctionEntry* Entry = NewObject<UK2Node_FunctionEntry>(FuncGraph);
    Entry->CreateNewGuid();
    Entry->PostPlacedNewNode();
    Entry->AllocateDefaultPins();
    FuncGraph->AddNode(Entry, true, false);
    CreatedNodeGUIDs.Add(Entry->NodeGuid);
    NodeCount++;
    return Entry;
}

UK2Node_FunctionResult* FCodeNodeEmitter::GetOrCreateFunctionResult(UEdGraph* FuncGraph)
{
    if (!FuncGraph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : FuncGraph->Nodes)
    {
        if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
        {
            return Result;
        }
    }

    UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(FuncGraph);
    Result->CreateNewGuid();
    Result->PostPlacedNewNode();
    Result->AllocateDefaultPins();
    FuncGraph->AddNode(Result, true, false);
    CreatedNodeGUIDs.Add(Result->NodeGuid);
    NodeCount++;
    return Result;
}

UK2Node_FunctionResult* FCodeNodeEmitter::CreateFunctionResult(UEdGraph* FuncGraph)
{
    if (!FuncGraph)
    {
        return nullptr;
    }

    // Find an existing FunctionResult to copy output pin definitions from
    UK2Node_FunctionResult* Template = nullptr;
    for (UEdGraphNode* Node : FuncGraph->Nodes)
    {
        if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
        {
            Template = Result;
            break;
        }
    }

    UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(FuncGraph);
    Result->CreateNewGuid();
    Result->PostPlacedNewNode();
    // When another result node already exists, UK2Node_FunctionResult::PostPlacedNewNode ->
    // SyncWithPrimaryResultNode copies that node's user-defined pins and calls ReconstructNode(),
    // which has already allocated this node's pins. Calling AllocateDefaultPins() again would add
    // a SECOND `execute` exec pin — the engine's user-pin loop is FindPin-guarded, its
    // CreatePin(PC_Exec, PN_Execute) is not.
    if (Result->Pins.Num() == 0)
    {
        Result->AllocateDefaultPins();
    }
    FuncGraph->AddNode(Result, true, false);
    CreatedNodeGUIDs.Add(Result->NodeGuid);
    NodeCount++;

    // Copy user-defined input pins from template (if any) so return values wire correctly
    bool bAddedPins = false;
    if (Template)
    {
        for (UEdGraphPin* Pin : Template->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                && !Result->FindPin(Pin->PinName))
            {
                Result->CreateUserDefinedPin(Pin->PinName, Pin->PinType, EGPD_Input);
                bAddedPins = true;
            }
        }
    }

    // Fallback: if no data input pins exist after AllocateDefaultPins + template copy
    // (e.g., blueprint not yet compiled, no existing FunctionResult), discover
    // return value pins from the FunctionEntry node
    if (!bAddedPins)
    {
        bool bHasDataInputPin = false;
        for (const UEdGraphPin* Pin : Result->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                bHasDataInputPin = true;
                break;
            }
        }

        if (!bHasDataInputPin)
        {
            const FString ReturnValuePrefix = UEdGraphSchema_K2::PN_ReturnValue.ToString();
            for (UEdGraphNode* Node : FuncGraph->Nodes)
            {
                UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
                if (!EntryNode) continue;

                for (const UEdGraphPin* Pin : EntryNode->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Output
                        && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                        && Pin->PinName.ToString().StartsWith(ReturnValuePrefix)
                        && !Result->FindPin(Pin->PinName))
                    {
                        Result->CreateUserDefinedPin(Pin->PinName, Pin->PinType, EGPD_Input);
                        bAddedPins = true;
                    }
                }
                break;
            }
        }
    }

    if (bAddedPins)
    {
        Result->ReconstructNode();
    }

    PlaceNode(Result, nullptr);
    return Result;
}

UK2Node_Select* FCodeNodeEmitter::CreateSelectNode(UEdGraphPin*& InOutExecPin, UEnum* IndexEnum)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_Select* Node = NewObject<UK2Node_Select>(Graph);
    if (IndexEnum)
    {
        Node->SetEnum(IndexEnum, true);
    }
    InitializeNode(Node);
    // Select is pure (no exec pins), so pass nullptr to skip exec wiring
    PlaceNode(Node, nullptr);
    return Node;
}

UK2Node_BreakStruct* FCodeNodeEmitter::CreateBreakStructNode(UScriptStruct* Struct)
{
    if (!Graph || !Struct)
    {
        return nullptr;
    }

    UK2Node_BreakStruct* Node = NewObject<UK2Node_BreakStruct>(Graph);
    Node->StructType = Struct;
    InitializeNode(Node);
    PlaceNode(Node, nullptr);
    return Node;
}

UK2Node_InputKey* FCodeNodeEmitter::CreateInputKeyNode(FName KeyName, bool bReleased)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_InputKey* Node = NewObject<UK2Node_InputKey>(Graph);
    Node->InputKey = FKey(KeyName);
    InitializeNode(Node);

    Node->NodePosX = 0;
    Node->NodePosY = FindFreeYPositionForEventNode();
    CurrentBaseY = Node->NodePosY;
    CurrentNodeX = 300;
    CurrentPureNodeYOffset = 80;

    // Advance InOutExecPin to the pressed or released output
    // (callers must retrieve the specific exec pin they need from the returned node)
    return Node;
}

UClass* FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass()
{
    static UClass* NodeClass = []() -> UClass*
    {
        static const TCHAR* ClassPath = TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction");
        UClass* ResolvedClass = FindObject<UClass>(nullptr, ClassPath);
        if (!ResolvedClass)
        {
            FModuleManager::Get().LoadModule(TEXT("InputBlueprintNodes"));
            ResolvedClass = FindObject<UClass>(nullptr, ClassPath);
        }
        return ResolvedClass && ResolvedClass->IsChildOf(UK2Node::StaticClass())
            ? ResolvedClass
            : nullptr;
    }();
    return NodeClass;
}

FObjectProperty* FCodeNodeEmitter::ResolveEnhancedInputActionProperty(const UClass* NodeClass)
{
    UClass* ResolvedClass = ResolveEnhancedInputActionNodeClass();
    if (NodeClass && NodeClass != ResolvedClass)
    {
        return nullptr;
    }
    static FObjectProperty* InputActionProperty = ResolvedClass
        ? FindFProperty<FObjectProperty>(ResolvedClass, TEXT("InputAction"))
        : nullptr;
    return InputActionProperty;
}

UInputAction* FCodeNodeEmitter::GetEnhancedInputAction(const UEdGraphNode* Node)
{
    UClass* NodeClass = ResolveEnhancedInputActionNodeClass();
    if (!Node || !NodeClass || !Node->IsA(NodeClass))
    {
        return nullptr;
    }
    FObjectProperty* Property = ResolveEnhancedInputActionProperty(NodeClass);
    return Property
        ? Cast<UInputAction>(Property->GetObjectPropertyValue_InContainer(Node))
        : nullptr;
}

bool FCodeNodeEmitter::SetEnhancedInputAction(UEdGraphNode* Node, UInputAction* InputAction)
{
    UClass* NodeClass = ResolveEnhancedInputActionNodeClass();
    FObjectProperty* Property = ResolveEnhancedInputActionProperty(NodeClass);
    if (!Node || !InputAction || !NodeClass || !Node->IsA(NodeClass) || !Property
        || !InputAction->IsA(Property->PropertyClass))
    {
        return false;
    }
    Property->SetObjectPropertyValue_InContainer(Node, InputAction);
    return true;
}

const TArray<FName>& FCodeNodeEmitter::GetEnhancedInputActionEventPinNames()
{
    static const TArray<FName> Names = []()
    {
        TArray<FName> Result;
        for (const TCHAR* EventPinName : BpirSharedConstants::EnhancedInput::EventPinNames)
        {
            Result.Emplace(EventPinName);
        }
        return Result;
    }();
    return Names;
}

UEdGraphNode* FCodeNodeEmitter::CreateEnhancedInputActionNode(UInputAction* InputAction)
{
    UClass* NodeClass = ResolveEnhancedInputActionNodeClass();
    if (!Graph || !InputAction || !NodeClass)
    {
        return nullptr;
    }

    UK2Node* Node = NewObject<UK2Node>(Graph, NodeClass);
    if (!SetEnhancedInputAction(Node, InputAction))
    {
        Node->MarkAsGarbage();
        return nullptr;
    }

    InitializeNode(Node);
    Node->NodePosX = 0;
    Node->NodePosY = FindFreeYPositionForEventNode();
    CurrentBaseY = Node->NodePosY;
    CurrentNodeX = 300;
    CurrentPureNodeYOffset = 80;
    return Node;
}

UK2Node_ComponentBoundEvent* FCodeNodeEmitter::CreateComponentEventNode(
    FName CompName, FName EventName, UBlueprint* BP)
{
    if (!Graph || !BP)
    {
        return nullptr;
    }

    UK2Node_ComponentBoundEvent* Node = NewObject<UK2Node_ComponentBoundEvent>(Graph);
    Node->ComponentPropertyName = CompName;
    Node->DelegatePropertyName = EventName;

    // Skeleton carries newly-added widget properties before recompile; GeneratedClass can be
    // stale after widget_import_xml until a full BP compile runs.
    UClass* const BPClass = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass : BP->GeneratedClass;

    FObjectProperty* CompProp = BPClass
        ? CastField<FObjectProperty>(BPClass->FindPropertyByName(CompName))
        : nullptr;

    if (CompProp)
    {
        FMulticastDelegateProperty* DelegateProp = FindFProperty<FMulticastDelegateProperty>(
            CompProp->PropertyClass, EventName, EFieldIterationFlags::IncludeSuper);

        // Display-name fallback: BPIR callers often use the editor-visible event name
        // (e.g. "OnClicked" shown in the Details panel) which can differ from the
        // UPROPERTY's internal name when the property uses meta=(DisplayName="..."),
        // or when casing differs. Iterate the class chain and match against DisplayName
        // metadata + the property name (case-insensitive, space-stripped).
        if (!DelegateProp)
        {
            const FString Wanted = EventName.ToString().Replace(TEXT(" "), TEXT(""));
            for (TFieldIterator<FMulticastDelegateProperty> It(CompProp->PropertyClass,
                EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                FMulticastDelegateProperty* Candidate = *It;
                const FString CandName = Candidate->GetName().Replace(TEXT(" "), TEXT(""));
                FString CandDisplay = Candidate->GetMetaData(TEXT("DisplayName"));
                CandDisplay = CandDisplay.Replace(TEXT(" "), TEXT(""));
                if (CandName.Equals(Wanted, ESearchCase::IgnoreCase)
                    || (!CandDisplay.IsEmpty() && CandDisplay.Equals(Wanted, ESearchCase::IgnoreCase)))
                {
                    DelegateProp = Candidate;
                    break;
                }
            }
        }

        if (DelegateProp)
        {
            Node->InitializeComponentBoundEventParams(CompProp, DelegateProp);
        }
        else
        {
            UE_LOG(LogCodeNodeEmitter, Warning,
                TEXT("CreateComponentEventNode: delegate '%s' not found on class '%s' for component '%s'."),
                *EventName.ToString(),
                *CompProp->PropertyClass->GetName(),
                *CompName.ToString());
            Node->DelegateOwnerClass = BPClass;
        }
    }
    else
    {
        UE_LOG(LogCodeNodeEmitter, Warning,
            TEXT("CreateComponentEventNode: FObjectProperty '%s' not found on class '%s'."),
            *CompName.ToString(),
            BPClass ? *BPClass->GetName() : TEXT("null"));
        Node->DelegateOwnerClass = BPClass;
    }

    InitializeNode(Node);

    Node->NodePosX = 0;
    Node->NodePosY = FindFreeYPositionForEventNode();
    CurrentBaseY = Node->NodePosY;
    CurrentNodeX = 300;
    CurrentPureNodeYOffset = 80;
    return Node;
}

UEdGraphNode_Comment* FCodeNodeEmitter::CreateCommentBox(
    const FString& Text, const TArray<UEdGraphNode*>& Nodes)
{
    if (!Graph || Nodes.IsEmpty())
    {
        return nullptr;
    }

    constexpr int32 Padding = 30;

    int32 MinX = INT32_MAX, MinY = INT32_MAX;
    int32 MaxX = INT32_MIN, MaxY = INT32_MIN;

    for (const UEdGraphNode* Node : Nodes)
    {
        if (!Node) { continue; }
        MinX = FMath::Min(MinX, Node->NodePosX);
        MinY = FMath::Min(MinY, Node->NodePosY);
        // Approximate node width/height since UEdGraphNode does not expose them at edit time
        MaxX = FMath::Max(MaxX, Node->NodePosX + 200);
        MaxY = FMath::Max(MaxY, Node->NodePosY + 100);
    }

    UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
    Comment->CreateNewGuid();
    Comment->PostPlacedNewNode();
    Comment->AllocateDefaultPins();
    Graph->AddNode(Comment, true, false);
    CreatedNodeGUIDs.Add(Comment->NodeGuid);
    NodeCount++;

    Comment->NodePosX = MinX - Padding;
    Comment->NodePosY = MinY - Padding;
    Comment->NodeWidth  = (MaxX - MinX) + Padding * 2;
    Comment->NodeHeight = (MaxY - MinY) + Padding * 2;
    Comment->NodeComment = Text;

    return Comment;
}

void FCodeNodeEmitter::FinalizeCommentBoxes(TMap<FString, TArray<UEdGraphNode*>>& CommentMap)
{
    for (auto& Pair : CommentMap)
    {
        if (!Pair.Value.IsEmpty())
        {
            CreateCommentBox(Pair.Key, Pair.Value);
        }
    }
}

UEdGraphNode* FCodeNodeEmitter::CreateGenericK2Node(const FString& NodeClassName, UEdGraph* InGraph, const TArray<FString>& /*ProvidedArgNames*/, const FString& ClassPath)
{
    UEdGraph* TargetGraph = InGraph ? InGraph : Graph;
    if (!TargetGraph)
    {
        return nullptr;
    }

    // Find the K2Node class by name — try the bare form, then `UK2Node_` prefixed form.
    UClass* NodeClass = FindFirstObjectSafe<UClass>(*NodeClassName);
    if ((!NodeClass || !NodeClass->IsChildOf(UK2Node::StaticClass()))
        && !NodeClassName.StartsWith(TEXT("UK2Node_")))
    {
        NodeClass = FindFirstObjectSafe<UClass>(*(TEXT("UK2Node_") + NodeClassName));
    }

    if (!NodeClass || !NodeClass->IsChildOf(UK2Node::StaticClass()))
    {
        UE_LOG(LogCodeNodeEmitter, Warning,
            TEXT("CreateGenericK2Node: could not find K2Node class '%s'"), *NodeClassName);
        return nullptr;
    }

    if (NodeClass->IsChildOf(UK2Node_AsyncAction::StaticClass()))
    {
        // Bypass contract: UK2Node_AsyncAction and its dedicated subclasses
        // (UK2Node_AsyncAction_<FactoryFunctionName>) MUST be routed through
        // CreateAsyncActionNode so InitializeProxyFromFunction runs before
        // AllocateDefaultPins. Spawning here without that init produces a node
        // stuck in "Missing Function" state with no delegate/payload pins.
        // EmitGenericK2NodeInstruction intercepts both the bare and the dedicated
        // forms before reaching this generic-fallback path; if a future refactor
        // re-routes async-action creation through here, this short-circuit must
        // be removed in lockstep with that change.
        UE_LOG(LogCodeNodeEmitter, Warning,
            TEXT("CreateGenericK2Node: generic UK2Node_AsyncAction creation is unsupported; use K2Node_AsyncAction_<FactoryFunctionName> so the factory is configured before pins are allocated"));
        return nullptr;
    }

    UK2Node* NewNode = NewObject<UK2Node>(TargetGraph, NodeClass);
    NewNode->CreateNewGuid();

    NewNode->AllocateDefaultPins();
    NewNode->PostPlacedNewNode();

    // Reject nodes that the engine itself considers incompatible with the target graph
    // (e.g. macro-only nodes such as UK2Node_AssignmentStatement placed into an event/
    // function ubergraph). Placing them is graph-illegal: the node is left half-typed and
    // un-reconstructed, and on UE 5.4 it becomes a GC-traversal hazard — a later full GC
    // (e.g. from `memreport`) dereferences its inconsistent pins and hard-crashes the
    // editor, while a full compile drives it through its macro-only compiler handler and
    // also crashes. Refusing placement here keeps the graph valid and GC-safe on every
    // engine version (it is exactly the gate the Blueprint editor applies before pasting a
    // node). The node was constructed under TargetGraph as its outer; drop it before return.
    if (!NewNode->IsCompatibleWithGraph(TargetGraph))
    {
        UE_LOG(LogCodeNodeEmitter, Warning,
            TEXT("CreateGenericK2Node: node class '%s' is not compatible with the target graph; refusing placement"),
            *NodeClassName);
        NewNode->MarkAsGarbage();
        return nullptr;
    }

    // ConstructObjectFromClass subclasses (CreateWidget, SpawnActor, ...) must have
    // ClassPin->DefaultObject populated before ReconstructNode, otherwise GetClassToSpawn()
    // can't specialize the ReturnValue pin and downstream property access fails.
    if (!ClassPath.IsEmpty())
    {
        if (UK2Node_ConstructObjectFromClass* ConstructNode = Cast<UK2Node_ConstructObjectFromClass>(NewNode))
        {
            if (UEdGraphPin* ClassPin = ConstructNode->GetClassPin())
            {
                if (UClass* ResolvedClass = ResolveUClass(ClassPath))
                {
                    ClassPin->DefaultObject = ResolvedClass;
                    NewNode->ReconstructNode();
                }
                else
                {
                    ClassPin->DefaultValue = ClassPath;
                    UE_LOG(LogCodeNodeEmitter, Warning,
                        TEXT("CreateGenericK2Node: could not resolve class '%s' for '%s', setting as string default"),
                        *ClassPath, *NodeClassName);
                }
            }
        }
    }

    TargetGraph->AddNode(NewNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    CreatedNodeGUIDs.Add(NewNode->NodeGuid);
    NodeCount++;
    PlaceNode(NewNode, nullptr);
    return NewNode;
}

UK2Node_GetSubsystem* FCodeNodeEmitter::CreateGetSubsystemNode(UClass* SubsystemClass)
{
    if (!Graph || !SubsystemClass)
    {
        return nullptr;
    }

    // Select the correct node class based on subsystem hierarchy
    UClass* NodeClass = UK2Node_GetSubsystem::StaticClass();

    static UClass* EngineSubsystemClass = FindFirstObjectSafe<UClass>(TEXT("UEngineSubsystem"));
    static UClass* EditorSubsystemClass = FindFirstObjectSafe<UClass>(TEXT("UEditorSubsystem"));
    static UClass* LocalPlayerSubsystemClass = FindFirstObjectSafe<UClass>(TEXT("ULocalPlayerSubsystem"));
    static UClass* WorldSubsystemClass = FindFirstObjectSafe<UClass>(TEXT("UWorldSubsystem"));

    if (EngineSubsystemClass && SubsystemClass->IsChildOf(EngineSubsystemClass))
    {
        NodeClass = FindFirstObjectSafe<UClass>(TEXT("UK2Node_GetEngineSubsystem"));
    }
    else if (EditorSubsystemClass && SubsystemClass->IsChildOf(EditorSubsystemClass))
    {
        NodeClass = FindFirstObjectSafe<UClass>(TEXT("UK2Node_GetEditorSubsystem"));
    }
    else if (LocalPlayerSubsystemClass && SubsystemClass->IsChildOf(LocalPlayerSubsystemClass))
    {
        NodeClass = FindFirstObjectSafe<UClass>(TEXT("UK2Node_GetSubsystemFromPC"));
    }

    if (!NodeClass)
    {
        NodeClass = UK2Node_GetSubsystem::StaticClass();
    }

    UK2Node_GetSubsystem* Node = NewObject<UK2Node_GetSubsystem>(Graph, NodeClass);
    Node->Initialize(SubsystemClass);
    // Must call CreateNewGuid and add to graph manually — Initialize must be called
    // before AllocateDefaultPins, so we can't use InitializeNode helper
    Node->CreateNewGuid();
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    CreatedNodeGUIDs.Add(Node->NodeGuid);
    NodeCount++;
    PlaceNode(Node, nullptr);
    return Node;
}

UK2Node_FormatText* FCodeNodeEmitter::CreateFormatTextNode(const FString& FormatString, FString* OutError)
{
    if (!Graph)
    {
        if (OutError) *OutError = TEXT("Graph is null");
        return nullptr;
    }

    FText FormatText;
    FString TextError;
    if (!CoerceStringToPersistedFText(FormatString, nullptr, FormatText, TextError))
    {
        if (OutError)
        {
            *OutError = FString::Printf(TEXT("FormatText Format requires an FText namespace and key: %s"), *TextError);
        }
        return nullptr;
    }

    UK2Node_FormatText* Node = NewObject<UK2Node_FormatText>(Graph);
    InitializeNode(Node);

    // Set the format string on the Format pin to trigger dynamic argument pin creation.
    // The Format pin is PC_Text, so the value lives in DefaultTextValue (FText), not DefaultValue.
    UEdGraphPin* FormatPin = Node->GetFormatPin();
    if (FormatPin)
    {
        FormatPin->DefaultTextValue = MoveTemp(FormatText);
        FormatPin->DefaultValue.Empty();
        FormatPin->DefaultObject = nullptr;
        Node->PinDefaultValueChanged(FormatPin);
    }

    PlaceNode(Node, nullptr); // Pure node — no exec pin
    return Node;
}

bool FCodeNodeEmitter::IsAsyncActionFactory(UFunction* Func)
{
    if (!Func) return false;
    if (!Func->HasAnyFunctionFlags(FUNC_Static)) return false;
    if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable)) return false;

    UClass* OwnerClass = Func->GetOwnerClass();
    if (!OwnerClass) return false;
    if (OwnerClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)) return false;

    // UE's documented opt-out — classes that own their own specialized K2Node set this.
    // See Runtime/Engine/Classes/Kismet/BlueprintAsyncActionBase.h and K2Node_AsyncAction.cpp:54-57.
    static const FName NAME_HasDedicatedAsyncNode(TEXT("HasDedicatedAsyncNode"));
    if (OwnerClass->HasMetaData(NAME_HasDedicatedAsyncNode)) return false;

    FObjectProperty* ReturnProp = CastField<FObjectProperty>(Func->GetReturnProperty());
    if (!ReturnProp || !ReturnProp->PropertyClass) return false;
    if (!ReturnProp->PropertyClass->IsChildOf(UBlueprintAsyncActionBase::StaticClass())) return false;

    return true;
}

UFunction* FCodeNodeEmitter::ResolveAsyncActionFactoryByName(const FString& FactoryFunctionName, FString* OutError)
{
    if (FactoryFunctionName.IsEmpty())
    {
        if (OutError)
        {
            *OutError = TEXT("K2Node_AsyncAction requires an explicit factory name. Use K2Node_AsyncAction_<FactoryFunctionName>(...) so BPIR can round-trip the configured async action deterministically.");
        }
        return nullptr;
    }

    TArray<UFunction*> Matches;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        if (!Candidate || Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists)) continue;
        if (!Candidate->IsChildOf(UBlueprintAsyncActionBase::StaticClass())) continue;

        for (TFieldIterator<UFunction> FuncIt(Candidate, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
        {
            UFunction* Func = *FuncIt;
            if (Func
                && Func->GetName().Equals(FactoryFunctionName, ESearchCase::CaseSensitive)
                && IsAsyncActionFactory(Func))
            {
                Matches.Add(Func);
            }
        }
    }

    Matches.Sort([](UFunction& A, UFunction& B)
    {
        const FString AName = FString::Printf(TEXT("%s::%s"), *A.GetOwnerClass()->GetPathName(), *A.GetName());
        const FString BName = FString::Printf(TEXT("%s::%s"), *B.GetOwnerClass()->GetPathName(), *B.GetName());
        return AName < BName;
    });

    if (Matches.Num() == 1)
    {
        return Matches[0];
    }

    if (OutError)
    {
        if (Matches.Num() == 0)
        {
            *OutError = FString::Printf(
                TEXT("No async action factory named '%s' was found. Use K2Node_AsyncAction_<FactoryFunctionName>(...) with a static BlueprintCallable factory returning UBlueprintAsyncActionBase."),
                *FactoryFunctionName);
        }
        else
        {
            const FString CandidateList = FString::JoinBy(Matches, TEXT(", "), [](UFunction* Func)
            {
                return FString::Printf(TEXT("%s::%s"), *Func->GetOwnerClass()->GetPathName(), *Func->GetName());
            });
            *OutError = FString::Printf(
                TEXT("Ambiguous async action factory '%s'. K2Node_AsyncAction_<FactoryFunctionName> matched multiple factories: %s"),
                *FactoryFunctionName,
                *CandidateList);
        }
    }
    return nullptr;
}

UFunction* FCodeNodeEmitter::ResolveDedicatedAsyncActionFactoryByName(const FString& FactoryFunctionName)
{
    if (FactoryFunctionName.IsEmpty()) return nullptr;

    // Sibling to ResolveAsyncActionFactoryByName but with the HasDedicatedAsyncNode predicate
    // INVERTED — we explicitly want owner classes that opt out of the generic UK2Node_AsyncAction
    // lane. Single walk: filter to UBlueprintAsyncActionBase subclasses carrying the meta, then
    // match the factory function by name + Static/BlueprintCallable flags.
    static const FName NAME_HasDedicatedAsyncNode(TEXT("HasDedicatedAsyncNode"));
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        if (!Candidate || Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists)) continue;
        if (!Candidate->IsChildOf(UBlueprintAsyncActionBase::StaticClass())) continue;
        if (!Candidate->HasMetaData(NAME_HasDedicatedAsyncNode)) continue;

        for (TFieldIterator<UFunction> FuncIt(Candidate, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
        {
            UFunction* Func = *FuncIt;
            if (Func
                && Func->GetName().Equals(FactoryFunctionName, ESearchCase::CaseSensitive)
                && Func->HasAllFunctionFlags(FUNC_Static | FUNC_BlueprintCallable))
            {
                return Func;
            }
        }
    }
    return nullptr;
}

UClass* FCodeNodeEmitter::ResolveDedicatedAsyncActionSubclass(UFunction* FactoryFunc)
{
    if (!IsValid(FactoryFunc)) return nullptr;

    // Sanity-check the factory shape (mirrors IsAsyncActionFactory minus the HasDedicatedAsyncNode
    // opt-out — that opt-out is the gate this probe is meant to detect, not exclude).
    if (!FactoryFunc->HasAnyFunctionFlags(FUNC_Static)) return nullptr;
    if (!FactoryFunc->HasAnyFunctionFlags(FUNC_BlueprintCallable)) return nullptr;

    UClass* OwnerClass = FactoryFunc->GetOwnerClass();
    if (!OwnerClass) return nullptr;
    if (OwnerClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)) return nullptr;

    static const FName NAME_HasDedicatedAsyncNode(TEXT("HasDedicatedAsyncNode"));
    if (!OwnerClass->HasMetaData(NAME_HasDedicatedAsyncNode)) return nullptr;

    FObjectProperty* ReturnProp = CastField<FObjectProperty>(FactoryFunc->GetReturnProperty());
    if (!ReturnProp || !ReturnProp->PropertyClass) return nullptr;
    if (!ReturnProp->PropertyClass->IsChildOf(UBlueprintAsyncActionBase::StaticClass())) return nullptr;

    const FString FactoryName = FactoryFunc->GetName();

    // Epic naming convention: UK2Node_AsyncAction_<FactoryFunctionName>. Cheap O(1) probe first.
    const FString ConventionalName = FString::Printf(TEXT("K2Node_AsyncAction_%s"), *FactoryName);
    if (UClass* Conventional = FindFirstObjectSafe<UClass>(*ConventionalName))
    {
        if (Conventional->IsChildOf(UK2Node_AsyncAction::StaticClass())
            && !Conventional->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
        {
            return Conventional;
        }
    }

    // Fallback: iterate UK2Node_AsyncAction subclasses. Plugins that don't follow Epic's exact
    // naming may still register a dedicated subclass via RegisterClassFactoryActions<OwnerClass>.
    // Match by class-name suffix (case-insensitive) as a best-effort signal — exact registrar
    // inspection would require executing GetMenuActions on each candidate which is too heavy
    // for a compile-time probe.
    const FString SuffixToken = FString(TEXT("_")) + FactoryName;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        if (!Candidate || Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists)) continue;
        if (!Candidate->IsChildOf(UK2Node_AsyncAction::StaticClass())) continue;
        if (Candidate == UK2Node_AsyncAction::StaticClass()) continue;

        const FString CandidateName = Candidate->GetName();
        if (CandidateName.EndsWith(SuffixToken, ESearchCase::IgnoreCase))
        {
            return Candidate;
        }
    }

    return nullptr;
}

UK2Node_AsyncAction* FCodeNodeEmitter::CreateAsyncActionNode(UFunction* FactoryFunc, UEdGraphPin*& InOutExecPin, UClass* NodeClass)
{
    if (!Graph || !FactoryFunc) return nullptr;

    // When NodeClass is non-null (HasDedicatedAsyncNode lane), spawn the dedicated subclass
    // so its overrides of AllocateDefaultPins materialize the correct dynamic pins. When null,
    // fall back to the generic UK2Node_AsyncAction base — InitializeProxyFromFunction is still
    // the contract entry-point for both lanes because the dedicated subclasses inherit it from
    // UK2Node_BaseAsyncTask.
    UClass* SpawnClass = (NodeClass && NodeClass->IsChildOf(UK2Node_AsyncAction::StaticClass()))
        ? NodeClass
        : UK2Node_AsyncAction::StaticClass();
    UK2Node_AsyncAction* Node = NewObject<UK2Node_AsyncAction>(Graph, SpawnClass);
    Node->CreateNewGuid();

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // InitializeProxyFromFunction was added in UE 5.6; it sets ProxyFactoryFunctionName,
    // ProxyFactoryClass, and ProxyClass. Must run BEFORE AllocateDefaultPins.
    Node->InitializeProxyFromFunction(FactoryFunc);
#else
    // UE 5.4/5.5 fallback: set the proxy fields directly, mirroring what InitializeProxyFromFunction
    // does internally (see K2Node_AsyncAction.cpp GetMenuActions SetNodeFunc lambda).
    if (FactoryFunc)
    {
        const FObjectProperty* ReturnProp = CastField<FObjectProperty>(FactoryFunc->GetReturnProperty());
        if (ReturnProp)
        {
            // These are protected UPROPERTYs; write them via reflection so we don't subclass.
            if (FNameProperty* NameProp = FindFProperty<FNameProperty>(
                    UK2Node_BaseAsyncTask::StaticClass(), TEXT("ProxyFactoryFunctionName")))
            {
                NameProp->SetPropertyValue_InContainer(Node, FactoryFunc->GetFName());
            }
            if (FObjectProperty* ClassProp = FindFProperty<FObjectProperty>(
                    UK2Node_BaseAsyncTask::StaticClass(), TEXT("ProxyFactoryClass")))
            {
                ClassProp->SetObjectPropertyValue_InContainer(Node, FactoryFunc->GetOuterUClass());
            }
            if (FObjectProperty* ProxyClassProp = FindFProperty<FObjectProperty>(
                    UK2Node_BaseAsyncTask::StaticClass(), TEXT("ProxyClass")))
            {
                ProxyClassProp->SetObjectPropertyValue_InContainer(Node, ReturnProp->PropertyClass);
            }
        }
    }
#endif

    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    CreatedNodeGUIDs.Add(Node->NodeGuid);
    NodeCount++;
    PlaceNode(Node, nullptr);

    // Wire exec in/out. Mirrors the pattern in CreateGenericK2Node's async-node handling
    // and the impure-call wiring in BpirCompiler.cpp.
    if (InOutExecPin)
    {
        UEdGraphPin* ExecInputPin = Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (ExecInputPin)
        {
            const UEdGraphSchema* Schema = InOutExecPin->GetSchema();
            if (Schema)
            {
                Schema->TryCreateConnection(InOutExecPin, ExecInputPin);
            }
        }
    }
    UEdGraphPin* ThenPin = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    if (ThenPin)
    {
        InOutExecPin = ThenPin;
    }

    return Node;
}

// ------------------------------------------------------------------------------------------------
// Expand Node Registry — maps BPIR function names to specialized K2Node classes
// ------------------------------------------------------------------------------------------------

void FCodeNodeEmitter::PopulateExpandNodeRegistry() const
{
    if (bExpandNodeRegistryPopulated)
    {
        return;
    }
    bExpandNodeRegistryPopulated = true;

    // UK2Node_CreateWidget (UMGEditor module — private header, so resolve by class name at runtime)
    if (UClass* CreateWidgetClass = FindFirstObjectSafe<UClass>(TEXT("K2Node_CreateWidget")))
    {
        if (CreateWidgetClass->IsChildOf(UK2Node::StaticClass()))
        {
            ExpandNodeRegistry.Add(FName(TEXT("Create")), CreateWidgetClass);
            ExpandNodeRegistry.Add(FName(TEXT("CreateWidget")), CreateWidgetClass);
        }
    }

    // UK2Node_SpawnActorFromClass (BlueprintGraph module — public header available)
    ExpandNodeRegistry.Add(FName(TEXT("SpawnActor")), UK2Node_SpawnActorFromClass::StaticClass());
    ExpandNodeRegistry.Add(FName(TEXT("SpawnActorFromClass")), UK2Node_SpawnActorFromClass::StaticClass());

    // UK2Node_ConstructObjectFromClass (generic — abstract, but subclasses can be registered)
    // Note: ConstructObjectFromClass is abstract, so we look for the concrete GenericCreateObject node
    if (UClass* GenericCreateClass = FindFirstObjectSafe<UClass>(TEXT("K2Node_GenericCreateObject")))
    {
        if (GenericCreateClass->IsChildOf(UK2Node::StaticClass()))
        {
            ExpandNodeRegistry.Add(FName(TEXT("ConstructObject")), GenericCreateClass);
            ExpandNodeRegistry.Add(FName(TEXT("ConstructObjectFromClass")), GenericCreateClass);
        }
    }
}

UClass* FCodeNodeEmitter::FindExpandNodeClass(const FString& FunctionName) const
{
    PopulateExpandNodeRegistry();

    UClass* const* Found = ExpandNodeRegistry.Find(FName(*FunctionName));
    return Found ? *Found : nullptr;
}

UK2Node* FCodeNodeEmitter::CreateExpandNode(UClass* K2NodeClass, const FString& ClassPath, UEdGraphPin*& InOutExecPin)
{
    if (!K2NodeClass || !Graph)
    {
        UE_LOG(LogCodeNodeEmitter, Warning, TEXT("CreateExpandNode: null K2NodeClass or Graph"));
        return nullptr;
    }

    if (!K2NodeClass->IsChildOf(UK2Node::StaticClass()))
    {
        UE_LOG(LogCodeNodeEmitter, Warning, TEXT("CreateExpandNode: '%s' is not a UK2Node subclass"),
            *K2NodeClass->GetName());
        return nullptr;
    }

    // Create the node instance
    UK2Node* Node = NewObject<UK2Node>(Graph, K2NodeClass);
    InitializeNode(Node);

    // Set the class selector pin value before ReconstructNode
    UK2Node_ConstructObjectFromClass* ConstructNode = Cast<UK2Node_ConstructObjectFromClass>(Node);
    if (ConstructNode)
    {
        UEdGraphPin* ClassPin = ConstructNode->GetClassPin();
        if (ClassPin && !ClassPath.IsEmpty())
        {
            // Resolve the class — LoadObject handles both in-memory and on-disk assets.
            // Guarded: ClassPath reaches here from BPIR source text (BpirCompiler.cpp), the one
            // BPIR load that does not route through a guarded resolver, and the BPIR tokenizer
            // cuts only at an unquoted '#' - '/' is special nowhere in it.
            UClass* TargetClass = PinWrightGuardedLoad::LoadObjectChecked<UClass>(ClassPath);

            if (TargetClass)
            {
                ClassPin->DefaultObject = TargetClass;
            }
            else
            {
                // Fall back to string default — ReconstructNode may still work
                ClassPin->DefaultValue = ClassPath;
                UE_LOG(LogCodeNodeEmitter, Warning,
                    TEXT("CreateExpandNode: could not resolve class '%s', setting as string default"),
                    *ClassPath);
            }

            // Reconstruct to generate typed output pins and spawn-variable pins
            Node->ReconstructNode();
        }
    }
    else
    {
        UE_LOG(LogCodeNodeEmitter, Warning,
            TEXT("CreateExpandNode: node '%s' is not a UK2Node_ConstructObjectFromClass subclass — "
                 "class pin configuration skipped"),
            *K2NodeClass->GetName());
    }

    // Place and wire exec
    PlaceNode(Node, InOutExecPin);
    InOutExecPin = FindExecPin(Node, UEdGraphSchema_K2::PN_Then);

    return Node;
}
