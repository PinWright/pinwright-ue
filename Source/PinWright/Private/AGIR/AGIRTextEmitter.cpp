// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRTextEmitter.h"


#include "AGIR/AGIRGrammar.h"
#include "AGIR/AGIRPinBindings.h"
#include "IrCore/IrTextUtils.h"

#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

#include "AnimGraphNode_Base.h"
#include "Handlers/Animation/AnimGraphNodeAccessor.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_BlendSpaceGraphBase.h"
#include "AnimGraphNode_BlendSpaceSampleResult.h"
#include "AnimGraphNode_CustomTransitionResult.h"
#include "Animation/BlendSpace.h"
#include "AnimationBlendSpaceSampleGraph.h"
#include "AnimationCustomTransitionGraph.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_LinkedAnimGraphBase.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "Animation/AnimStateMachineTypes.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "Compat/EngineVersionCompat.h"
#include "BlendSpaceGraph.h"
#include "EdGraph/EdGraph.h"

namespace
{
FString Quote(const FString& Value)
{
    return FIrTextUtils::Quote(Value);
}

FString NameToken(const FString& Value)
{
    return FIrTextUtils::FormatNameToken(Value);
}

FString CamelToSnake(const FString& Value)
{
    return FIrTextUtils::CamelToSnakeIdentifier(Value);
}

FString LocalIdMnemonicForNode(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return TEXT("node");
    }

    if (Node->IsA<UAnimGraphNode_StateMachineBase>())
    {
        return TEXT("state_machine");
    }
    if (Node->IsA<UAnimGraphNode_BlendSpaceGraphBase>())
    {
        return TEXT("blend_space");
    }
    if (Node->IsA<UAnimGraphNode_LayeredBoneBlend>())
    {
        return TEXT("layered_blend");
    }
    if (Node->IsA<UAnimGraphNode_LinkedAnimGraphBase>())
    {
        return TEXT("linked_anim");
    }
    if (Node->IsA<UAnimGraphNode_LinkedInputPose>())
    {
        return TEXT("linked_input_pose");
    }
    if (Node->IsA<UAnimGraphNode_SaveCachedPose>())
    {
        return TEXT("save_cached_pose");
    }
    if (Node->IsA<UAnimGraphNode_UseCachedPose>())
    {
        return TEXT("use_cached_pose");
    }
    if (Node->IsA<UAnimGraphNode_Root>())
    {
        return TEXT("output");
    }

    UClass* NodeClass = Node->GetClass();
    FString ClassName = NodeClass ? NodeClass->GetName() : TEXT("node");
    ClassName.RemoveFromStart(TEXT("AnimGraphNode_"));
    ClassName.RemoveFromEnd(TEXT("_C"));
    return CamelToSnake(ClassName);
}

// Returns true for FProperty types whose runtime value would conflict with
// AGIR's pose-link emission path. FPoseLink-family struct fields are wired by
// walking `UEdGraphPin::LinkedTo` because the runtime LinkID is not populated
// pre-compile.
bool IsPoseLinkStructProperty(const FStructProperty* StructProp)
{
    if (!StructProp || !StructProp->Struct)
    {
        return false;
    }
    const FName StructName = StructProp->Struct->GetFName();
    return StructName == FName(TEXT("PoseLink"))
        || StructName == FName(TEXT("ComponentSpacePoseLink"))
        || StructName == FName(TEXT("PoseLinkBase"));
}

FString FormatPositionSuffix(const UEdGraphNode* Node)
{
    return FIrTextUtils::FormatPositionSuffix(
        Node ? Node->NodePosX : 0,
        Node ? Node->NodePosY : 0);
}

FString FormatGuidAnnotation(const UEdGraphNode* Node)
{
    if (!Node || !Node->NodeGuid.IsValid())
    {
        return FString();
    }
    return FString::Printf(TEXT(" guid=%s"), *Quote(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
}

FString FormatFieldList(const TArray<FString>& Fields)
{
    return FIrTextUtils::FormatFieldList(Fields);
}

// Convention: pose-link arrays expose their editor pin as `<FieldName>_<Index>`
// (e.g. `BlendPose_0`). This helper walks the editor pin list looking for the
// array form, falling back to the bare field name.
UEdGraphPin* FindPoseInputPin(UAnimGraphNode_Base* Node, FName FieldName, int32 ArrayIndex)
{
    if (!Node)
    {
        return nullptr;
    }
    const FString IndexedName = (ArrayIndex >= 0)
        ? FString::Printf(TEXT("%s_%d"), *FieldName.ToString(), ArrayIndex)
        : FieldName.ToString();
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input)
        {
            continue;
        }
        if (Pin->PinName.ToString() == IndexedName || Pin->PinName == FieldName)
        {
            return Pin;
        }
    }
    return nullptr;
}
} // namespace

