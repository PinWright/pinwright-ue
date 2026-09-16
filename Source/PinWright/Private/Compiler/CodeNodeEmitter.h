// Copyright (c) 2026 Alexander Penkin. MIT License.

// CodeNodeEmitter.h - K2 node creation and visual layout engine for Blueprint code compilation

#pragma once
#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UFunction;
class UEnum;
class UScriptStruct;
class UInputAction;
class FObjectProperty;
class UK2Node;
class UK2Node_CallFunction;
class UK2Node_CallParentFunction;
class UK2Node_VariableGet;
class UK2Node_VariableSet;
class UK2Node_Self;
class UK2Node_IfThenElse;
class UK2Node_MacroInstance;
class UK2Node_DynamicCast;
class UK2Node_SwitchEnum;
class UK2Node_SwitchName;
class UK2Node_SwitchInteger;
class UK2Node_SwitchString;
class UK2Node_MakeStruct;
class UK2Node_MakeArray;
class UK2Node_ExecutionSequence;
class UK2Node_Timeline;
class UK2Node_CustomEvent;
class UK2Node_Event;
class UK2Node_FunctionEntry;
class UK2Node_FunctionResult;
class UK2Node_Select;
class UK2Node_BreakStruct;
class UK2Node_InputKey;
class UK2Node_ComponentBoundEvent;
class UK2Node_AsyncAction;
class UK2Node_GetSubsystem;
class UK2Node_FormatText;
class UEdGraphNode_Comment;

namespace BpirCompilerMacroUtils
{
    UEdGraph* FindMacroGraphByName(const TArray<UEdGraph*>& MacroGraphs, FName MacroName, const UEdGraph* ExcludedGraph = nullptr);
    UEdGraph* FindMacroGraphByName(UBlueprint* Blueprint, FName MacroName, const UEdGraph* ExcludedGraph = nullptr);
}

// Creates K2 nodes, wires exec pins, and manages grid-based visual layout within a single graph.
// Pure nodes (no exec pins) are stacked vertically to the left; impure nodes advance the X cursor.
class FCodeNodeEmitter
{
public:
    FCodeNodeEmitter(UBlueprint* InBlueprint, UEdGraph* InGraph);
    void SetGraph(UEdGraph* InGraph);

    // Layout helpers
    void ResetPlacementForChain(UEdGraphPin* StartPin, int32 YOffset = 0);
    void PlaceNode(UEdGraphNode* Node, UEdGraphPin* LastExecPin, int32 XOff = 0, int32 YOff = 0);
    int32 FindFreeYPositionForEventNode();

