// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRCompiler.h"

#include "AGIR/AGIRCliffHandlers.h"
#include "AGIR/AGIRCompilerHelpers.h"
#include "AGIR/AGIRLayoutEngine.h"
#include "AGIR/AGIROpcodes.h"
#include "AGIR/AGIRParser.h"
#include "AGIR/AGIRPinResolver.h"
#include "AGIR/AGIRTextEmitter.h"
#include "IrCore/IrTextUtils.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_CustomTransitionResult.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimStateConduitNode.h"
#include "AnimStateAliasNode.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimStateMachineTypes.h"
#include "AnimationCustomTransitionGraph.h"
#include "AnimationCustomTransitionSchema.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetUtils.h"
#include "Utils/BlueprintGraphSnapshot.h"
#include "Utils/GuardedLoad.h"
#include "Utils/PropertyUtils.h"
#include "Utils/TransactionUtils.h"

namespace AGIRCliff
{
// Single definition of the round-trip diagnostic hint declared (extern) in
// AGIRCliffHandlers.h, so every pose-resolution site across the per-family
// translation units shares one literal.
const TCHAR* const GAGIRPoseSymbolNotFoundHint = TEXT(
    "If this %-pose-ref came from unmodified anim.decompile_agir output, this is "
    "likely a decompiler/compiler round-trip gap, not your edit — see the anim "
    "wiki 'Round-trip limitations & how to diagnose a round-trip failure' "
    "section before rewriting your AGIR.");
}

namespace
{
// Pose-wire / symbol-map types live in AGIRCliffHandlers.h under namespace
// AGIRCliff so per-family handler .cpp files (AGIRCompiler_BlendSpace.cpp
// etc.) share a single declaration.
using AGIRCliff::FAGIRPendingPoseWire;
using AGIRCliff::FAGIRPendingPoseWires;
using AGIRCliff::FAGIRSymbolMap;
using AGIRCliff::FAGIRCacheNameMap;
using AGIRCliff::FAGIRPendingCacheLink;
using AGIRCliff::FAGIRPendingCacheLinks;
using AGIRCliff::GAGIRPoseSymbolNotFoundHint;


// Find the top-level anim graph on AnimBP whose class is exactly UAnimationGraph
// and whose name matches BlockName.
UEdGraph* FindAnimGraphByName(UAnimBlueprint* AnimBP, const FString& BlockName)
{
    if (!AnimBP || BlockName.IsEmpty())
    {
        return nullptr;
    }
    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (Graph && Graph->GetClass() == UAnimationGraph::StaticClass() && Graph->GetName() == BlockName)
        {
            return Graph;
        }
    }
    return nullptr;
}

// Walk `AnimBP->ImplementedInterfaces[i].Graphs` (and their child graphs) once
// per Compile() invocation, indexing every UAnimationGraph-class graph by name.
// AGIR `anim_layer "<Name>"` blocks resolve via this map, replacing the prior
// per-block linear walk.
void BuildLayerGraphMap(UAnimBlueprint* AnimBP, TMap<FString, UEdGraph*>& OutLayerGraphMap)
{
    if (!AnimBP)
    {
        return;
    }
    for (const FBPInterfaceDescription& InterfaceDesc : AnimBP->ImplementedInterfaces)
    {
        for (UEdGraph* InterfaceGraph : InterfaceDesc.Graphs)
        {
            if (!InterfaceGraph)
            {
                continue;
            }
            if (InterfaceGraph->GetClass() == UAnimationGraph::StaticClass())
            {
                OutLayerGraphMap.Add(InterfaceGraph->GetName(), InterfaceGraph);
            }
            TArray<UEdGraph*> Children;
            InterfaceGraph->GetAllChildrenGraphs(Children);
            for (UEdGraph* Child : Children)
            {
                if (Child && Child->GetClass() == UAnimationGraph::StaticClass())
                {
                    OutLayerGraphMap.Add(Child->GetName(), Child);
                }
            }
        }
    }
}

// Returns the existing UAnimGraphNode_Root in `Graph`, or nullptr if absent.
// The schema's CreateDefaultNodesForGraph populates one root in any new
// UAnimationGraph, so a Root is expected on any compile target.
UAnimGraphNode_Root* FindRootNode(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_Root* RootNode = Cast<UAnimGraphNode_Root>(Node))
        {
            return RootNode;
        }
    }
    return nullptr;
}

// Pull shared helpers into the file-local anonymous namespace so existing
// unqualified call sites (IsPoseRefValue / ApplyNodeGuidIfPresent /
// FindArgValue / WriteUObjectFieldByName) keep resolving. Definitions live in
// AGIRCompilerHelpers.h (inline) and AGIRCompilerHelpers.cpp (out-of-line).
using AGIRCliff::Helpers::IsPoseRefValue;
using AGIRCliff::Helpers::ApplyNodeGuidIfPresent;
using AGIRCliff::Helpers::FindArgValue;
using AGIRCliff::Helpers::WriteUObjectFieldByName;

// Strip surrounding string or name tokens so reflective writes receive the raw
// payload that ImportText_Direct expects for FName / FString fields. Kept
// local — only used by ApplyArgsToUObject's neighbours below, and the shared
// WriteUObjectFieldByName performs its own unquoting internally.
FString UnquoteIfQuoted(const FString& Value)
{
    FString Unwrapped;
    FString Error;
    if (FIrTextUtils::TryUnwrapStringLiteral(Value, Unwrapped, Error) ||
        FIrTextUtils::TryUnwrapNameToken(Value, Unwrapped, Error))
    {
        return Unwrapped;
    }
    return Value;
}

// Apply each AGIR arg to the supplied UObject by name. Recognised attribute
// keys (priority, bidirectional, ...) are translated to UPROPERTY names per
// the emitter convention; unrecognised keys are passed through verbatim so
// future additions surface via reflection without touching this list.
void ApplyArgsToUObject(UObject* Target, const FAGIRInstruction& Inst,
    const TMap<FString, FName>& KeyMap,
    const TSet<FString>& IgnoredKeys,
    TArray<FString>& OutWarnings)
{
    if (!Target)
    {
        return;
    }
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty() || IgnoredKeys.Contains(Arg.Name))
        {
            continue;
        }
        FName FieldName;
        if (const FName* Mapped = KeyMap.Find(Arg.Name))
        {
            FieldName = *Mapped;
        }
        else
        {
            FieldName = FName(*Arg.Name);
        }
        const FString WriteError = WriteUObjectFieldByName(Target, FieldName, Arg.Value);
        if (!WriteError.IsEmpty())
        {
            OutWarnings.Add(FString::Printf(TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
                *WriteError, Inst.SourceLine));
        }
    }
}

// State / transition / conduit forward decls — they live below
// `EmitInstruction` because they call back into NodeGuid + reflective helpers
// that are local to this translation unit.
FAGIRCompileResult CompileStateInstruction(
    UAnimBlueprint* AnimBP,
    UAnimationStateMachineGraph* InnerGraph,
    const FAGIRInstruction& StateInst,
    TMap<FString, UEdGraphNode*>& StateSymbols,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings);

FAGIRCompileResult CompileTransitionInstruction(
    UAnimBlueprint* AnimBP,
    UAnimationStateMachineGraph* InnerGraph,
    const FAGIRInstruction& TransitionInst,
    const TMap<FString, UEdGraphNode*>& StateSymbols,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings);

FAGIRCompileResult CompileConduitInstruction(
    UAnimationStateMachineGraph* InnerGraph,
    const FAGIRInstruction& ConduitInst,
    TMap<FString, UEdGraphNode*>& StateSymbols,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings);

FAGIRCompileResult CompileStateAliasInstruction(
    UAnimationStateMachineGraph* InnerGraph,
    const FAGIRInstruction& AliasInst,
    TMap<FString, UEdGraphNode*>& StateSymbols,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings);

FAGIRCompileResult RebindStateAliasTargets(
    const FAGIRInstruction& MachineInst,
    const TMap<FString, UEdGraphNode*>& StateSymbols,
    TArray<FString>& OutWarnings);