FAGIRTextEmitter::FAGIRTextEmitter(UAnimBlueprint* InAnimBP)
    : AnimBlueprint(InAnimBP)
{
}

FString FAGIRTextEmitter::MakeLocalId(const FString& Mnemonic, int32 Counter)
{
    const FString SanitizedMnemonic = CamelToSnake(Mnemonic);
    return FString::Printf(TEXT("%%%s_%d"), *SanitizedMnemonic, Counter);
}

FString FAGIRTextEmitter::IdFor(UEdGraphNode* Node)
{
    if (!Node)
    {
        return FString();
    }
    if (const FString* Existing = LocalIds.Find(Node))
    {
        return *Existing;
    }
    const FString Mnemonic = LocalIdMnemonicForNode(Node);
    int32& Counter = LocalMnemonicCounters.FindOrAdd(Mnemonic);
    const FString NewId = MakeLocalId(Mnemonic, Counter++);
    LocalIds.Add(Node, NewId);
    return NewId;
}

void FAGIRTextEmitter::AssignLocalIds(UEdGraph* Graph)
{
    if (!Graph)
    {
        return;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Cast<UAnimGraphNode_Base>(Node))
        {
            IdFor(Node);
        }
    }
}

FString FAGIRTextEmitter::EmitGraph(UEdGraph* Graph, EAGIREntryKind Kind)
{
    if (!Graph)
    {
        return FString();
    }

    LocalIds.Reset();
    LocalMnemonicCounters.Reset();
    AssignLocalIds(Graph);

    TArray<FString> Lines;
    Lines.Add(FString::Printf(
        TEXT("entry %s %s {"),
        EntryKindToText(Kind),
        *NameToken(Graph->GetName())));

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
        if (!AnimNode)
        {
            continue;
        }
        const FString Line = EmitNode(AnimNode);
        if (!Line.IsEmpty())
        {
            Lines.Add(TEXT("    ") + Line);
        }
    }

    Lines.Add(TEXT("}"));
    return FString::Join(Lines, TEXT("\n"));
}

FString FAGIRTextEmitter::EmitNode(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    if (UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node))
    {
        return EmitStateMachine(MachineNode);
    }
    if (Node->IsA<UAnimGraphNode_BlendSpaceGraphBase>())
    {
        return EmitBlendSpaceGraph(Node);
    }
    if (Node->IsA<UAnimGraphNode_LayeredBoneBlend>())
    {
        return EmitLayeredBlend(Node);
    }
    if (Node->IsA<UAnimGraphNode_LinkedAnimGraphBase>())
    {
        return EmitLinkedAnim(Node);
    }
    if (Node->IsA<UAnimGraphNode_LinkedInputPose>())
    {
        return EmitLinkedInputPose(Node);
    }
    if (Node->IsA<UAnimGraphNode_SaveCachedPose>())
    {
        return EmitSaveCachedPose(Node);
    }
    if (Node->IsA<UAnimGraphNode_UseCachedPose>())
    {
        return EmitUseCachedPose(Node);
    }
    if (Node->IsA<UAnimGraphNode_Root>())
    {
        return EmitOutput(Node);
    }
    return EmitGenericCall(Node);
}

FString FAGIRTextEmitter::FormatNodeAnnotation(UEdGraphNode* Node) const
{
    return FormatGuidAnnotation(Node) + FormatPositionSuffix(Node);
}

void FAGIRTextEmitter::AppendNodeFields(
    UAnimGraphNode_Base* Node, TArray<FString>& OutFields, bool bIncludePoseLinks)
{
    if (bIncludePoseLinks)
    {
        AppendPoseLinkFields(Node, OutFields);
    }

    TSet<FName> BoundPins;
    AGIRPinBindings::AppendBindingFields(Node, OutFields, BoundPins, Warnings);
    AppendReflectedNodeFields(Node, BoundPins, OutFields);
}

