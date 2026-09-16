// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/StateTreeDumpBuilder.h"

#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonBuilders.h"
#include "Utils/JsonSidecarRegistry.h"

// StateTree authoring headers live in the conditional StateTreeModule /
// StateTreeEditorModule (Build.cs TryAddConditionalModule). Gate the whole
// dump-builder TU exactly like the StateTree authoring handlers so the plugin
// still links on engines / configs where those modules are absent.
#if __has_include("StateTree.h") && __has_include("StateTreeEditorData.h") && __has_include("StateTreeState.h")
#include "Compat/EngineVersionCompat.h"
#include "JsonObjectConverter.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "Compat/InstancedStructCompat.h"
#define MCP_STATE_TREE_DUMP_AVAILABLE 1
#else
#define MCP_STATE_TREE_DUMP_AVAILABLE 0
#endif

namespace
{
#if MCP_STATE_TREE_DUMP_AVAILABLE
    // Bare-member-name resolution (strip the "EEnum::" prefix, int fallback) lives
    // in the shared JsonBuilders::EnumMemberName; alias it so the existing call sites
    // read unchanged and the prefix-strip logic has a single home.
    using JsonBuilders::EnumMemberName;

    // Expand one FStateTreeEditorNode: its Node/Instance FInstancedStructs are
    // type-erased (a UScriptStruct* plus a heap memory block, neither reflected as a
    // UPROPERTY), so a plain field walk yields {}. Read the inner type and serialize the
    // inner struct's reflected fields, mirroring PropertyExport.cpp::StructToJsonObject.
    TSharedPtr<FJsonValue> ExportInstancedStruct(const FInstancedStruct& Instanced)
    {
        const UScriptStruct* InnerType = Instanced.GetScriptStruct();
        if (!InnerType || !Instanced.GetMemory())
        {
            return MakeShared<FJsonValueNull>();
        }

        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("_kind"), InnerType->GetStructCPPName());
        FJsonObjectConverter::UStructToJsonObject(InnerType, Instanced.GetMemory(), Obj, 0, 0);
        return MakeShared<FJsonValueObject>(Obj);
    }

    TSharedPtr<FJsonObject> BuildEditorNodeJson(const FStateTreeEditorNode& Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("id"), Node.ID.ToString(EGuidFormats::DigitsWithHyphens));
        Obj->SetStringField(TEXT("name"), Node.GetName().ToString());

        if (const UScriptStruct* NodeType = Node.Node.GetScriptStruct())
        {
            Obj->SetStringField(TEXT("nodeType"), NodeType->GetStructCPPName());
        }
        Obj->SetField(TEXT("node"), ExportInstancedStruct(Node.Node));
        Obj->SetField(TEXT("instance"), ExportInstancedStruct(Node.Instance));
        return Obj;
    }

    void AppendNodeArray(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, const TArray<FStateTreeEditorNode>& Nodes)
    {
        if (Nodes.Num() == 0)
        {
            return;
        }
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Nodes.Num());
        for (const FStateTreeEditorNode& Node : Nodes)
        {
            Values.Add(MakeShared<FJsonValueObject>(BuildEditorNodeJson(Node)));
        }
        Parent->SetArrayField(Field, Values);
    }

    TSharedPtr<FJsonObject> BuildTransitionJson(const FStateTreeTransition& Transition)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("id"), Transition.ID.ToString(EGuidFormats::DigitsWithHyphens));
        Obj->SetStringField(TEXT("trigger"), EnumMemberName(StaticEnum<EStateTreeTransitionTrigger>(), static_cast<int64>(Transition.Trigger)));

        // Target state link (FStateTreeStateLink): Name + ID + LinkType.
        TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
        Target->SetStringField(TEXT("name"), Transition.State.Name.ToString());
        Target->SetStringField(TEXT("id"), Transition.State.ID.ToString(EGuidFormats::DigitsWithHyphens));
        Target->SetStringField(TEXT("linkType"), EnumMemberName(StaticEnum<EStateTreeTransitionType>(), static_cast<int64>(Transition.State.LinkType)));
        Obj->SetObjectField(TEXT("target"), Target);

        // OnEvent required tag: FStateTreeEventDesc::Tag was introduced in UE 5.5; on 5.4
        // the transition carries a bare FGameplayTag EventTag.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        if (Transition.EventTag.IsValid())
        {
            Obj->SetStringField(TEXT("requiredEventTag"), Transition.EventTag.ToString());
        }