// Forward decl: needed to recurse into custom_transition_body blocks. Body
// definition lives below EmitInstruction.
FAGIRCompileResult CompileBlockIntoGraph(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIREntryBlock& Block,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings);

// Wave 3: walk a custom_transition_body block's children and instantiate them
// inside the existing UAnimationCustomTransitionGraph. The graph already
// carries a schema-default UAnimGraphNode_CustomTransitionResult; AGIR's
// `output %nN` instruction wires upstream into that sink node's `Result` pin
// (mirrors how the top-level Output opcode wires into UAnimGraphNode_Root).
// Body Call instructions create new editor nodes; pose-ref args queue wires
// resolved against the body-local symbol map in Pass 2.
FAGIRCompileResult CompileCustomTransitionBody(
    UAnimBlueprint* AnimBP,
    UEdGraph* CustomGraph,
    const FAGIRInstruction& BodyInst,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings);

FAGIRCompileResult CompileCallInstruction(
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    const FString& BodyLabel,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings);

FAGIRCompileResult CompileOutputInstruction(
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    UAnimGraphNode_Base* ResultNode,
    FName ResultPinName,
    FAGIRPendingPoseWires& PendingWires,
    const FString& BodyLabel,
    bool bApplyOutputGuid);

FAGIRCompileResult ResolvePendingPoseWires(
    const FAGIRSymbolMap& Symbols,
    const FAGIRPendingPoseWires& PendingWires,
    const FString& BodyLabel);

// Pass-2 resolution of deferred use_cached_pose -> save_cached_pose linkage
// against the completed block-scoped CacheNameMap. Non-fatal: a use with no
// matching save anywhere in the block leaves SaveCachedPoseNode null and warns
// (the compile still succeeds; the engine's EarlyValidation self-heals a
// cross-graph save, or the AnimBP compile legitimately reports a genuinely
// missing save). Definition lives below ResolvePendingPoseWires.
void ResolvePendingCacheLinks(
    const FAGIRPendingCacheLinks& PendingCacheLinks,
    const FAGIRCacheNameMap& CacheNameMap,
    const FString& BodyLabel,
    TArray<FString>& OutWarnings);

FAGIRCompileResult CompilePoseResultBody(
    UAnimBlueprint* AnimBP,
    const TCHAR* BodyName,
    UEdGraph* TargetGraph,
    UAnimGraphNode_Base* ResultNode,
    FName ResultPinName,
    const TArray<TSharedPtr<FAGIRInstruction>>& Children,
    int32 SourceLine,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings);

// Build the inner state-machine sub-graph: walk Children dispatching to per-
// kind handlers. States and conduits are created first so transitions can look
// them up by name regardless of source order.
FAGIRCompileResult CompileStateMachineChildren(
    UAnimBlueprint* AnimBP,
    UAnimationStateMachineGraph* InnerGraph,
    const FAGIRInstruction& MachineInst,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    TMap<FString, UEdGraphNode*> StateSymbols;

    // Pass A: states + conduits + aliases — all three populate StateSymbols
    // and must precede the transition pass because transitions resolve by-name
    // from this map (and aliases are valid transition endpoints in Lyra's
    // LocomotionSM, where PivotSources / JumpSources stand in for groups of
    // states that share an outbound transition).
    for (const TSharedPtr<FAGIRInstruction>& Child : MachineInst.Children)
    {
        if (!Child.IsValid())
        {
            continue;
        }
        const EAGIROpcode Opcode = Child->Opcode;
        if (Opcode == EAGIROpcode::State)
        {
            FAGIRCompileResult Result = CompileStateInstruction(
                AnimBP, InnerGraph, *Child, StateSymbols, OutNodesCreated, OutWarnings);
            if (!Result.ErrorCode.IsEmpty())
            {
                return Result;
            }
        }
        else if (Opcode == EAGIROpcode::Conduit)
        {
            FAGIRCompileResult Result = CompileConduitInstruction(
                InnerGraph, *Child, StateSymbols, OutNodesCreated, OutWarnings);
            if (!Result.ErrorCode.IsEmpty())
            {
                return Result;
            }
        }
        else if (Opcode == EAGIROpcode::StateAlias)
        {
            FAGIRCompileResult Result = CompileStateAliasInstruction(
                InnerGraph, *Child, StateSymbols, OutNodesCreated, OutWarnings);
            if (!Result.ErrorCode.IsEmpty())
            {
                return Result;
            }
        }
    }

    // Pass B: transitions resolve from/to by looking up the just-built map.
    for (const TSharedPtr<FAGIRInstruction>& Child : MachineInst.Children)
    {
        if (!Child.IsValid() || Child->Opcode != EAGIROpcode::Transition)
        {
            continue;
        }
        FAGIRCompileResult Result = CompileTransitionInstruction(
            AnimBP, InnerGraph, *Child, StateSymbols, OutNodesCreated, OutWarnings);
        if (!Result.ErrorCode.IsEmpty())
        {
            return Result;
        }
    }

    // Pass C: rebind alias->state references now that every state / conduit /
    // alias name is in the symbol map. Aliases are created in Pass A with no
    // targets so a forward reference (alias appears textually before its
    // target state) still resolves. Unknown alias targets degrade to a warning
    // rather than an error to mirror the existing AGIR_FIELD_WRITE convention.
    FAGIRCompileResult RebindResult = RebindStateAliasTargets(MachineInst, StateSymbols, OutWarnings);
    if (!RebindResult.ErrorCode.IsEmpty())
    {
        return RebindResult;
    }

    return FAGIRCompileResult();
}

FAGIRCompileResult CompileStateInstruction(
    UAnimBlueprint* AnimBP,
    UAnimationStateMachineGraph* InnerGraph,
    const FAGIRInstruction& StateInst,
    TMap<FString, UEdGraphNode*>& StateSymbols,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    const FString StateName = StateInst.SymbolName;
    if (StateName.IsEmpty())
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_BAD_OPCODE"),
            FString::Printf(TEXT("State block missing name (line %d)."), StateInst.SourceLine));
    }

    const FVector2D Position = StateInst.bHasPosition ? StateInst.Position : FVector2D::ZeroVector;
    UAnimStateNode* StateNode = AnimGraphConstructionUtils::CreateState(
        InnerGraph, FName(*StateName), Position);
    if (!StateNode)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create state '%s' (line %d)."),
                *StateName, StateInst.SourceLine));
    }
    ApplyNodeGuidIfPresent(StateNode, StateInst.NodeGuid);

    // Emitter args mapping for state nodes. `always_reset_on_entry` is the
    // only common attr; notify-event references (StateEntered etc.) round-trip
    // by name when present.
    static const TMap<FString, FName> KeyMap = {
        { TEXT("always_reset_on_entry"), FName(TEXT("bAlwaysResetOnEntry")) },
        { TEXT("state_entered"),         FName(TEXT("StateEntered")) },
        { TEXT("state_left"),            FName(TEXT("StateLeft")) },
        { TEXT("state_fully_blended"),   FName(TEXT("StateFullyBlended")) },
    };
    static const TSet<FString> IgnoredKeys;
    ApplyArgsToUObject(StateNode, StateInst, KeyMap, IgnoredKeys, OutWarnings);

    StateSymbols.Add(StateName, StateNode);
    ++OutNodesCreated;

    if (StateInst.Children.Num() > 0)
    {
        UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(StateNode->BoundGraph);
        UAnimGraphNode_StateResult* ResultNode = StateGraph ? StateGraph->GetResultNode() : nullptr;
        FAGIRCompileResult BodyResult = CompilePoseResultBody(
            AnimBP,
            TEXT("state body"),
            StateNode->BoundGraph,
            ResultNode,
            FName(TEXT("Result")),
            StateInst.Children,
            StateInst.SourceLine,
            OutNodesCreated,
            OutWarnings);
        if (!BodyResult.ErrorCode.IsEmpty())
        {
            return BodyResult;
        }
    }

    return FAGIRCompileResult();
}