void FAGIRTextEmitter::AppendReflectedNodeFields(
    UAnimGraphNode_Base* Node,
    const TSet<FName>& SuppressedProperties,
    TArray<FString>& OutFields) const
{
    if (!Node)
    {
        return;
    }

    FStructProperty* NodeStructProp = PinWright::Anim::GetFNodeProperty(Node);
    if (!NodeStructProp || !NodeStructProp->Struct)
    {
        return;
    }

    const void* NodeData = NodeStructProp->ContainerPtrToValuePtr<void>(Node);
    if (!NodeData)
    {
        return;
    }

    UObject* DefaultNode = Node->GetClass()->GetDefaultObject();
    const void* DefaultData = (DefaultNode != nullptr)
        ? NodeStructProp->ContainerPtrToValuePtr<void>(DefaultNode)
        : nullptr;

    const TSet<FName> ExplicitProperties;
    FReflectedFieldEmitOptions Options;
    Options.FieldSeparator = TEXT(": ");
    Options.bEmitArraysAsBracketList = false;
    FIrTextUtils::AppendReflectedFields(
        NodeStructProp->Struct,
        NodeData,
        DefaultData,
        Node,
        ExplicitProperties,
        [](const FStructProperty* StructProp)
        {
            return IsPoseLinkStructProperty(StructProp);
        },
        [&SuppressedProperties](FName PropertyName)
        {
            return SuppressedProperties.Contains(PropertyName);
        },
        Options,
        OutFields);
}

void FAGIRTextEmitter::AppendPoseLinkFields(UAnimGraphNode_Base* Node, TArray<FString>& OutFields)
{
    if (!Node)
    {
        return;
    }

    FStructProperty* NodeStructProp = PinWright::Anim::GetFNodeProperty(Node);
    if (!NodeStructProp || !NodeStructProp->Struct)
    {
        return;
    }

    // Iterate runtime struct properties looking for FPoseLink / FComponentSpacePoseLink
    // (single) and TArray of those (multi-pose). Resolve the editor pin via name
    // convention (`Field` for singletons, `Field_N` for array entries) and walk
    // `LinkedTo[0]` to find the upstream UAnimGraphNode_Base.
    for (TFieldIterator<FProperty> It(NodeStructProp->Struct); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property)
        {
            continue;
        }

        if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
        {
            if (!IsPoseLinkStructProperty(StructProp))
            {
                continue;
            }
            UEdGraphPin* Pin = FindPoseInputPin(Node, Property->GetFName(), -1);
            if (!Pin || Pin->LinkedTo.Num() == 0 || !Pin->LinkedTo[0])
            {
                continue;
            }
            UEdGraphNode* UpstreamNode = Pin->LinkedTo[0]->GetOwningNode();
            if (const FString* Id = UpstreamNode ? LocalIds.Find(UpstreamNode) : nullptr)
            {
                OutFields.Add(FString::Printf(TEXT("%s: %s"), *Property->GetName(), **Id));
            }
            continue;
        }

        if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
        {
            FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner);
            if (!InnerStruct || !IsPoseLinkStructProperty(InnerStruct))
            {
                continue;
            }
            // Use editor-pin scan: pose-array pins use `<Field>_<Index>` convention.
            const FString FieldNameStr = Property->GetName();
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input)
                {
                    continue;
                }
                const FString PinNameStr = Pin->PinName.ToString();
                if (!PinNameStr.StartsWith(FieldNameStr + TEXT("_")))
                {
                    continue;
                }
                if (Pin->LinkedTo.Num() == 0 || !Pin->LinkedTo[0])
                {
                    continue;
                }
                UEdGraphNode* UpstreamNode = Pin->LinkedTo[0]->GetOwningNode();
                if (const FString* Id = UpstreamNode ? LocalIds.Find(UpstreamNode) : nullptr)
                {
                    OutFields.Add(FString::Printf(TEXT("%s: %s"), *PinNameStr, **Id));
                }
            }
        }
    }
}

FString FAGIRTextEmitter::EmitGenericCall(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    TArray<FString> Fields;
    AppendNodeFields(Node, Fields, /*bIncludePoseLinks=*/true);

    return FString::Printf(
        TEXT("%s = call %s%s%s"),
        *IdFor(Node),
        *NameToken(Node->GetClass()->GetPathName()),
        *FormatFieldList(Fields),
        *FormatNodeAnnotation(Node));
}

