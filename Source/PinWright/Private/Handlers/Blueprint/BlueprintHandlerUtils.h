// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintHandlerUtils.h - Shared utilities for Blueprint handler files
// Extracted from PinWright_BlueprintHandlers.cpp
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"

// These headers are included here because 14+ consumer files rely on
// BlueprintHandlerUtils.h transitively providing them.
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
// Full include - FLiveInstanceSurvey appears by value on FBlueprintCompileDiagnostics below.
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"

// Forward declarations
class UEdGraph;
class UEdGraphNode;
class UBlueprint;
class UK2Node_Tunnel;

// Full include — FBpirTypeSpec now appears by value in callers' locals
// (parse result scratch) and in the AddUserDefinedPin / BuildNamedPinDescriptor
// signatures below.
#include "Compiler/BpirTypeSpec.h"

// K2Node headers — consumer files depend on full type definitions
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_Literal.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"

// Pin category schema header (lives in BlueprintGraph/Classes/, which UHT auto-resolves)
#include "EdGraphSchema_K2.h"

// ---------------------------------------------------------------------------
// Shared utilities for blueprint handler files - declarations
// ---------------------------------------------------------------------------

namespace BlueprintHandlerUtils
{

struct FBlueprintCompileDiagnostics
{
    bool bCompiled = false;
    FString Status;
    TArray<FString> Errors;
    TArray<FString> Warnings;
    // The live instances this compile destroyed and re-created, measured BEFORE the
    // compile ran (afterwards the old objects are gone and the count reads zero).
    // Empty for the common case; AddCompileDiagnosticsToJson emits it only when it is
    // not, so an untouched compile keeps its previous response shape.
    // Why every compile verb reports it: Handlers/Blueprint/BlueprintReinstancingGuard.h.
    BlueprintReinstancingGuard::FLiveInstanceSurvey Reinstanced;
};

struct FNamedPinTypeDescriptor
{
    FString Name;
    FEdGraphPinType Type;
};

enum class EParsedPinParamMode : uint8
{
    AllowWildcardFallback,
    Strict
};

struct FParsedPinParam
{
    FString Name;
    FString OriginalName;
    FString OriginalType;
    FBpirTypeSpec Spec;
    bool bParseOk = false;
    FString ParseErr;
    int32 ParseErrCol = INDEX_NONE;
};

struct FBlueprintOverrideInfo
{
    UFunction* Function = nullptr;
    UClass* OverrideClass = nullptr;
    bool bCanPlaceAsEvent = false;

    bool IsValid() const
    {
        return Function != nullptr && OverrideClass != nullptr;
    }
};

struct FBlueprintOrphanNodeInfo
{
    FGuid NodeGuid;
    FString NodeType;
    FString Title;
    FString SourceGraphName;
    bool bHasExecPins = false;
    TWeakObjectPtr<UEdGraphNode> WeakNodePtr;
};

// Graph-qualified identity keeps equal NodeGuids in separate authored subgraphs distinct.
struct FBlueprintOrphanNodeKey
{
    const UEdGraph* Graph = nullptr;
    FGuid NodeGuid;

    bool operator==(const FBlueprintOrphanNodeKey& Other) const
    {
        return Graph == Other.Graph && NodeGuid == Other.NodeGuid;
    }
};

FORCEINLINE uint32 GetTypeHash(const FBlueprintOrphanNodeKey& Key)
{
    return HashCombine(PointerHash(Key.Graph), GetTypeHash(Key.NodeGuid));
}

struct FBlueprintOrphanNodeShape
{
    bool bHasExecPins = false;
    bool bHasLinkedDataOutput = false;
};

// Authored graph-family orphan analysis shared by the finder and BPIR warning pass. The
// exec set is the canonical seed/BFS result; the data set is that same result extended
// backward over non-exec inputs for includeDataOnly callers. StandalonePureNodes makes
// BPIR's round-trip preservation exception explicit instead of re-deriving it in BPIR.
struct FBlueprintOrphanReachability
{
    TSet<FBlueprintOrphanNodeKey> ExecReachableNodes;
    TSet<FBlueprintOrphanNodeKey> DataReachableNodes;
    TSet<FBlueprintOrphanNodeKey> StandalonePureNodes;
    TMap<FBlueprintOrphanNodeKey, FBlueprintOrphanNodeShape> NodeShapes;

    // Structural graph nodes are never orphan candidates. Knots are data/exec connectors,
    // so their candidacy is controlled by includeDataOnly and the reachability sets below.
    bool IsStructuralNode(UEdGraphNode* Node) const;

    // Authoritative candidate + reachability decision shared by finder, deletion, and BPIR.
    bool IsOrphan(UEdGraphNode* Node, bool bIncludeDataOnly) const;

