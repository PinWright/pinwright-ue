// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Animation/AnimGraphDumpBuilder.h"

#include "Handlers/Animation/AnimGraphConstructionUtils.h"

#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateGraph.h"
#include "AnimationTransitionGraph.h"
#include "AnimationCustomTransitionGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationConduitGraphSchema.h"
#include "AnimationBlendSpaceSampleGraph.h"
#include "BlendSpaceGraph.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateConduitNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "Animation/AnimStateMachineTypes.h"  // ETransitionLogicType
#include "AlphaBlend.h"                        // EAlphaBlendOption
#include "Curves/CurveFloat.h"                 // UCurveFloat (CustomBlendCurve)
#include "AnimationAuthoringHelpers.h"         // TransitionLogicTypeToString (shared logicType vocabulary)
#include "Utils/JsonBuilders.h"                // EnumValueToString (shared reflected enum name helper)
#include "Compat/EngineVersionCompat.h"
#if UE_VERSION_OLDER_THAN(5, 4, 0)
#include "Misc/SecureHash.h"  // FMD5 fallback for FGuid::NewDeterministicGuid (5.4+)
#endif

namespace
{
    FString GuidStringForGraph(const UEdGraph* Graph)
    {
        if (!Graph)
        {
            return FString();
        }
        // Prefer the GUID of the first non-default content node — stable across saves
        // when nodes exist. Fall back to a deterministic GUID derived from the graph name
        // so empty graphs still get a non-colliding key.
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid.IsValid())
            {
                return Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
            }
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        const FGuid Synth = FGuid::NewDeterministicGuid(Graph->GetPathName());
#else
        // FGuid::NewDeterministicGuid() was added in UE 5.4. On 5.3 derive a stable GUID from
        // the graph path via MD5 (the 16-byte digest maps directly onto FGuid's four uint32s).
        FGuid Synth;
        {
            const FString Path = Graph->GetPathName();
            FMD5 Md5;
            Md5.Update(reinterpret_cast<const uint8*>(*Path), Path.Len() * sizeof(TCHAR));
            uint8 Digest[16];
            Md5.Final(Digest);
            FMemory::Memcpy(&Synth, Digest, sizeof(FGuid));
        }
#endif
        return Synth.ToString(EGuidFormats::DigitsWithHyphensLower);
    }

    // Returns the graph that owns Graph in the page hierarchy: walk outers until we hit a UEdGraph.
    FString ParentGraphName(const UEdGraph* Graph)
    {
        if (!Graph)
        {
            return FString();
        }
        if (const UEdGraph* Parent = Cast<UEdGraph>(Graph->GetOuter()))
        {
            return Parent->GetName();
        }
        return FString();
    }

    // Schema-based classification with two narrow class checks for the two graph types
    // whose schema is K2-derived (state machine) or whose container kind requires the class
    // (blend-space, blend-space-sample). Returns empty string for non-anim graphs (caller skips).
    FString ClassifyAnimGraphPage(const UEdGraph* Graph, const TSet<const UEdGraph*>& AnimLayerSet)
    {
        if (!Graph)
        {
            return FString();
        }

        // Class-based first: state machine graph has a non-anim K2 schema.
        if (Graph->IsA<UAnimationStateMachineGraph>())
        {
            return TEXT("StateMachineGraph");
        }
        if (Graph->IsA<UBlendSpaceGraph>())
        {
            return TEXT("BlendSpaceGraph");
        }
        if (Graph->IsA<UAnimationBlendSpaceSampleGraph>())
        {
            return TEXT("BlendSpaceSample");
        }
        if (Graph->IsA<UAnimationStateGraph>())
        {
            return TEXT("StateGraph");
        }
        if (Graph->IsA<UAnimationCustomTransitionGraph>())
        {
            return TEXT("CustomTransition");
        }
        if (Graph->IsA<UAnimationTransitionGraph>())
        {
            return TEXT("TransitionRule");
        }

        // Conduit graph has no dedicated UClass — detect by schema.
        if (Graph->Schema && Graph->Schema->IsChildOf(UAnimationConduitGraphSchema::StaticClass()))
        {
            return TEXT("Conduit");
        }

        if (Graph->IsA<UAnimationGraph>())
        {
            return AnimLayerSet.Contains(Graph) ? TEXT("AnimLayer") : TEXT("AnimGraph");
        }

        return FString();
    }

    void BuildPagesArray(UAnimBlueprint* AnimBP, TArray<TSharedPtr<FJsonValue>>& OutArray)
    {
        TSet<const UEdGraph*> AnimLayerSet;
        AnimGraphConstructionUtils::CollectInterfaceLayerGraphs(AnimBP, AnimLayerSet);

        TArray<UEdGraph*> AllGraphs;
        AnimBP->GetAllGraphs(AllGraphs);

        // GetAllGraphs does not walk ImplementedInterfaces; add those graphs (and children)
        // so anim layer override pose graphs are visible in pages[].
        // Sorted because TSet iteration is pointer-hash order and pages[] must be deterministic across sessions.
        TArray<const UEdGraph*> SortedLayers = AnimLayerSet.Array();
        SortedLayers.Sort([](const UEdGraph& A, const UEdGraph& B) { return A.GetPathName() < B.GetPathName(); });
        for (const UEdGraph* Layer : SortedLayers)
        {
            AllGraphs.AddUnique(const_cast<UEdGraph*>(Layer));
        }

        for (UEdGraph* Graph : AllGraphs)
        {
            const FString Kind = ClassifyAnimGraphPage(Graph, AnimLayerSet);
            if (Kind.IsEmpty())
            {
                continue;
            }

            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Graph->GetName());
            Entry->SetStringField(TEXT("guid"), GuidStringForGraph(Graph));
            Entry->SetStringField(TEXT("kind"), Kind);
            Entry->SetStringField(TEXT("parent"), ParentGraphName(Graph));
            OutArray.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    void BuildStateMachinesArray(UAnimBlueprint* AnimBP, TArray<TSharedPtr<FJsonValue>>& OutArray)
    {
        TArray<UAnimGraphNode_StateMachineBase*> Machines;
        FBlueprintEditorUtils::GetAllNodesOfClass<UAnimGraphNode_StateMachineBase>(AnimBP, Machines);

        for (UAnimGraphNode_StateMachineBase* Machine : Machines)
        {
            if (!Machine || !Machine->EditorStateMachineGraph)
            {
                continue;
            }
            UAnimationStateMachineGraph* MachineGraph = Machine->EditorStateMachineGraph;

            TSharedRef<FJsonObject> MachineObj = MakeShared<FJsonObject>();
            MachineObj->SetStringField(TEXT("name"), MachineGraph->GetName());

            FString PageName;
            if (const UEdGraph* OwnerGraph = Cast<UEdGraph>(Machine->GetOuter()))
            {
                PageName = OwnerGraph->GetName();
            }
            MachineObj->SetStringField(TEXT("page"), PageName);

            // Surface which state the machine's entry node points at so the
            // structured readback can observe the effect of
            // animation.authoring.set_state_machine_entry (the write verb that
            // rewires this link). GetOutputNode() follows the entry node's single
            // output connection to the entry state node (a UAnimStateNode /
            // conduit / alias, all UAnimStateNodeBase); it returns null when the
            // entry is unset, in which case entry_state is the empty string.
            // GetStateName() is the same name source used for the states[] and
            // transitions[] entries below, so entry_state matches those name tokens.
            FString EntryStateName;
            if (UAnimStateEntryNode* EntryNode = MachineGraph->EntryNode.Get())
            {
                if (UAnimStateNodeBase* EntryState = Cast<UAnimStateNodeBase>(EntryNode->GetOutputNode()))
                {
                    EntryStateName = EntryState->GetStateName();
                }
            }
            MachineObj->SetStringField(TEXT("entry_state"), EntryStateName);

            TArray<TSharedPtr<FJsonValue>> StatesArr;
            TArray<TSharedPtr<FJsonValue>> TransitionsArr;
            TArray<TSharedPtr<FJsonValue>> ConduitsArr;

            for (UEdGraphNode* Node : MachineGraph->Nodes)
            {
                if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
                {
                    TSharedRef<FJsonObject> StateObj = MakeShared<FJsonObject>();
                    // Use GetStateName() everywhere for the state-name source so
                    // pages[].name, state_machines[].states[].name, and
                    // transitions[].{from,to} all agree (transitions are forced
                    // to GetStateName() because they read the state-node side,
                    // not the bound graph). GetStateName() falls back to the
                    // bound-graph name internally.
                    StateObj->SetStringField(TEXT("name"), StateNode->GetStateName());
                    StateObj->SetStringField(TEXT("guid"),
                        StateNode->NodeGuid.IsValid() ? StateNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString());
                    StatesArr.Add(MakeShared<FJsonValueObject>(StateObj));
                }
                else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(Node))
                {
                    TSharedRef<FJsonObject> ConduitObj = MakeShared<FJsonObject>();
                    ConduitObj->SetStringField(TEXT("name"), ConduitNode->GetStateName());
                    ConduitObj->SetStringField(TEXT("rule_graph"),
                        ConduitNode->BoundGraph ? ConduitNode->BoundGraph->GetName() : FString());
                    ConduitsArr.Add(MakeShared<FJsonValueObject>(ConduitObj));
                }
                else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
                {
                    TSharedRef<FJsonObject> TransObj = MakeShared<FJsonObject>();
                    UAnimStateNodeBase* Prev = TransNode->GetPreviousState();
                    UAnimStateNodeBase* Next = TransNode->GetNextState();
                    TransObj->SetStringField(TEXT("from"), Prev ? Prev->GetStateName() : FString());
                    TransObj->SetStringField(TEXT("to"),   Next ? Next->GetStateName() : FString());
                    TransObj->SetNumberField(TEXT("priority"), TransNode->PriorityOrder);
                    TransObj->SetStringField(TEXT("rule_graph"),
                        TransNode->BoundGraph ? TransNode->BoundGraph->GetName() : FString());
                    TransObj->SetBoolField(TEXT("bidirectional"), TransNode->Bidirectional);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
                    // UAnimStateTransitionNode::bDisabled was added in UE 5.6
                    TransObj->SetBoolField(TEXT("disabled"), TransNode->bDisabled);
#else
                    TransObj->SetBoolField(TEXT("disabled"), !TransNode->IsNodeEnabled());
#endif
                    // Surface the two advanced fields that animation.authoring.set_transition_settings
                    // writes (LogicType / BlendMode) as string names matching that RPC's input
                    // vocabulary, so the structured readback round-trips every authored field rather
                    // than leaving them recoverable only from agir.txt as raw enum indices. Keys are
                    // snake_case to match the rest of the anim_graph.json schema; round-trip parity is
                    // by value vocabulary ('Inertialization'/'Cubic'), not by key spelling.
                    const ETransitionLogicType::Type Logic = TransNode->LogicType.GetValue();
                    TransObj->SetStringField(TEXT("logic_type"),
                        AnimationAuthoringHelpers::TransitionLogicTypeToString(Logic));
                    TransObj->SetStringField(TEXT("blend_mode"),
                        JsonBuilders::EnumValueToString(TransNode->BlendMode));
                    // CustomBlendCurve only applies to the Custom logic type; emit its asset path
                    // there so a Custom transition's blend curve is also readable.
                    if (Logic == ETransitionLogicType::TLT_Custom &&
                        TransNode->CustomBlendCurve != nullptr)
                    {
                        TransObj->SetStringField(TEXT("blend_curve_path"),
                            TransNode->CustomBlendCurve->GetPathName());
                    }
                    TransitionsArr.Add(MakeShared<FJsonValueObject>(TransObj));
                }
            }

            MachineObj->SetArrayField(TEXT("states"), StatesArr);
            MachineObj->SetArrayField(TEXT("transitions"), TransitionsArr);
            MachineObj->SetArrayField(TEXT("conduits"), ConduitsArr);

            OutArray.Add(MakeShared<FJsonValueObject>(MachineObj));
        }
    }

    void BuildAnimNodeClassesArray(UAnimBlueprint* AnimBP, TArray<TSharedPtr<FJsonValue>>& OutArray)
    {
        TArray<UAnimGraphNode_Base*> AllNodes;
        FBlueprintEditorUtils::GetAllNodesOfClass<UAnimGraphNode_Base>(AnimBP, AllNodes);

        TMap<FString, int32> Counts;
        for (UAnimGraphNode_Base* Node : AllNodes)
        {
            if (!Node)
            {
                continue;
            }
            Counts.FindOrAdd(Node->GetClass()->GetPathName())++;
        }

        TArray<FString> SortedKeys;
        Counts.GetKeys(SortedKeys);
        SortedKeys.Sort();

        for (const FString& ClassPath : SortedKeys)
        {
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("class"), ClassPath);
            Entry->SetNumberField(TEXT("count"), Counts[ClassPath]);
            OutArray.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }
}

namespace AnimGraphDumpBuilder
{
    TSharedPtr<FJsonObject> BuildAnimGraphJson(UAnimBlueprint* AnimBP)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        if (!AnimBP)
        {
            Root->SetArrayField(TEXT("pages"), {});
            Root->SetArrayField(TEXT("state_machines"), {});
            Root->SetArrayField(TEXT("anim_node_classes"), {});
            return Root;
        }

        TArray<TSharedPtr<FJsonValue>> Pages;
        BuildPagesArray(AnimBP, Pages);
        Root->SetArrayField(TEXT("pages"), Pages);

        TArray<TSharedPtr<FJsonValue>> Machines;
        BuildStateMachinesArray(AnimBP, Machines);
        Root->SetArrayField(TEXT("state_machines"), Machines);

        TArray<TSharedPtr<FJsonValue>> AnimNodeClasses;
        BuildAnimNodeClassesArray(AnimBP, AnimNodeClasses);
        Root->SetArrayField(TEXT("anim_node_classes"), AnimNodeClasses);

        return Root;
    }
}