FString FAGIRTextEmitter::EmitBlendSpaceGraph(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    UAnimGraphNode_BlendSpaceGraphBase* BlendSpaceNode = Cast<UAnimGraphNode_BlendSpaceGraphBase>(Node);
    TArray<FString> Fields;
    AppendNodeFields(Node, Fields, /*bIncludePoseLinks=*/true);

    // Asset reference is owned by the editor node, not the runtime struct;
    // surface it explicitly so AGIR captures the link even though reflection
    // skips editor-only fields.
    FString AssetClause;
    if (BlendSpaceNode)
    {
        // BlendSpace is a protected UPROPERTY with no public C++ accessor as of
        // UE 5.6; reach it through reflection rather than friend-classing around
        // the protection. Abort loudly if the field name moves under us — a
        // silent miss would emit an empty `blend_space` block that round-trips
        // wrong without any signal.
        FObjectProperty* BlendSpaceProp = FindFProperty<FObjectProperty>(BlendSpaceNode->GetClass(), TEXT("BlendSpace"));
        check(BlendSpaceProp);
        if (UObject* BlendSpaceAsset = BlendSpaceProp->GetObjectPropertyValue_InContainer(BlendSpaceNode))
        {
            AssetClause = FString::Printf(TEXT(" asset=%s"), *Quote(BlendSpaceAsset->GetPathName()));
        }
    }

    TArray<FString> Lines;
    Lines.Add(FString::Printf(
        TEXT("blend_space %s%s%s {%s"),
        *NameToken(Node->GetNodeTitle(ENodeTitleType::ListView).ToString()),
        *AssetClause,
        *FormatFieldList(Fields),
        *FormatNodeAnnotation(Node)));

    // Walk owned sample sub-graphs. The parent UBlendSpaceGraph carries the
    // sample graphs as `SubGraphs` (mirrors the protected `Graphs` array on
    // BlendSpaceGraphBase, which the engine indexes for SamplePoseLinks).
    auto AppendIndented = [&Lines](const FString& Inner)
    {
        if (Inner.IsEmpty())
        {
            return;
        }
        TArray<FString> InnerLines;
        Inner.ParseIntoArray(InnerLines, TEXT("\n"), false);
        for (const FString& InnerLine : InnerLines)
        {
            Lines.Add(TEXT("    ") + InnerLine);
        }
    };

    UBlendSpaceGraph* OwningGraph = BlendSpaceNode ? BlendSpaceNode->GetBlendSpaceGraph() : nullptr;
    if (OwningGraph)
    {
        for (UEdGraph* SubGraph : OwningGraph->SubGraphs)
        {
            UAnimationBlendSpaceSampleGraph* SampleGraph = Cast<UAnimationBlendSpaceSampleGraph>(SubGraph);
            if (!SampleGraph)
            {
                continue;
            }
            AppendIndented(EmitBlendSpaceSampleGraph(SampleGraph));
        }
    }

    Lines.Add(TEXT("}"));
    return FString::Join(Lines, TEXT("\n"));
}

FString FAGIRTextEmitter::EmitBlendSpaceSampleGraph(UAnimationBlendSpaceSampleGraph* SampleGraph)
{
    if (!SampleGraph)
    {
        return FString();
    }

    TArray<FString> Lines;
    Lines.Add(FString::Printf(TEXT("sample_graph %s {"),
        *NameToken(SampleGraph->GetName())));

    const FString BodyText = EmitPoseResultBodyGraph(
        SampleGraph,
        UAnimGraphNode_BlendSpaceSampleResult::StaticClass());
    if (!BodyText.IsEmpty())
    {
        TArray<FString> BodyLines;
        BodyText.ParseIntoArray(BodyLines, TEXT("\n"), false);
        for (const FString& BodyLine : BodyLines)
        {
            Lines.Add(TEXT("    ") + BodyLine);
        }
    }

    Lines.Add(TEXT("}"));
    return FString::Join(Lines, TEXT("\n"));
}