FAGIRCompileResult CompileTransitionInstruction(
    UAnimBlueprint* AnimBP,
    UAnimationStateMachineGraph* /*InnerGraph*/,
    const FAGIRInstruction& TransitionInst,
    const TMap<FString, UEdGraphNode*>& StateSymbols,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    const FString* FromValue = FindArgValue(TransitionInst, TEXT("from"));
    const FString* ToValue = FindArgValue(TransitionInst, TEXT("to"));
    if (!FromValue || !ToValue || FromValue->IsEmpty() || ToValue->IsEmpty())
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_BAD_OPCODE"),
            FString::Printf(TEXT("Transition missing from/to (line %d)."),
                TransitionInst.SourceLine));
    }

    UEdGraphNode* const* FromNode = StateSymbols.Find(*FromValue);
    UEdGraphNode* const* ToNode = StateSymbols.Find(*ToValue);
    if (!FromNode || !ToNode)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_SYMBOL_NOT_FOUND"),
            FString::Printf(TEXT("Transition '%s -> %s' references unknown state (line %d)."),
                **FromValue, **ToValue, TransitionInst.SourceLine));
    }

    UAnimStateNodeBase* FromState = Cast<UAnimStateNodeBase>(*FromNode);
    UAnimStateNodeBase* ToState = Cast<UAnimStateNodeBase>(*ToNode);
    if (!FromState || !ToState)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_SYMBOL_NOT_FOUND"),
            FString::Printf(TEXT("Transition endpoint is not a UAnimStateNodeBase (line %d)."),
                TransitionInst.SourceLine));
    }

    const FVector2D Position = TransitionInst.bHasPosition ? TransitionInst.Position : FVector2D::ZeroVector;
    UAnimStateTransitionNode* TransNode = AnimGraphConstructionUtils::CreateTransition(
        FromState, ToState, Position);
    if (!TransNode)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create transition '%s -> %s' (line %d)."),
                **FromValue, **ToValue, TransitionInst.SourceLine));
    }
    ApplyNodeGuidIfPresent(TransNode, TransitionInst.NodeGuid);

    // Emitter args → editor UPROPERTY names. `from` / `to` were consumed above;
    // `rule` carries the bound-graph reference name only (rule body content is
    // owned by BPIR per Decisions section).
    static const TMap<FString, FName> KeyMap = {
        { TEXT("priority"),               FName(TEXT("PriorityOrder")) },
        { TEXT("crossfade_duration"),     FName(TEXT("CrossfadeDuration")) },
        { TEXT("blend_mode"),             FName(TEXT("BlendMode")) },
        { TEXT("bidirectional"),          FName(TEXT("Bidirectional")) },
        { TEXT("disabled"),                FName(TEXT("bDisabled")) },
        { TEXT("auto_rule"),               FName(TEXT("bAutomaticRuleBasedOnSequencePlayerInState")) },
        { TEXT("auto_rule_trigger_time"),  FName(TEXT("AutomaticRuleTriggerTime")) },
        { TEXT("logic_type"),              FName(TEXT("LogicType")) },
    };
    static const TSet<FString> IgnoredKeys = {
        TEXT("from"), TEXT("to"), TEXT("rule"),
    };
    ApplyArgsToUObject(TransNode, TransitionInst, KeyMap, IgnoredKeys, OutWarnings);

    // TLT_Custom requires an authored sub-graph alongside the reflective write
    // of `LogicType`. The engine only creates the graph from PostEditChangeProperty
    // (UAnimStateTransitionNode.cpp:450), which we can't fire from a deterministic
    // compile path; mirror the four-line body of the engine's
    // CreateCustomTransitionGraph (UAnimStateTransitionNode.cpp:639-665) inline.
    if (TransNode->LogicType.GetValue() == ETransitionLogicType::TLT_Custom &&
        TransNode->CustomTransitionGraph == nullptr)
    {
        TransNode->CustomTransitionGraph = FBlueprintEditorUtils::CreateNewGraph(
            TransNode, NAME_None,
            UAnimationCustomTransitionGraph::StaticClass(),
            UAnimationCustomTransitionSchema::StaticClass());
        if (TransNode->CustomTransitionGraph)
        {
            FEdGraphUtilities::RenameGraphToNameOrCloseToName(
                TransNode->CustomTransitionGraph, TEXT("CustomTransition"));
            const UEdGraphSchema* Schema = TransNode->CustomTransitionGraph->GetSchema();
            if (Schema)
            {
                Schema->CreateDefaultNodesForGraph(*TransNode->CustomTransitionGraph);
            }
            if (UEdGraph* ParentGraph = TransNode->GetGraph())
            {
                if (ParentGraph->SubGraphs.Find(TransNode->CustomTransitionGraph) == INDEX_NONE)
                {
                    ParentGraph->SubGraphs.Add(TransNode->CustomTransitionGraph);
                }
            }
        }
        else
        {
            OutWarnings.Add(FString::Printf(
                TEXT("AGIR_FIELD_WRITE: failed to create custom transition graph (line %d)"),
                TransitionInst.SourceLine));
        }

        // Wave 3: recurse the body. The grammar emits a single
        // `custom_transition_body { ... }` child whose Children are the anim
        // graph nodes that populate the custom transition graph alongside the
        // schema-default UAnimGraphNode_CustomTransitionResult. The body uses
        // a dedicated walker because the sink node (`output %nN` target) is a
        // UAnimGraphNode_CustomTransitionResult rather than UAnimGraphNode_Root.
        if (TransNode->CustomTransitionGraph)
        {
            for (const TSharedPtr<FAGIRInstruction>& Child : TransitionInst.Children)
            {
                if (!Child.IsValid() || Child->Opcode != EAGIROpcode::CustomTransitionBody)
                {
                    continue;
                }
                FAGIRCompileResult BodyResult = CompileCustomTransitionBody(
                    AnimBP, TransNode->CustomTransitionGraph, *Child,
                    OutNodesCreated, OutWarnings);
                if (!BodyResult.ErrorCode.IsEmpty())
                {
                    return BodyResult;
                }
            }
        }
    }

    ++OutNodesCreated;
    return FAGIRCompileResult();
}

FAGIRCompileResult CompileConduitInstruction(
    UAnimationStateMachineGraph* InnerGraph,
    const FAGIRInstruction& ConduitInst,
    TMap<FString, UEdGraphNode*>& StateSymbols,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    const FString ConduitName = ConduitInst.SymbolName;
    if (ConduitName.IsEmpty())
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_BAD_OPCODE"),
            FString::Printf(TEXT("Conduit missing name (line %d)."), ConduitInst.SourceLine));
    }

    const FVector2D Position = ConduitInst.bHasPosition ? ConduitInst.Position : FVector2D::ZeroVector;
    UAnimStateConduitNode* ConduitNode = AnimGraphConstructionUtils::CreateConduit(
        InnerGraph, FName(*ConduitName), Position);
    if (!ConduitNode)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create conduit '%s' (line %d)."),
                *ConduitName, ConduitInst.SourceLine));
    }
    ApplyNodeGuidIfPresent(ConduitNode, ConduitInst.NodeGuid);

    // Conduit bound-graph (rule body) is BPIR's responsibility; AGIR records
    // `rule="<graph_name>"` for round-trip but no editor-side write is needed.
    // No conduit-level UPROPERTY is currently written through reflection;
    // unknown keys surface as warnings rather than falling through to a raw
    // reflective write so a typo in the AGIR text can't silently land on an
    // unrelated UEdGraphNode field.
    static const TSet<FString> IgnoredKeys = {
        TEXT("rule"),
    };
    static const TSet<FString> AllowedKeys; // currently empty for conduit
    for (const FAGIRArg& Arg : ConduitInst.Args)
    {
        if (Arg.Name.IsEmpty() || IgnoredKeys.Contains(Arg.Name))
        {
            continue;
        }
        if (!AllowedKeys.Contains(Arg.Name))
        {
            OutWarnings.Add(FString::Printf(
                TEXT("AGIR_FIELD_WRITE: unknown conduit attribute '%s' (line %d)"),
                *Arg.Name, ConduitInst.SourceLine));
        }
    }

    StateSymbols.Add(ConduitName, ConduitNode);
    ++OutNodesCreated;
    return FAGIRCompileResult();
}

