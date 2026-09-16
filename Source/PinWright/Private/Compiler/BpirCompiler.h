// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirCompiler.h - Three-pass BPIR compiler: parse -> emit -> wire

#pragma once

#include "CoreMinimal.h"
#include "Compiler/CompilerTypes.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/BpirTypeSpec.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class FCodeNodeEmitter;
class FCodePinResolver;
class FCodeFunctionResolver;
class FBpirValueResolver;
class UK2Node_Tunnel;
class UK2Node_CustomEvent;
class UK2Node_CallFunction;
class UFunction;
struct FBpirInstruction;

DECLARE_LOG_CATEGORY_EXTERN(LogBpirCompiler, Log, All);

struct FBpirReusedMacroGraphSnapshot
{
    TWeakObjectPtr<UEdGraph> Graph;
    FString ExportedNodesText;
};

/** Compile mode for FBpirCompiler::Compile. */
enum class EBpirCompileMode : uint8
{
    /** Default: no Phase 0 deletion. Pre-existing entry nodes are left untouched; SetupCustomEvent
     *  detects signature conflicts and reports "already exists" errors. Failed compiles roll back
     *  cleanly (rollback only has to delete nodes created during this compile). This is the safe
     *  in-process mode used by tests and any caller that wants atomic semantics. */
    Default,
    /** Replace: upsert — Phase 0 deletes existing entry nodes named in the incoming BPIR
     *  (along with their exec-reachable subgraphs) before re-creation. Used by RPC handlers so
     *  that retrying a timed-out compile does not accumulate duplicate entry nodes. SetupCustomEvent
     *  also silently replaces mismatched-signature events in this mode instead of erroring. */
    Replace,
    /** Extend: Phase 0 deletion is skipped; new body is appended after the terminal exec pin of the
     *  existing entry's exec chain. Falls back to Default when no existing entry is found.
     *  Errors if the existing exec chain has forked (branch/sequence) control flow. */
    Extend,
};

class PINWRIGHT_API FBpirCompiler
{
public:
    explicit FBpirCompiler(UBlueprint* InTargetBlueprint);
    ~FBpirCompiler();

    FBpirCompiler(const FBpirCompiler&) = delete;
    FBpirCompiler& operator=(const FBpirCompiler&) = delete;

    /** Compile BPIR code into Blueprint nodes.
     *  Default: no Phase 0 deletion — pre-existing entries stay; signature conflicts error.
     *  Replace: upsert — Phase 0 deletes matching-name entries before re-creating them, and
     *  SetupCustomEvent silently replaces on signature mismatch (used by RPC handlers for
     *  retry idempotency).
     *  Extend: skips Phase 0; appends new body to the terminal exec pin of the existing
     *  entry's chain. Falls back to Default when no existing entry is found. Errors if the
     *  existing chain has forked exec flow. */
    FCompileResult Compile(const FString& Code, EBpirCompileMode Mode = EBpirCompileMode::Default);

    /** Backward-compat overload: bReplaceMode=true maps to Replace (upsert), false to Default. */
    FCompileResult Compile(const FString& Code, bool bReplaceMode);

    /** Insert BPIR body code after an existing node (no entry wrapper needed). */
    FCompileResult InsertCodeAfterNode(UEdGraphNode* InsertionPointNode, const FString& Code, const FString& ExecPinName = TEXT(""));

    /** Insert BPIR body code before an existing node. */
    FCompileResult InsertCodeBeforeNode(UEdGraphNode* TargetNode, const FString& Code);

    /** Inject an external pin as a $variable for subsequent compilation. */
    void InjectExternalVariable(const FString& VarName, UEdGraphPin* Pin);

    /** Set exit tunnel so `return` wires to it (composite subgraph / macro context). */
    void SetExitTunnel(UK2Node_Tunnel* ExitTunnel);

    /** Compile BPIR body code into an existing graph, wired from EntryExecPin. */
    FCompileResult CompileBodyIntoGraph(const FString& BodyCode, UEdGraph* TargetGraph, UEdGraphPin* EntryExecPin);