FString FAGIRTextEmitter::EmitLayeredBlend(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    TArray<FString> Fields;
    AppendNodeFields(Node, Fields, /*bIncludePoseLinks=*/true);

    return FString::Printf(
        TEXT("%s = layered_blend %s%s%s"),
        *IdFor(Node),
        *NameToken(Node->GetName()),
        *FormatFieldList(Fields),
        *FormatNodeAnnotation(Node));
}

FString FAGIRTextEmitter::EmitLinkedAnim(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    // `InstanceClass` and the layer name surface through reflection on the
    // runtime FAnimNode_LinkedAnimGraph struct — no extra editor-side lookup.
    TArray<FString> Fields;
    AppendNodeFields(Node, Fields, /*bIncludePoseLinks=*/true);

    return FString::Printf(
        TEXT("%s = linked_anim%s%s"),
        *IdFor(Node),
        *FormatFieldList(Fields),
        *FormatNodeAnnotation(Node));
}

FString FAGIRTextEmitter::EmitLinkedInputPose(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    TArray<FString> Fields;
    AppendNodeFields(Node, Fields, /*bIncludePoseLinks=*/false);

    // `Inputs`, `InputPoseIndex`, and `FunctionReference` are class-level
    // UPROPERTY on UAnimGraphNode_LinkedInputPose itself, not on the runtime
    // FAnimNode_LinkedInputPose struct walked by AppendReflectedNodeFields.
    // The compiler's TryWriteEditorClassField fallback consumes them via
    // ImportText_Direct on the editor class, so emit the CDO-delta of those
    // editor-class fields here to round-trip.
    UClass* Class = Node->GetClass();
    UObject* DefaultObject = Class ? Class->GetDefaultObject() : nullptr;
    if (Class && DefaultObject)
    {
        static const FName EditorFieldNames[] = {
            FName(TEXT("Inputs")),
            FName(TEXT("InputPoseIndex")),
            FName(TEXT("FunctionReference")),
        };
        for (const FName& FieldName : EditorFieldNames)
        {
            FProperty* Property = Class->FindPropertyByName(FieldName);
            if (!Property)
            {
                continue;
            }
            const void* InstanceValue = Property->ContainerPtrToValuePtr<void>(Node);
            const void* DefaultValue = Property->ContainerPtrToValuePtr<void>(DefaultObject);
            if (Property->Identical(InstanceValue, DefaultValue, PPF_None))
            {
                continue;
            }
            FString Exported;
            // Pass the already-computed CDO default (not nullptr) so struct-valued
            // editor-class fields (Inputs, FunctionReference) diff each sub-field
            // against its per-field archetype default instead of the type zero-value.
            // With nullptr, a non-default sub-field equal to its zero-value is dropped
            // and a sub-field left at a non-zero default is spuriously emitted — the
            // same inversion IrTextUtils was fixed for (B-decompile-struct-subfield-dropped).
            Property->ExportTextItem_Direct(Exported, InstanceValue, DefaultValue, Node, PPF_None);
            Fields.Add(FString::Printf(TEXT("%s: %s"),
                *Property->GetName(), *Quote(Exported)));
        }
    }

    return FString::Printf(
        TEXT("%s = linked_input_pose %s%s%s"),
        *IdFor(Node),
        *NameToken(Node->GetName()),
        *FormatFieldList(Fields),
        *FormatNodeAnnotation(Node));
}

FString FAGIRTextEmitter::EmitSaveCachedPose(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(Node);
    TArray<FString> Fields;
    AppendNodeFields(Node, Fields, /*bIncludePoseLinks=*/true);

    const FString CacheName = SaveNode ? SaveNode->CacheName : FString();
    return FString::Printf(
        TEXT("%s = save_cached_pose name=%s%s%s"),
        *IdFor(Node),
        *NameToken(CacheName),
        *FormatFieldList(Fields),
        *FormatNodeAnnotation(Node));
}

FString FAGIRTextEmitter::EmitUseCachedPose(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    UAnimGraphNode_UseCachedPose* UseNode = Cast<UAnimGraphNode_UseCachedPose>(Node);
    TArray<FString> Fields;
    AppendNodeFields(Node, Fields, /*bIncludePoseLinks=*/false);

    // Linkage to the SaveCachedPose node is by CacheName (FString); the weak
    // pointer is intentionally not emitted (per Decisions section).
    FString SourceName;
    if (UseNode && UseNode->SaveCachedPoseNode.IsValid())
    {
        SourceName = UseNode->SaveCachedPoseNode->CacheName;
    }

    return FString::Printf(
        TEXT("%s = use_cached_pose source=%s%s%s"),
        *IdFor(Node),
        *NameToken(SourceName),
        *FormatFieldList(Fields),
        *FormatNodeAnnotation(Node));
}