FAGIRCompileResult CompileStateAliasInstruction(
    UAnimationStateMachineGraph* InnerGraph,
    const FAGIRInstruction& AliasInst,
    TMap<FString, UEdGraphNode*>& StateSymbols,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    const FString AliasName = AliasInst.SymbolName;
    if (AliasName.IsEmpty())
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_BAD_OPCODE"),
            FString::Printf(TEXT("State alias missing name (line %d)."), AliasInst.SourceLine));
    }

    const FVector2D Position = AliasInst.bHasPosition ? AliasInst.Position : FVector2D::ZeroVector;
    UAnimStateAliasNode* AliasNode = AnimGraphConstructionUtils::CreateStateAlias(
        InnerGraph, FName(*AliasName), Position);
    if (!AliasNode)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create state alias '%s' (line %d)."),
                *AliasName, AliasInst.SourceLine));
    }
    ApplyNodeGuidIfPresent(AliasNode, AliasInst.NodeGuid);

    // global_alias is the only directly-applied attr here; aliases= is
    // resolved in Pass C once every target state symbol exists. Unknown attrs
    // surface as warnings so unrelated UEdGraphNode reflective writes can't
    // land silently on a typo.
    if (const FString* GlobalValue = FindArgValue(AliasInst, TEXT("global_alias")))
    {
        AliasNode->bGlobalAlias = (UnquoteIfQuoted(*GlobalValue).ToLower() == TEXT("true"));
    }

    static const TSet<FString> KnownKeys = {
        TEXT("aliases"),
        TEXT("global_alias"),
    };
    for (const FAGIRArg& Arg : AliasInst.Args)
    {
        if (Arg.Name.IsEmpty() || KnownKeys.Contains(Arg.Name))
        {
            continue;
        }
        OutWarnings.Add(FString::Printf(
            TEXT("AGIR_FIELD_WRITE: unknown state_alias attribute '%s' (line %d)"),
            *Arg.Name, AliasInst.SourceLine));
    }

    StateSymbols.Add(AliasName, AliasNode);
    ++OutNodesCreated;
    return FAGIRCompileResult();
}

FAGIRCompileResult RebindStateAliasTargets(
    const FAGIRInstruction& MachineInst,
    const TMap<FString, UEdGraphNode*>& StateSymbols,
    TArray<FString>& OutWarnings)
{
    for (const TSharedPtr<FAGIRInstruction>& Child : MachineInst.Children)
    {
        if (!Child.IsValid() || Child->Opcode != EAGIROpcode::StateAlias)
        {
            continue;
        }

        const FString AliasName = Child->SymbolName;
        UEdGraphNode* const* AliasNodeEntry = StateSymbols.Find(AliasName);
        UAnimStateAliasNode* AliasNode = AliasNodeEntry
            ? Cast<UAnimStateAliasNode>(*AliasNodeEntry)
            : nullptr;
        if (!AliasNode)
        {
            // Pass A failure should already have surfaced an error; defensive
            // skip here keeps the rebind loop from null-deref'ing.
            continue;
        }

        // Global aliases never carry an explicit target list; the engine's
        // resolution treats them as "any state" and ValidateNodeDuringCompilation
        // requires the target set be empty in that mode.
        if (AliasNode->bGlobalAlias)
        {
            AliasNode->GetAliasedStates().Reset();
            continue;
        }

        const FString* AliasesValue = FindArgValue(*Child, TEXT("aliases"));
        if (!AliasesValue || AliasesValue->IsEmpty())
        {
            AliasNode->GetAliasedStates().Reset();
            continue;
        }

        const FString Unwrapped = UnquoteIfQuoted(*AliasesValue);
        TArray<FString> TargetNames;
        Unwrapped.ParseIntoArray(TargetNames, TEXT(","), true);

        TSet<TWeakObjectPtr<UAnimStateNodeBase>>& TargetSet = AliasNode->GetAliasedStates();
        TargetSet.Reset();
        for (const FString& RawName : TargetNames)
        {
            const FString TargetName = RawName.TrimStartAndEnd();
            if (TargetName.IsEmpty())
            {
                continue;
            }
            UEdGraphNode* const* TargetEntry = StateSymbols.Find(TargetName);
            UAnimStateNodeBase* Target = TargetEntry
                ? Cast<UAnimStateNodeBase>(*TargetEntry)
                : nullptr;
            if (!Target)
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("AGIR_SYMBOL_NOT_FOUND: state_alias '%s' references unknown target '%s' (line %d)"),
                    *AliasName, *TargetName, Child->SourceLine));
                continue;
            }
            TargetSet.Add(Target);
        }
    }

    return FAGIRCompileResult();
}

// Pass 1 dispatcher: instantiate one editor node per Call instruction, queue
// pose-input args for Pass 2, dispatch cliff opcodes to per-family handlers in
// AGIRCompiler_<Family>.cpp.
//
// StateMachineOrdinal: per-block state_machine positional-id counter; see the
// StateMachine case below for why.
FAGIRCompileResult EmitInstruction(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const TSharedPtr<FAGIRInstruction>& Instruction,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    FAGIRCacheNameMap& CacheNameMap,
    FAGIRPendingCacheLinks& PendingCacheLinks,
    UAnimGraphNode_Root* RootNode,
    int32& StateMachineOrdinal,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    if (!Instruction.IsValid())
    {
        return FAGIRCompileResult();
    }

    const FAGIRInstruction& Inst = *Instruction;
    const FVector2D Position = Inst.bHasPosition ? Inst.Position : FVector2D::ZeroVector;

    switch (Inst.Opcode)
    {
    case EAGIROpcode::Call:
        return CompileCallInstruction(
            TargetGraph,
            Inst,
            Symbols,
            PendingWires,
            FString(),
            OutNodesCreated,
            OutWarnings);

    case EAGIROpcode::Output:
        return CompileOutputInstruction(
            TargetGraph,
            Inst,
            RootNode,
            FName(TEXT("Result")),
            PendingWires,
            FString(),
            true);

    case EAGIROpcode::StateMachine:
    {
        const FString MachineName = Inst.SymbolName;
        if (MachineName.IsEmpty())
        {
            return FAGIRCompileResult::MakeError(
                TEXT("AGIR_BAD_OPCODE"),
                FString::Printf(TEXT("state_machine block missing name (line %d)."),
                    Inst.SourceLine));
        }

        UAnimGraphNode_StateMachine* MachineNode = AnimGraphConstructionUtils::CreateStateMachine(
            AnimBP, TargetGraph, FName(*MachineName), Position);
        if (!MachineNode)
        {
            return FAGIRCompileResult::MakeError(
                TEXT("AGIR_NODE_CREATE_FAILED"),
                FString::Printf(TEXT("Failed to create state_machine '%s' on graph '%s' (line %d)."),
                    *MachineName, *TargetGraph->GetName(), Inst.SourceLine));
        }
        ApplyNodeGuidIfPresent(MachineNode, Inst.NodeGuid);

        // The decompiler emits a state machine with the unassigned name-token
        // opener (`state_machine <Name> {`) — no `%n = ` — yet it pre-allocates
        // a `%state_machine_<n>` AGIR-local id (FAGIRTextEmitter::EmitStateMachine)
        // and references it from the consuming `output` line. ParseBlockHeader
        // leaves ResultName empty for that opener form, so without a positional
        // fallback the machine never lands in Symbols and `output %state_machine_<n>`
        // fails to resolve (AGIR_SYMBOL_NOT_FOUND). Advance the ordinal once per
        // state_machine instruction — in instruction (= emit) order, mirroring
        // the emitter — and register the node under the same positional id when
        // no explicit binding was supplied. The id token is spelled by
        // FAGIRTextEmitter::MakeLocalId (the emitter's single source of truth);
        // the leading '%' is stripped because Symbols is keyed by bare name
        // (AGIRPinResolver stores SymbolName without '%').
        const int32 Ordinal = StateMachineOrdinal++;

        if (!Inst.ResultName.IsEmpty())
        {
            Symbols.Add(Inst.ResultName, MachineNode);
        }
        else
        {
            Symbols.Add(FAGIRTextEmitter::MakeLocalId(TEXT("state_machine"), Ordinal).RightChop(1), MachineNode);
        }

        ++OutNodesCreated;

        UAnimationStateMachineGraph* InnerGraph = MachineNode->EditorStateMachineGraph;
        if (!InnerGraph)
        {
            return FAGIRCompileResult::MakeError(
                TEXT("AGIR_NODE_CREATE_FAILED"),
                FString::Printf(TEXT("state_machine '%s' has no inner graph (line %d)."),
                    *MachineName, Inst.SourceLine));
        }

        return CompileStateMachineChildren(AnimBP, InnerGraph, Inst, OutNodesCreated, OutWarnings);
    }

    case EAGIROpcode::State:
    case EAGIROpcode::Transition:
    case EAGIROpcode::Conduit:
    case EAGIROpcode::StateAlias:
        // These opcodes only appear inside a state_machine block; the
        // CompileStateMachineChildren walker dispatches them. Reaching this
        // point means the AGIR text emitted them at top level, which is a
        // grammar violation rather than a compile-implementation gap.
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_BAD_OPCODE"),
            FString::Printf(TEXT("Opcode 'state'/'transition'/'conduit'/'state_alias' only valid inside state_machine block (line %d)."),
                Inst.SourceLine));

    case EAGIROpcode::BlendSpace:
    {
        FAGIRCompileResult OutResult;
        if (!AGIRCliff::CompileBlendSpaceInstruction(
                AnimBP, TargetGraph, Inst, Symbols, PendingWires,
                OutNodesCreated, OutWarnings, OutResult))
        {
            return OutResult;
        }
        return FAGIRCompileResult();
    }

    case EAGIROpcode::LayeredBlend:
    {
        FAGIRCompileResult OutResult;
        if (!AGIRCliff::CompileLayeredBlendInstruction(
                AnimBP, TargetGraph, Inst, Symbols, PendingWires,
                OutNodesCreated, OutWarnings, OutResult))
        {
            return OutResult;
        }
        return FAGIRCompileResult();
    }

    case EAGIROpcode::LinkedAnim:
    {
        FAGIRCompileResult OutResult;
        if (!AGIRCliff::CompileLinkedAnimInstruction(
                AnimBP, TargetGraph, Inst, Symbols, PendingWires,
                OutNodesCreated, OutWarnings, OutResult))
        {
            return OutResult;
        }
        return FAGIRCompileResult();
    }

    case EAGIROpcode::LinkedInputPose:
    {
        FAGIRCompileResult OutResult;
        if (!AGIRCliff::CompileLinkedInputPoseInstruction(
                AnimBP, TargetGraph, Inst, Symbols, PendingWires,
                OutNodesCreated, OutWarnings, OutResult))
        {
            return OutResult;
        }
        return FAGIRCompileResult();
    }

    case EAGIROpcode::SaveCachedPose:
    {
        FAGIRCompileResult OutResult;
        if (!AGIRCliff::CompileSaveCachedPoseInstruction(
                AnimBP, TargetGraph, Inst, Symbols, PendingWires, CacheNameMap,
                OutNodesCreated, OutWarnings, OutResult))
        {
            return OutResult;
        }
        return FAGIRCompileResult();
    }

    case EAGIROpcode::UseCachedPose:
    {
        FAGIRCompileResult OutResult;
        if (!AGIRCliff::CompileUseCachedPoseInstruction(
                AnimBP, TargetGraph, Inst, Symbols, PendingWires, PendingCacheLinks,
                OutNodesCreated, OutWarnings, OutResult))
        {
            return OutResult;
        }
        return FAGIRCompileResult();
    }

    default:
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_UNSUPPORTED_OPCODE"),
            FString::Printf(TEXT("Unsupported AGIR opcode at line %d."), Inst.SourceLine));
    }
}

