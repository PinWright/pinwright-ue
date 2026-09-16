// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

// Gate the whole test TU on the StateTree authoring headers exactly like the production
// builder (StateTreeDumpBuilder.cpp). On engines / configs where StateTreeModule /
// StateTreeEditorModule are absent, the sidecar is compiled out and there is nothing to test.
#if __has_include("StateTree.h") && __has_include("StateTreeEditorData.h") && __has_include("StateTreeState.h")

#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/StateTreeDumpBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "AssetDumpTestHelpers.h"

#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "Compat/InstancedStructCompat.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::LoadJsonFile;

    // Authors a transient StateTree with a Root subtree, one child state "Patrol", a
    // Root->Patrol transition (OnStateCompleted), and one task node on Patrol whose inner
    // FInstancedStruct is a concrete task struct (resolved by reflection so no hard link).
    UStateTree* NewTransientStateTree(FString& OutObjectPath, FString& OutChildStateName, FString& OutTaskNodeType)
    {
        const FString AssetName = FString::Printf(TEXT("ST_StateTreeDump_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        // /Engine/Transient/ is an in-memory mount, so DumpSingleAsset's DoesPackageExist
        // gate does not short-circuit on a /Game/ package that was never saved to disk.
        const FString PackageName = FString::Printf(TEXT("/Engine/Transient/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);

        UStateTree* StateTree = NewObject<UStateTree>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!StateTree)
        {
            return nullptr;
        }

        UStateTreeEditorData* EditorData = NewObject<UStateTreeEditorData>(StateTree, TEXT("EditorData"), RF_Transactional);
        StateTree->EditorData = EditorData;

        UStateTreeState& RootState = EditorData->AddRootState();
        RootState.Name = FName(TEXT("Root"));

        OutChildStateName = TEXT("Patrol");
        UStateTreeState& Child = RootState.AddChildState(FName(*OutChildStateName), EStateTreeStateType::State);

        // Root -> Patrol transition on OnStateCompleted.
        RootState.AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Child);

        // One task on Patrol whose Node FInstancedStruct holds a concrete task struct.
        // Resolve by reflection to avoid hard-linking a specific StateTree task type.
        OutTaskNodeType.Empty();
        if (UScriptStruct* TaskStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/StateTreeModule.StateTreeRunParallelStateTreeTask")))
        {
            FStateTreeEditorNode& TaskNode = Child.Tasks.AddDefaulted_GetRef();
            TaskNode.ID = FGuid::NewGuid();
            TaskNode.Node.InitializeAs(TaskStruct);
            OutTaskNodeType = TaskStruct->GetStructCPPName();
        }

        StateTree->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return StateTree;
    }

    TSharedPtr<FJsonObject> FindStateByName(const TArray<TSharedPtr<FJsonValue>>* States, const FString& Name)
    {
        if (!States)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *States)
        {
            TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (Obj.IsValid() && Obj->GetStringField(TEXT("name")) == Name)
            {
                return Obj;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateTreeDumpBuilderShapeTest,
    "PinWright.Assets.StateTree.DumpBuilder.Shape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStateTreeDumpBuilderShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    FString ChildStateName;
    FString TaskNodeType;
    UStateTree* StateTree = NewTransientStateTree(ObjectPath, ChildStateName, TaskNodeType);
    TestNotNull(TEXT("Transient UStateTree created"), StateTree);
    if (!StateTree)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Json = StateTreeDumpBuilder::BuildStateTreeJson(StateTree);
    TestTrue(TEXT("BuildStateTreeJson returns non-null"), Json.IsValid());
    if (!Json.IsValid())
    {
        StateTree->RemoveFromRoot();
        return false;
    }

    TestEqual(TEXT("assetKind"), Json->GetStringField(TEXT("assetKind")), FString(TEXT("StateTree")));

    const TArray<TSharedPtr<FJsonValue>>* SubTrees = nullptr;
    TestTrue(TEXT("subTrees array exists"), Json->TryGetArrayField(TEXT("subTrees"), SubTrees));
    TestTrue(TEXT("subTrees has the Root state"), SubTrees && SubTrees->Num() == 1);

    TSharedPtr<FJsonObject> RootJson = FindStateByName(SubTrees, TEXT("Root"));
    TestTrue(TEXT("Root state present in dump"), RootJson.IsValid());
    if (!RootJson.IsValid())
    {
        StateTree->RemoveFromRoot();
        return false;
    }

    // The Patrol child state must surface under Root.children — the topology the opaque
    // EditorData pointer previously hid entirely.
    const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
    TestTrue(TEXT("Root.children array exists"), RootJson->TryGetArrayField(TEXT("children"), Children));
    TSharedPtr<FJsonObject> ChildJson = FindStateByName(Children, ChildStateName);
    TestTrue(TEXT("Patrol child state present under Root.children"), ChildJson.IsValid());

    // The Root->Patrol transition must surface with its trigger and target name.
    const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
    TestTrue(TEXT("Root.transitions array exists"), RootJson->TryGetArrayField(TEXT("transitions"), Transitions));
    if (Transitions && Transitions->Num() >= 1)
    {
        TSharedPtr<FJsonObject> Transition = (*Transitions)[0]->AsObject();
        TestTrue(TEXT("transition is object"), Transition.IsValid());
        if (Transition.IsValid())
        {
            TestEqual(TEXT("transition trigger is OnStateCompleted"),
                Transition->GetStringField(TEXT("trigger")), FString(TEXT("OnStateCompleted")));
            const TSharedPtr<FJsonObject>* Target = nullptr;
            TestTrue(TEXT("transition target object exists"), Transition->TryGetObjectField(TEXT("target"), Target));
            if (Target)
            {
                TestEqual(TEXT("transition target name is Patrol"),
                    (*Target)->GetStringField(TEXT("name")), ChildStateName);
            }
        }
    }

    // When a concrete task struct was resolvable, Patrol.tasks[0] must expand the inner
    // FInstancedStruct (nodeType + _kind) rather than emitting an opaque value.
    if (ChildJson.IsValid() && !TaskNodeType.IsEmpty())
    {
        const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
        TestTrue(TEXT("Patrol.tasks array exists"), ChildJson->TryGetArrayField(TEXT("tasks"), Tasks));
        if (Tasks && Tasks->Num() >= 1)
        {
            TSharedPtr<FJsonObject> Task = (*Tasks)[0]->AsObject();
            TestTrue(TEXT("task entry is object"), Task.IsValid());
            if (Task.IsValid())
            {
                TestEqual(TEXT("task nodeType matches resolved struct"),
                    Task->GetStringField(TEXT("nodeType")), TaskNodeType);
                const TSharedPtr<FJsonObject>* NodeObj = nullptr;
                TestTrue(TEXT("task node FInstancedStruct expanded to object"),
                    Task->TryGetObjectField(TEXT("node"), NodeObj));
                if (NodeObj)
                {
                    TestEqual(TEXT("expanded node carries inner _kind"),
                        (*NodeObj)->GetStringField(TEXT("_kind")), TaskNodeType);
                }
            }
        }
    }

    StateTree->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateTreeAssetDumpWritesStateTreeAspectFileTest,
    "PinWright.Assets.StateTree.AssetDump.WritesStateTreeAspectFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStateTreeAssetDumpWritesStateTreeAspectFileTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    FString ChildStateName;
    FString TaskNodeType;
    UStateTree* StateTree = NewTransientStateTree(ObjectPath, ChildStateName, TaskNodeType);
    TestNotNull(TEXT("Transient UStateTree created"), StateTree);
    if (!StateTree)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("StateTreeDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient StateTree"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json remains present"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Properties));
    // The whole point of the fix: the state_tree.json sidecar is emitted, where before only
    // an opaque EditorData object pointer existed in properties.json.
    TestTrue(TEXT("state_tree.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::StateTree));

    const FString StateTreePath = FindDumpFile(Result.WrittenPaths, DumpFileNames::StateTree);
    TSharedPtr<FJsonObject> Json = LoadJsonFile(StateTreePath);
    TestTrue(TEXT("state_tree.json parses"), Json.IsValid());
    if (Json.IsValid())
    {
        TestEqual(TEXT("state_tree.json assetKind"),
            Json->GetStringField(TEXT("assetKind")), FString(TEXT("StateTree")));
        const TArray<TSharedPtr<FJsonValue>>* SubTrees = nullptr;
        TestTrue(TEXT("state_tree.json subTrees array exists"), Json->TryGetArrayField(TEXT("subTrees"), SubTrees));
        TestTrue(TEXT("state_tree.json subTrees non-empty"), SubTrees && SubTrees->Num() >= 1);
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    StateTree->RemoveFromRoot();
    return true;
}

#endif // StateTree headers available