    /** Access the emit map after compilation (for post-compile wiring by FBpirSubgraphCompiler). */
    const TMap<int32, FEmittedNodeInfo>& GetEmitMap() const { return EmitMap; }

    // --- Static undo system ---
    static TArray<FGuid> PopLastCreatedNodes();
    static bool DeleteNodesByGUIDs(UBlueprint* Blueprint, const TArray<FGuid>& NodeGUIDs);

    // Peek the Phase0RanStack top without popping. Returns false when the stack is empty.
    // blueprint.undo_last_bpir uses this to refuse rollback when the last compile ran
    // Phase 0 sweeps (which un-creating nodes cannot reverse).
    static bool DidLastCompileRunPhase0();

    // Exposes the file-local RunLayoutPass helper for automation tests.
    static void RunLayoutPassForTest(
        UBlueprint* Blueprint,
        const TArray<FGuid>& CreatedGUIDs,
        UEdGraph* MainGraph,
        const TArray<UEdGraph*>& ExtraGraphs,
        UEdGraphNode* AnchorHint = nullptr);

    /** Returns the most-specialized object class for a pin.
     *  When the pin's owning node is a UK2Node_ConstructObjectFromClass subclass,
     *  prefers GetClassToSpawn() over PinSubCategoryObject — the spawn class is set
     *  from ClassPin->DefaultObject and remains correct even when ReconstructNode
     *  doesn't fully propagate subcategory specialization. Returns nullptr if Pin is
     *  null or not an object-class pin. */
    static UClass* GetAuthoritativePinClass(UEdGraphPin* Pin);

private:
    // Pass 2: Entry point setup
    UEdGraphPin* SetupEntryPoint(FBpirEntryBlock& Block);
    UEdGraphPin* SetupBuiltinEvent(const FString& EventName);
    UEdGraphPin* SetupCustomEvent(const FString& Name, const TArray<FBpirEntryBlock::FParam>& Params, bool bReplaceMode, const FBpirEntryMetadata& Metadata);
    UEdGraphPin* SetupComponentEvent(const FString& CompName, const FString& EventName);
    UEdGraphPin* SetupWidgetEvent(const FString& WidgetName, const FString& EventName, const TArray<FBpirEntryBlock::FParam>& Params);

    // Register author-declared param aliases on the PinResolver after positional-matching
    // them against the non-exec output data pins of EntryNode.  Shared between Phase 1
    // (SetupWidgetEvent / SetupComponentEvent) and Phase 2 (re-registration after
    // PinResolver->Clear()).
    void RegisterEntryParamAliases(UEdGraphNode* EntryNode, const TArray<FBpirEntryBlock::FParam>& Params);
    UEdGraphPin* SetupKeyEvent(const FString& KeyName, bool bReleased);
    UEdGraphPin* SetupInputActionEvent(const FString& InputActionPath);
    UEdGraphPin* SetupConstructionScript();
    UEdGraphPin* SetupOverride(
        const FString& Name,
        const TArray<FBpirEntryBlock::FParam>& Params,
        const FBpirTypeSpec& ReturnType,
        const TArray<FBpirEntryBlock::FParam>& OutputParams,
        EBpirCompileMode Mode,
        const FBpirEntryMetadata& Metadata);
    UEdGraphPin* SetupFunction(const FString& Name, const FBpirTypeSpec& ReturnType, const TArray<FBpirEntryBlock::FParam>& Params, const TArray<FBpirEntryBlock::FParam>& OutputParams, const FBpirEntryMetadata& Metadata);
    UEdGraphPin* SetupMacro(const FString& Name, const TArray<FBpirEntryBlock::FParam>& InputParams,
                            const TArray<FBpirEntryBlock::FParam>& OutputParams, const TArray<FString>& ExecOutputNames,
                            const FBpirEntryBlock& Block);

    // Ensure a CallFunction's ReturnValue pin carries the type declared by a
    // BPIR entry function in the same compile. See BpirDeclaredReturnTypes.
    // Looks up the BPIR-declared return type by the BPIR call-site function
    // name (matches the key used by SetupFunction to store the declaration)
    // rather than the resolved UFunction's FName, which can diverge when the
    // skeleton-generated UFunction is resolved through a different path.
    UEdGraphPin* EnsureCallReturnPinFromBpirDeclaration(UK2Node_CallFunction* CallNode, UEdGraphPin* ReturnPin, const FString& BpirFunctionName);