    bool HasExecPins(const UEdGraphNode* Node) const
    {
        if (!Node || !Node->NodeGuid.IsValid())
        {
            return false;
        }
        const FBlueprintOrphanNodeKey Key{Node->GetGraph(), Node->NodeGuid};
        if (const FBlueprintOrphanNodeShape* Shape = NodeShapes.Find(Key))
        {
            return Shape->bHasExecPins;
        }
        return false;
    }

    bool IsExecReachable(const UEdGraphNode* Node) const
    {
        if (!Node || !Node->NodeGuid.IsValid())
        {
            return false;
        }
        const FBlueprintOrphanNodeKey Key{Node->GetGraph(), Node->NodeGuid};
        return ExecReachableNodes.Contains(Key);
    }

    bool IsDataReachable(const UEdGraphNode* Node) const
    {
        if (!Node || !Node->NodeGuid.IsValid())
        {
            return false;
        }
        const FBlueprintOrphanNodeKey Key{Node->GetGraph(), Node->NodeGuid};
        return DataReachableNodes.Contains(Key);
    }

    bool IsStandalonePure(const UEdGraphNode* Node) const
    {
        if (!Node || !Node->NodeGuid.IsValid())
        {
            return false;
        }
        const FBlueprintOrphanNodeKey Key{Node->GetGraph(), Node->NodeGuid};
        return StandalonePureNodes.Contains(Key);
    }
};

struct FBlueprintOrphanDeltaCleanupResult
{
    bool bCleanupNewOrphans = true;
    TArray<FBlueprintOrphanNodeInfo> NewOrphans;
    TArray<FBlueprintOrphanNodeInfo> DeletedNewOrphans;
};

struct FBlueprintGraphTargetParts
{
    FString ClassName;
    FString MemberName;
};

enum class EBlueprintPathParamAliasSet : uint8
{
    ResolveBlueprintPath,
    ResolveExplicitBlueprintPath
};

struct FBlueprintPathScalarFieldName
{
    const TCHAR* Name;
    uint8 ResolveBlueprintPathOrder;
    uint8 ResolveExplicitBlueprintPathOrder;
};

template <typename TVisitor>
inline bool VisitBlueprintPathScalarFieldNames(
    EBlueprintPathParamAliasSet AliasSet,
    TVisitor&& Visitor)
{
    static const FBlueprintPathScalarFieldName FieldNames[] = {
        { TEXT("path"), 2, 1 },
        { TEXT("assetPath"), 6, 5 },
        { TEXT("blueprintPath"), 4, 2 },
        { TEXT("blueprint_path"), 5, 3 },
        { TEXT("requestedPath"), 1, 4 },
        { TEXT("name"), 3, 0 }
    };

    const uint8 MaxOrder = AliasSet == EBlueprintPathParamAliasSet::ResolveBlueprintPath ? 6 : 5;
    for (uint8 Order = 1; Order <= MaxOrder; ++Order)
    {
        for (const FBlueprintPathScalarFieldName& FieldName : FieldNames)
        {
            const uint8 FieldOrder = AliasSet == EBlueprintPathParamAliasSet::ResolveBlueprintPath
                ? FieldName.ResolveBlueprintPathOrder
                : FieldName.ResolveExplicitBlueprintPathOrder;
            if (FieldOrder == Order && !Visitor(FieldName.Name))
            {
                return false;
            }
        }
    }
    return true;
}

template <typename TVisitor>
inline bool VisitBlueprintPathCandidateArrayFieldNames(TVisitor&& Visitor)
{
    static const TCHAR* const FieldNames[] = {
        TEXT("blueprintCandidates"),
        TEXT("candidates")
    };
    for (const TCHAR* FieldName : FieldNames)
    {
        if (!Visitor(FieldName))
        {
            return false;
        }
    }
    return true;
}

inline TArray<FString> MakeBlueprintPathParamAliases(
    const TCHAR* CanonicalName,
    EBlueprintPathParamAliasSet AliasSet = EBlueprintPathParamAliasSet::ResolveBlueprintPath)
{
    TArray<FString> Aliases;
    VisitBlueprintPathScalarFieldNames(AliasSet,
        [&Aliases, Canonical = FString(CanonicalName)](const TCHAR* AliasName)
        {
            if (!Canonical.Equals(AliasName))
            {
                Aliases.Add(AliasName);
            }
            return true;
        });
    return Aliases;
}

inline TArray<FParamAliasSpec> MakeBlueprintPathParamTypedAliases(
    EBlueprintPathParamAliasSet AliasSet = EBlueprintPathParamAliasSet::ResolveBlueprintPath)
{
    TArray<FParamAliasSpec> Aliases;
    if (AliasSet != EBlueprintPathParamAliasSet::ResolveBlueprintPath)
    {
        return Aliases;
    }

    VisitBlueprintPathCandidateArrayFieldNames(
        [&Aliases](const TCHAR* AliasName)
        {
            Aliases.Add(FParamAliasSpec{
                FString(AliasName),
                TEXT("array"),
                TEXT("Array of Blueprint asset path candidates; the first resolvable string is used.")
            });
            return true;
        });
    return Aliases;
}

inline FParamSpec BlueprintPathParamReq(
    const TCHAR* Name,
    const TCHAR* Type,
    const TCHAR* Desc,
    EBlueprintPathParamAliasSet AliasSet = EBlueprintPathParamAliasSet::ResolveBlueprintPath)
{
    FParamSpec Spec{FString(Name), FString(Type), FString(Desc), true, TEXT("")};
    Spec.Aliases = MakeBlueprintPathParamAliases(Name, AliasSet);
    Spec.TypedAliases = MakeBlueprintPathParamTypedAliases(AliasSet);
    return Spec;
}

inline FParamSpec BlueprintPathParamOpt(
    const TCHAR* Name,
    const TCHAR* Type,
    const TCHAR* Desc,
    EBlueprintPathParamAliasSet AliasSet = EBlueprintPathParamAliasSet::ResolveBlueprintPath)
{
    FParamSpec Spec{FString(Name), FString(Type), FString(Desc), false, TEXT("")};
    Spec.Aliases = MakeBlueprintPathParamAliases(Name, AliasSet);
    Spec.TypedAliases = MakeBlueprintPathParamTypedAliases(AliasSet);
    return Spec;
}

// Engine bug workaround (UE K2Node_VariableSet.cpp:450 — LOCTEXT(...).ToString() skips
// FText::Format so {VariableName} stays literal). Detects that exact broken shape and
// splices the identifier from the trailing "Set <id>" suffix into the leading sentence.
// Returns the input unchanged for any non-matching string. Exposed for direct testing.
PINWRIGHT_API FString RepairUnableToSetReadOnlyMessage(const FString& In);

FString JsonValueToString(const TSharedPtr<FJsonValue>& Value);
FName ResolveMetadataKey(const FString& RawKey);
FString DescribePinType(const FEdGraphPinType& PinType);
void AppendPinsJson(const TArray<TSharedPtr<FUserPinInfo>>& Pins, TArray<TSharedPtr<FJsonValue>>& Out);
PINWRIGHT_API FString ResolveExplicitBlueprintPath(const TSharedPtr<FJsonObject>& Payload, bool bNormalize = true);
PINWRIGHT_API bool ValidateNamedTypePinParamElements(
    const TArray<TSharedPtr<FJsonValue>>& In,
    const TCHAR* ParamName,
    FString& OutError);
PINWRIGHT_API bool ParseNamedTypePinParams(
    const TArray<TSharedPtr<FJsonValue>>& In,
    TArray<FParsedPinParam>& Out,
    EParsedPinParamMode Mode,
    FString& OutError,
    const TCHAR* StrictParamLabel);
PINWRIGHT_API FString StripEnumScope(const FString& InName);
PINWRIGHT_API bool TryResolveEnumLiteralToValue(const UEnum* EnumType, const FString& InLiteral, int64& OutValue);
PINWRIGHT_API bool TryApplyEnumPinDefaultValue(
    const UEdGraphSchema* Schema,
    UEdGraphPin* Pin,
    const FString& RequestedValue,
    FString& OutAppliedLiteral,
    FString& OutErrorCode,
    FString& OutErrorMessage);
PINWRIGHT_API FBlueprintCompileDiagnostics CompileBlueprintWithDiagnostics(UBlueprint* Blueprint);
// True iff PostErrors holds a compile-error occurrence beyond what BaselineErrors
// already carried — i.e. THIS mutation introduced a NEW error rather than merely
// leaving pre-existing errors (from graphs it never touched) in place. Powers
// compile_bpir's opt-in allowPreexistingErrors mode: an otherwise-valid placement
// is kept only when it adds no new errors versus the pre-placement baseline.
// Exact-string MULTISET comparison — an error is NEW when it appears in PostErrors
// more times than in BaselineErrors, so an added duplicate of a generic message is
// caught rather than masked. The engine's compile-error text is deterministic for a
// given broken node, so an unchanged pre-existing error matches verbatim.
PINWRIGHT_API bool HasNewCompileErrorsBeyondBaseline(
    const TArray<FString>& BaselineErrors,
    const TArray<FString>& PostErrors);
PINWRIGHT_API void AddCompileDiagnosticsToJson(
    const FBlueprintCompileDiagnostics& Diagnostics,
    const TSharedPtr<FJsonObject>& Out,
    const TCHAR* ErrorsFieldName = TEXT("errors"),
    const TCHAR* WarningsFieldName = TEXT("warnings"));
PINWRIGHT_API bool TryResolveBlueprintOverride(
    UBlueprint* Blueprint,
    const FString& FunctionName,
    FBlueprintOverrideInfo& OutInfo,
    FString& OutErrorMessage);
PINWRIGHT_API UEdGraph* FindFunctionGraphByName(
    UBlueprint* Blueprint,
    const FString& FunctionName,
    bool* bOutIsInterfaceOwned = nullptr);
// Builds a typed pin descriptor from a parsed FBpirTypeSpec. Callers parse raw
// text via BpirTypeSpecParser::ParseTypeSpec first.
PINWRIGHT_API bool BuildNamedPinDescriptor(
    const FString& Name,
    const FBpirTypeSpec& TypeSpec,
    FNamedPinTypeDescriptor& OutDescriptor);
PINWRIGHT_API bool ArePinTypesEquivalent(
    const FEdGraphPinType& A,
    const FEdGraphPinType& B);
PINWRIGHT_API bool GetFunctionSignatureDescriptors(
    UFunction* Function,
    TArray<FNamedPinTypeDescriptor>& OutInputs,
    TArray<FNamedPinTypeDescriptor>& OutOutputs,
    FString& OutErrorMessage);
PINWRIGHT_API bool DoPinTypeDescriptorsMatch(
    const TArray<FNamedPinTypeDescriptor>& ExpectedInputs,
    const TArray<FNamedPinTypeDescriptor>& ExpectedOutputs,
    const TArray<FNamedPinTypeDescriptor>& ActualInputs,
    const TArray<FNamedPinTypeDescriptor>& ActualOutputs,
    FString& OutMismatchMessage);
PINWRIGHT_API FBlueprintGraphTargetParts ParseGraphTargetSpec(const FString& TargetSpec);
PINWRIGHT_API bool ResolveGraphVariableTarget(
    UBlueprint* Blueprint,
    const FBlueprintGraphTargetParts& TargetParts,
    UClass* InferredOwnerClass,
    UClass*& OutOwnerClass,
    FName& OutVariableName,
    FString& OutErrorCode,
    FString& OutErrorMessage);
PINWRIGHT_API bool DoesGraphVariableExist(
    UBlueprint* Blueprint,
    UClass* OwnerClass,
    FName VariableName);
PINWRIGHT_API UFunction* ResolveGraphCallableFunction(
    UBlueprint* Blueprint,
    const FBlueprintGraphTargetParts& TargetParts,
    UClass* PreferredClass,
    FString& OutErrorCode,
    FString& OutErrorMessage);

FProperty* FindBlueprintProperty(UBlueprint* Blueprint, const FString& PropertyName);

bool CollectVariableMetadata(const UBlueprint* Blueprint, const FBPVariableDescription& VarDesc, TSharedPtr<FJsonObject>& OutMetadata);
TSharedPtr<FJsonObject> BuildVariableJson(const UBlueprint* Blueprint, const FBPVariableDescription& VarDesc);
TArray<TSharedPtr<FJsonValue>> CollectBlueprintVariables(UBlueprint* Blueprint);
TArray<TSharedPtr<FJsonValue>> CollectBlueprintFunctions(UBlueprint* Blueprint);
void CollectEventPins(UK2Node* Node, TArray<TSharedPtr<FJsonValue>>& Out);
TArray<TSharedPtr<FJsonValue>> CollectBlueprintEvents(UBlueprint* Blueprint);
TSharedPtr<FJsonObject> FindNamedEntry(const TArray<TSharedPtr<FJsonValue>>& Array, const FString& FieldName, const FString& DesiredValue);
TSharedPtr<FJsonObject> EnsureBlueprintEntry(const FString& Key);

// BFS-collect all exec-downstream nodes from RootNodes (the roots themselves are
// included). Used by event / custom-event / function removal paths to gather the
// full chain of nodes that should be deleted together.
PINWRIGHT_API void CollectExecChainNodes(
    const TArray<UEdGraphNode*>& RootNodes,
    TSet<UEdGraphNode*>& OutNodes);

// Cascade-delete UK2Node_CreateDelegate nodes whose SelectedFunctionName matches
// any name in RemovedFunctionNames. Call AFTER removing the source event/function
// nodes. Returns the number of CreateDelegate nodes deleted.
PINWRIGHT_API int32 CascadeRemoveStaleCreateDelegates(
    UBlueprint* Blueprint,
    const TSet<FName>& RemovedFunctionNames);

// Scrub orphan UFunction entries from GeneratedClass + SkeletonGeneratedClass
// whose FName matches one in WipedFunctionNames. Call AFTER removing the
// corresponding K2Node_CustomEvent / K2Node_ComponentBoundEvent / function graph,
// and BEFORE the post-BPIR full compile runs.
//
// Why this is necessary: FKismetEditorUtilities::CompileBlueprint with
// RegenerateSkeletonOnly skips CleanAndSanitizeClass, which is the only engine
// pass that clears Class->Children / FuncMap. Orphan UFunctions otherwise survive
// on the class through the save path and crash TFieldIterator<UFunction> on
// cold reload (see B-bp-saved-state-corruption-mcp-edits P0-10).
//
// Scoped to WipedFunctionNames only — does NOT wipe user-authored functions like
// FBlueprintEditorUtils::RemoveStaleFunctions does.
//
// Returns number of UFunction entries scrubbed (sum across both classes).
PINWRIGHT_API int32 ScrubStaleUFunctionsFromClass(
    UBlueprint* Blueprint,
    const TSet<FName>& WipedFunctionNames);

// One record per integrity failure found by ValidateBlueprintGraphIntegrity.
struct FBlueprintIntegrityFailure
{
    FString NodeGuid;
    FString NodeKind;
    FString GraphName;
    FString Reason;
};

PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildBlueprintIntegrityFailuresJson(
    const TArray<FBlueprintIntegrityFailure>& Failures);

// Walk every ubergraph + function graph, plus WidgetTree panel slots for Widget
// Blueprints, and check for latent saved-state corruption that the in-process
// compiler misses.
//
// Checks performed:
//   1. UK2Node_CreateDelegate: IsValid(&msg, /*bDontUseSkeletalClassForSelf=*/true)
//      Resolves SelectedFunctionName against the generated class (reload-time scope),
//      not the SKEL class (in-process scope). This is the primary gate for the
//      `bind_dispatcher` / `Create_Event` corruption pattern.
//   2. UK2Node_ComponentBoundEvent: ComponentPropertyName must resolve to an
//      FObjectProperty on GeneratedClass, and DelegatePropertyName must resolve
//      to an FMulticastDelegateProperty on that component type.
//   3. Every UEdGraphPin::LinkedTo[i]: the linked pin must be non-null, its owning
//      node must be non-null and live in this blueprint's graph set, and the
//      linked pin must still be present in that node's Pins array.
//
//   4. UWidgetBlueprint panel slots: every slot must be non-null, have Content,
//      have Parent equal to its panel, and Content->Slot must point back to it.
//
// Returns true iff all checks pass. On failure, populates OutFailures (appended,
// does not clear).
PINWRIGHT_API bool ValidateBlueprintGraphIntegrity(
    UBlueprint* Blueprint,
    TArray<FBlueprintIntegrityFailure>& OutFailures);

// Returns the function name a CreateDelegate node would store in SelectedFunctionName
// if it referenced this node. NAME_None for unsupported node types. Capture this
// BEFORE calling FBlueprintEditorUtils::RemoveNode — afterwards Node may be invalid.
PINWRIGHT_API FName GetEffectiveFunctionNameForRemoval(UEdGraphNode* Node);

// Re-resolve BPIR-emitted UK2Node_CreateDelegate nodes against the post-full-compile
// GeneratedClass. The BPIR emit pass calls HandleAnyChange during Phase 2 which runs
// against SkeletonGeneratedClass; the subsequent full compile regenerates custom-event
// UFunction state and may leave the node's SelectedFunctionGuid stale relative to the
// canonical GUID walked by FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName.
// The P0-5 stale-GUID branch in the integrity gate then rejects a semantically-valid
// binding. Clear+rewrite each CreateDelegate node's GUID via HandleAnyChange after the
// compile so the gate sees a consistent post-compile snapshot.
//
// Scope is narrowed to node GUIDs produced by the current BPIR transaction — pre-existing
// CreateDelegate nodes are intentionally untouched so genuine stale references elsewhere
// in the graph still trip the gate.
PINWRIGHT_API void RefreshBpirDelegateNodes(
    UBlueprint* Blueprint,
    const TArray<FGuid>& CreatedNodeGUIDs);

TSharedPtr<FJsonObject> BuildBlueprintSnapshot(UBlueprint* Blueprint, const FString& NormalizedPath);

// Parse a BPIR type string and convert to a pin type. On parse or resolution
// failure, returns a PC_Wildcard-typed pin so the graph schema can retype later.
// Returns true on full success; false when the wildcard fallback was used.
PINWRIGHT_API bool MakePinTypeFromBpirText(FStringView Source, FEdGraphPinType& OutPinType);

// Add a user-defined pin to a function entry / result / custom-event / macro tunnel node.
// Takes a parsed FBpirTypeSpec; on conversion miss the pin is created with
// PC_Wildcard so the graph schema can retype it later.
bool AddUserDefinedPin(UK2Node* Node, const FString& PinName, const FBpirTypeSpec& TypeSpec, EEdGraphPinDirection Direction);

// Return the first UK2Node_FunctionEntry in Graph->Nodes (a function/delegate-signature
// graph's single entry node), or nullptr if Graph is null or has none. Shared home for the
// "find the function-entry node" walk that otherwise recurs inline across Blueprint graph
// handlers. (Named FindGraphFunctionEntryNode rather than FindFunctionEntryNode so it never
// forms an ambiguous overload set with the identically-signatured file-static
// FindFunctionEntryNode in BpirCompiler.cpp when a Unity blob leaks a `using namespace
// BlueprintHandlerUtils` forward into that translation unit.)
PINWRIGHT_API UK2Node_FunctionEntry* FindGraphFunctionEntryNode(UEdGraph* Graph);

// Outcome of AddDispatcherWithSignatureGraph, mapped by callers onto their own error
// codes (blueprint.add_dispatcher distinguishes all three failure modes;
// interaction.add_interaction_events only needs Success vs. not).
enum class EAddDispatcherResult : uint8
{
    Success,
    MemberExists,     // a member variable of that name already exists
    GraphUnavailable, // the signature graph / schema / function-entry node could not be created
    ParamFailed       // a delegate signature parameter pin failed to add
};

// Create an Event Dispatcher (multicast-delegate) member variable AND its backing
// delegate signature graph on Blueprint, so the resulting FMulticastDelegateProperty
// carries a real <Name>__DelegateSignature UFunction. A dispatcher created with only the
// PC_MCDelegate member variable (no signature graph) compiles with a null SignatureFunction
// — the engine warns "No SignatureFunction in MulticastDelegateProperty '<name>'" and the
// event cannot be bound. This is the shared recipe behind blueprint.add_dispatcher and
// interaction.add_interaction_events: add the member variable, create + configure the
// signature graph, author SignatureParams onto its function-entry node as output pins, and
// register the graph in Blueprint->DelegateSignatureGraphs. Does NOT compile or save the
// Blueprint — the caller controls that. On any failure the partial member variable is rolled
// back so the Blueprint is left unchanged. When non-null, *OutSignatureGraph receives the
// graph on Success and *OutFailedParamName receives the offending parameter name on ParamFailed.
PINWRIGHT_API EAddDispatcherResult AddDispatcherWithSignatureGraph(
    UBlueprint* Blueprint,
    FName DispatcherName,
    const TArray<FParsedPinParam>& SignatureParams,
    UEdGraph** OutSignatureGraph = nullptr,
    FString* OutFailedParamName = nullptr);

// Read a {name,type} pin-param array field from a raw request payload, returning an
// empty array when the field is absent, non-array, or empty. Shared by the
// add_function / create_rpc_function readers so the extraction idiom stays one copy.
PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> ReadPinParamArrayField(
    const TSharedPtr<FJsonObject>& Payload,
    const TCHAR* FieldName);

// Single source of truth for the "Accepted forms: primitives ... wrappers (...)"
// type-vocabulary tail shared by every TYPE_NOT_FOUND message (blueprint.add_variable,
// blueprint.add_function, networking.create_rpc_function). Append it to a "Could not
// resolve <label> '<name>'..." message so all handlers list the same accepted forms.
PINWRIGHT_API const TCHAR* GetAcceptedPinTypeFormsText();

// True iff a successfully-parsed pin param resolves to a concrete (non-wildcard) pin
// type — i.e. BuildNamedPinDescriptor succeeds on its spec. Shared by Strict-mode
// parsing and the wildcard-fallback scan so the "does this resolve?" rule lives once.
// Assumes P.bParseOk; callers handle the parse-miss case separately.
PINWRIGHT_API bool ResolvesToConcretePin(const FParsedPinParam& Param);

// Scan parsed pin params for the first token that would silently degrade to a
// wildcard pin — either a parse miss (bParseOk==false, e.g. the documented-but-
// unparseable 'class:/Script/X.Y' form) or a token that parses yet resolves to no
// UClass/UEnum/UScriptStruct (ResolvesToConcretePin returns false). On the first
// such token, fills OutErrorMessage with a TYPE_NOT_FOUND-style message listing the
// accepted forms (mirroring blueprint.add_variable's loud rejection) and returns
// true. Returns false when every param resolves to a concrete pin type. ParamLabel
// names the role in the message (e.g. TEXT("input"), TEXT("output")).
// Lets the create_rpc_function / add_function input paths reject an unresolved type
// at the call site instead of returning success with a wildcard pin that only fails
// at a later blueprint.compile (B-rpc-input-class-path-silent-wildcard).
PINWRIGHT_API bool FindFirstPinParamWildcardFallback(
    const TArray<FParsedPinParam>& Params,
    const TCHAR* ParamLabel,
    FString& OutErrorMessage);

// Reject the first input/output token that would silently degrade to a wildcard pin:
// runs FindFirstPinParamWildcardFallback over Inputs then Outputs and, on the first
// hit, sends a TYPE_NOT_FOUND error through Ctx and returns true (the caller should
// then `return true`). Returns false when every param resolves to a concrete pin.
// Collapses the duplicated post-parse rejection idiom shared by blueprint.add_function
// and networking.create_rpc_function into one call site.
PINWRIGHT_API bool RejectWildcardPinParams(
    FHandlerContext& Ctx,
    const TArray<FParsedPinParam>& Inputs,
    const TArray<FParsedPinParam>& Outputs);

// Author parsed function inputs as entry-node outputs and function outputs as
// result-node inputs, matching UE's function-graph signature convention. Returns
// false with a caller-facing error when a required node or pin cannot be created.
PINWRIGHT_API bool AddParsedPinParamsToNodes(
    UK2Node_FunctionEntry* EntryNode,
    UK2Node_FunctionResult* ResultNode,
    const TArray<FParsedPinParam>& ParsedInputs,
    const TArray<FParsedPinParam>& ParsedOutputs,
    const TCHAR* LogContext,
    FString& OutError);
#if WITH_DEV_AUTOMATION_TESTS
PINWRIGHT_API void SetForceParsedPinCreationFailureForTests(bool bEnabled);
#endif
UFunction* ResolveFunction(UBlueprint* Blueprint, const FString& FunctionName);

// Returns true if Node is a Blueprint entry-point node (K2Node_Event derivatives,
// K2Node_FunctionEntry, input events, component-bound events, macro entry tunnels).
// Single source of truth replacing the duplicated IsEntryNode functions in
// GraphWalker.cpp and BlueprintGraphOrphanHandler.cpp.
//
// The named classes above are a fast path, not the definition. The definition is the
// engine's own compile root set — UE::KismetCompiler::Private::GatherRootSet with
// bIncludeNodesThatCouldBeExpandedToRootSet=true — whose shape clause ("impure UK2Node
// with no input pins at all") covers event-shaped node classes this module cannot name or
// link, notably UK2Node_EnhancedInputAction in the EnhancedInput plugin's optional
// InputBlueprintNodes module. See the implementation comment on IsEngineCompileRootSetNode.
bool IsBlueprintEntryNode(UEdGraphNode* Node);

// Returns true if Node is a macro graph entry tunnel: a bare UK2Node_Tunnel
// (not a UK2Node_MacroInstance) configured as output-only (bCanHaveOutputs &&
// !bCanHaveInputs). Single source of truth for the macro-entry-tunnel predicate
// shared by IsBlueprintEntryNode, GraphWalker::ClassifyNode, and the BPIR
// decompiler's entry-param resolver.
bool IsMacroEntryTunnel(const UEdGraphNode* Node);

// True iff Node is a bound-graph exit tunnel (a bare input-only UK2Node_Tunnel).
// Exit tunnels are graph boundaries, not sweepable orphan candidates. Keep this
// predicate shared by the authored finder and BPIR warning pass.
bool IsBlueprintOrphanExitTunnel(const UEdGraphNode* Node);

// Finds the entry and exit tunnels for a macro graph using the shared macro-entry
// predicate above. Returns true only when both tunnels are present.
bool FindMacroTunnelPair(UEdGraph* Graph, UK2Node_Tunnel*& OutEntryTunnel, UK2Node_Tunnel*& OutExitTunnel);

// Collect every graph reachable from this Blueprint's top-level graph lists
// (UbergraphPages, FunctionGraphs, MacroGraphs, DelegateSignatureGraphs) and
// recursively any SubGraphs hanging off those (composite graphs, math expressions,
// state-machine bound graphs). Uses a work-stack to handle arbitrary nesting depth.
// Returns a flat, deduplicated list in DFS order: each top-level graph followed by
// its full subtree before the next sibling top-level graph.
TArray<UEdGraph*> CollectAllBlueprintGraphsRecursive(UBlueprint* Blueprint);

// Collect every entry-point node reachable from Root and all of its transitively
// nested subgraphs. Appends to OutEntries (does not clear). Tolerates Root == nullptr.
void CollectEntryNodesRecursive(UEdGraph* Root, TArray<UEdGraphNode*>& OutEntries);

// Collect Root and every transitively nested child graph in stable DFS order. Appends to
// OutGraphs (does not clear). Tolerates Root == nullptr and skips repeated graph objects.
void CollectBlueprintOrphanGraphFamily(UEdGraph* Root, TArray<UEdGraph*>& OutGraphs);

// Collect "latent execution roots" of a single graph: nodes that drive the graph
// from an exec OUTPUT pin but have no connected exec INPUT pin and are not already
// IsBlueprintEntryNode entries. The canonical case is an auto-play K2Node_Timeline
// (its Update/Finished/Impact exec outputs root the chain while its Play/Stop exec
// inputs are unconnected), but the test is class-agnostic — any exec-output-only
// driver qualifies. This deliberately EXCLUDES timelines (or any latent node)
// whose Play exec input is wired from an upstream caller, because those are already
// reachable from their real event/function entry and would otherwise show up as
// spurious duplicate roots. Used only as a fallback by get_execution_flow when a
// graph has zero IsBlueprintEntryNode entries, so it never broadens the shared
// entry-node semantics that the decompiler / orphan walker rely on. Appends to
// OutRoots (does not clear). Tolerates Graph == nullptr.
void CollectLatentExecRootNodes(UEdGraph* Graph, TArray<UEdGraphNode*>& OutRoots);

// Build the exec-reachability set for a list of graphs: seeds from every
// IsBlueprintEntryNode, engine-rooted Timelines, and composites whose inner BoundGraph has an entry,
// then BFS over output exec edges (knots included). This legacy helper returns a GUID-only
// projection; consumers needing graph-qualified identity use BuildBlueprintOrphanReachability.
// Graphs == nullptr entries are skipped.
TSet<FGuid> BuildExecReachabilitySet(const TArray<UEdGraph*>& Graphs);

// Build the authoritative orphan model for an authored graph family. The overload taking
// one graph builds that graph only; callers needing nested composite graphs should first
// use CollectBlueprintOrphanGraphFamily. All sets are keyed by graph + NodeGuid. The
// backward data closure is demand-driven and only built when bIncludeDataOnly is true and
// the family contains a pure node with a linked data output.
FBlueprintOrphanReachability BuildBlueprintOrphanReachability(
    const TArray<UEdGraph*>& Graphs,
    bool bIncludeDataOnly = true);
FBlueprintOrphanReachability BuildBlueprintOrphanReachability(
    UEdGraph* Graph,
    bool bIncludeDataOnly = true);

PINWRIGHT_API TArray<FBlueprintOrphanNodeInfo> FindBlueprintOrphanNodes(
    UBlueprint* Blueprint,
    bool bIncludeDataOnly = true,
    UEdGraph* TargetGraph = nullptr,
    bool bIncludeNestedGraphs = true);
PINWRIGHT_API TSet<FGuid> SnapshotBlueprintOrphanGuids(
    UBlueprint* Blueprint,
    bool bIncludeDataOnly = true,
    UEdGraph* TargetGraph = nullptr);
PINWRIGHT_API FBlueprintOrphanDeltaCleanupResult CleanupNewBlueprintOrphans(
    UBlueprint* Blueprint,
    const TSet<FGuid>& BeforeOrphanGuids,
    bool bCleanupNewOrphans,
    bool bIncludeDataOnly = true,
    UEdGraph* TargetGraph = nullptr);
PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildOrphanNodeInfoJsonArray(
    const TArray<FBlueprintOrphanNodeInfo>& Orphans);
PINWRIGHT_API void AddOrphanDeltaCleanupResultToJson(
    const FBlueprintOrphanDeltaCleanupResult& CleanupResult,
    const TSharedPtr<FJsonObject>& Out);

// Graph node pin/link helper functions
// True if Node has at least one exec pin of the given Direction that is wired
// (LinkedTo.Num() > 0). Scans ALL exec pins of that direction, so a node
// with several exec pins per direction (e.g. a Timeline's Update/Finished/Impact
// outputs or Play/Stop inputs) is classified by whether ANY of them is connected.
bool HasConnectedExecPin(UEdGraphNode* Node, EEdGraphPinDirection Direction);
UEdGraphPin* FindOutputPin(UEdGraphNode* Node, const FName& PinName = NAME_None);
UEdGraphPin* FindInputPin(UEdGraphNode* Node, const FName& PinName);

// Resolve blueprint path from context payload -- checks multiple field names
FString ResolveBlueprintPath(FHandlerContext& Ctx);

// Build the FText category label for a blueprint variable/function/macro from a
// user-typed string. A category is an editor-only organizational label, NOT
// persisted localizable game/UI text — so it is built straight from the string
// (mirroring UE's own My-Blueprint-panel category commit) and is deliberately
// NOT routed through CoerceStringToPersistedFText: that gate (from
// F-require-ftext-localization-identity) exists for genuine persisted UI/game
// text and would wrongly reject a plain string like "Pickup".
// Regression: B-variable-category-ftext-localization-error.
PINWRIGHT_API FText MakeBlueprintCategoryText(const FString& Category);

} // namespace BlueprintHandlerUtils