FString FAGIRTextEmitter::EmitOutput(UAnimGraphNode_Base* Node)
{
    if (!Node)
    {
        return FString();
    }

    TArray<FString> Fields;
    AppendPoseLinkFields(Node, Fields);

    // Output is a terminator; pose input is the only meaningful payload. Pick
    // the single connected upstream id (first pose input) to print as the bare
    // reference for readability, and leave reflected fields out (Result struct
    // only carries the link).
    FString PoseRef;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() == 0 || !Pin->LinkedTo[0])
        {
            continue;
        }
        UEdGraphNode* Upstream = Pin->LinkedTo[0]->GetOwningNode();
        if (const FString* Id = Upstream ? LocalIds.Find(Upstream) : nullptr)
        {
            PoseRef = *Id;
            break;
        }
    }

    return FString::Printf(
        TEXT("output %s%s"),
        *PoseRef,
        *FormatNodeAnnotation(Node));
}

FString FAGIRTextEmitter::EmitStateMachine(UAnimGraphNode_StateMachineBase* MachineNode)
{
    if (!MachineNode)
    {
        return FString();
    }

    UAnimationStateMachineGraph* InnerGraph = MachineNode->EditorStateMachineGraph;
    const FString MachineName = InnerGraph ? InnerGraph->GetName() : MachineNode->GetName();

    // Pre-allocate the AGIR-local id so downstream pose-link refs (e.g. an
    // Output node consuming this state machine's pose) resolve via LocalIds,
    // but do NOT write `%n = ` to the text — the parser only accepts the
    // unassigned `state_machine <name-token> {` opener form (4A).
    (void)IdFor(MachineNode);

    TArray<FString> Lines;
    Lines.Add(FString::Printf(
        TEXT("state_machine %s {%s"),
        *NameToken(MachineName),
        *FormatNodeAnnotation(MachineNode)));

    // Append `Inner` indented by 4 spaces per line. EmitState now returns a
    // multi-line block (opener / attrs / `}`), so prefixing only the first
    // line would leave the rest at column 0.
    auto AppendIndented = [&Lines](const FString& Inner)
    {
        if (Inner.IsEmpty())
        {
            return;
        }
        TArray<FString> InnerLines;
        Inner.ParseIntoArray(InnerLines, TEXT("\n"), false);
        for (const FString& InnerLine : InnerLines)
        {
            Lines.Add(TEXT("    ") + InnerLine);
        }
    };

    if (InnerGraph)
    {
        // Emit states + conduits + aliases first so transitions can reference
        // them by name even when the editor node order interleaves the families.
        for (UEdGraphNode* InnerNode : InnerGraph->Nodes)
        {
            if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(InnerNode))
            {
                AppendIndented(EmitState(StateNode));
            }
            else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(InnerNode))
            {
                AppendIndented(EmitConduit(ConduitNode));
            }
            else if (UAnimStateAliasNode* AliasNode = Cast<UAnimStateAliasNode>(InnerNode))
            {
                AppendIndented(EmitStateAlias(AliasNode));
            }
        }

        for (UEdGraphNode* InnerNode : InnerGraph->Nodes)
        {
            if (UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(InnerNode))
            {
                AppendIndented(EmitTransition(TransitionNode));
            }
        }
    }

    Lines.Add(TEXT("}"));
    return FString::Join(Lines, TEXT("\n"));
}