    // Pass 2: Emit a single instruction -> K2 node
    bool EmitInstruction(int32 InstructionIndex, FBpirInstruction& Inst, FBpirEntryBlock& Block, UEdGraphPin*& InOutExecPin);

    // Generic K2Node fallback path used when `Inst.FunctionName` starts with `K2Node_` /
    // `UK2Node_` and no UFunction resolves. Routes `K2Node_AsyncAction_<Factory>` to the
    // async-action handler and rejects bare `K2Node_AsyncAction` before falling back to
    // FindObject-based class construction.
    bool EmitGenericK2NodeInstruction(
        const FString& TypeName,
        const FBpirInstruction& Inst,
        FEmittedNodeInfo& Emit,
        UEdGraphPin*& InOutExecPin,
        TFunctionRef<bool(UFunction*, UClass*)> EmitAsyncActionFactoryNode);

    // Pass 3: Wire data pins for one instruction
    bool WireDataPins(int32 InstructionIndex, FBpirInstruction& Inst, FBpirEntryBlock& Block);

    // Pass 3: Wire exec pins (auto-chain + labels)
    bool WireExecPins(FBpirEntryBlock& Block, UEdGraphPin* EntryExecPin);

    // Resolves a label+pin-name pair to the exec input pin of the first impure instruction at
    // that label. Returns nullptr on failure; sets bHadError=true if an error was recorded.
    // bLogOnMissingLabel adds a UE_LOG for the "label not found" case (used by the normal arm;
    // suppressed for the default arm where the label may legitimately be a convergence point).
    UEdGraphPin* ResolveTargetExecInput(const FBpirExecTarget& Target, FBpirEntryBlock& Block,
        int32 SourceLine, bool bLogOnMissingLabel, bool& bHadError);

    // Find the first exec-target instruction index at or after a label (-1 if not found):
    // the first whose emitted K2Node has an exec INPUT pin (the real "sits in the exec chain"
    // criterion — not merely an impure authored opcode; see the rationale on the .cpp
    // definition). Despite the historical "Impure" name, an opcode-Pure node that kept an exec
    // input pin still qualifies, and an impure-opcode node emitted pure does not.
    int32 FindFirstImpureAtLabel(const FString& LabelName, FBpirEntryBlock& Block);

    // Get exec input pin of an emitted instruction's node.
    // OverrideInputPinName (when non-empty) names a specific input exec pin on the target node;
    // hard-fails (returns nullptr) if that pin is missing — caller logs a compile error citing
    // the missing pin and lists available pins.
    UEdGraphPin* GetExecInputPin(int32 InstructionIndex, const FBpirInstruction& Inst, const FString& OverrideInputPinName = FString());

    // Resolve an exec output pin name to the actual pin on a node
    UEdGraphPin* FindExecOutputPin(int32 InstructionIndex, const FBpirInstruction& Inst, const FString& PinName);

    // Get or create emit info for an instruction index
    FEmittedNodeInfo& GetOrCreateEmitInfo(int32 InstructionIndex);

    // Resolve a target reference ($target or %ref) to its UClass
    UClass* ResolveTargetClass(const FString& TargetRef, FBpirEntryBlock& Block);

    // Shared delegate-synthesis tail for dispatcher and FieldNotify bind opcodes.
    void WireCreateDelegateForBindNode(UEdGraphPin* DelegateInPin,
        const FString& EventArgValue, bool bSelfContext, FBpirEntryBlock& Block);

    void SwitchToGraph(UEdGraph* NewGraph);

    // Pre-emit VariableGet/Self nodes for all $var, self, and Set TypeArg references in a block
    void PreEmitVariableRefs(FBpirEntryBlock& Block);

    // Pass 0: Force-load all externally-referenced UClass paths before any graph mutation.
    // Prevents ResolveUClass -> LoadObject -> FlushAsyncLoading -> FlushCompilationQueue re-entrance
    // during Emit/Wire phases while the compiler is mutating graphs.
    void PreloadExternalClasses(TArrayView<const FBpirEntryBlock> Blocks);

