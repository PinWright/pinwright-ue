// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirTextEmitter.h - Converts individual Blueprint nodes into BPIR text lines

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;
class UEdGraphPin;
class UEnum;
class UBlueprint;
class UK2Node_CallFunction;
class UK2Node_VariableSet;
class UK2Node_VariableGet;
class UK2Node_DynamicCast;
class UK2Node_IfThenElse;
class UK2Node_MacroInstance;
class UK2Node_Timeline;
class UK2Node_FunctionResult;
class UK2Node_MakeStruct;
class UK2Node_BreakStruct;
class FGraphWalker;

/**
 * Decompiler-side LabelMap value: maps an exec output pin name (key) to the
 * allocated target label and, optionally, the target node's input exec pin name.
 * TargetInputPinName is empty when the target has fewer than two input exec pins
 * (the dominant case); it's populated only when disambiguation is needed for
 * multi-input-exec targets like Gate, MultiGate, DoOnce, or user macros with
 * multiple entry tunnels.
 */
struct FBpirEmitTarget
{
    FString Label;
    FString TargetInputPinName;
};

using FBpirLabelMap = TMap<FString, FBpirEmitTarget>;

/**
 * Emits BPIR text for individual Blueprint nodes.
 *
 * All methods return formatted BPIR instruction strings. The caller is
 * responsible for indentation and line assembly. Value references (%name)
 * and label references (@label) are resolved by the caller and passed in.
 *
 * LabelMap: maps exec output pin names to allocated label names for
 * multi-output nodes (branch, switch, loop, etc.)
 */
class PINWRIGHT_API FBpirTextEmitter
{
public:
    FBpirTextEmitter();

    static FString NormalizeEventName(const FString& RawName);

    // Signature EmitEntrySignature falls back to when the grammar has no form for the
    // entry node (AnimGraph output-pose roots and other engine compile-root nodes reach
    // the decompiler through the class-agnostic root-set backstop). Callers that must not
    // render anonymous stubs compare the emitted signature against this.
    static const TCHAR* const UnknownEntrySignature;

    // Entry signatures. SourceGraphName and bIsInterfaceGraph describe the un-cloned
    // source graph — the entry node may live on a composite-flattening clone whose
    // auto-suffixed name and ownership must not leak into the signature.
    FString EmitEntrySignature(
        UEdGraphNode* EntryNode,
        const UBlueprint* BlueprintContext,
        const FString& SourceGraphName,
        bool bIsInterfaceGraph);

    // Entry-level decorator lines: 0, 1, or 2 lines (`@meta(...)` and/or `@flags(...)`)
    // that go immediately above the `entry …` signature. Empty array when the entry
    // carries only default-valued metadata. Order is fixed: @meta first, @flags second.
    // Engine event entries (UK2Node_Event) and component/input events return an empty
    // array — they carry no user-editable metadata.
    TArray<FString> EmitEntryDecoratorLines(UEdGraphNode* EntryNode);

    // Returns the optional bare BPIR enabled-state marker for a graph node
    // (`disabled` or `devonly`), or an empty string for the default
    // enabled state. The caller owns spacing around the returned token.
    static FString GetNodeEnabledStateMarker(const UEdGraphNode* Node);