// Two-phase compile against a UAnimationGraph (top-level anim graph or anim
// layer interface override implementation graph).
FAGIRCompileResult CompileBlockIntoGraph(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIREntryBlock& Block,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    if (!TargetGraph)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            FString::Printf(TEXT("Target anim graph '%s' not found."), *Block.Name));
    }

    UAnimGraphNode_Root* RootNode = FindRootNode(TargetGraph);

    FAGIRSymbolMap Symbols;
    FAGIRPendingPoseWires PendingWires;
    // Block-scoped name table for save/use_cached_pose cross-references; Pass-1
    // save handlers populate it, Pass-2 (ResolvePendingCacheLinks) resolves the
    // deferred use handlers against the completed map so a use emitted before
    // its save (forward reference) still links.
    FAGIRCacheNameMap CacheNameMap;
    FAGIRPendingCacheLinks PendingCacheLinks;
    // Per-block state_machine ordinal; see StateMachine case in EmitInstruction.
    int32 StateMachineOrdinal = 0;

    for (const TSharedPtr<FAGIRInstruction>& Instruction : Block.Instructions)
    {
        FAGIRCompileResult InstResult = EmitInstruction(
            AnimBP,
            TargetGraph,
            Instruction,
            Symbols,
            PendingWires,
            CacheNameMap,
            PendingCacheLinks,
            RootNode,
            StateMachineOrdinal,
            OutNodesCreated,
            OutWarnings);
        if (!InstResult.ErrorCode.IsEmpty())
        {
            return InstResult;
        }
    }

    ResolvePendingCacheLinks(PendingCacheLinks, CacheNameMap, FString(), OutWarnings);
    return ResolvePendingPoseWires(Symbols, PendingWires, FString());
}

FAGIRCompileResult CompileCallInstruction(
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    const FString& BodyLabel,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    // Guarded, because SymbolName is a substring of the caller's AGIR text and the dispatch
    // boundary cannot see it: `text` is an IR document typed `string`, and the tokenizer treats
    // '/' as an ordinary character. A raw load on "/Game//X.X_C" ends the editor process.
    FString LoadRefusal;
    UClass* NodeClass =
        PinWrightGuardedLoad::LoadObjectChecked<UClass>(Inst.SymbolName, &LoadRefusal);
    if (!NodeClass)
    {
        const FString Detail = LoadRefusal.IsEmpty() ? FString() : (TEXT(" ") + LoadRefusal);
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_CLASS_NOT_FOUND"),
            BodyLabel.IsEmpty()
                ? FString::Printf(TEXT("Could not load anim node class '%s' (line %d).%s"),
                    *Inst.SymbolName, Inst.SourceLine, *Detail)
                : FString::Printf(TEXT("Could not load anim node class '%s' inside %s (line %d).%s"),
                    *Inst.SymbolName, *BodyLabel, Inst.SourceLine, *Detail));
    }
    if (!NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()))
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_CLASS_NOT_FOUND"),
            BodyLabel.IsEmpty()
                ? FString::Printf(TEXT("Class '%s' is not a UAnimGraphNode_Base subclass (line %d)."),
                    *Inst.SymbolName, Inst.SourceLine)
                : FString::Printf(TEXT("Could not load anim node class '%s' inside %s (line %d)."),
                    *Inst.SymbolName, *BodyLabel, Inst.SourceLine));
    }

    const FVector2D Position = Inst.bHasPosition ? Inst.Position : FVector2D::ZeroVector;
    UAnimGraphNode_Base* NewNode = AnimGraphConstructionUtils::CreateAnimNode(TargetGraph, NodeClass, Position);
    if (!NewNode)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            BodyLabel.IsEmpty()
                ? FString::Printf(TEXT("Failed to create node of class '%s' on graph '%s' (line %d)."),
                    *Inst.SymbolName, TargetGraph ? *TargetGraph->GetName() : TEXT("<null>"), Inst.SourceLine)
                : FString::Printf(TEXT("Failed to create node '%s' inside %s (line %d)."),
                    *Inst.SymbolName, *BodyLabel, Inst.SourceLine));
    }
    ApplyNodeGuidIfPresent(NewNode, Inst.NodeGuid);

    if (!Inst.ResultName.IsEmpty())
    {
        Symbols.Add(Inst.ResultName, NewNode);
    }

    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty())
        {
            continue;
        }
        if (IsPoseRefValue(Arg.Value))
        {
            FAGIRPendingPoseWire Wire;
            Wire.DownstreamNode = NewNode;
            Wire.InputPinName = FName(*Arg.Name);
            Wire.UpstreamRef = Arg.Value;
            Wire.SourceLine = Inst.SourceLine;
            PendingWires.Add(MoveTemp(Wire));
            continue;
        }

        const FString WriteError = AGIRCliff::Helpers::WriteAnimNodeArg(
            NewNode, FName(*Arg.Name), Arg.Value);
        if (!WriteError.IsEmpty())
        {
            OutWarnings.Add(FString::Printf(TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
                *WriteError, Inst.SourceLine));
        }
    }

    ++OutNodesCreated;
    return FAGIRCompileResult();
}