    // Node creation — each method creates, configures, places the node, and tracks its GUID.
    // InOutExecPin is advanced to the new node's output exec pin after the call.
    // bAsInterfaceMessage spawns UK2Node_Message (the `message` opcode / interface
    // message call) instead of UK2Node_CallFunction. Message derives from CallFunction,
    // so the caller's wiring, return-pin and exec handling are identical.
    UK2Node_CallFunction*       CreateCallFunctionNode(UFunction* Function, UEdGraphPin*& InOutExecPin, bool bAsInterfaceMessage = false);
    UK2Node_CallFunction*       CreateCallParentFunctionNode(UFunction* Function, UEdGraphPin*& InOutExecPin);
    UK2Node_VariableGet*        CreateVariableGetNode(FName VarName);
    UK2Node_VariableSet*        CreateVariableSetNode(FName VarName, UEdGraphPin*& InOutExecPin);
    UK2Node_VariableGet*        CreateExternalVariableGetNode(FName VarName, UClass* TargetClass);
    UK2Node_VariableSet*        CreateExternalVariableSetNode(FName VarName, UClass* TargetClass, UEdGraphPin*& InOutExecPin);
    UK2Node_Self*               CreateSelfNode();
    UK2Node_IfThenElse*         CreateBranchNode(UEdGraphPin*& InOutExecPin);
    UK2Node_MacroInstance*      CreateMacroNode(FName MacroName, UEdGraphPin*& InOutExecPin);
    UK2Node_DynamicCast*        CreateCastNode(UClass* TargetClass, UEdGraphPin*& InOutExecPin);
    UK2Node_SwitchEnum*         CreateSwitchEnumNode(UEnum* Enum, UEdGraphPin*& InOutExecPin);
    UK2Node_SwitchName*         CreateSwitchNameNode(UEdGraphPin*& InOutExecPin);
    UK2Node_SwitchInteger*      CreateSwitchIntegerNode(UEdGraphPin*& InOutExecPin);
    UK2Node_SwitchString*       CreateSwitchStringNode(UEdGraphPin*& InOutExecPin);
    UK2Node_MakeStruct*         CreateMakeStructNode(UScriptStruct* Struct);
    UK2Node_MakeArray*          CreateMakeArrayNode(int32 NumInputs);
    UK2Node_ExecutionSequence*  CreateSequenceNode(int32 NumOutputs, UEdGraphPin*& InOutExecPin);
    UK2Node_Timeline*           CreateTimelineNode(const FString& Name, UEdGraphPin*& InOutExecPin);
    UK2Node_CustomEvent*        CreateCustomEventNode(FName EventName);
    UK2Node_Event*              CreateEventNode(FName EventName);
    UK2Node_FunctionEntry*      GetOrCreateFunctionEntry(UEdGraph* FuncGraph);
    UK2Node_FunctionResult*     GetOrCreateFunctionResult(UEdGraph* FuncGraph);
    UK2Node_FunctionResult*     CreateFunctionResult(UEdGraph* FuncGraph);
    UK2Node_Select*             CreateSelectNode(UEdGraphPin*& InOutExecPin, UEnum* IndexEnum = nullptr);
    UK2Node_BreakStruct*        CreateBreakStructNode(UScriptStruct* Struct);
    UK2Node_InputKey*           CreateInputKeyNode(FName KeyName, bool bReleased);
    UEdGraphNode*               CreateEnhancedInputActionNode(UInputAction* InputAction);
    UK2Node_ComponentBoundEvent* CreateComponentEventNode(FName CompName, FName EventName, UBlueprint* BP);
    UEdGraphNode_Comment*       CreateCommentBox(const FString& Text, const TArray<UEdGraphNode*>& Nodes);
    // ProvidedArgNames is retained for source compatibility with existing callers; async-action
    // factory selection is explicit via K2Node_AsyncAction_<FactoryFunction>.
    UEdGraphNode*               CreateGenericK2Node(const FString& NodeClassName, UEdGraph* Graph, const TArray<FString>& ProvidedArgNames = TArray<FString>(), const FString& ClassPath = FString());
    UK2Node_GetSubsystem*       CreateGetSubsystemNode(UClass* SubsystemClass);
    UK2Node_FormatText*         CreateFormatTextNode(const FString& FormatString, FString* OutError = nullptr);

    // Spawns a UK2Node_AsyncAction pre-configured for FactoryFunc via InitializeProxyFromFunction,
    // allocates its pins, and wires exec in/out. Use when the resolved UFunction is an async-action
    // factory (IsAsyncActionFactory returns true). When NodeClass is non-null, the node is
    // constructed as that subclass (used for HasDedicatedAsyncNode factories whose dedicated
    // UK2Node_AsyncAction_<FactoryFunctionName> subclass produces dynamic delegate/payload pins).
    UK2Node_AsyncAction*        CreateAsyncActionNode(UFunction* FactoryFunc, UEdGraphPin*& InOutExecPin, UClass* NodeClass = nullptr);

    // Returns true when Func is a static BlueprintCallable factory whose return type is a
    // UBlueprintAsyncActionBase subclass and whose owner class is not deprecated or opted out.
    // Mirrors UE's BlueprintActionDatabaseRegistrar::IsFactoryMethod rule plus the
    // HasDedicatedAsyncNode opt-out (owner classes that own a specialized K2Node set this metadata
    // to prevent BPIR from spawning a generic UK2Node_AsyncAction for their factories).
    static bool                 IsAsyncActionFactory(UFunction* Func);
    static UFunction*           ResolveAsyncActionFactoryByName(const FString& FactoryFunctionName, FString* OutError = nullptr);

    // Sibling resolver to ResolveAsyncActionFactoryByName for the HasDedicatedAsyncNode lane.
    // ResolveAsyncActionFactoryByName filters through IsAsyncActionFactory which opts out of
    // HasDedicatedAsyncNode owner classes, so dedicated-subclass factories never surface there.
    // This helper walks UBlueprintAsyncActionBase subclasses that DO carry the HasDedicatedAsyncNode
    // meta and returns the static BlueprintCallable UFunction whose name matches, or nullptr.
    static UFunction*           ResolveDedicatedAsyncActionFactoryByName(const FString& FactoryFunctionName);