    // Impure function call: "call FuncName(Pin: %ref, ...)" or "%name = call FuncName(...)"
    FString EmitCallNode(
        UEdGraphNode* Node,
        const FString& ResultName,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Event dispatcher: "call_dispatcher Name(args)", "bind_dispatcher Name(args)", etc.
    FString EmitDispatcherNode(
        UEdGraphNode* Node,
        const FString& ValueName,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Branch: "%b = branch(%condRef) [true -> @then, false -> @else]"
    FString EmitBranch(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Switch: "%sw = switch(%selRef) [CaseA -> @case_A, ...]"
    FString EmitSwitch(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // ForEach / ForEachBreak / While loop
    FString EmitLoop(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Sequence: "%seq = sequence(N) [0 -> @s0, 1 -> @s1, ...]"
    FString EmitSequence(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap);

    // Variable set: "set VarName = %valueRef"
    FString EmitVariableSet(
        UEdGraphNode* Node,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Variable get: "%v = get VarName" (rarely needed; usually inline $VarName)
    // LabelMap is empty for pure gets; non-empty for validated (exec) gets, producing
    // "%v = get VarName [then -> @then]".
    FString EmitVariableGet(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap);

    // Macro: DoOnce, FlipFlop, Gate, MultiGate
    FString EmitMacro(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Latent: "%d = latent Delay(Duration: 2.0) [completed -> @after]"
    FString EmitLatent(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Cast: "%c = cast<ClassName>(%objRef) [success -> @ok, fail -> @nope]"
    FString EmitCast(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Return: "return %valueRef" or "return"
    FString EmitReturn(
        UEdGraphNode* Node,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Macro return: "return [ExitPin] (PinName: %valueRef, ...)" for exit tunnel
    FString EmitMacroReturn(
        UEdGraphNode* Node,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin,
        const FString& ExitPinName = FString());

    // Struct make: "%v = make<StructName>(X: %ref, Y: %ref)"
    FString EmitMakeStruct(
        UEdGraphNode* Node,
        const FString& ResultName,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Struct break: "%b = break<StructName>(%structRef)"
    FString EmitBreakStruct(
        UEdGraphNode* Node,
        const FString& ResultName,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Timeline: "%tl = timeline TimelineName(...) [update -> @update, finished -> @finished]"
    FString EmitTimeline(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap);

    // Pure function call: "%name = call FuncName(Pin: %ref, ...)"
    FString EmitPureNode(
        UEdGraphNode* Node,
        const FString& ResultName,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Generic node fallback: "call ClassName(args)" for any UEdGraphNode
    FString EmitGenericNode(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Configured async action: "call K2Node_AsyncAction_<FactoryFunction>(args)"
    FString EmitAsyncActionNode(
        UEdGraphNode* Node,
        const FString& ResultName,
        const FBpirLabelMap& LabelMap,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    // Unknown node: "# [UNKNOWN] ClassName: NodeTitle"
    FString EmitUnknownNode(UEdGraphNode* Node);

    // Convert a UE pin type to a BPIR type string
    static FString PinTypeToBpirType(const struct FEdGraphPinType& PinType);

    // Returns ": <BpirType>" for the node's primary output data pin, or empty
    // when the node has no value-producing output (exec-only / delegate / hidden).
    // Spliced after %ResultName at every "%name = ..." binding site so readers can
    // see the register's type without walking to the callee asset.
    static FString ResolvePrimaryOutputTypeAnnotation(UEdGraphNode* Node);

    // Get the clean variable name from a VariableGet or VariableSet node
    static FString GetVariableName(UEdGraphNode* Node);

    // Get a display-friendly function name from a call node
    static FString GetFunctionDisplayName(UK2Node_CallFunction* Node);

    // Format exec target clauses: " [pin -> @label, ...]" (public for round-trip tests).
    FString FormatExecTargets(const FBpirLabelMap& LabelMap);

    // Format enum switch clauses with qualified enum case labels (public for round-trip tests).
    FString FormatEnumExecTargets(const FBpirLabelMap& LabelMap, UEnum* EnumType);

    // Consume the number of generated prefix instructions produced by the last emission.
    // The decompiler uses this metadata to position only synthesized helpers.
    int32 ConsumeGeneratedPrefixLineCount();

private:
    // Collect data input args as "PinName: ValueRef" pairs.
    // PinNamesToSkip: if non-null, any pin whose PinName is in the set is excluded.
    FString FormatArgs(
        UEdGraphNode* Node,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin,
        bool bSkipSelfPin = true,
        const TSet<FName>* PinNamesToSkip = nullptr,
        FString* OutGeneratedPrefix = nullptr);

    // Format the target prefix(es) for member function calls. Returns one
    // "Target: <ref>" string per linked source pin. Empty array when the node
    // has no connected self pin or the resolved target is implicit self. The
    // single-self case always returns at most one element; the multi-element
    // case occurs only when the node permits multi-self fan-out
    // (UK2Node_CallFunction::AllowMultipleSelfs(false) == true, or
    // UK2Node_BaseMCDelegate-derived dispatcher nodes).
    TArray<FString> FormatTargetPrefix(
        UEdGraphNode* Node,
        const TFunction<FString(UEdGraphPin*)>& ResolvePin);

    int32 LastGeneratedPrefixLineCount = 0;
};

namespace BpirTextEmitterInternal
{
    // Exposed via this namespace so test code can reach the production
    // implementation directly instead of duplicating the predicate.
    PINWRIGHT_API bool IsPinOmittableAtCallSite(const UEdGraphPin* Pin);
}