FString FAGIRTextEmitter::EmitState(UAnimStateNode* StateNode)
{
    if (!StateNode)
    {
        return FString();
    }

    const FString StateName = StateNode->BoundGraph
        ? StateNode->BoundGraph->GetName()
        : StateNode->GetStateName();

    TArray<FString> Attributes;
    if (StateNode->bAlwaysResetOnEntry)
    {
        Attributes.Add(TEXT("always_reset_on_entry=true"));
    }

    const FString AttributeText = Attributes.Num() > 0
        ? FString(TEXT(" ")) + FString::Join(Attributes, TEXT(" "))
        : FString();

    TArray<FString> Lines;
    Lines.Add(FString::Printf(
        TEXT("state %s%s {%s"),
        *NameToken(StateName),
        *AttributeText,
        *FormatNodeAnnotation(StateNode)));
    const FString BodyText = EmitPoseResultBodyGraph(
        StateNode->BoundGraph,
        UAnimGraphNode_StateResult::StaticClass());
    if (!BodyText.IsEmpty())
    {
        TArray<FString> BodyLines;
        BodyText.ParseIntoArray(BodyLines, TEXT("\n"), false);
        for (const FString& BodyLine : BodyLines)
        {
            Lines.Add(TEXT("    ") + BodyLine);
        }
    }
    Lines.Add(TEXT("}"));
    return FString::Join(Lines, TEXT("\n"));
}

FString FAGIRTextEmitter::EmitPoseResultBodyGraph(UEdGraph* BodyGraph, UClass* ResultSinkClass)
{
    if (!BodyGraph)
    {
        return FString();
    }

    UAnimGraphNode_Base* ResultSink = nullptr;
    TArray<UAnimGraphNode_Base*> BodyNodes;
    for (UEdGraphNode* InnerNode : BodyGraph->Nodes)
    {
        UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(InnerNode);
        if (!AnimNode)
        {
            continue;
        }

        IdFor(AnimNode);
        if (ResultSinkClass && AnimNode->IsA(ResultSinkClass))
        {
            if (!ResultSink)
            {
                ResultSink = AnimNode;
            }
            continue;
        }
        BodyNodes.Add(AnimNode);
    }

    TArray<FString> Lines;
    for (UAnimGraphNode_Base* AnimNode : BodyNodes)
    {
        const FString InnerLine = EmitNode(AnimNode);
        if (!InnerLine.IsEmpty())
        {
            TArray<FString> InnerLines;
            InnerLine.ParseIntoArray(InnerLines, TEXT("\n"), false);
            for (const FString& Sub : InnerLines)
            {
                Lines.Add(Sub);
            }
        }
    }

    if (ResultSink)
    {
        for (UEdGraphPin* Pin : ResultSink->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() == 0 || !Pin->LinkedTo[0])
            {
                continue;
            }
            UEdGraphNode* Upstream = Pin->LinkedTo[0]->GetOwningNode();
            if (const FString* Id = Upstream ? LocalIds.Find(Upstream) : nullptr)
            {
                Lines.Add(FString::Printf(TEXT("output %s"), **Id));
                break;
            }
        }
    }

    return FString::Join(Lines, TEXT("\n"));
}