FAGIRCompileResult CompileOutputInstruction(
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    UAnimGraphNode_Base* ResultNode,
    FName ResultPinName,
    FAGIRPendingPoseWires& PendingWires,
    const FString& BodyLabel,
    bool bApplyOutputGuid)
{
    if (!ResultNode)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            BodyLabel.IsEmpty()
                ? FString::Printf(TEXT("Target graph '%s' has no UAnimGraphNode_Root for 'output' instruction (line %d)."),
                    TargetGraph ? *TargetGraph->GetName() : TEXT("<null>"), Inst.SourceLine)
                : FString::Printf(TEXT("%s missing result node for 'output' (line %d)."),
                    *BodyLabel, Inst.SourceLine));
    }
    if (bApplyOutputGuid)
    {
        ApplyNodeGuidIfPresent(ResultNode, Inst.NodeGuid);
    }

    const FString UpstreamRef = !Inst.SymbolName.IsEmpty()
        ? Inst.SymbolName
        : (Inst.Args.Num() > 0 ? Inst.Args[0].Value : FString());
    if (IsPoseRefValue(UpstreamRef))
    {
        FAGIRPendingPoseWire Wire;
        Wire.DownstreamNode = ResultNode;
        Wire.InputPinName = ResultPinName;
        Wire.UpstreamRef = UpstreamRef;
        Wire.SourceLine = Inst.SourceLine;
        PendingWires.Add(MoveTemp(Wire));
    }
    return FAGIRCompileResult();
}

FAGIRCompileResult ResolvePendingPoseWires(
    const FAGIRSymbolMap& Symbols,
    const FAGIRPendingPoseWires& PendingWires,
    const FString& BodyLabel)
{
    for (const FAGIRPendingPoseWire& Wire : PendingWires)
    {
        FAGIRPinReference Ref;
        if (!FAGIRPinResolver::ParseReference(Wire.UpstreamRef, Ref))
        {
            return FAGIRCompileResult::MakeError(
                TEXT("AGIR_INVALID_PIN_REFERENCE"),
                BodyLabel.IsEmpty()
                    ? FString::Printf(TEXT("Malformed pose reference '%s' (line %d)."),
                        *Wire.UpstreamRef, Wire.SourceLine)
                    : FString::Printf(TEXT("Malformed pose reference '%s' inside %s (line %d)."),
                        *Wire.UpstreamRef, *BodyLabel, Wire.SourceLine));
        }

        UEdGraphNode* UpstreamNode = FAGIRPinResolver::ResolveReference(Ref, Symbols);
        UAnimGraphNode_Base* UpstreamAnim = Cast<UAnimGraphNode_Base>(UpstreamNode);
        if (!UpstreamAnim)
        {
            // Attribution hint: an unresolved pose ref is either user-authored
            // bad AGIR or a decompiler/compiler round-trip gap, and the raw
            // message can't tell them apart — steer the caller to the wiki
            // round-trip-limitations rule instead of re-reading their own text.
            return FAGIRCompileResult::MakeError(
                TEXT("AGIR_SYMBOL_NOT_FOUND"),
                BodyLabel.IsEmpty()
                    ? FString::Printf(TEXT("Pose reference '%s' did not resolve to a UAnimGraphNode_Base (line %d)."),
                        *Wire.UpstreamRef, Wire.SourceLine)
                    : FString::Printf(TEXT("Pose reference '%s' inside %s (line %d) did not resolve."),
                        *Wire.UpstreamRef, *BodyLabel, Wire.SourceLine))
                .WithHint(GAGIRPoseSymbolNotFoundHint);
        }

        const FAGIRPinResolver::FWireResult WireResult = FAGIRPinResolver::WirePoseInput(
                Wire.DownstreamNode,
                Wire.InputPinName,
                UpstreamAnim,
                FName(TEXT("Pose")));
        if (!WireResult.IsSuccess())
        {
            const FString WireErrorCode =
                WireResult.ErrorCode.IsEmpty() ? FString(TEXT("AGIR_POSE_TYPE_MISMATCH")) : WireResult.ErrorCode;
            FAGIRCompileResult WireError = FAGIRCompileResult::MakeError(
                WireErrorCode,
                BodyLabel.IsEmpty()
                    ? FString::Printf(TEXT("Failed to wire pose link '%s' to %s.%s (line %d)."),
                        *Wire.UpstreamRef,
                        *Wire.DownstreamNode->GetName(),
                        *Wire.InputPinName.ToString(),
                        Wire.SourceLine)
                    : FString::Printf(TEXT("Failed to wire pose link '%s' inside %s (line %d)."),
                        *Wire.UpstreamRef, *BodyLabel, Wire.SourceLine));
            // The pin resolver relays AGIR_SYMBOL_NOT_FOUND for a missing pose
            // node — the same unresolved-pose-ref failure class as above, so it
            // carries the round-trip hint too. Type-mismatch / invalid-pin codes
            // do not (the round-trip framing would mislead).
            if (WireErrorCode == TEXT("AGIR_SYMBOL_NOT_FOUND"))
            {
                WireError.WithHint(GAGIRPoseSymbolNotFoundHint);
            }
            return WireError;
        }
    }

    return FAGIRCompileResult();
}

void ResolvePendingCacheLinks(
    const FAGIRPendingCacheLinks& PendingCacheLinks,
    const FAGIRCacheNameMap& CacheNameMap,
    const FString& BodyLabel,
    TArray<FString>& OutWarnings)
{
    for (const FAGIRPendingCacheLink& Link : PendingCacheLinks)
    {
        if (!Link.UseNode)
        {
            continue;
        }
        if (UAnimGraphNode_SaveCachedPose* const* SavePtr = CacheNameMap.Find(Link.CacheName))
        {
            Link.UseNode->SaveCachedPoseNode = *SavePtr;
            continue;
        }

        // No save_cached_pose with this name in the block. Non-fatal: the
        // engine's EarlyValidation re-resolves SaveCachedPoseNode from the
        // serialized NameOfCache against a save in another graph on AnimBP
        // compile, or the use is genuinely dangling and the AnimBP compile
        // legitimately reports the missing save.
        const FString Warning = BodyLabel.IsEmpty()
            ? FString::Printf(
                TEXT("AGIR_CACHED_POSE_UNRESOLVED: use_cached_pose source='%s' has no matching save_cached_pose in this block (line %d)"),
                *Link.CacheName, Link.SourceLine)
            : FString::Printf(
                TEXT("AGIR_CACHED_POSE_UNRESOLVED: use_cached_pose source='%s' has no matching save_cached_pose inside %s (line %d)"),
                *Link.CacheName, *BodyLabel, Link.SourceLine);
        OutWarnings.Add(Warning);
    }
}

