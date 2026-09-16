// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Tests/TestUtils.h"

#if __has_include("StateTree.h") && __has_include("StateTreeEditorData.h") && __has_include("StateTreeState.h")
#include "GameplayTagsManager.h"
#include "GameplayTagsEditorModule.h"  // IGameplayTagsEditorModule::AddTransientEditorGameplayTag — register the test event tag at runtime
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#define MCP_STATE_TREE_AUTHORING_TESTS_AVAILABLE 1
#else
#define MCP_STATE_TREE_AUTHORING_TESTS_AVAILABLE 0
#endif

#if MCP_STATE_TREE_AUTHORING_TESTS_AVAILABLE
namespace
{
    constexpr const TCHAR* Counterfactual =
        TEXT("Counterfactual: reverting production node/binding/trigger mutation leaves Evaluators, Tasks, transition Conditions, editor bindings, or event tag fields empty/default.");

    bool InvokeStateTreeHandler(FAutomationTestBase& Test, const FString& MethodName, const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture)
    {
        const bool bFound = InvokeHandlerWithCapture(MethodName, Payload, Capture);
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), *MethodName), bFound);
        if (!bFound)
        {
            return false;
        }
        if (!Capture.bSuccess)
        {
            Test.AddError(FString::Printf(TEXT("%s failed: %s %s"), *MethodName, *Capture.ErrorCode, *Capture.Message));
        }
        return Capture.bSuccess;
    }

    bool ExpectDefaultNoSave(FAutomationTestBase& Test, const FTestResponseCapture& Capture, const TCHAR* HandlerName)
    {
        bool bSaved = true;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetBoolField(TEXT("saved"), bSaved))
        {
            Test.AddError(FString::Printf(TEXT("%s did not return saved"), HandlerName));
            return false;
        }
        if (bSaved)
        {
            Test.AddError(FString::Printf(TEXT("%s saved despite default save=false"), HandlerName));
            return false;
        }
        return true;
    }

    FString MakeStateTreePackagePath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/McpTests/StateTree/%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UStateTree* LoadStateTreeAsset(const FString& StateTreePath)
    {
        if (UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *StateTreePath))
        {
            return StateTree;
        }

        const FString AssetName = FPackageName::GetLongPackageAssetName(StateTreePath);
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *StateTreePath, *AssetName);
        return LoadObject<UStateTree>(nullptr, *ObjectPath);
    }

    UStateTreeEditorData* LoadEditorData(FAutomationTestBase& Test, const FString& StateTreePath)
    {
        UStateTree* StateTree = LoadStateTreeAsset(StateTreePath);
        Test.TestNotNull(TEXT("StateTree asset is loadable"), StateTree);
        if (!StateTree)
        {
            return nullptr;
        }

        UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
        Test.TestNotNull(TEXT("StateTree EditorData exists"), EditorData);
        return EditorData;
    }

    UStateTreeState* FindStateRecursive(UStateTreeState* State, const FString& Name)
    {
        if (!State)
        {
            return nullptr;
        }

        if (State->Name.ToString().Equals(Name, ESearchCase::IgnoreCase))
        {
            return State;
        }

        for (UStateTreeState* Child : State->Children)
        {
            if (UStateTreeState* Found = FindStateRecursive(Child, Name))
            {
                return Found;
            }
        }

        return nullptr;
    }

    UStateTreeState* FindState(UStateTreeEditorData& EditorData, const FString& Name)
    {
        for (UStateTreeState* SubTree : EditorData.SubTrees)
        {
            if (UStateTreeState* Found = FindStateRecursive(SubTree, Name))
            {
                return Found;
            }
        }
        return nullptr;
    }

    bool CreateStateTree(FAutomationTestBase& Test, const FString& PackagePath, FString& OutStateTreePath)
    {
        CleanupTestAsset(PackagePath);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(PackagePath));
        Payload->SetStringField(TEXT("path"), FPackageName::GetLongPackagePath(PackagePath));

        FTestResponseCapture Capture;
        if (!InvokeStateTreeHandler(Test, TEXT("ai.create_state_tree"), Payload, Capture))
        {
            return false;
        }

        if (!Capture.Result.IsValid() || !Capture.Result->TryGetStringField(TEXT("stateTreePath"), OutStateTreePath))
        {
            Test.AddError(TEXT("ai.create_state_tree did not return stateTreePath"));
            return false;
        }

        return true;
    }

    bool AddState(FAutomationTestBase& Test, const FString& StateTreePath, const FString& StateName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("stateTreePath"), StateTreePath);
        Payload->SetStringField(TEXT("stateName"), StateName);
        Payload->SetStringField(TEXT("parentStateName"), TEXT("Root"));

        FTestResponseCapture Capture;
        return InvokeStateTreeHandler(Test, TEXT("ai.add_state_tree_state"), Payload, Capture);
    }

    bool AddTransition(FAutomationTestBase& Test, const FString& StateTreePath, const FString& FromState, const FString& ToState)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("stateTreePath"), StateTreePath);
        Payload->SetStringField(TEXT("fromState"), FromState);
        Payload->SetStringField(TEXT("toState"), ToState);

        FTestResponseCapture Capture;
        return InvokeStateTreeHandler(Test, TEXT("ai.add_state_tree_transition"), Payload, Capture);
    }

    bool AddEvaluator(FAutomationTestBase& Test, const FString& StateTreePath, const FString& Name, FString& OutNodeId)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("stateTreePath"), StateTreePath);
        Payload->SetStringField(TEXT("evaluatorClass"), TEXT("StateTreeEvaluatorBase"));
        Payload->SetStringField(TEXT("name"), Name);

        FTestResponseCapture Capture;
        if (!InvokeStateTreeHandler(Test, TEXT("state_tree.add_evaluator"), Payload, Capture))
        {
            return false;
        }

        if (!Capture.Result.IsValid() || !Capture.Result->TryGetStringField(TEXT("nodeId"), OutNodeId))
        {
            Test.AddError(TEXT("state_tree.add_evaluator did not return nodeId"));
            return false;
        }

        return ExpectDefaultNoSave(Test, Capture, TEXT("state_tree.add_evaluator"));
    }

    bool AddTask(FAutomationTestBase& Test, const FString& StateTreePath, const FString& StateName, const FString& Name, FString& OutNodeId)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("stateTreePath"), StateTreePath);
        Payload->SetStringField(TEXT("stateName"), StateName);
        Payload->SetStringField(TEXT("taskClass"), TEXT("StateTreeTaskBase"));
        Payload->SetStringField(TEXT("name"), Name);

        FTestResponseCapture Capture;
        if (!InvokeStateTreeHandler(Test, TEXT("state_tree.add_task"), Payload, Capture))
        {
            return false;
        }

        if (!Capture.Result.IsValid() || !Capture.Result->TryGetStringField(TEXT("nodeId"), OutNodeId))
        {
            Test.AddError(TEXT("state_tree.add_task did not return nodeId"));
            return false;
        }

        return ExpectDefaultNoSave(Test, Capture, TEXT("state_tree.add_task"));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateTreeAddEvaluatorCreatesEditorNodeTest,
    "PinWright.state_tree.AddEvaluatorCreatesEditorNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStateTreeAddEvaluatorCreatesEditorNodeTest::RunTest(const FString& Parameters)
{
    AddInfo(Counterfactual);

    const FString PackagePath = MakeStateTreePackagePath(TEXT("ST_AddEvaluator"));
    FString StateTreePath;
    if (!CreateStateTree(*this, PackagePath, StateTreePath))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    FString EvaluatorId;
    if (!AddEvaluator(*this, StateTreePath, TEXT("DistanceEvaluator"), EvaluatorId))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    UStateTreeEditorData* EditorData = LoadEditorData(*this, StateTreePath);
    TestNotNull(TEXT("EditorData loaded after evaluator add"), EditorData);
    TestEqual(TEXT("Evaluator editor node persisted"), EditorData ? EditorData->Evaluators.Num() : 0, 1);

    if (EditorData && EditorData->Evaluators.Num() == 1)
    {
        const FStateTreeEditorNode& Evaluator = EditorData->Evaluators[0];
        TestTrue(TEXT("Evaluator node has ID"), Evaluator.ID.IsValid());
        TestTrue(TEXT("Evaluator node has evaluator struct"), Evaluator.Node.GetScriptStruct() && Evaluator.Node.GetScriptStruct()->IsChildOf(FStateTreeEvaluatorBase::StaticStruct()));
    }

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateTreeAddTaskConditionAndBindingTest,
    "PinWright.state_tree.AddTaskConditionAndBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStateTreeAddTaskConditionAndBindingTest::RunTest(const FString& Parameters)
{
    AddInfo(Counterfactual);

    const FString PackagePath = MakeStateTreePackagePath(TEXT("ST_TaskConditionBinding"));
    FString StateTreePath;
    if (!CreateStateTree(*this, PackagePath, StateTreePath)
        || !AddState(*this, StateTreePath, TEXT("Attack"))
        || !AddTransition(*this, StateTreePath, TEXT("Root"), TEXT("Attack")))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    FString EvaluatorId;
    FString TaskId;
    if (!AddEvaluator(*this, StateTreePath, TEXT("BindableEvaluator"), EvaluatorId)
        || !AddTask(*this, StateTreePath, TEXT("Root"), TEXT("BoundTask"), TaskId))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> ConditionPayload = MakeShared<FJsonObject>();
    ConditionPayload->SetStringField(TEXT("stateTreePath"), StateTreePath);
    ConditionPayload->SetStringField(TEXT("stateName"), TEXT("Root"));
    ConditionPayload->SetStringField(TEXT("toState"), TEXT("Attack"));
    ConditionPayload->SetStringField(TEXT("target"), TEXT("transition"));
    ConditionPayload->SetStringField(TEXT("conditionClass"), TEXT("StateTreeConditionBase"));
    ConditionPayload->SetStringField(TEXT("name"), TEXT("TransitionGate"));

    FTestResponseCapture ConditionCapture;
    if (!InvokeStateTreeHandler(*this, TEXT("state_tree.add_condition"), ConditionPayload, ConditionCapture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    if (!ExpectDefaultNoSave(*this, ConditionCapture, TEXT("state_tree.add_condition")))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> BindingPayload = MakeShared<FJsonObject>();
    BindingPayload->SetStringField(TEXT("stateTreePath"), StateTreePath);
    BindingPayload->SetStringField(TEXT("sourceId"), EvaluatorId);
    BindingPayload->SetStringField(TEXT("sourcePath"), TEXT("Name"));
    BindingPayload->SetStringField(TEXT("targetId"), TaskId);
    BindingPayload->SetStringField(TEXT("targetPath"), TEXT("Name"));

    FTestResponseCapture BindingCapture;
    if (!InvokeStateTreeHandler(*this, TEXT("state_tree.bind_property"), BindingPayload, BindingCapture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    if (!ExpectDefaultNoSave(*this, BindingCapture, TEXT("state_tree.bind_property")))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    UStateTreeEditorData* EditorData = LoadEditorData(*this, StateTreePath);
    UStateTreeState* RootState = EditorData ? FindState(*EditorData, TEXT("Root")) : nullptr;
    TestNotNull(TEXT("Root state found"), RootState);

    TestEqual(TEXT("Task editor node persisted"), RootState ? RootState->Tasks.Num() : 0, 1);
    TestEqual(TEXT("Transition condition editor node persisted"), RootState && RootState->Transitions.Num() > 0 ? RootState->Transitions[0].Conditions.Num() : 0, 1);
    // FStateTreeEditorPropertyBindings::GetNumBindings() was added in UE 5.6.
    // On 5.4/5.5 use GetBindings().Num() which is the underlying array accessor.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    TestEqual(TEXT("Editor property binding persisted"),
        EditorData && EditorData->GetPropertyEditorBindings()
            ? EditorData->GetPropertyEditorBindings()->GetBindings().Num() : 0, 1);
#else
    TestEqual(TEXT("Editor property binding persisted"),
        EditorData && EditorData->GetPropertyEditorBindings()
            ? EditorData->GetPropertyEditorBindings()->GetNumBindings() : 0, 1);
#endif

    // Direction guard. Bindings are DIRECTIONAL — StateTreeAuthoringHandler.cpp calls
    // EditorData->AddPropertyBinding(SourcePath, TargetPath), and swapping those two arguments
    // still leaves exactly one binding, so the count above cannot catch an inverted write. Read
    // the stored binding back and pin which end is which. The handler's own sourcePath/targetPath
    // response fields are echoed straight from the request, so they are not evidence of anything;
    // only the persisted struct IDs are. Both property paths are "Name" here, so the struct IDs
    // are the only thing that can distinguish source from target.
    //
    // GetBindings() is header-inline on every supported engine (TConstArrayView of
    // FStateTreePropertyPathBinding), as are GetSourcePath()/GetTargetPath()/GetStructID(), so
    // this needs no version branch and no additional link dependency.
    if (FStateTreeEditorPropertyBindings* Bindings = EditorData ? EditorData->GetPropertyEditorBindings() : nullptr)
    {
        TConstArrayView<FStateTreePropertyPathBinding> StoredBindings = Bindings->GetBindings();
        TestEqual(TEXT("Exactly one stored editor binding to inspect"), StoredBindings.Num(), 1);
        if (StoredBindings.Num() == 1)
        {
            const FStateTreePropertyPathBinding& Binding = StoredBindings[0];
            const FString BoundSourceId = Binding.GetSourcePath().GetStructID().ToString(EGuidFormats::DigitsWithHyphens);
            const FString BoundTargetId = Binding.GetTargetPath().GetStructID().ToString(EGuidFormats::DigitsWithHyphens);

            // add_evaluator / add_task both return nodeId via MakeNodeResult in
            // DigitsWithHyphens, so these compare like for like.
            TestEqual(TEXT("Binding source struct is the evaluator node (not the task)"), BoundSourceId, EvaluatorId);
            TestEqual(TEXT("Binding target struct is the task node (not the evaluator)"), BoundTargetId, TaskId);
            TestNotEqual(TEXT("Binding source and target are different nodes"), BoundSourceId, BoundTargetId);

            TestFalse(TEXT("Binding source property path is not empty"), Binding.GetSourcePath().ToString().IsEmpty());
            TestFalse(TEXT("Binding target property path is not empty"), Binding.GetTargetPath().ToString().IsEmpty());
        }
    }
    else
    {
        AddError(TEXT("StateTree EditorData has no property editor bindings container"));
    }

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateTreeSetTransitionTriggerEventTagTest,
    "PinWright.state_tree.SetTransitionTriggerEventTag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStateTreeSetTransitionTriggerEventTagTest::RunTest(const FString& Parameters)
{
    AddInfo(Counterfactual);

    const FString PackagePath = MakeStateTreePackagePath(TEXT("ST_TransitionTrigger"));
    FString StateTreePath;
    if (!CreateStateTree(*this, PackagePath, StateTreePath)
        || !AddState(*this, StateTreePath, TEXT("Attack"))
        || !AddTransition(*this, StateTreePath, TEXT("Root"), TEXT("Attack")))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    // Register the event tag transiently for this editor session rather than
    // depending on a project-defined tag (e.g. a game's GameplayEvent.MeleeHit),
    // which does not exist on a clean CI host. AddTransientEditorGameplayTag is
    // editor-only, not persisted to INI, and immediately RequestGameplayTag-
    // resolvable — the set_transition_trigger handler requires the tag to be
    // registered, so this keeps the test self-contained on every host.
    const FString EventTagName = TEXT("PinWrightTest.Event.MeleeHit");
    if (IGameplayTagsEditorModule::IsAvailable())
    {
        IGameplayTagsEditorModule::Get().AddTransientEditorGameplayTag(EventTagName);
    }
    const FGameplayTag EventTag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*EventTagName), false);
    TestTrue(TEXT("Configured gameplay event tag exists"), EventTag.IsValid());
    if (!EventTag.IsValid())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> TriggerPayload = MakeShared<FJsonObject>();
    TriggerPayload->SetStringField(TEXT("stateTreePath"), StateTreePath);
    TriggerPayload->SetStringField(TEXT("fromState"), TEXT("Root"));
    TriggerPayload->SetStringField(TEXT("toState"), TEXT("Attack"));
    TriggerPayload->SetStringField(TEXT("trigger"), TEXT("OnEvent"));
    TriggerPayload->SetStringField(TEXT("gameplayEventTag"), EventTag.ToString());

    FTestResponseCapture TriggerCapture;
    if (!InvokeStateTreeHandler(*this, TEXT("state_tree.set_transition_trigger"), TriggerPayload, TriggerCapture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    if (!ExpectDefaultNoSave(*this, TriggerCapture, TEXT("state_tree.set_transition_trigger")))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    UStateTreeEditorData* EditorData = LoadEditorData(*this, StateTreePath);
    UStateTreeState* RootState = EditorData ? FindState(*EditorData, TEXT("Root")) : nullptr;
    TestNotNull(TEXT("Root state found"), RootState);
    // Exactly one: AddRootState() seeds Root with no transitions, ai.add_state_tree_state does
    // not touch them, and the single AddTransition() above adds one. A count of 2+ means a
    // handler is duplicating the transition instead of editing it in place.
    TestEqual(TEXT("Root has exactly one transition"), RootState ? RootState->Transitions.Num() : 0, 1);

    if (RootState && RootState->Transitions.Num() > 0)
    {
        const FStateTreeTransition& Transition = RootState->Transitions[0];
        TestTrue(TEXT("Transition trigger persisted as OnEvent"), Transition.Trigger == EStateTreeTransitionTrigger::OnEvent);
        // FStateTreeTransition::RequiredEvent (FStateTreeEventDesc) was added in UE 5.5.
        // In 5.4 the gameplay event tag was stored in the plain FGameplayTag EventTag field.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        TestTrue(TEXT("Transition required event tag persisted"), Transition.EventTag == EventTag);
#else
        TestTrue(TEXT("Transition required event tag persisted"), Transition.RequiredEvent.Tag == EventTag);
#endif
    }

    CleanupTestAsset(PackagePath);
    return true;
}
#endif