FString FAGIRTextEmitter::EmitTransition(UAnimStateTransitionNode* TransitionNode)
{
    if (!TransitionNode)
    {
        return FString();
    }

    UAnimStateNodeBase* Prev = TransitionNode->GetPreviousState();
    UAnimStateNodeBase* Next = TransitionNode->GetNextState();
    const FString FromName = Prev ? Prev->GetStateName() : FString();
    const FString ToName = Next ? Next->GetStateName() : FString();
    const FString RuleGraph = TransitionNode->BoundGraph ? TransitionNode->BoundGraph->GetName() : FString();

    TArray<FString> Attributes;
    Attributes.Add(FString::Printf(TEXT("priority=%d"), TransitionNode->PriorityOrder));
    Attributes.Add(FString::Printf(TEXT("rule=%s"), *NameToken(RuleGraph)));
    if (TransitionNode->Bidirectional)
    {
        Attributes.Add(TEXT("bidirectional=true"));
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // UAnimStateTransitionNode::bDisabled was added in UE 5.6; on 5.4 use the
    // base-class ENodeEnabledState to determine whether the transition is disabled.
    if (TransitionNode->bDisabled)
    {
        Attributes.Add(TEXT("disabled=true"));
    }
#else
    if (!TransitionNode->IsNodeEnabled())
    {
        Attributes.Add(TEXT("disabled=true"));
    }
#endif
    if (TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState)
    {
        Attributes.Add(TEXT("auto_rule=true"));
        Attributes.Add(FString::Printf(TEXT("auto_rule_trigger_time=%s"),
            *FString::SanitizeFloat(TransitionNode->AutomaticRuleTriggerTime)));
    }
    Attributes.Add(FString::Printf(TEXT("crossfade_duration=%s"),
        *FString::SanitizeFloat(TransitionNode->CrossfadeDuration)));
    Attributes.Add(FString::Printf(TEXT("blend_mode=%d"),
        static_cast<int32>(TransitionNode->BlendMode)));
    Attributes.Add(FString::Printf(TEXT("logic_type=%d"),
        static_cast<int32>(TransitionNode->LogicType.GetValue())));

    const FString HeadLine = FString::Printf(
        TEXT("transition %s -> %s %s%s"),
        *NameToken(FromName),
        *NameToken(ToName),
        *FString::Join(Attributes, TEXT(" ")),
        *FormatNodeAnnotation(TransitionNode));

    // Wave 3: TLT_Custom transitions own a UAnimationCustomTransitionGraph with
    // a schema-default UAnimGraphNode_CustomTransitionResult plus any user-
    // authored body nodes. Emit a nested `custom_transition_body { ... }` block
    // when the graph exists. The body opcode itself has no head payload.
    if (TransitionNode->LogicType.GetValue() != ETransitionLogicType::TLT_Custom ||
        TransitionNode->CustomTransitionGraph == nullptr)
    {
        return HeadLine;
    }

    TArray<FString> Lines;
    Lines.Add(HeadLine + TEXT(" {"));
    Lines.Add(TEXT("    custom_transition_body {"));
    const FString BodyText = EmitPoseResultBodyGraph(
        TransitionNode->CustomTransitionGraph,
        UAnimGraphNode_CustomTransitionResult::StaticClass());
    if (!BodyText.IsEmpty())
    {
        TArray<FString> BodyLines;
        BodyText.ParseIntoArray(BodyLines, TEXT("\n"), false);
        for (const FString& BodyLine : BodyLines)
        {
            Lines.Add(TEXT("        ") + BodyLine);
        }
    }
    Lines.Add(TEXT("    }"));
    Lines.Add(TEXT("}"));
    return FString::Join(Lines, TEXT("\n"));
}

FString FAGIRTextEmitter::EmitConduit(UAnimStateConduitNode* ConduitNode)
{
    if (!ConduitNode)
    {
        return FString();
    }

    const FString ConduitName = ConduitNode->BoundGraph
        ? ConduitNode->BoundGraph->GetName()
        : ConduitNode->GetStateName();
    const FString RuleGraph = ConduitNode->BoundGraph ? ConduitNode->BoundGraph->GetName() : FString();

    return FString::Printf(
        TEXT("conduit %s rule=%s%s"),
        *NameToken(ConduitName),
        *NameToken(RuleGraph),
        *FormatNodeAnnotation(ConduitNode));
}

FString FAGIRTextEmitter::EmitStateAlias(UAnimStateAliasNode* AliasNode)
{
    if (!AliasNode)
    {
        return FString();
    }

    const FString AliasName = AliasNode->GetStateName();

    // Resolve aliased state references to their by-name tokens. The compiler
    // post-pass rebinds these against the same StateSymbols map that backs
    // transition from/to lookup, so the names must match GetStateName() on
    // each target state / conduit. Sort for deterministic output.
    TArray<FString> AliasedNames;
    for (const TWeakObjectPtr<UAnimStateNodeBase>& WeakState : AliasNode->GetAliasedStates())
    {
        if (UAnimStateNodeBase* Target = WeakState.Get())
        {
            AliasedNames.Add(Target->GetStateName());
        }
    }
    AliasedNames.Sort();

    TArray<FString> Attributes;
    if (AliasedNames.Num() > 0)
    {
        // Pack as a single quoted comma-separated value so the whitespace-
        // splitting attribute parser doesn't break the list across args.
        Attributes.Add(FString::Printf(TEXT("aliases=%s"),
            *Quote(FString::Join(AliasedNames, TEXT(",")))));
    }
    if (AliasNode->bGlobalAlias)
    {
        Attributes.Add(TEXT("global_alias=true"));
    }

    const FString AttributeText = Attributes.Num() > 0
        ? FString(TEXT(" ")) + FString::Join(Attributes, TEXT(" "))
        : FString();

    return FString::Printf(
        TEXT("state_alias %s%s%s"),
        *NameToken(AliasName),
        *AttributeText,
        *FormatNodeAnnotation(AliasNode));
}
