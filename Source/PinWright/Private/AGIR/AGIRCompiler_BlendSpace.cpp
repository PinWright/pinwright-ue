// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRCliffHandlers.h"


#include "AGIR/AGIRCompilerHelpers.h"
#include "AGIR/AGIROpcodes.h"
#include "AGIR/AGIRPinResolver.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_BlendSpaceGraph.h"
#include "AnimGraphNode_BlendSpaceGraphBase.h"
#include "AnimGraphNode_BlendSpaceSampleResult.h"
#include "AnimationBlendSpaceSampleGraph.h"
#include "AnimationGraphSchema.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/BlendSpace.h"
#include "BlendSpaceGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Utils/GuardedLoad.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

namespace
{
// Pull a single arg by name. Returns nullptr when not present.
const FString* FindArg(const FAGIRInstruction& Inst, const TCHAR* Name)
{
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name == Name)
        {
            return &Arg.Value;
        }
    }
    return nullptr;
}

// Strip surrounding `"..."` so a class path / asset path arg can be loaded.
FString Unquote(const FString& Value)
{
    if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
    {
        return Value.Mid(1, Value.Len() - 2);
    }
    return Value;
}

// Compile one `sample_graph "<name>" { ... }` child into a fresh
// UAnimationBlendSpaceSampleGraph attached to the parent BlendSpaceGraph. The
// sub-graph carries its own pose-wire pass scoped to the sample (Pass 1
// instantiate, Pass 2 wire). The inner `output` (or `%nNN` upstream of the
// sample result) wires into UAnimGraphNode_BlendSpaceSampleResult::Result.
bool CompileSampleGraph(
    UAnimBlueprint* AnimBP,
    UAnimGraphNode_BlendSpaceGraphBase* BlendSpaceNode,
    const FAGIRInstruction& SampleInst,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult)
{
    UBlendSpaceGraph* OwningGraph = BlendSpaceNode->GetBlendSpaceGraph();
    if (!OwningGraph)
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("blend_space node has no owning BlendSpaceGraph (line %d)."),
                SampleInst.SourceLine));
        return false;
    }

    const FName SampleName = SampleInst.SymbolName.IsEmpty()
        ? FName(*FString::Printf(TEXT("Sample_%d"), OwningGraph->SubGraphs.Num()))
        : FName(*SampleInst.SymbolName);

    UAnimationBlendSpaceSampleGraph* SampleGraph = CastChecked<UAnimationBlendSpaceSampleGraph>(
        FBlueprintEditorUtils::CreateNewGraph(
            OwningGraph, SampleName,
            UAnimationBlendSpaceSampleGraph::StaticClass(),
            UAnimationGraphSchema::StaticClass()));

    // Schema-default sink result node. Mirrors the engine's
    // UAnimGraphNode_BlendSpaceGraphBase::AddGraphInternal flow but skips the
    // default sequence-player attachment (AGIR drives node content explicitly).
    FGraphNodeCreator<UAnimGraphNode_BlendSpaceSampleResult> ResultCreator(*SampleGraph);
    UAnimGraphNode_BlendSpaceSampleResult* ResultNode = ResultCreator.CreateNode(/*bSelectNewNode=*/false);
    ResultCreator.Finalize();
    SampleGraph->ResultNode = ResultNode;
    SampleGraph->bAllowDeletion = false;
    SampleGraph->bAllowRenaming = true;

    OwningGraph->Modify();
    OwningGraph->SubGraphs.Add(SampleGraph);

    // Mirror engine convention: BlendSpaceGraphBase tracks both the parent
    // graph's SubGraphs and its own protected `Graphs` array. The protected
    // array is the index source for SamplePoseLinks during AnimBP compile.
    if (FArrayProperty* GraphsProp = FindFProperty<FArrayProperty>(
            BlendSpaceNode->GetClass(), TEXT("Graphs")))
    {
        FScriptArrayHelper Helper(GraphsProp, GraphsProp->ContainerPtrToValuePtr<void>(BlendSpaceNode));
        const int32 NewIndex = Helper.AddValue();
        FObjectProperty* InnerObj = CastFieldChecked<FObjectProperty>(GraphsProp->Inner);
        InnerObj->SetObjectPropertyValue(Helper.GetRawPtr(NewIndex), SampleGraph);
    }

    // Pass 1: instantiate every node inside the sample graph. Args ending in a
    // pose ref (`%nNN`) defer to Pass 2 wiring; reflective fields write through
    // `WriteAnimNodeArg` (mirrors EmitInstruction's `Call` branch).
    AGIRCliff::FAGIRSymbolMap SampleSymbols;
    AGIRCliff::FAGIRPendingPoseWires SamplePending;

    for (const TSharedPtr<FAGIRInstruction>& Child : SampleInst.Children)
    {
        if (!Child.IsValid())
        {
            continue;
        }

        if (Child->Opcode == EAGIROpcode::Call)
        {
            // Guarded: SymbolName is a substring of the caller's AGIR text, which the dispatch
            // boundary cannot type as a path. A raw load on "//" ends the editor process.
            FString ClassRefusal;
            UClass* NodeClass =
                PinWrightGuardedLoad::LoadObjectChecked<UClass>(Child->SymbolName, &ClassRefusal);
            if (!NodeClass || !NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()))
            {
                const FString Detail = ClassRefusal.IsEmpty() ? FString() : (TEXT(" ") + ClassRefusal);
                OutResult = FAGIRCompileResult::MakeError(
                    TEXT("AGIR_CLASS_NOT_FOUND"),
                    FString::Printf(TEXT("Could not load anim node class '%s' inside sample_graph (line %d).%s"),
                        *Child->SymbolName, Child->SourceLine, *Detail));
                return false;
            }

            const FVector2D ChildPos = Child->bHasPosition ? Child->Position : FVector2D::ZeroVector;
            UAnimGraphNode_Base* SampleNode = AnimGraphConstructionUtils::CreateAnimNode(
                SampleGraph, NodeClass, ChildPos);
            if (!SampleNode)
            {
                OutResult = FAGIRCompileResult::MakeError(
                    TEXT("AGIR_NODE_CREATE_FAILED"),
                    FString::Printf(TEXT("Failed to create node '%s' inside sample_graph (line %d)."),
                        *Child->SymbolName, Child->SourceLine));
                return false;
            }

            // Apply the AGIR-side NodeGuid annotation on the child so
            // cross-graph references survive round-trip. Inlined for the same
            // anonymous-namespace reason as the parent blend_space node above.
            // The schema-default UAnimGraphNode_BlendSpaceSampleResult has no
            // AGIR-side GUID and stays untouched.
            if (!Child->NodeGuid.IsEmpty())
            {
                FGuid Parsed;
                if (FGuid::Parse(Child->NodeGuid, Parsed))
                {
                    SampleNode->NodeGuid = Parsed;
                }
            }

            if (!Child->ResultName.IsEmpty())
            {
                SampleSymbols.Add(Child->ResultName, SampleNode);
            }

            for (const FAGIRArg& Arg : Child->Args)
            {
                if (Arg.Name.IsEmpty())
                {
                    continue;
                }
                if (!Arg.Value.IsEmpty() && Arg.Value[0] == TEXT('%'))
                {
                    AGIRCliff::FAGIRPendingPoseWire Wire;
                    Wire.DownstreamNode = SampleNode;
                    Wire.InputPinName = FName(*Arg.Name);
                    Wire.UpstreamRef = Arg.Value;
                    Wire.SourceLine = Child->SourceLine;
                    SamplePending.Add(MoveTemp(Wire));
                    continue;
                }
                const FString WriteError = AGIRCliff::Helpers::WriteAnimNodeArg(
                    SampleNode, FName(*Arg.Name), Arg.Value);
                if (!WriteError.IsEmpty())
                {
                    OutWarnings.Add(FString::Printf(TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
                        *WriteError, Child->SourceLine));
                }
            }
            ++OutNodesCreated;
        }
        else if (Child->Opcode == EAGIROpcode::Output)
        {
            // Wire the upstream pose into the sample result's `Result` pin.
            const FString UpstreamRef = !Child->SymbolName.IsEmpty()
                ? Child->SymbolName
                : (Child->Args.Num() > 0 ? Child->Args[0].Value : FString());
            if (!UpstreamRef.IsEmpty() && UpstreamRef[0] == TEXT('%'))
            {
                AGIRCliff::FAGIRPendingPoseWire Wire;
                Wire.DownstreamNode = ResultNode;
                Wire.InputPinName = FName(TEXT("Result"));
                Wire.UpstreamRef = UpstreamRef;
                Wire.SourceLine = Child->SourceLine;
                SamplePending.Add(MoveTemp(Wire));
            }
        }
        else
        {
            OutWarnings.Add(FString::Printf(
                TEXT("AGIR_BAD_OPCODE: opcode '%d' not allowed inside sample_graph (line %d)"),
                static_cast<int32>(Child->Opcode), Child->SourceLine));
        }
    }

    // Pass 2: resolve pose wires inside the sample graph against the local
    // symbol table (sample-scoped — mirrors top-level CompileBlockIntoGraph).
    for (const AGIRCliff::FAGIRPendingPoseWire& Wire : SamplePending)
    {
        FAGIRPinReference Ref;
        if (!FAGIRPinResolver::ParseReference(Wire.UpstreamRef, Ref))
        {
            OutResult = FAGIRCompileResult::MakeError(
                TEXT("AGIR_INVALID_PIN_REFERENCE"),
                FString::Printf(TEXT("Malformed pose reference '%s' inside sample_graph (line %d)."),
                    *Wire.UpstreamRef, Wire.SourceLine));
            return false;
        }

        UEdGraphNode* UpstreamNode = FAGIRPinResolver::ResolveReference(Ref, SampleSymbols);
        UAnimGraphNode_Base* UpstreamAnim = Cast<UAnimGraphNode_Base>(UpstreamNode);
        if (!UpstreamAnim)
        {
            // Same unresolved-pose-ref failure class as the top-level wire site —
            // carry the shared round-trip hint so a sample-graph round-trip gap is
            // attributable on the tool surface (E-agir-roundtrip-symbol-not-found-undiagnosable).
            OutResult = FAGIRCompileResult::MakeError(
                TEXT("AGIR_SYMBOL_NOT_FOUND"),
                FString::Printf(TEXT("Pose reference '%s' inside sample_graph (line %d) did not resolve."),
                    *Wire.UpstreamRef, Wire.SourceLine))
                .WithHint(AGIRCliff::GAGIRPoseSymbolNotFoundHint);
            return false;
        }

        const FAGIRPinResolver::FWireResult WireResult = FAGIRPinResolver::WirePoseInput(
                Wire.DownstreamNode,
                Wire.InputPinName,
                UpstreamAnim,
                FName(TEXT("Pose")));
        if (!WireResult.IsSuccess())
        {
            OutResult = FAGIRCompileResult::MakeError(
                WireResult.ErrorCode.IsEmpty() ? TEXT("AGIR_POSE_TYPE_MISMATCH") : WireResult.ErrorCode,
                FString::Printf(TEXT("Failed to wire pose link '%s' inside sample_graph (line %d)."),
                    *Wire.UpstreamRef, Wire.SourceLine));
            return false;
        }
    }

    (void)AnimBP;
    return true;
}
}  // namespace