FAGIRCompileResult CompilePoseResultBody(
    UAnimBlueprint* AnimBP,
    const TCHAR* BodyName,
    UEdGraph* TargetGraph,
    UAnimGraphNode_Base* ResultNode,
    FName ResultPinName,
    const TArray<TSharedPtr<FAGIRInstruction>>& Children,
    int32 SourceLine,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    const FString BodyLabel = BodyName ? FString(BodyName) : FString(TEXT("graph body"));
    if (!TargetGraph)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            FString::Printf(TEXT("%s graph is null (line %d)."),
                *BodyLabel, SourceLine));
    }

    FAGIRSymbolMap Symbols;
    FAGIRPendingPoseWires PendingWires;
    // Cached-pose name table is block-scoped — a save/use pair inside a state
    // body links within that body only, mirroring how the top-level
    // CompileBlockIntoGraph scopes its own map per AnimGraph block. Use nodes
    // defer their SaveCachedPoseNode linkage to the post-loop
    // ResolvePendingCacheLinks pass so a forward reference (use before save)
    // resolves against the completed map.
    FAGIRCacheNameMap CacheNameMap;
    FAGIRPendingCacheLinks PendingCacheLinks;

    for (const TSharedPtr<FAGIRInstruction>& Child : Children)
    {
        if (!Child.IsValid())
        {
            continue;
        }

        switch (Child->Opcode)
        {
        case EAGIROpcode::Call:
        {
            FAGIRCompileResult CallResult = CompileCallInstruction(
                TargetGraph,
                *Child,
                Symbols,
                PendingWires,
                BodyLabel,
                OutNodesCreated,
                OutWarnings);
            if (!CallResult.ErrorCode.IsEmpty())
            {
                return CallResult;
            }
            break;
        }

        case EAGIROpcode::Output:
        {
            FAGIRCompileResult OutputResult = CompileOutputInstruction(
                TargetGraph,
                *Child,
                ResultNode,
                ResultPinName,
                PendingWires,
                BodyLabel,
                false);
            if (!OutputResult.ErrorCode.IsEmpty())
            {
                return OutputResult;
            }
            break;
        }

        // Cliff opcodes the decompiler can emit inside any pose body (state
        // bodies, custom transition bodies, anim function bodies). Routed
        // through the same per-family handlers EmitInstruction uses at the
        // top level; otherwise the result-name symbol never lands in the
        // local Symbols map and the Pass-2 pose-wire resolver throws
        // AGIR_SYMBOL_NOT_FOUND.
        case EAGIROpcode::LinkedAnim:
        {
            FAGIRCompileResult OutResult;
            if (!AGIRCliff::CompileLinkedAnimInstruction(
                    AnimBP, TargetGraph, *Child, Symbols, PendingWires,
                    OutNodesCreated, OutWarnings, OutResult))
            {
                return OutResult;
            }
            break;
        }

        case EAGIROpcode::LinkedInputPose:
        {
            FAGIRCompileResult OutResult;
            if (!AGIRCliff::CompileLinkedInputPoseInstruction(
                    AnimBP, TargetGraph, *Child, Symbols, PendingWires,
                    OutNodesCreated, OutWarnings, OutResult))
            {
                return OutResult;
            }
            break;
        }

        case EAGIROpcode::BlendSpace:
        {
            FAGIRCompileResult OutResult;
            if (!AGIRCliff::CompileBlendSpaceInstruction(
                    AnimBP, TargetGraph, *Child, Symbols, PendingWires,
                    OutNodesCreated, OutWarnings, OutResult))
            {
                return OutResult;
            }
            break;
        }

        case EAGIROpcode::LayeredBlend:
        {
            FAGIRCompileResult OutResult;
            if (!AGIRCliff::CompileLayeredBlendInstruction(
                    AnimBP, TargetGraph, *Child, Symbols, PendingWires,
                    OutNodesCreated, OutWarnings, OutResult))
            {
                return OutResult;
            }
            break;
        }

        case EAGIROpcode::SaveCachedPose:
        {
            FAGIRCompileResult OutResult;
            if (!AGIRCliff::CompileSaveCachedPoseInstruction(
                    AnimBP, TargetGraph, *Child, Symbols, PendingWires,
                    CacheNameMap, OutNodesCreated, OutWarnings, OutResult))
            {
                return OutResult;
            }
            break;
        }

        case EAGIROpcode::UseCachedPose:
        {
            FAGIRCompileResult OutResult;
            if (!AGIRCliff::CompileUseCachedPoseInstruction(
                    AnimBP, TargetGraph, *Child, Symbols, PendingWires,
                    PendingCacheLinks, OutNodesCreated, OutWarnings, OutResult))
            {
                return OutResult;
            }
            break;
        }

        default:
            OutWarnings.Add(FString::Printf(
                TEXT("AGIR_BAD_OPCODE: opcode '%d' not allowed inside %s (line %d)"),
                static_cast<int32>(Child->Opcode), *BodyLabel, Child->SourceLine));
            break;
        }
    }

    ResolvePendingCacheLinks(PendingCacheLinks, CacheNameMap, BodyLabel, OutWarnings);
    return ResolvePendingPoseWires(Symbols, PendingWires, BodyLabel);
}

FAGIRCompileResult CompileCustomTransitionBody(
    UAnimBlueprint* AnimBP,
    UEdGraph* CustomGraph,
    const FAGIRInstruction& BodyInst,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    if (!CustomGraph)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            FString::Printf(TEXT("Custom transition graph is null (line %d)."),
                BodyInst.SourceLine));
    }

    UAnimGraphNode_CustomTransitionResult* ResultNode = nullptr;
    for (UEdGraphNode* Node : CustomGraph->Nodes)
    {
        if (UAnimGraphNode_CustomTransitionResult* Found = Cast<UAnimGraphNode_CustomTransitionResult>(Node))
        {
            ResultNode = Found;
            break;
        }
    }

    return CompilePoseResultBody(
        AnimBP,
        TEXT("custom_transition_body"),
        CustomGraph,
        ResultNode,
        FName(TEXT("Result")),
        BodyInst.Children,
        BodyInst.SourceLine,
        OutNodesCreated,
        OutWarnings);
}

FAGIRCompileResult CompileAnimGraphBlock(
    UAnimBlueprint* AnimBP,
    const FAGIREntryBlock& Block,
    const FAGIRCompileOptions& Options,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    (void)Options;
    OutNodesCreated = 0;

    UEdGraph* TargetGraph = FindAnimGraphByName(AnimBP, Block.Name);
    if (!TargetGraph)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            FString::Printf(TEXT("Anim graph '%s' not found on %s."),
                *Block.Name, *AnimBP->GetPathName()));
    }

    return CompileBlockIntoGraph(AnimBP, TargetGraph, Block, OutNodesCreated, OutWarnings);
}

FAGIRCompileResult CompileAnimLayerBlock(
    UAnimBlueprint* AnimBP,
    const FAGIREntryBlock& Block,
    const FAGIRCompileOptions& Options,
    TMap<FString, UEdGraph*>& LayerGraphMap,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    (void)Options;
    OutNodesCreated = 0;

    // Anim layer interface override graph: lookup precomputed map (built once
    // per Compile() call by walking ImplementedInterfaces). If the override
    // implementation graph hasn't been materialised yet but an implemented
    // interface declares a function with the matching name, auto-create the
    // implementation graph against that interface so AGIR Replace flows that
    // re-emit a layer can populate it; the new graph is added to the map.
    UEdGraph* const* MapEntry = LayerGraphMap.Find(Block.Name);
    UEdGraph* TargetGraph = MapEntry ? *MapEntry : nullptr;
    if (!TargetGraph)
    {
        UClass* MatchingInterface = nullptr;
        const FName FunctionName(*Block.Name);
        for (const FBPInterfaceDescription& Description : AnimBP->ImplementedInterfaces)
        {
            UClass* InterfaceClass = Description.Interface.Get();
            if (InterfaceClass && FindUField<UFunction>(InterfaceClass, FunctionName))
            {
                MatchingInterface = InterfaceClass;
                break;
            }
        }
        if (!MatchingInterface)
        {
            return FAGIRCompileResult::MakeError(
                TEXT("AGIR_TARGET_NOT_FOUND"),
                FString::Printf(TEXT("no implemented interface declares function '%s'"),
                    *Block.Name));
        }

        TargetGraph = AnimGraphConstructionUtils::CreateAnimLayerInterfaceImplementationGraph(
            AnimBP, MatchingInterface, FunctionName);
        if (!TargetGraph)
        {
            return FAGIRCompileResult::MakeError(
                TEXT("AGIR_TARGET_NOT_FOUND"),
                FString::Printf(TEXT("Failed to create anim layer override graph '%s' on %s."),
                    *Block.Name, *AnimBP->GetPathName()));
        }
        LayerGraphMap.Add(Block.Name, TargetGraph);
    }

    return CompileBlockIntoGraph(AnimBP, TargetGraph, Block, OutNodesCreated, OutWarnings);
}