    // State
    UBlueprint* TargetBlueprint;
    UEdGraph* CurrentGraph;
    UK2Node_Tunnel* CurrentMacroExitTunnel = nullptr;
    EBpirCompileMode CompileMode = EBpirCompileMode::Default; // Set at Compile() entry
    bool bCompileReplaceMode = false; // Legacy alias: true when CompileMode != Extend (used by SetupCustomEvent)

    TUniquePtr<FCodeNodeEmitter> NodeEmitter;
    TUniquePtr<FCodePinResolver> PinResolver;
    TUniquePtr<FCodeFunctionResolver> FunctionResolver;
    TUniquePtr<FBpirValueResolver> ValueResolver;

    // Custom events created during Phase 1 — tracked for skeleton recompile
    TMap<FString, UK2Node_CustomEvent*> CreatedCustomEvents;

    // Event name mapping (BeginPlay -> ReceiveBeginPlay, etc.). AActor-shaped:
    // applies only when ParentClass is an AActor subclass and the literal name
    // does not resolve directly. See ResolveOverrideEventName.
    TMap<FString, FName> EventNameMap;

    // Class-aware resolver for override/event name shorthand. Tries the literal
    // BaseName against ParentClass first (so UUserWidget::Tick stays "Tick"),
    // and only falls back to the Receive-prefixed mapping when the literal
    // lookup fails AND ParentClass is an AActor subclass. Returns FName(*BaseName)
    // for unmapped names so existing override-resolution diagnostics still fire.
    // ParentClass is explicit at every call site because the four consumers each
    // operate on different blueprint contexts (compile-time blueprint vs handler-
    // visible blueprint), and an implicit TargetBlueprint->ParentClass lookup
    // would be silently wrong inside the BlueprintGraph/Event handler call sites.
    FName ResolveOverrideEventName(const FString& BaseName, UClass* ParentClass) const;

    // Emit state: maps instruction index -> emitted node info (separate from IR)
    TMap<int32, FEmittedNodeInfo> EmitMap;

    // External variable injections deferred until CompileBodyIntoGraph resets state
    TMap<FString, UEdGraphPin*> PendingExternalInjections;

    // Function graphs created during compilation (for rollback on failure)
    TArray<UEdGraph*> CreatedFunctionGraphs;

    // Macro graphs created during compilation (for rollback on failure)
    TArray<UEdGraph*> CreatedMacroGraphs;

    // Existing macro graphs rewritten during replace-mode compilation (layout only, never rollback deletion)
    TArray<UEdGraph*> ReusedMacroGraphs;

    // Reused macro graph bodies are destructively rebuilt; snapshots restore them if compile fails.
    TArray<FBpirReusedMacroGraphSnapshot> ReusedMacroGraphSnapshots;

    // Errors accumulated during compilation
    TArray<FCompileError> AccumulatedErrors;

    // Downstream exec pins saved during Extend mode setup (e.g., link to FunctionResult).
    // After WireExecPins, reconnect the last exec-out of the new body to these pins.
    struct FExtendReconnect
    {
        // The entry exec pin that is the splice point (returned by SetupOverride in Extend mode).
        // After compilation, the new body's last exec-out must link to DownstreamPins.
        UEdGraphPin* SplicePin = nullptr;
        TArray<UEdGraphPin*> DownstreamPins;
    };
    TArray<FExtendReconnect> PendingExtendReconnects;

    // Warnings accumulated during compilation (non-fatal, included in success results)
    TArray<FString> AccumulatedWarnings;

    /** Map of BPIR-declared functions' return types (FunctionName -> structured type spec).
     *  Populated in SetupFunction; consumed at CallFunction emission to patch the
     *  ReturnValue pin's subcategory when the callee's skeleton signature hasn't
     *  yet propagated the specialized object<T> class.
     *  Cleared at the start of each Compile() call. */
    TMap<FName, FBpirTypeSpec> BpirDeclaredReturnTypes;
};
