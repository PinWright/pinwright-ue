// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirValueResolver.h - Resolves BPIR value references to UEdGraphPins

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UEnum;
class UK2Node_BreakStruct;
class FCodeNodeEmitter;
class FCodePinResolver;
struct FBpirInstruction;
struct FBpirEntryBlock;
struct FEmittedNodeInfo;

// Resolves %name, %name.PinName, $varName, self, and literal values
// to UEdGraphPin* pointers for wiring during the BPIR compile pass.
class FBpirValueResolver
{
public:
    FBpirValueResolver(FCodeNodeEmitter& InNodeEmitter, FCodePinResolver& InPinResolver,
                       UBlueprint* InBlueprint, UEdGraph* InGraph);

    // Main entry point: resolve a value reference string to a pin.
    // Returns nullptr on failure (with error logged).
    UEdGraphPin* ResolveValue(const FString& ValueRef, FBpirEntryBlock& Block);

    // Check if a value reference is a literal (can be set as default value instead of wired)
    static bool IsLiteral(const FString& ValueRef);

    // Extract literal text suitable for SetPinDefaultValue
    static PINWRIGHT_API FString GetLiteralText(const FString& ValueRef);

    // Resolve the enum type behind a literal or %alias value reference that may not emit a pin.
    static UEnum* ResolveEnumTypeFromValueRef(const FString& ValueRef, const FBpirEntryBlock& Block);

    // Parse `cast<T>(inner)` syntax — angle/paren matching with depth, and stripping
    // of the optional `Ident:` keyword prefix inside the parens. Pure string-to-string,
    // no graph state. Returns true on shape match; OutType and OutInner are trimmed.
    static bool ParseCastSyntax(const FString& Expr, FString& OutType, FString& OutInner);

    // Set the graph (when switching between event graph and function graph)
    void SetGraph(UEdGraph* InGraph);

    // Set the emit map for resolving %references to emitted nodes
    void SetEmitMap(const TMap<int32, FEmittedNodeInfo>* InEmitMap) { EmitMap = InEmitMap; }

    // Pre-emit: create VariableGet/Self nodes ahead of the wire pass
    void PreEmitDollarVar(const FString& VarName);
    void PreEmitSelf();
    void PreEmitExternalGet(const FString& TargetName, const FString& PropertyName);

    // Inject an externally-owned pin as a cached $variable (skips VariableGet creation)
    void InjectCachedVariable(const FString& VarName, UEdGraphPin* Pin);

private:
    // Resolution methods for each reference type
    UEdGraphPin* ResolvePercentRef(const FString& Name, FBpirEntryBlock& Block);
    UEdGraphPin* ResolvePercentRefPin(const FString& Name, const FString& PinName, FBpirEntryBlock& Block);
    UEdGraphPin* ResolveDollarVar(const FString& VarName);
    UEdGraphPin* ResolveSelf();

    // Resolve a `cast<T>(inner)` RHS expression to a pure K2Node_DynamicCast result pin.
    // Emits the cast node (SetPurity(true)), wires the recursively-resolved inner pin to
    // the cast source, and returns the cast result pin. Returns nullptr on parse/resolve
    // failure (with error logged).
    UEdGraphPin* ResolveCastExpression(const FString& ValueRef, FBpirEntryBlock& Block);

    // Find an output pin on Node by name (case-insensitive, then space-normalized fallback)
    UEdGraphPin* FindOutputPinByName(UEdGraphNode* Node, const FString& PinName);

    // Find the first non-exec output pin on a node (the data value pin)
    static UEdGraphPin* FindFirstDataOutputPin(UEdGraphNode* Node);

    // Resolve chained property access (e.g., "AsActor.bHidden") through object/struct pins
    UEdGraphPin* ResolveChainedPropertyAccess(UEdGraphNode* Node, const FString& FirstPinName, const TArray<FString>& PropertyChain);

    // Walk a chain of property segments starting from an already-resolved pin.
    // Extracted from ResolveChainedPropertyAccess so PreEmitExternalGet can reuse it
    // after resolving the first segment through its own struct/object branch.
    UEdGraphPin* ResolveChainFromPin(UEdGraphPin* StartPin, TConstArrayView<FString> RemainingChain);

    // Auto-insert a BreakStruct node for a struct-typed pin and return the named member output pin
    UEdGraphPin* ResolveStructMemberThroughPin(UEdGraphPin* StructPin, const FString& MemberName);

    FCodeNodeEmitter& NodeEmitter;
    FCodePinResolver& PinResolver;
    UBlueprint* Blueprint;
    UEdGraph* Graph;
    const TMap<int32, FEmittedNodeInfo>* EmitMap = nullptr;

    // Cache: variable name -> already-created VariableGet node's output pin
    TMap<FString, UEdGraphPin*> VariableGetCache;
    // Cache: variable names known not to be Blueprint members; these fall back to %locals.
    TSet<FString> MissingVariableGetCache;
    // Cache: "target.Property" -> external VariableGet node's output pin
    TMap<FString, UEdGraphPin*> ExternalGetCache;
    // Cache: "nodePtr_pinName_propertyName" -> auto-created VariableGet output pin for chain resolution
    TMap<FString, UEdGraphPin*> AutoPropertyGetCache;
    // Cache: struct-typed source pin -> auto-created BreakStruct or CallFunction node
    TMap<UEdGraphPin*, UEdGraphNode*> AutoBreakStructCache;
    // Cache: self node pin (created once)
    UEdGraphPin* CachedSelfPin = nullptr;

    // Recursion depth for alias chains (`%a = %b` → `%b = %c` → ...) to detect cycles.
    int32 AliasDepth = 0;
};