    // Sibling probe to IsAsyncActionFactory. Returns the dedicated UK2Node_AsyncAction subclass
    // for a factory whose owner class carries the HasDedicatedAsyncNode UCLASS meta, or nullptr
    // when (a) the factory is not a valid async-action factory, (b) the owner class does not
    // carry the meta, or (c) no matching UK2Node_AsyncAction_<FactoryFunctionName> subclass
    // exists. Resolution is hybrid: FindObject by Epic naming convention first (O(1)), then
    // TObjectIterator filtered to UK2Node_AsyncAction subclasses (O(n)) for plugins that don't
    // follow the convention exactly.
    static UClass*              ResolveDedicatedAsyncActionSubclass(UFunction* FactoryFunc);

    // InputBlueprintNodes is optional and deliberately not linked. These helpers resolve
    // UK2Node_EnhancedInputAction and its InputAction property through reflection.
    static UClass*              ResolveEnhancedInputActionNodeClass();
    static FObjectProperty*     ResolveEnhancedInputActionProperty(const UClass* NodeClass);
    static UInputAction*        GetEnhancedInputAction(const UEdGraphNode* Node);
    static bool                 SetEnhancedInputAction(UEdGraphNode* Node, UInputAction* InputAction);
    static const TArray<FName>& GetEnhancedInputActionEventPinNames();

    // Expand node registry — maps BPIR function names (e.g. "Create", "SpawnActor") to
    // K2Node subclasses that produce typed output pins (e.g. UK2Node_CreateWidget).
    // Returns the K2Node UClass for the given function name, or nullptr if not registered.
    UClass* FindExpandNodeClass(const FString& FunctionName) const;

    // Creates an expand node of the given K2Node class, sets its class selector pin to
    // ClassPath, reconstructs to generate typed pins, wires exec, and returns the node.
    // Returns nullptr on failure.
    UK2Node* CreateExpandNode(UClass* K2NodeClass, const FString& ClassPath, UEdGraphPin*& InOutExecPin);

    // Undo tracking — returns the GUIDs of every node created since construction or last clear
    TArray<FGuid>& GetCreatedNodeGUIDs() { return CreatedNodeGUIDs; }

    // Build comment boxes from a map of label -> node list, using bounding-box padding
    void FinalizeCommentBoxes(TMap<FString, TArray<UEdGraphNode*>>& CommentMap);

private:
    // Assigns a new GUID, calls PostPlacedNewNode/AllocateDefaultPins, adds to graph, tracks GUID
    void InitializeNode(UEdGraphNode* Node);

    // Returns the first exec-output pin matching PinName on Node (searches "then" / "execute" as fallback)
    UEdGraphPin* FindExecPin(UEdGraphNode* Node, const FString& PinName = TEXT("execute"));
    UEdGraphPin* FindExecPin(UEdGraphNode* Node, FName PinName);

    // Returns the cached StandardMacros Blueprint, loading it on first use
    UBlueprint* GetStandardMacrosLibrary();

    // Finds a macro sub-graph by name inside a macro library Blueprint
    UEdGraph* FindMacroGraph(UBlueprint* MacroLib, FName MacroName);

    UBlueprint* Blueprint;
    UEdGraph* Graph;

    // X position for the next impure node in the current execution chain
    int32 CurrentNodeX;

    // Baseline Y for the current chain (set by ResetPlacementForChain)
    int32 CurrentBaseY;

    // Vertical stacking offset for pure (no-exec) nodes placed to the left of the chain
    int32 CurrentPureNodeYOffset;

    // Total nodes created (used for minor Y-jitter avoidance on event roots)
    int32 NodeCount;

    // GUIDs of all nodes created through this emitter, used for undo support
    TArray<FGuid> CreatedNodeGUIDs;

    // Lazily loaded StandardMacros asset reference
    UBlueprint* CachedStandardMacros;

    // Populates ExpandNodeRegistry with known function-name -> K2Node class mappings.
    // Called lazily on first FindExpandNodeClass() call.
    void PopulateExpandNodeRegistry() const;

    // Registry mapping BPIR function names to K2Node classes that produce typed output.
    // Populated lazily via PopulateExpandNodeRegistry(). Mutable because it's a cache.
    mutable TMap<FName, UClass*> ExpandNodeRegistry;
    mutable bool bExpandNodeRegistryPopulated = false;
};