bool AGIRCliff::CompileBlendSpaceInstruction(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& /*PendingWires*/,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult)
{
    // Resolve the editor node class. AGIR's blend_space header carries the
    // display name in SymbolName (per parser bLeadsWithName). The concrete
    // class can vary (UAnimGraphNode_BlendSpaceGraph / AimOffsetLookAt /
    // RotationOffsetBlendSpace) — accept an optional `class=` arg, default to
    // the standard non-template variant. Wave 3 round-trip emission may add
    // class= explicitly for the variants.
    UClass* NodeClass = UAnimGraphNode_BlendSpaceGraph::StaticClass();
    if (const FString* ClassArg = FindArg(Inst, TEXT("class")))
    {
        // `class=` needs no quoting at all and Unquote() returns its input verbatim, so this is
        // raw caller text arriving at a load. The repro on board
        // B-ir-source-class-refs-reach-createpackage-fatal is exactly this line.
        FString ClassRefusal;
        UClass* Loaded = PinWrightGuardedLoad::LoadObjectChecked<UClass>(
            Unquote(*ClassArg), &ClassRefusal);
        if (!Loaded || !Loaded->IsChildOf(UAnimGraphNode_BlendSpaceGraphBase::StaticClass()))
        {
            const FString Detail = ClassRefusal.IsEmpty() ? FString() : (TEXT(" ") + ClassRefusal);
            OutResult = FAGIRCompileResult::MakeError(
                TEXT("AGIR_CLASS_NOT_FOUND"),
                FString::Printf(TEXT("blend_space class '%s' is not a UAnimGraphNode_BlendSpaceGraphBase subclass (line %d).%s"),
                    **ClassArg, Inst.SourceLine, *Detail));
            return false;
        }
        NodeClass = Loaded;
    }

    const FVector2D Position = Inst.bHasPosition ? Inst.Position : FVector2D::ZeroVector;
    UAnimGraphNode_BlendSpaceGraphBase* EditorNode = Cast<UAnimGraphNode_BlendSpaceGraphBase>(
        AnimGraphConstructionUtils::CreateAnimNode(TargetGraph, NodeClass, Position));
    if (!EditorNode)
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create blend_space node '%s' on graph '%s' (line %d)."),
                *Inst.SymbolName, *TargetGraph->GetName(), Inst.SourceLine));
        return false;
    }

    // Apply the AGIR-side NodeGuid annotation so cross-graph pointer fields
    // (e.g. K2Node_TransitionRuleGetter) can resolve back to this node after
    // round-trip. Mirrors the helper at AGIRCompiler.cpp:127-138 — inlined here
    // because that helper lives in an anonymous namespace and is unreachable
    // from this TU.
    if (!Inst.NodeGuid.IsEmpty())
    {
        FGuid Parsed;
        if (FGuid::Parse(Inst.NodeGuid, Parsed))
        {
            EditorNode->NodeGuid = Parsed;
        }
    }

    // Engine convention: BlendSpace + BlendSpaceGraph protected UPROPERTYs are
    // populated by SetupFromAsset / PostPlacedNewNode. Compile path bypasses
    // both — we own sample sub-graph creation, so just create the dummy
    // BlendSpaceGraph parent with the bare UEdGraphSchema (mirrors
    // SetupFromAsset's `UEdGraphSchema::StaticClass()` arg, NOT the AnimGraph
    // schema; see plan Decisions section).
    if (FObjectProperty* BlendSpaceGraphProp = FindFProperty<FObjectProperty>(
            EditorNode->GetClass(), TEXT("BlendSpaceGraph")))
    {
        if (!BlendSpaceGraphProp->GetObjectPropertyValue_InContainer(EditorNode))
        {
            UBlendSpaceGraph* NewBlendSpaceGraph = CastChecked<UBlendSpaceGraph>(
                FBlueprintEditorUtils::CreateNewGraph(
                    EditorNode, NAME_None,
                    UBlendSpaceGraph::StaticClass(),
                    UEdGraphSchema::StaticClass()));
            BlendSpaceGraphProp->SetObjectPropertyValue_InContainer(EditorNode, NewBlendSpaceGraph);

            // Mirror the engine: register the BlendSpaceGraph as a sub-graph of
            // the parent so editor navigation and ParentGraph->SubGraphs stay
            // consistent with what SetupFromAsset would have produced.
            TargetGraph->Modify();
            if (TargetGraph->SubGraphs.Find(NewBlendSpaceGraph) == INDEX_NONE)
            {
                TargetGraph->SubGraphs.Add(NewBlendSpaceGraph);
            }
        }
    }

    // Optional asset arg: `asset="/Game/.../BS_Foo.BS_Foo"`. The engine
    // `BlendSpace` field is protected, so reach it reflectively. We do NOT
    // duplicate the asset (that's the SetupFromAsset behaviour). External
    // reference is enough for AGIR fidelity; the runtime compile pulls
    // SamplePoseLinks from the editor sub-graphs we wire up below.
    if (const FString* AssetArg = FindArg(Inst, TEXT("asset")))
    {
        const FString AssetPath = Unquote(*AssetArg);
        if (!AssetPath.IsEmpty())
        {
            // `asset=` is caller text too; guarded for the same reason as `class=` above.
            FString AssetRefusal;
            UBlendSpace* AssetObj =
                PinWrightGuardedLoad::LoadObjectChecked<UBlendSpace>(AssetPath, &AssetRefusal);
            if (AssetObj)
            {
                if (FObjectProperty* BlendSpaceProp = FindFProperty<FObjectProperty>(
                        EditorNode->GetClass(), TEXT("BlendSpace")))
                {
                    BlendSpaceProp->SetObjectPropertyValue_InContainer(EditorNode, AssetObj);
                }
            }
            else
            {
                OutWarnings.Add(AssetRefusal.IsEmpty()
                    ? FString::Printf(
                        TEXT("AGIR_FIELD_WRITE: blend_space asset '%s' did not load (line %d)"),
                        *AssetPath, Inst.SourceLine)
                    : FString::Printf(TEXT("AGIR_FIELD_WRITE: blend_space asset (line %d): %s"),
                        Inst.SourceLine, *AssetRefusal));
            }
        }
    }

    // Walk children: every `sample_graph` child compiles into a fresh
    // UAnimationBlendSpaceSampleGraph attached to the dummy parent graph. The
    // BlendSpaceSampleGraph opcode is a Wave 2 addition (see AGIROpcodes.h);
    // children of any other shape are flagged via OutWarnings rather than
    // silently dropped so the test surface catches grammar drift.
    for (const TSharedPtr<FAGIRInstruction>& Child : Inst.Children)
    {
        if (!Child.IsValid())
        {
            continue;
        }
        if (Child->Opcode == EAGIROpcode::BlendSpaceSampleGraph)
        {
            if (!CompileSampleGraph(AnimBP, EditorNode, *Child, OutNodesCreated, OutWarnings, OutResult))
            {
                return false;
            }
        }
        else
        {
            OutWarnings.Add(FString::Printf(
                TEXT("AGIR_BAD_OPCODE: opcode '%d' not allowed inside blend_space block (line %d)"),
                static_cast<int32>(Child->Opcode), Child->SourceLine));
        }
    }

    // Apply remaining reflected args (sync group, etc.) via WriteAnimNodeArg.
    // Skip the keys we consumed above so they don't trip the reflective writer.
    static const TSet<FString> IgnoredKeys = { TEXT("class"), TEXT("asset"), TEXT("guid") };
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty() || IgnoredKeys.Contains(Arg.Name))
        {
            continue;
        }
        // Pose-ref args on blend_space are unusual (X/Y are scalar pins, not
        // pose links); route everything through WriteAnimNodeArg.
        const FString WriteError = AGIRCliff::Helpers::WriteAnimNodeArg(
            EditorNode, FName(*Arg.Name), Arg.Value);
        if (!WriteError.IsEmpty())
        {
            OutWarnings.Add(FString::Printf(TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
                *WriteError, Inst.SourceLine));
        }
    }

    if (!Inst.ResultName.IsEmpty())
    {
        Symbols.Add(Inst.ResultName, EditorNode);
    }
    ++OutNodesCreated;
    return true;
}