#else
        if (Transition.RequiredEvent.Tag.IsValid())
        {
            Obj->SetStringField(TEXT("requiredEventTag"), Transition.RequiredEvent.Tag.ToString());
        }
        if (const UScriptStruct* PayloadStruct = Transition.RequiredEvent.PayloadStruct)
        {
            Obj->SetStringField(TEXT("requiredEventPayloadStruct"), PayloadStruct->GetPathName());
        }
#endif

        AppendNodeArray(Obj, TEXT("conditions"), Transition.Conditions);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildStateJson(const UStateTreeState* State)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!State)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("name"), State->Name.ToString());
        Obj->SetStringField(TEXT("id"), State->ID.ToString(EGuidFormats::DigitsWithHyphens));
        Obj->SetStringField(TEXT("type"), EnumMemberName(StaticEnum<EStateTreeStateType>(), static_cast<int64>(State->Type)));
        Obj->SetStringField(TEXT("selectionBehavior"), EnumMemberName(StaticEnum<EStateTreeStateSelectionBehavior>(), static_cast<int64>(State->SelectionBehavior)));
        // UStateTreeState::Tag was added in UE 5.5; pre-5.5 states have no gameplay tag.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        if (State->Tag.IsValid())
        {
            Obj->SetStringField(TEXT("tag"), State->Tag.ToString());
        }
#endif

        AppendNodeArray(Obj, TEXT("enterConditions"), State->EnterConditions);
        AppendNodeArray(Obj, TEXT("tasks"), State->Tasks);

        // SingleTask is a non-array FStateTreeEditorNode; only emit it when populated.
        if (State->SingleTask.ID.IsValid() && State->SingleTask.Node.GetScriptStruct())
        {
            Obj->SetObjectField(TEXT("singleTask"), BuildEditorNodeJson(State->SingleTask));
        }

        if (State->Transitions.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Transitions;
            Transitions.Reserve(State->Transitions.Num());
            for (const FStateTreeTransition& Transition : State->Transitions)
            {
                Transitions.Add(MakeShared<FJsonValueObject>(BuildTransitionJson(Transition)));
            }
            Obj->SetArrayField(TEXT("transitions"), Transitions);
        }

        if (State->Children.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Children;
            Children.Reserve(State->Children.Num());
            for (const UStateTreeState* Child : State->Children)
            {
                Children.Add(MakeShared<FJsonValueObject>(BuildStateJson(Child)));
            }
            Obj->SetArrayField(TEXT("children"), Children);
        }

        return Obj;
    }
#endif // MCP_STATE_TREE_DUMP_AVAILABLE
}

namespace StateTreeDumpBuilder
{
    TSharedPtr<FJsonObject> BuildStateTreeJson(const UStateTree* StateTree)
    {
#if MCP_STATE_TREE_DUMP_AVAILABLE
        if (!StateTree)
        {
            return nullptr;
        }

        const UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
        if (!EditorData)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("assetKind"), TEXT("StateTree"));
        Root->SetStringField(TEXT("path"), StateTree->GetPathName());

        TArray<TSharedPtr<FJsonValue>> SubTrees;
        SubTrees.Reserve(EditorData->SubTrees.Num());
        for (const UStateTreeState* SubTree : EditorData->SubTrees)
        {
            SubTrees.Add(MakeShared<FJsonValueObject>(BuildStateJson(SubTree)));
        }
        Root->SetArrayField(TEXT("subTrees"), SubTrees);

        AppendNodeArray(Root, TEXT("evaluators"), EditorData->Evaluators);
        AppendNodeArray(Root, TEXT("globalTasks"), EditorData->GlobalTasks);

        return Root;
#else
        (void)StateTree;
        return nullptr;
#endif
    }
}

namespace
{
    UClass* GetStateTreeSidecarClass()
    {
#if MCP_STATE_TREE_DUMP_AVAILABLE
        return UStateTree::StaticClass();
#else
        // No StateTree class available at compile time — return nullptr so
        // RunRegisteredJsonSidecars never matches this spec (it skips null ClassFn results).
        return nullptr;
#endif
    }

    TSharedPtr<FJsonObject> BuildStateTreeSidecar(UObject* Asset)
    {
#if MCP_STATE_TREE_DUMP_AVAILABLE
        return StateTreeDumpBuilder::BuildStateTreeJson(Cast<UStateTree>(Asset));
#else
        (void)Asset;
        return nullptr;
#endif
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("state_tree"), DumpFileNames::StateTree,
    &GetStateTreeSidecarClass, &BuildStateTreeSidecar,
    nullptr, nullptr, TEXT("StateTree has no EditorData."), 100);