FAGIRCompileResult CompileAnimFunctionBlock(
    UAnimBlueprint* AnimBP,
    const FAGIREntryBlock& Block,
    const FAGIRCompileOptions& Options,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings)
{
    (void)Options;
    OutNodesCreated = 0;

    // Anim function graphs (anim BP functions returning a pose link) live on
    // FunctionGraphs alongside the top-level AnimGraph; the engine treats
    // both uniformly as UAnimationGraph, so the same lookup-by-name applies.
    UEdGraph* TargetGraph = FindAnimGraphByName(AnimBP, Block.Name);
    if (!TargetGraph)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            FString::Printf(TEXT("Anim function graph '%s' not found on %s."),
                *Block.Name, *AnimBP->GetPathName()));
    }

    return CompileBlockIntoGraph(AnimBP, TargetGraph, Block, OutNodesCreated, OutWarnings);
}

// Replace-mode pre-clear. Removes every authored node from each top-level
// UAnimationGraph reachable from the BP — both FunctionGraphs and per-interface
// implementation graphs — leaving the schema-default UAnimGraphNode_Root in
// place so subsequent compile passes can rebuild the graph contents.
void ClearTargetForReplace(UAnimBlueprint* AnimBP)
{
    AnimGraphConstructionUtils::ClearAnimGraph(AnimBP);
}
}

FAGIRCompileResult FAGIRCompiler::Compile(FStringView Code, const FAGIRCompileOptions& Options)
{
    TArray<FAGIREntryBlock> Blocks;
    TArray<FAGIRParseError> Errors;
    if (!FAGIRParser::Parse(Code, Blocks, Errors))
    {
        const FAGIRParseError& Error = Errors.IsValidIndex(0) ? Errors[0] : FAGIRParseError();
        return FAGIRCompileResult::MakeError(
            Error.Code.IsEmpty() ? TEXT("AGIR_PARSE_ERROR") : Error.Code,
            Error.Message.IsEmpty() ? TEXT("Failed to parse AGIR text.") : Error.Message);
    }

    if (Options.Context.IsEmpty())
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            TEXT("AGIR compile requires a target anim blueprint asset path in Options.Context."));
    }

    // Guarded as defence in depth: Options.Context arrives as the `context` param, which the
    // dispatch gate types `path` and already refuses "//" on - but this compiler is also driven
    // from tests and from internally-composed strings that never crossed that boundary.
    FString ContextRefusal;
    UAnimBlueprint* AnimBP =
        PinWrightGuardedLoad::LoadObjectChecked<UAnimBlueprint>(Options.Context, &ContextRefusal);
    if (!AnimBP)
    {
        return FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            ContextRefusal.IsEmpty()
                ? FString::Printf(TEXT("Could not load anim blueprint at %s. AGIR does not auto-create anim blueprints (USkeleton is not carried in AGIR text)."), *Options.Context)
                : ContextRefusal);
    }

    UPackage* const Package = AnimBP->GetOutermost();
    const bool bPackageWasDirty = Package && Package->IsDirty();
    const BlueprintGraphSnapshot::FBlueprintGraphSnapshot GraphSnapshot =
        BlueprintGraphSnapshot::Capture(AnimBP);

    FAGIRCompileResult Result;
    Result.AssetPath = AnimBP->GetPathName();
    {
        FScopedTransaction Transaction(NSLOCTEXT("AGIRCompiler", "Compile", "Compile AGIR Code"));
        PinWrightTransactionUtils::PrepareTransactionalSnapshot(AnimBP);

        auto RollbackFailure = [&Transaction, &GraphSnapshot, Package, bPackageWasDirty](
            FAGIRCompileResult Failure)
        {
            PinWrightTransactionUtils::ApplyAndCancelTransaction(Transaction);
            BlueprintGraphSnapshot::RollbackToSnapshot(GraphSnapshot);
            if (Package && !bPackageWasDirty)
            {
                Package->SetDirtyFlag(false);
            }
            return Failure;
        };

        // Pre-pass before ClearTargetForReplace and per-block compile: layer
        // resolution and BuildLayerGraphMap need ImplementedInterfaces populated.
        TArray<FString> ManifestWarnings;
        for (const FAGIREntryBlock& Block : Blocks)
        {
            if (Block.Kind != EAGIREntryKind::InterfaceManifest)
            {
                continue;
            }
            for (const TSharedPtr<FAGIRInstruction>& Inst : Block.Instructions)
            {
                if (!Inst.IsValid() || Inst->Opcode != EAGIROpcode::Implements)
                {
                    continue;
                }
                const FString& ClassPath = Inst->SymbolName;
                if (ClassPath.IsEmpty())
                {
                    continue;
                }
                // `implements` carries its class ref in the IR text, through DecodeDelimitedToken,
                // which accepts every character - so this string can hold "//" and reach the Fatal.
                FString InterfaceRefusal;
                UClass* InterfaceClass =
                    PinWrightGuardedLoad::LoadObjectChecked<UClass>(ClassPath, &InterfaceRefusal);
                if (!InterfaceClass)
                {
                    ManifestWarnings.Add(InterfaceRefusal.IsEmpty()
                        ? FString::Printf(
                            TEXT("AGIR_INTERFACE_LOAD_FAILED: could not load interface class '%s'"),
                            *ClassPath)
                        : FString::Printf(TEXT("AGIR_INTERFACE_LOAD_FAILED: %s"), *InterfaceRefusal));
                    continue;
                }

                bool bAlreadyImplemented = false;
                for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
                {
                    if (Desc.Interface == InterfaceClass)
                    {
                        bAlreadyImplemented = true;
                        break;
                    }
                }
                if (bAlreadyImplemented)
                {
                    continue;
                }

                const bool bInterfaceAdded =
                    FBlueprintEditorUtils::ImplementNewInterface(AnimBP, InterfaceClass->GetClassPathName());
                if (!bInterfaceAdded)
                {
                    return RollbackFailure(FAGIRCompileResult::MakeError(
                        TEXT("AGIR_INTERFACE_MUTATION_FAILED"),
                        FString::Printf(TEXT("Failed to implement interface '%s' on %s."),
                            *ClassPath, *AnimBP->GetPathName())));
                }
            }
        }

        if (Options.Mode == EAGIRCompileMode::Replace)
        {
            ClearTargetForReplace(AnimBP);
        }

        Result.Warnings.Append(ManifestWarnings);

        // Hoist the ImplementedInterfaces walk once: anim layer override graphs
        // are looked up by name (and inserted on auto-create) without re-walking
        // the interface descriptions per block.
        TMap<FString, UEdGraph*> LayerGraphMap;
        BuildLayerGraphMap(AnimBP, LayerGraphMap);

        for (const FAGIREntryBlock& Block : Blocks)
        {
            if (Block.Kind == EAGIREntryKind::InterfaceManifest)
            {
                continue;
            }

            int32 BlockNodesCreated = 0;
            FAGIRCompileResult BlockResult;
            switch (Block.Kind)
            {
            case EAGIREntryKind::AnimGraph:
                BlockResult = CompileAnimGraphBlock(AnimBP, Block, Options, BlockNodesCreated, Result.Warnings);
                break;
            case EAGIREntryKind::AnimLayer:
                BlockResult = CompileAnimLayerBlock(AnimBP, Block, Options, LayerGraphMap, BlockNodesCreated, Result.Warnings);
                break;
            case EAGIREntryKind::AnimFunction:
                BlockResult = CompileAnimFunctionBlock(AnimBP, Block, Options, BlockNodesCreated, Result.Warnings);
                break;
            default:
                BlockResult = FAGIRCompileResult::MakeError(
                    TEXT("AGIR_UNSUPPORTED_BLOCK"),
                    TEXT("Unsupported AGIR entry block kind."));
                break;
            }

            if (!BlockResult.ErrorCode.IsEmpty())
            {
                return RollbackFailure(MoveTemp(BlockResult));
            }

            Result.NodesCreated += BlockNodesCreated;
            ++Result.BlocksCompiled;
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

        if (Options.bRunLayout)
        {
            FAGIRLayoutEngine::Layout(AnimBP);
        }
    }

    if (Options.bSave)
    {
        McpSafeAssetSave(AnimBP);
    }

    Result.bSuccess = true;
    return Result;
}
