// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for AI domain handlers
// Covers: AIHandler.cpp (25 handlers), BehaviorTreeHandler.cpp (6 handlers), NavigationHandler.cpp (11 handlers)
#include "Misc/AutomationTest.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Decorators/BTDecorator_BlueprintBase.h"
#include "BehaviorTree/Services/BTService_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Pawn.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "Compat/EngineVersionCompat.h"
#include "Navigation/NavLinkProxy.h"
#include "NavModifierComponent.h"
#include "NavAreas/NavArea_Null.h"
#include "NavAreas/NavArea_Obstacle.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"


namespace
{
    // Build a {x,y,z} JSON object from an FVector for nav-link handler payloads and echo
    // assertions. Shared by the nav-link tests so the wire-vector shape lives in one place.
    TSharedPtr<FJsonObject> MakeNavVecJson(const FVector& V)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), V.X);
        Obj->SetNumberField(TEXT("y"), V.Y);
        Obj->SetNumberField(TEXT("z"), V.Z);
        return Obj;
    }

    // Walk a reloaded blueprint's SCS and return the first component template that casts to T.
    // Used by the nav-modifier echo regression to read an SCS-added component back off the
    // asset, so the GetAllNodes/ComponentTemplate/Cast walk lives in one place.
    template <typename T>
    T* FindFirstSCSComponentTemplate(UBlueprint* Blueprint)
    {
        if (!Blueprint || !Blueprint->SimpleConstructionScript) return nullptr;
        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node && Node->ComponentTemplate)
            {
                if (T* Comp = Cast<T>(Node->ComponentTemplate))
                {
                    return Comp;
                }
            }
        }
        return nullptr;
    }

    // Create an AI Controller blueprint via blueprint.create (parentClass=AIController) for a
    // regression probe, asserting the handler was found and succeeded. Returns false (and
    // records the failure on Test) when the caller should abort. Shared by the stop-BT and
    // assign-BB regressions, whose controller-creation step is otherwise identical.
    bool CreateAIControllerProbe(FAutomationTestBase& Test, const FString& ControllerName, const FString& BasePath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ControllerName);
        Payload->SetStringField(TEXT("savePath"), BasePath);
        Payload->SetStringField(TEXT("parentClass"), TEXT("AIController"));
        FTestResponseCapture Capture;
        if (!Test.TestTrue(TEXT("blueprint.create handler found"),
                InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture)))
        {
            return false;
        }
        if (!Test.TestTrue(TEXT("blueprint.create succeeded"), Capture.bSuccess))
        {
            Test.AddError(FString::Printf(TEXT("blueprint.create error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        return true;
    }

    // Create a real Behavior Tree asset via behavior_tree.create for a regression probe,
    // asserting the handler was found and succeeded, and resolving its asset path (falling
    // back to a path constructed from BTPackagePath.BTName when the response omits it).
    // Returns false (and records the failure on Test) when the caller should abort. Shared
    // by the run-BT and stop-BT regressions, whose BT-creation step is otherwise identical.
    bool CreateBehaviorTreeProbe(FAutomationTestBase& Test, const FString& BTName, const FString& BasePath,
        const FString& BTPackagePath, FString& OutBTAssetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), BTName);
        Payload->SetStringField(TEXT("savePath"), BasePath);
        FTestResponseCapture Capture;
        if (!Test.TestTrue(TEXT("behavior_tree.create handler found"),
                InvokeHandlerWithCapture(TEXT("behavior_tree.create"), Payload, Capture)))
        {
            return false;
        }
        if (!Test.TestTrue(TEXT("behavior_tree.create succeeded"), Capture.bSuccess))
        {
            Test.AddError(FString::Printf(TEXT("behavior_tree.create error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        OutBTAssetPath.Reset();
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("assetPath"), OutBTAssetPath);
        }
        if (OutBTAssetPath.IsEmpty())
        {
            OutBTAssetPath = FString::Printf(TEXT("%s.%s"), *BTPackagePath, *BTName);
        }
        return true;
    }

    // Load a controller blueprint and read the object currently set on its CDO for the
    // first ValueClass-typed FObjectProperty — the same single-match reflection walk the
    // assign/stop handlers use (FindFirstObjectPropertyOfClass in AIHandler.cpp). Shared
    // by the stop-BT regression (UBehaviorTree) and the assign-BB regression (UBlackboardData).
    UObject* ReadCDOObjectProp(const FString& ControllerObjectPath, const UClass* ValueClass)
    {
        UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ControllerObjectPath));
        if (!Blueprint || !Blueprint->GeneratedClass || !ValueClass) return nullptr;
        UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
        if (!CDO) return nullptr;
        for (TFieldIterator<FObjectProperty> PropIt(Blueprint->GeneratedClass); PropIt; ++PropIt)
        {
            FObjectProperty* ObjProp = *PropIt;
            if (ObjProp && ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(ValueClass))
            {
                return ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO));
            }
        }
        return nullptr;
    }
}

// ============================================================================
// AIHandler.cpp — ai.assign_behavior_tree
// ============================================================================

// Behavioral regression for B-stop-behavior-tree-noop-wrong-var:
// ai.stop_behavior_tree must actually clear the behavior tree that
// ai.assign_behavior_tree assigned (the discovered UBehaviorTree* CDO property /
// the "DefaultBehaviorTree" member variable), not remove an unrelated
// "AssignedBehaviorTree" member variable by name. The old no-op removed the wrong
// variable and hard-coded stopped:true. This test assigns a BT, stops it, then reads
// the CDO back: it fails if the BT property is still set or stop misreports stopped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIStopBehaviorTreeClearsAssignmentTest,
    "PinWright.ai.stop_behavior_tree.ClearsAssignedTree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIStopBehaviorTreeClearsAssignmentTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BasePath = TEXT("/Game/__PW_GatewayTests/AIStopBT");
    const FString ControllerName = FString::Printf(TEXT("BP_StopBTProbe_%s"), *Suffix);
    const FString BTName = FString::Printf(TEXT("BT_StopProbe_%s"), *Suffix);
    const FString ControllerPackagePath = BasePath / ControllerName;
    const FString BTPackagePath = BasePath / BTName;
    const FString ControllerObjectPath = FString::Printf(TEXT("%s.%s"), *ControllerPackagePath, *ControllerName);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ControllerPackagePath);
        CleanupTestAsset(BTPackagePath);
    };

    // 1. Create the AI Controller blueprint.
    if (!CreateAIControllerProbe(*this, ControllerName, BasePath)) return true;

    // 2. Create a real Behavior Tree asset to assign.
    FString BTAssetPath;
    if (!CreateBehaviorTreeProbe(*this, BTName, BasePath, BTPackagePath, BTAssetPath)) return true;

    // 3. Assign the BT — assign_behavior_tree writes onto the CDO (DefaultBehaviorTree).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("controllerPath"), ControllerObjectPath);
        Payload->SetStringField(TEXT("behaviorTreePath"), BTAssetPath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.assign_behavior_tree handler found"),
                InvokeHandlerWithCapture(TEXT("ai.assign_behavior_tree"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("ai.assign_behavior_tree succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("assign_behavior_tree error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // Read the BT object currently set on the controller CDO via the same discovery the
    // assign/stop handlers use (first UBehaviorTree* property on the CDO).
    auto ReadAssignedBT = [&]() -> UObject*
    {
        return ReadCDOObjectProp(ControllerObjectPath, UBehaviorTree::StaticClass());
    };

    // Precondition: assign actually set a BT on the CDO (otherwise the test proves nothing).
    if (!TestNotNull(TEXT("BT assigned on CDO before stop"), ReadAssignedBT())) return true;

    // 4. Stop — must clear the assignment and report stopped honestly.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("controllerPath"), ControllerObjectPath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.stop_behavior_tree handler found"),
                InvokeHandlerWithCapture(TEXT("ai.stop_behavior_tree"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("ai.stop_behavior_tree succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("stop_behavior_tree error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        bool bStopped = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("stopped"), bStopped);
        }
        // Old bug: stopped was hard-coded true regardless. Now it must reflect a real clear.
        TestTrue(TEXT("stop reports stopped:true when a BT was assigned"), bStopped);
    }

    // 5. Read back — the BT must be cleared on the CDO. Old code left it set.
    TestNull(TEXT("BT cleared on CDO after stop"), ReadAssignedBT());
    return true;
}

// ============================================================================
// AIHandler.cpp — ai.run_behavior_tree
// ============================================================================

// Behavioral regression for B-run-behavior-tree-silent-noop:
// ai.run_behavior_tree is documented as an alias for ai.assign_behavior_tree, so it
// must actually wire the BT onto the controller CDO (the DefaultBehaviorTree property
// the engine reads), not add an empty, valueless "AssignedBehaviorTree" member variable
// and never touch DefaultBehaviorTree. The old no-op hard-coded assigned:true while the
// BT ended up wired nowhere — a silent success-with-no-effect. This test runs the verb,
// reads the CDO back via the same discovery the assign/stop handlers use (first
// UBehaviorTree* property on the CDO), and fails if the BT is not set there or if the
// response reports assigned:true while the CDO is empty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIRunBehaviorTreeWritesCDOTest,
    "PinWright.ai.run_behavior_tree.WritesCDO",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIRunBehaviorTreeWritesCDOTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BasePath = TEXT("/Game/__PW_GatewayTests/AIRunBT");
    const FString ControllerName = FString::Printf(TEXT("BP_RunBTProbe_%s"), *Suffix);
    const FString BTName = FString::Printf(TEXT("BT_RunProbe_%s"), *Suffix);
    const FString ControllerPackagePath = BasePath / ControllerName;
    const FString BTPackagePath = BasePath / BTName;
    const FString ControllerObjectPath = FString::Printf(TEXT("%s.%s"), *ControllerPackagePath, *ControllerName);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ControllerPackagePath);
        CleanupTestAsset(BTPackagePath);
    };

    // 1. Create the AI Controller blueprint.
    if (!CreateAIControllerProbe(*this, ControllerName, BasePath)) return true;

    // 2. Create a real Behavior Tree asset to run.
    FString BTAssetPath;
    if (!CreateBehaviorTreeProbe(*this, BTName, BasePath, BTPackagePath, BTAssetPath)) return true;

    // 3. Run the BT — the alias must write onto the CDO (DefaultBehaviorTree) and report
    //    assigned honestly. The old no-op added an empty AssignedBehaviorTree var instead.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("controllerPath"), ControllerObjectPath);
        Payload->SetStringField(TEXT("behaviorTreePath"), BTAssetPath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.run_behavior_tree handler found"),
                InvokeHandlerWithCapture(TEXT("ai.run_behavior_tree"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("ai.run_behavior_tree succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("run_behavior_tree error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        bool bAssigned = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("assigned"), bAssigned);
        }
        // Old bug: assigned was hard-coded true regardless. Now it must reflect a real CDO write.
        TestTrue(TEXT("run reports assigned:true when the BT was wired onto the CDO"), bAssigned);
    }

    // 4. Read the CDO back via the same discovery the assign/stop handlers use (first
    //    UBehaviorTree* property on the CDO). The un-fixed handler left this null — it
    //    only added an empty AssignedBehaviorTree member variable and never set a value.
    UObject* AssignedBT = ReadCDOObjectProp(ControllerObjectPath, UBehaviorTree::StaticClass());
    TestNotNull(TEXT("BT wired onto CDO after run_behavior_tree"), AssignedBT);
    return true;
}

// ============================================================================
// AIHandler.cpp — ai.assign_blackboard
// ============================================================================

// Behavioral regression for E-ai-assign-verbs-need-set-default-fallback:
// ai.assign_blackboard must actually write the Blackboard onto the controller CDO,
// not merely register a "DefaultBlackboard" member-variable definition and leave the
// CDO null. The old new-variable path called FindPropertyByName immediately after
// AddMemberVariable (before any recompile), so the property did not yet exist on
// GeneratedClass, the cast failed, propertyAssigned stayed false, and the CDO write
// targeted the stale CDO. The fix mirrors the assign_behavior_tree path: compile the
// blueprint, then set the value on the fresh CDO. This test assigns a blackboard and
// reads the CDO back via the same discovery the handler uses (first UBlackboardData*
// property on the CDO) — it fails if the assign left the CDO unset, which is exactly
// what the un-fixed handler did.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIAssignBlackboardWritesCDOTest,
    "PinWright.ai.assign_blackboard.WritesCDO",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIAssignBlackboardWritesCDOTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BasePath = TEXT("/Game/__PW_GatewayTests/AIAssignBB");
    const FString ControllerName = FString::Printf(TEXT("BP_AssignBBProbe_%s"), *Suffix);
    const FString BBName = FString::Printf(TEXT("BB_AssignProbe_%s"), *Suffix);
    const FString ControllerPackagePath = BasePath / ControllerName;
    const FString BBPackagePath = BasePath / BBName;
    const FString ControllerObjectPath = FString::Printf(TEXT("%s.%s"), *ControllerPackagePath, *ControllerName);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ControllerPackagePath);
        CleanupTestAsset(BBPackagePath);
    };

    // 1. Create the AI Controller blueprint.
    if (!CreateAIControllerProbe(*this, ControllerName, BasePath)) return true;

    // 2. Create a real Blackboard asset to assign.
    FString BBAssetPath;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), BBName);
        Payload->SetStringField(TEXT("path"), BasePath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.create_blackboard_asset handler found"),
                InvokeHandlerWithCapture(TEXT("ai.create_blackboard_asset"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("ai.create_blackboard_asset succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_blackboard_asset error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("blackboardPath"), BBAssetPath);
        }
        if (BBAssetPath.IsEmpty())
        {
            BBAssetPath = FString::Printf(TEXT("%s.%s"), *BBPackagePath, *BBName);
        }
    }

    // 3. Assign the blackboard — assign_blackboard must write onto the CDO.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("controllerPath"), ControllerObjectPath);
        Payload->SetStringField(TEXT("blackboardPath"), BBAssetPath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.assign_blackboard handler found"),
                InvokeHandlerWithCapture(TEXT("ai.assign_blackboard"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("ai.assign_blackboard succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("assign_blackboard error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        // The handler must honestly report that the value persisted on the CDO.
        bool bPropertyAssigned = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("propertyAssigned"), bPropertyAssigned);
        }
        TestTrue(TEXT("assign_blackboard reports propertyAssigned:true"), bPropertyAssigned);
    }

    // 4. Read the CDO back via the same discovery the handler uses (first
    //    UBlackboardData* property on the CDO). The un-fixed handler left this null.
    UObject* AssignedBB = ReadCDOObjectProp(ControllerObjectPath, UBlackboardData::StaticClass());
    TestNotNull(TEXT("blackboard assigned on CDO after assign_blackboard"), AssignedBB);
    return true;
}

// ============================================================================
// AIHandler.cpp — ai.add_blackboard_key
// ============================================================================

// blackboardPath provided but asset will not exist — handler produces NOT_FOUND without crashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIAddBlackboardKeyValidParamsTest,
    "PinWright.ai.add_blackboard_key.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIAddBlackboardKeyValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blackboardPath"), TEXT("/Game/AI/Blackboards/TestBB"));
    Payload->SetStringField(TEXT("keyName"), TEXT("TargetLocation"));
    Payload->SetStringField(TEXT("keyType"), TEXT("Vector"));
    TestTrue(TEXT("ai.add_blackboard_key handler found"), InvokeHandler(TEXT("ai.add_blackboard_key"), Payload));
    return true;
}

namespace
{
    // Load a Blackboard asset and return the BaseClass set on the named Object key's
    // UBlackboardKeyType_Object subobject (nullptr if the key/type is absent). Mirrors
    // what property.get reads back over the wire — the field the handler used to drop.
    UClass* ReadObjectKeyBaseClass(const FString& BlackboardObjectPath, const FName& KeyName)
    {
        UBlackboardData* Blackboard = Cast<UBlackboardData>(UEditorAssetLibrary::LoadAsset(BlackboardObjectPath));
        if (!Blackboard) return nullptr;
        for (const FBlackboardEntry& Entry : Blackboard->Keys)
        {
            if (Entry.EntryName == KeyName)
            {
                if (UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
                {
                    return ObjectKey->BaseClass;
                }
            }
        }
        return nullptr;
    }
}

// Behavioral regression for B-add-blackboard-key-base-object-class-dropped:
// ai.add_blackboard_key used to read the documented baseObjectClass param into an
// unused local (with a `// Could set base class here` stub) and create every Object
// key with the default BaseClass = /Script/CoreUObject.Object. The fix resolves the
// supplied class (bare name or /Script path), assigns it to the created
// UBlackboardKeyType_Object::BaseClass before saving, echoes the resolved class, and
// rejects an unresolvable class instead of silently falling back to UObject. This
// test creates a real Blackboard, adds an Object key with baseObjectClass:"Pawn",
// then reads the saved key's BaseClass back — it fails (BaseClass stays UObject) if
// the fix were reverted. It also asserts the success response echoes baseObjectClass
// and that an unresolvable class is rejected with a clean error, not a fake success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIAddBlackboardKeyAppliesBaseObjectClassTest,
    "PinWright.ai.add_blackboard_key.AppliesBaseObjectClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIAddBlackboardKeyAppliesBaseObjectClassTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BasePath = TEXT("/Game/__PW_GatewayTests/AIAddBBKey");
    const FString BBName = FString::Printf(TEXT("BB_BaseClassProbe_%s"), *Suffix);
    const FString BBPackagePath = BasePath / BBName;

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(BBPackagePath);
    };

    // 1. Create a real Blackboard asset.
    FString BBAssetPath;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), BBName);
        Payload->SetStringField(TEXT("path"), BasePath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.create_blackboard_asset handler found"),
                InvokeHandlerWithCapture(TEXT("ai.create_blackboard_asset"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("ai.create_blackboard_asset succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_blackboard_asset error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("blackboardPath"), BBAssetPath);
        }
        if (BBAssetPath.IsEmpty())
        {
            BBAssetPath = FString::Printf(TEXT("%s.%s"), *BBPackagePath, *BBName);
        }
    }

    // 2. Add an Object key with a bare-name baseObjectClass ("Pawn"). The fix must
    //    resolve it to /Script/Engine.Pawn and set it on the key's BaseClass, and the
    //    success response must echo the resolved class.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blackboardPath"), BBAssetPath);
        Payload->SetStringField(TEXT("keyName"), TEXT("TargetPawn"));
        Payload->SetStringField(TEXT("keyType"), TEXT("Object"));
        Payload->SetStringField(TEXT("baseObjectClass"), TEXT("Pawn"));
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.add_blackboard_key handler found"),
                InvokeHandlerWithCapture(TEXT("ai.add_blackboard_key"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("ai.add_blackboard_key succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_blackboard_key error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        // The success response must now echo the resolved baseObjectClass (it never did before).
        FString EchoedBaseClass;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("baseObjectClass"), EchoedBaseClass);
        }
        TestEqual(TEXT("response echoes resolved baseObjectClass"),
            EchoedBaseClass, FString(TEXT("/Script/Engine.Pawn")));
    }

    // 3. Read the saved key's BaseClass back from the asset. The un-fixed handler left
    //    this as UObject; the fix sets it to Pawn.
    UClass* AppliedBaseClass = ReadObjectKeyBaseClass(BBAssetPath, FName(TEXT("TargetPawn")));
    if (!TestNotNull(TEXT("Object key has a BaseClass set"), AppliedBaseClass)) return true;
    TestEqual(TEXT("Object key BaseClass is the requested Pawn, not UObject"),
        AppliedBaseClass, APawn::StaticClass());

    // 4. An unresolvable baseObjectClass must be rejected with a clean error, not a
    //    silent fallback to UObject (the old code would have fake-succeeded).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blackboardPath"), BBAssetPath);
        Payload->SetStringField(TEXT("keyName"), TEXT("BadKey"));
        Payload->SetStringField(TEXT("keyType"), TEXT("Object"));
        Payload->SetStringField(TEXT("baseObjectClass"), TEXT("DefinitelyNotARealClass_ZZZ"));
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.add_blackboard_key handler found (bad class)"),
                InvokeHandlerWithCapture(TEXT("ai.add_blackboard_key"), Payload, Capture))) return true;
        TestFalse(TEXT("unresolvable baseObjectClass is rejected, not fake-succeeded"), Capture.bSuccess);
        TestEqual(TEXT("unresolvable baseObjectClass emits CLASS_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
    }

    return true;
}

// ============================================================================
// AIHandler.cpp — ai.add_composite_node (disabled / orphaning dead end)
// ============================================================================
// Board E-ai-bt-authoring-verbs-dead-end: ai.add_composite_node returned success
// with no node id and orphaned the composite on a graph-less Behavior Tree
// asset. It is now hard-disabled (DEPRECATED_HANDLER) like ai.add_decorator /
// ai.add_service, steering authoring to behavior_tree.create -> add_node ->
// connect_nodes. The disabled route has no live inputs: an empty payload must reach
// the deprecation error without a required-parameter gate getting in front of it.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIAddCompositeNodeDeprecatedTest,
    "PinWright.ai.add_composite_node.DisabledOrphaningSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIAddCompositeNodeDeprecatedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("disabled composite route declares no behaviorTreePath input"),
        GetRegisteredParamSpec(TEXT("ai.add_composite_node"), TEXT("behaviorTreePath")) == nullptr);
    TestTrue(TEXT("disabled composite route declares no compositeType input"),
        GetRegisteredParamSpec(TEXT("ai.add_composite_node"), TEXT("compositeType")) == nullptr);
    TestHandlerReturnsDeprecated(*this, TEXT("ai.add_composite_node"), Payload,
        TEXT("behavior_tree.add_node"));
    return true;
}

// ============================================================================
// AIHandler.cpp — ai.add_task_node (disabled / orphaning dead end)
// ============================================================================
// Board E-ai-bt-authoring-verbs-dead-end: ai.add_task_node built the task UObject
// but never attached it to anything and returned success with no node id (a pure
// orphan). Now hard-disabled (DEPRECATED_HANDLER) like ai.add_decorator /
// ai.add_service. Same revert-catching shape as the composite test above.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIAddTaskNodeDeprecatedTest,
    "PinWright.ai.add_task_node.DisabledOrphaningSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIAddTaskNodeDeprecatedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("disabled task route declares no behaviorTreePath input"),
        GetRegisteredParamSpec(TEXT("ai.add_task_node"), TEXT("behaviorTreePath")) == nullptr);
    TestTrue(TEXT("disabled task route declares no taskType input"),
        GetRegisteredParamSpec(TEXT("ai.add_task_node"), TEXT("taskType")) == nullptr);
    TestHandlerReturnsDeprecated(*this, TEXT("ai.add_task_node"), Payload,
        TEXT("behavior_tree.add_node"));
    return true;
}

// ============================================================================
// AIHandler.cpp — ai.add_decorator (disabled / orphaning dead end)
// ============================================================================
// Board E-ai-bt-authoring-verbs-dead-end / F-bt-attach-decorator-service-to-parent:
// ai.add_decorator created orphaned Behavior Tree decorators and is hard-disabled
// (DEPRECATED_HANDLER), steering to behavior_tree.attach_decorator. Same
// revert-catching shape as the composite/task tests above so the whole disabled
// BT-authoring surface is guarded uniformly.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIAddDecoratorDeprecatedTest,
    "PinWright.ai.add_decorator.DisabledOrphaningSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIAddDecoratorDeprecatedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("behaviorTreePath"), TEXT("/Game/AI/BehaviorTrees/BT_DoesNotExist"));
    Payload->SetStringField(TEXT("decoratorType"), TEXT("Blackboard"));
    TestHandlerReturnsDeprecated(*this, TEXT("ai.add_decorator"), Payload,
        TEXT("behavior_tree.attach_decorator"));
    return true;
}

// ============================================================================
// AIHandler.cpp — ai.add_service (disabled / orphaning dead end)
// ============================================================================
// Board E-ai-bt-authoring-verbs-dead-end / F-bt-attach-decorator-service-to-parent:
// ai.add_service only created a fake Behavior Tree service reference and is
// hard-disabled (DEPRECATED_HANDLER), steering to behavior_tree.attach_service.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIAddServiceDeprecatedTest,
    "PinWright.ai.add_service.DisabledOrphaningSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIAddServiceDeprecatedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("behaviorTreePath"), TEXT("/Game/AI/BehaviorTrees/BT_DoesNotExist"));
    Payload->SetStringField(TEXT("serviceType"), TEXT("Blackboard"));
    TestHandlerReturnsDeprecated(*this, TEXT("ai.add_service"), Payload,
        TEXT("behavior_tree.attach_service"));
    return true;
}

// ============================================================================
// AIHandler.cpp — ai.configure_slot_behavior
// ============================================================================

// Behavioral regression for B-configure-slot-behavior-ignores-behavior-and-tags:
// ai.configure_slot_behavior used to read behaviorType into a local and never use
// it, and silently dropped any activityTag not already in the gameplay-tag registry,
// while always reporting {behaviorCount, "Slot behavior configured"} success. Both
// were success-shaped no-ops. The fix validates both params before mutating: an
// unresolvable behaviorType and any unregistered tag are now rejected with
// INVALID_PARAMS (the dropped tags surfaced in droppedTags) instead of a fake
// success. This test creates a real SmartObjectDefinition + slot, then asserts both
// rejection paths fire — it fails if either defect were reverted (the old handler
// returned success for both inputs).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIConfigureSlotBehaviorRejectsNoOpInputsTest,
    "PinWright.ai.configure_slot_behavior.RejectsNoOpInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIConfigureSlotBehaviorRejectsNoOpInputsTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BasePath = TEXT("/Game/__PW_GatewayTests/AISmartObjects");
    const FString DefName = FString::Printf(TEXT("SOD_SlotBehaviorProbe_%s"), *Suffix);
    const FString DefPackagePath = BasePath / DefName;

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(DefPackagePath);
    };

    // 1. Create a SmartObjectDefinition asset.
    FString DefinitionPath;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), DefName);
        Payload->SetStringField(TEXT("path"), BasePath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.create_smart_object_definition handler found"),
                InvokeHandlerWithCapture(TEXT("ai.create_smart_object_definition"), Payload, Capture))) return true;
        // The Smart Object handlers are reflection-only against the optional
        // SmartObjects plugin: on hosts where it is disabled they reject with
        // PLUGIN_DISABLED and there is no real slot to configure — skip the
        // behavioral assertions on such a host rather than fail spuriously.
        if (!Capture.bSuccess && Capture.ErrorCode == TEXT("PLUGIN_DISABLED"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
                TEXT("SmartObjects plugin disabled on this host; skipping configure_slot_behavior behavioral checks."));
            return true;
        }
        if (!TestTrue(TEXT("ai.create_smart_object_definition succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_smart_object_definition error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("definitionPath"), DefinitionPath);
        }
        if (DefinitionPath.IsEmpty())
        {
            DefinitionPath = DefPackagePath;
        }
    }

    // 2. Add a slot so there is a valid slotIndex 0 to target.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("definitionPath"), DefinitionPath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.add_smart_object_slot handler found"),
                InvokeHandlerWithCapture(TEXT("ai.add_smart_object_slot"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("ai.add_smart_object_slot succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_smart_object_slot error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        // slotIndex 0 confirms the handler's reflected Slots-array insertion landed
        // on the freshly created definition.
        double ReportedSlotIndex = -1.0;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(TEXT("slotIndex"), ReportedSlotIndex);
        }
        TestEqual(TEXT("add_smart_object_slot reports slotIndex 0"), ReportedSlotIndex, 0.0);
    }

    // 3. Defect 1 — an unresolvable behaviorType must be rejected, not fake-succeeded.
    //    The old handler ignored behaviorType entirely and returned success.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("definitionPath"), DefinitionPath);
        Payload->SetNumberField(TEXT("slotIndex"), 0);
        Payload->SetStringField(TEXT("behaviorType"), TEXT("DefinitelyNotARealBehaviorType_ZZZ"));
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.configure_slot_behavior handler found (behaviorType case)"),
                InvokeHandlerWithCapture(TEXT("ai.configure_slot_behavior"), Payload, Capture))) return true;
        TestTrue(TEXT("configure_slot_behavior responded (behaviorType case)"), Capture.bWasCalled);
        TestFalse(TEXT("unknown behaviorType is rejected, not fake-succeeded"), Capture.bSuccess);
        TestEqual(TEXT("unknown behaviorType emits INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    // 4. Defect 2 — an unregistered activityTag must be rejected and surfaced in
    //    droppedTags, not silently dropped under a success response.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("definitionPath"), DefinitionPath);
        Payload->SetNumberField(TEXT("slotIndex"), 0);
        TArray<TSharedPtr<FJsonValue>> Tags;
        Tags.Add(MakeShared<FJsonValueString>(TEXT("AI.Activity.NeverRegisteredFuzzTag_ZZZ")));
        Payload->SetArrayField(TEXT("activityTags"), Tags);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.configure_slot_behavior handler found (activityTags case)"),
                InvokeHandlerWithCapture(TEXT("ai.configure_slot_behavior"), Payload, Capture))) return true;
        TestTrue(TEXT("configure_slot_behavior responded (activityTags case)"), Capture.bWasCalled);
        TestFalse(TEXT("unregistered activityTag is rejected, not silently dropped"), Capture.bSuccess);
        TestEqual(TEXT("dropped tag emits INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        // The dropped tag must be surfaced so the no-op is visible to the caller.
        TestTrue(TEXT("droppedTags lists the unregistered tag"),
            JsonStringArrayContains(Capture.Result, TEXT("droppedTags"),
                TEXT("AI.Activity.NeverRegisteredFuzzTag_ZZZ")));
    }

    return true;
}

// ============================================================================
// BehaviorTreeHandler.cpp — behavior_tree.create
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTCreateValidParamsTest,
    "PinWright.behavior_tree.create.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTCreateValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestBehaviorTree"));
    Payload->SetStringField(TEXT("savePath"), TEXT("/Game/AI/BehaviorTrees"));
    TestTrue(TEXT("behavior_tree.create handler found"), InvokeHandler(TEXT("behavior_tree.create"), Payload));
    CleanupTestAsset(TEXT("/Game/AI/BehaviorTrees/TestBehaviorTree"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBehaviorTreeCreateBlueprintNodeClassesTest,
    "PinWright.behavior_tree.create_blueprint_node_classes.DefaultParents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBehaviorTreeCreateBlueprintNodeClassesTest::RunTest(const FString& Parameters)
{
    struct FCreateCase
    {
        const TCHAR* Method;
        const TCHAR* NamePrefix;
        UClass* ExpectedParent;
    };

    const FString SavePath = TEXT("/Game/__PW_GatewayTests/BehaviorTreeBlueprintNodes");
    TArray<FString> PackagePaths;
    ON_SCOPE_EXIT
    {
        for (const FString& PackagePath : PackagePaths)
        {
            CleanupTestAsset(PackagePath);
        }
    };

    const FCreateCase Cases[] = {
        { TEXT("behavior_tree.create_task_blueprint"), TEXT("BTTask_Test_"), UBTTask_BlueprintBase::StaticClass() },
        { TEXT("behavior_tree.create_service_blueprint"), TEXT("BTService_Test_"), UBTService_BlueprintBase::StaticClass() },
        { TEXT("behavior_tree.create_decorator_blueprint"), TEXT("BTDecorator_Test_"), UBTDecorator_BlueprintBase::StaticClass() }
    };

    for (const FCreateCase& CreateCase : Cases)
    {
        const FString Name = FString::Printf(
            TEXT("%s%s"),
            CreateCase.NamePrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackagePath = SavePath / Name;
        PackagePaths.Add(PackagePath);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Name);
        Payload->SetStringField(TEXT("savePath"), SavePath);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(CreateCase.Method, Payload, Capture);
        TestTrue(FString::Printf(TEXT("%s handler registered"), CreateCase.Method), bFound);
        if (!bFound)
        {
            continue;
        }

        TestTrue(FString::Printf(TEXT("%s responded"), CreateCase.Method), Capture.bWasCalled);
        if (!TestTrue(FString::Printf(TEXT("%s succeeded"), CreateCase.Method), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("%s error: %s %s"), CreateCase.Method, *Capture.ErrorCode, *Capture.Message));
            continue;
        }

        FString AssetPath;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("assetPath"), AssetPath);
        }
        if (AssetPath.IsEmpty())
        {
            AssetPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *FPackageName::GetLongPackageAssetName(PackagePath));
        }

        UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(AssetPath));
        if (!TestNotNull(FString::Printf(TEXT("%s created loadable UBlueprint"), CreateCase.Method), Blueprint))
        {
            continue;
        }

        TestTrue(FString::Printf(TEXT("%s parent class is %s or child"),
                CreateCase.Method,
                *CreateCase.ExpectedParent->GetName()),
            Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(CreateCase.ExpectedParent));
    }

    return true;
}

// ============================================================================
// NavigationHandler.cpp — navigation.configure_nav_mesh_settings
// ============================================================================

// All params are optional — empty payload must not crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavConfigureNavMeshSettingsNoCrashTest,
    "PinWright.navigation.configure_nav_mesh_settings.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavConfigureNavMeshSettingsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("tileSizeUU"), 1000.0);
    Payload->SetNumberField(TEXT("cellSize"), 19.0);
    TestTrue(TEXT("navigation.configure_nav_mesh_settings handler found"),
        InvokeHandler(TEXT("navigation.configure_nav_mesh_settings"), Payload));
    return true;
}

// ============================================================================
// NavigationHandler.cpp — navigation.set_nav_agent_properties
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavSetNavAgentPropertiesNoCrashTest,
    "PinWright.navigation.set_nav_agent_properties.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavSetNavAgentPropertiesNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("agentRadius"), 35.0);
    Payload->SetNumberField(TEXT("agentHeight"), 144.0);
    TestTrue(TEXT("navigation.set_nav_agent_properties handler found"),
        InvokeHandler(TEXT("navigation.set_nav_agent_properties"), Payload));
    return true;
}

// Regression for E-navmesh-config-requires-bounds-volume-prereq: when no
// RecastNavMesh exists (no NavMeshBoundsVolume placed — the common fresh-level
// state and the default state of the automation test world), both nav-config
// setters fail with [NO_NAVMESH], and that error message must NAME the remedy
// (create a NavMeshBoundsVolume via volume.create_nav_mesh_bounds_volume) the
// way [NO_GAME_INSTANCE] names "Start Play-In-Editor first." Reverting the
// error-string sharpening drops "NavMeshBoundsVolume" from the message and
// fails this test. The assertion is gated on the NO_NAVMESH branch actually
// being hit, so it can never flake if the test world happens to carry a nav mesh
// — but that skip is reported as a WARNING rather than passing silently, and any
// OTHER error code is an AddError, so a regression that changes the code cannot
// slip out through the same gate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavConfigNoNavMeshErrorNamesRemedyTest,
    "PinWright.navigation.no_navmesh_error_names_remedy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavConfigNoNavMeshErrorNamesRemedyTest::RunTest(const FString& Parameters)
{
    auto CheckRemedyNamed = [this](const TCHAR* Method)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("tileSizeUU"), 1000.0);
        Payload->SetNumberField(TEXT("agentRadius"), 35.0);
        FTestResponseCapture Capture;
        if (!TestTrue(*FString::Printf(TEXT("%s handler found"), Method),
                InvokeHandlerWithCapture(Method, Payload, Capture)))
        {
            return;
        }
        // Only the no-nav-mesh failure path is under test here. If the editor
        // world already carries a RecastNavMesh the setter succeeds and the
        // [NO_NAVMESH] branch isn't exercised — that's fine, not a failure.
        // But it must not be SILENT: this is the sole coverage of the shared
        // remedy string anywhere in Tests/, and another test in the same session
        // can leave a NavMeshBoundsVolume in the editor world (see
        // Tests/World/TestVolumeHandlers.cpp), which would take every assertion
        // below out of play with nothing in the log to say so.
        if (!Capture.bSuccess && Capture.ErrorCode == TEXT("NO_NAVMESH"))
        {
            TestTrue(*FString::Printf(TEXT("%s [NO_NAVMESH] message names the NavMeshBoundsVolume remedy"), Method),
                Capture.Message.Contains(TEXT("NavMeshBoundsVolume")));
            TestTrue(*FString::Printf(TEXT("%s [NO_NAVMESH] message names the create verb"), Method),
                Capture.Message.Contains(TEXT("create_nav_mesh_bounds_volume")));
        }
        else if (Capture.bSuccess)
        {
            AddWarning(FString::Printf(
                TEXT("%s: editor world already carries a RecastNavMesh; skipping ")
                TEXT("navigation.no_navmesh_error_names_remedy."), Method));
        }
        else
        {
            // Failed for some OTHER reason. The setter is supposed to reach either
            // success or NO_NAVMESH from a bare editor world, so a different code is
            // a regression in the handler's error path, not an environment quirk.
            AddError(FString::Printf(
                TEXT("%s failed with unexpected error code [%s] (expected success or ")
                TEXT("[NO_NAVMESH]): %s"), Method, *Capture.ErrorCode, *Capture.Message));
        }
    };

    CheckRemedyNamed(TEXT("navigation.configure_nav_mesh_settings"));
    CheckRemedyNamed(TEXT("navigation.set_nav_agent_properties"));
    return true;
}

// ============================================================================
// NavigationHandler.cpp — navigation.rebuild_navigation (RPC_NO_PARAMS)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavRebuildNavigationNoCrashTest,
    "PinWright.navigation.rebuild_navigation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavRebuildNavigationNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("navigation.rebuild_navigation handler found"),
        InvokeHandler(TEXT("navigation.rebuild_navigation"), Payload));
    return true;
}

// ============================================================================
// NavigationHandler.cpp — navigation.create_nav_modifier_component
// ============================================================================

// Regression for E-nav-modifier-create-no-areaclass-echo: create_nav_modifier_component
// resolves and applies areaClass + failsafeExtent onto the new component template, but its
// success result used to echo only componentName / blueprintPath / existsAfter + the asset
// verification block — none of the nav-area / extent it just wrote. The natural verify
// surface (scs.get / asset.dump) is intentionally sparse and OMITS AreaClass whenever it
// equals the UNavModifierComponent constructor default (NavArea_Null), so its absence there
// can't be told apart from a silently-failed set. The fix echoes resolvedAreaClass (read off
// ModComp->AreaClass->GetPathName() after the write) and failsafeExtent. This test creates a
// BP asset, adds a nav modifier with an explicit non-default areaClass + extent, and asserts
// the result echoes both fields read straight off the stored component. It fails if the echo
// were reverted (the fields would be absent) or if resolvedAreaClass were sourced from the
// raw payload rather than the resolved class on the template.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavCreateNavModifierComponentEchoesAreaClassTest,
    "PinWright.navigation.create_nav_modifier_component.EchoesResolvedAreaClassFromAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavCreateNavModifierComponentEchoesAreaClassTest::RunTest(const FString& Parameters)
{
    const FString BlueprintName = FString::Printf(
        TEXT("BP_NavModEcho_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString BasePath = TEXT("/Game/__PW_GatewayTests/NavModifierEcho");
    const FString PackagePath = BasePath / BlueprintName;
    const FString BlueprintObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    // 1. Create a real BP asset to host the nav modifier component (an AI Controller BP is a
    //    convenient asset with an SCS that create_nav_modifier_component can mutate).
    if (!CreateAIControllerProbe(*this, BlueprintName, BasePath)) return true;

    // 2. Add a NavModifierComponent with an explicit, non-default area class + failsafe extent.
    //    NavArea_Obstacle is deliberately NOT the constructor default (NavArea_Null), so the
    //    echoed resolvedAreaClass is meaningful and would survive a sparse dump too.
    const FString AreaClassPath = UNavArea_Obstacle::StaticClass()->GetPathName();
    const FVector ExpectedExtent(200.0, 150.0, 75.0);

    FTestResponseCapture Capture;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BlueprintObjectPath);
        Payload->SetStringField(TEXT("componentName"), TEXT("NavModifier"));
        Payload->SetStringField(TEXT("areaClass"), AreaClassPath);
        Payload->SetObjectField(TEXT("failsafeExtent"), MakeNavVecJson(ExpectedExtent));

        if (!TestTrue(TEXT("navigation.create_nav_modifier_component handler found"),
                InvokeHandlerWithCapture(TEXT("navigation.create_nav_modifier_component"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("navigation.create_nav_modifier_component succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_nav_modifier_component error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    if (!TestTrue(TEXT("create_nav_modifier_component returned a result object"), Capture.Result.IsValid())) return true;

    // 3. resolvedAreaClass must echo the path of the class that actually landed on the
    //    component template — absent before the fix.
    FString EchoedAreaClass;
    if (TestTrue(TEXT("result echoes resolvedAreaClass"),
            Capture.Result->TryGetStringField(TEXT("resolvedAreaClass"), EchoedAreaClass)))
    {
        TestEqual(TEXT("echoed resolvedAreaClass is the applied NavArea_Obstacle path"),
            EchoedAreaClass, AreaClassPath);
    }

    // 4. failsafeExtent must echo the {x,y,z} that was applied — also absent before the fix.
    const TSharedPtr<FJsonObject>* ExtentObj = nullptr;
    if (TestTrue(TEXT("result echoes failsafeExtent object"),
            Capture.Result->TryGetObjectField(TEXT("failsafeExtent"), ExtentObj) && ExtentObj && ExtentObj->IsValid()))
    {
        TestEqual(TEXT("failsafeExtent.x"), (*ExtentObj)->GetNumberField(TEXT("x")), ExpectedExtent.X);
        TestEqual(TEXT("failsafeExtent.y"), (*ExtentObj)->GetNumberField(TEXT("y")), ExpectedExtent.Y);
        TestEqual(TEXT("failsafeExtent.z"), (*ExtentObj)->GetNumberField(TEXT("z")), ExpectedExtent.Z);
    }

    // 5. Cross-check the echo against the live template: resolvedAreaClass must be read off the
    //    stored component, not just reflected from the payload. Reload the BP and confirm the
    //    component's AreaClass path equals what was echoed.
    UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(BlueprintObjectPath));
    if (TestNotNull(TEXT("blueprint reloads"), Blueprint)
        && TestNotNull(TEXT("blueprint has SCS"), Blueprint->SimpleConstructionScript.Get()))
    {
        UNavModifierComponent* ModComp = FindFirstSCSComponentTemplate<UNavModifierComponent>(Blueprint);
        if (TestNotNull(TEXT("nav modifier component template present"), ModComp) && ModComp->AreaClass)
        {
            TestEqual(TEXT("echoed resolvedAreaClass matches the stored template AreaClass"),
                EchoedAreaClass, ModComp->AreaClass->GetPathName());
        }
    }

    return true;
}

// ============================================================================
// NavigationHandler.cpp — navigation.create_nav_link_proxy
// ============================================================================

// Behavioral regression for B-nav-link-proxy-appends-default-link:
// ANavLinkProxy's constructor pre-seeds PointLinks[0] with a default link, and the
// old create_nav_link_proxy did PointLinks.Add(NewLink) — leaving the caller's link
// at index 1 and a stray default at index 0. That broke the sibling RPCs that address
// PointLinks[0]: configure_nav_link edits [0] and the smart-link copy reads [0], so
// both operated on the phantom default the caller never asked for. The fix replaces
// PointLinks[0] instead of appending. This test spawns a proxy, asserts it carries
// exactly ONE link holding the caller's geometry (not two), then runs configure_nav_link
// and asserts the snapRadius landed on that same caller link. It fails if the append
// behavior were reverted (the proxy would carry two links and snapRadius would land on
// the phantom default while the caller's link stayed untouched).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavCreateNavLinkProxyReplacesDefaultLinkTest,
    "PinWright.navigation.create_nav_link_proxy.ReplacesDefaultLink",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavCreateNavLinkProxyReplacesDefaultLinkTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world available"), World)) return true;

    const FString ActorName = FString::Printf(
        TEXT("NavLinkProxy_AppendProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Destroys every actor this test spawns and restores the persistent level's dirty
    // flag on scope exit, so the create handler's MarkPackageDirty() doesn't leave the
    // host map dirty (and get it saved at editor.save_all / shutdown).
    FScopedEditorWorldActorGuard WorldGuard;

    // Locate the spawned proxy by label/name so we can read its PointLinks back.
    auto FindProxy = [&]() -> ANavLinkProxy*
    {
        for (TActorIterator<ANavLinkProxy> It(World); It; ++It)
        {
            if (It->GetActorLabel() == ActorName || It->GetName() == ActorName) { return *It; }
        }
        return nullptr;
    };

    const FVector StartPoint(600.0, 0.0, 300.0);
    const FVector EndPoint(900.0, 0.0, 0.0);

    // 1. Spawn the proxy with the caller's link geometry.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorName);
        Payload->SetObjectField(TEXT("location"), MakeNavVecJson(FVector(600.0, 0.0, 0.0)));
        Payload->SetObjectField(TEXT("startPoint"), MakeNavVecJson(StartPoint));
        Payload->SetObjectField(TEXT("endPoint"), MakeNavVecJson(EndPoint));
        Payload->SetStringField(TEXT("direction"), TEXT("BothWays"));

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("navigation.create_nav_link_proxy handler found"),
                InvokeHandlerWithCapture(TEXT("navigation.create_nav_link_proxy"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("navigation.create_nav_link_proxy succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_nav_link_proxy error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    ANavLinkProxy* Proxy = FindProxy();
    if (!TestNotNull(TEXT("spawned NavLinkProxy found in world"), Proxy)) return true;

    // 2. The proxy must carry exactly ONE link — the caller's — not the constructor
    //    default plus the appended caller link. The old append left Num()==2.
    if (!TestEqual(TEXT("proxy carries exactly one PointLink (no phantom default)"),
            Proxy->PointLinks.Num(), 1)) return true;

    // 3. That single link must hold the caller's geometry, proving the default was
    //    replaced (not kept at index 0 with the caller's link pushed to index 1).
    TestEqual(TEXT("PointLinks[0].Left is the caller startPoint"), Proxy->PointLinks[0].Left, StartPoint);
    TestEqual(TEXT("PointLinks[0].Right is the caller endPoint"), Proxy->PointLinks[0].Right, EndPoint);

    // 4. configure_nav_link edits PointLinks[0]; with the fix that is the caller's link.
    //    Apply snapRadius 50 and confirm it landed on the caller's link, and that the
    //    proxy still holds a single link (no resurrected duplicate).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorName);
        Payload->SetObjectField(TEXT("startPoint"), MakeNavVecJson(StartPoint));
        Payload->SetObjectField(TEXT("endPoint"), MakeNavVecJson(EndPoint));
        Payload->SetNumberField(TEXT("snapRadius"), 50.0);

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("navigation.configure_nav_link handler found"),
                InvokeHandlerWithCapture(TEXT("navigation.configure_nav_link"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("navigation.configure_nav_link succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("configure_nav_link error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    if (!TestEqual(TEXT("proxy still carries exactly one PointLink after configure"),
            Proxy->PointLinks.Num(), 1)) return true;
    // Old bug: snapRadius:50 landed on the phantom default at [0] while the caller's link
    // (then at [1]) stayed at SnapRadius 30. With the fix the caller's link IS [0].
    TestEqual(TEXT("snapRadius landed on the caller's link"), Proxy->PointLinks[0].SnapRadius, 50.0f);
    return true;
}

// ============================================================================
// NavigationHandler.cpp — navigation.configure_nav_link
// ============================================================================

// Regression for E-configure-nav-link-no-echo: configure_nav_link applies the caller's
// endpoints / direction / snapRadius to PointLinks[0] but its success result used to echo
// only actorName + modified + actor-identity verification — none of the geometry it just
// wrote, forcing a separate (spilling) actor.describe to confirm the re-tune. The fix echoes
// the resolved startPoint / endPoint / direction / snapRadius read straight off the edited
// link, mirroring set_nav_area_class's areaClass echo. This test spawns a proxy, re-tunes it,
// and asserts the result echoes back exactly the applied geometry. It fails if the echo were
// reverted (the fields would be absent) or if the echo were sourced from the payload rather
// than the stored link (the asserted values are read from the actual PointLinks[0] write).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavConfigureNavLinkEchoesAppliedGeometryTest,
    "PinWright.navigation.configure_nav_link.EchoesAppliedGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavConfigureNavLinkEchoesAppliedGeometryTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world available"), World)) return true;

    const FString ActorName = FString::Printf(
        TEXT("NavLinkProxy_EchoProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Destroys spawned actors and restores the level's dirty flag on scope exit.
    FScopedEditorWorldActorGuard WorldGuard;

    const FVector StartPoint(0.0, 0.0, 0.0);
    const FVector EndPoint(350.0, 0.0, 140.0);
    const double SnapRadius = 40.0;

    // 1. Spawn the proxy to configure.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorName);
        Payload->SetObjectField(TEXT("location"), MakeNavVecJson(FVector(0.0, 0.0, 0.0)));
        Payload->SetObjectField(TEXT("startPoint"), MakeNavVecJson(FVector(-100.0, 0.0, 0.0)));
        Payload->SetObjectField(TEXT("endPoint"), MakeNavVecJson(FVector(100.0, 0.0, 0.0)));

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("navigation.create_nav_link_proxy handler found"),
                InvokeHandlerWithCapture(TEXT("navigation.create_nav_link_proxy"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("navigation.create_nav_link_proxy succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_nav_link_proxy error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // 2. Re-tune the link and capture the success result.
    FTestResponseCapture Capture;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorName);
        Payload->SetObjectField(TEXT("startPoint"), MakeNavVecJson(StartPoint));
        Payload->SetObjectField(TEXT("endPoint"), MakeNavVecJson(EndPoint));
        Payload->SetStringField(TEXT("direction"), TEXT("BothWays"));
        Payload->SetNumberField(TEXT("snapRadius"), SnapRadius);

        if (!TestTrue(TEXT("navigation.configure_nav_link handler found"),
                InvokeHandlerWithCapture(TEXT("navigation.configure_nav_link"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("navigation.configure_nav_link succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("configure_nav_link error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    if (!TestTrue(TEXT("configure_nav_link returned a result object"), Capture.Result.IsValid())) return true;

    // 3. The result must echo the applied geometry — these fields were entirely absent
    //    before the fix, leaving the caller no inline confirmation of the re-tune.
    TestTrue(TEXT("result echoes modified=true"),
        Capture.Result->HasField(TEXT("modified")) && Capture.Result->GetBoolField(TEXT("modified")));

    // direction echoed verbatim.
    FString EchoedDirection;
    if (TestTrue(TEXT("result echoes direction"),
            Capture.Result->TryGetStringField(TEXT("direction"), EchoedDirection)))
    {
        TestEqual(TEXT("echoed direction matches applied"), EchoedDirection, FString(TEXT("BothWays")));
    }

    // snapRadius echoed as the value stored on the link.
    double EchoedSnap = -1.0;
    if (TestTrue(TEXT("result echoes snapRadius"),
            Capture.Result->TryGetNumberField(TEXT("snapRadius"), EchoedSnap)))
    {
        TestEqual(TEXT("echoed snapRadius matches applied"), EchoedSnap, SnapRadius);
    }

    // startPoint / endPoint echoed as {x,y,z}, matching the input shape, and read off the
    // stored link (so they reflect the actual write, not just the raw payload).
    auto CheckEchoedVec = [&](const TCHAR* Field, const FVector& Expected)
    {
        const TSharedPtr<FJsonObject>* VecObj = nullptr;
        if (!TestTrue(FString::Printf(TEXT("result echoes %s object"), Field),
                Capture.Result->TryGetObjectField(Field, VecObj) && VecObj && VecObj->IsValid())) return;
        TestEqual(FString::Printf(TEXT("%s.x"), Field), (*VecObj)->GetNumberField(TEXT("x")), Expected.X);
        TestEqual(FString::Printf(TEXT("%s.y"), Field), (*VecObj)->GetNumberField(TEXT("y")), Expected.Y);
        TestEqual(FString::Printf(TEXT("%s.z"), Field), (*VecObj)->GetNumberField(TEXT("z")), Expected.Z);
    };
    CheckEchoedVec(TEXT("startPoint"), StartPoint);
    CheckEchoedVec(TEXT("endPoint"), EndPoint);

    return true;
}

// ============================================================================
// NavigationHandler.cpp — navigation.get_navigation_info (RPC_NO_PARAMS)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavGetNavigationInfoNoCrashTest,
    "PinWright.navigation.get_navigation_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavGetNavigationInfoNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("navigation.get_navigation_info handler found"),
        InvokeHandler(TEXT("navigation.get_navigation_info"), Payload));
    return true;
}

// Regression for E-nav-modifier-create-no-areaclass-echo: create_nav_modifier_component
// applies the resolved AreaClass / FailsafeExtent to the new component template but its
// success result used to echo only componentName / blueprintPath / existsAfter + the
// asset-verification block — never the area class it just authored. That forced the caller
// onto the intentionally-sparse scs.get / asset.dump readback, which DROPS AreaClass when it
// equals the UNavModifierComponent constructor default (NavArea_Null) — making
// "omitted because == default (correct)" indistinguishable from "set silently failed". The
// fix echoes resolvedAreaClass (read off ModComp->AreaClass after resolution) + failsafeExtent.
// This test authors a transient Blueprint, creates the component (a) with an explicit
// NON-default areaClass and (b) WITHOUT an areaClass arg, and asserts the result echoes back
// the resolved class in BOTH cases — crucially the no-arg fallback to NavArea_Null, the exact
// equals-default value the sparse readback can't confirm. It fails if the echo were reverted
// (the fields would be absent) or sourced from the raw arg rather than the stored template
// (the no-arg case asserts the template's NavArea_Null default, which the payload never carried).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavCreateNavModifierComponentEchoesResolvedAreaClassTest,
    "PinWright.navigation.create_nav_modifier_component.EchoesResolvedAreaClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavCreateNavModifierComponentEchoesResolvedAreaClassTest::RunTest(const FString& Parameters)
{
    // Author a throwaway transient Actor Blueprint to attach the nav modifier to. The shared
    // helper places it under GetTransientPackage() with a collision-safe unique name and lets
    // GC reclaim it; its GetPathName() resolves through the handler's LoadObject by path.
    UBlueprint* Blueprint = CompilerTestUtils::CreateTransientTestBP(TEXT("BP_NavModifierEchoProbe"));
    if (!TestNotNull(TEXT("blueprint created"), Blueprint)) return true;
    const FString BPPath = Blueprint->GetPathName();

    // Case (a): explicit non-default areaClass — proves the echo reflects the resolved class.
    {
        const FString ExplicitArea = UNavArea_Obstacle::StaticClass()->GetPathName();

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BPPath);
        Payload->SetStringField(TEXT("componentName"), TEXT("NavModObstacle"));
        Payload->SetStringField(TEXT("areaClass"), ExplicitArea);
        Payload->SetObjectField(TEXT("failsafeExtent"), MakeNavVecJson(FVector(250.0, 250.0, 250.0)));

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("create_nav_modifier_component handler found"),
                InvokeHandlerWithCapture(TEXT("navigation.create_nav_modifier_component"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("create_nav_modifier_component succeeded (explicit area)"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        if (!TestTrue(TEXT("result object present (explicit area)"), Capture.Result.IsValid())) return true;

        FString EchoedArea;
        if (TestTrue(TEXT("result echoes resolvedAreaClass (explicit area)"),
                Capture.Result->TryGetStringField(TEXT("resolvedAreaClass"), EchoedArea)))
        {
            TestEqual(TEXT("echoed resolvedAreaClass matches the explicit areaClass"), EchoedArea, ExplicitArea);
        }

        // failsafeExtent echoed as {x,y,z} read off the stored template.
        const TSharedPtr<FJsonObject>* ExtentObj = nullptr;
        if (TestTrue(TEXT("result echoes failsafeExtent object (explicit area)"),
                Capture.Result->TryGetObjectField(TEXT("failsafeExtent"), ExtentObj) && ExtentObj && ExtentObj->IsValid()))
        {
            TestEqual(TEXT("failsafeExtent.x"), (*ExtentObj)->GetNumberField(TEXT("x")), 250.0);
            TestEqual(TEXT("failsafeExtent.y"), (*ExtentObj)->GetNumberField(TEXT("y")), 250.0);
            TestEqual(TEXT("failsafeExtent.z"), (*ExtentObj)->GetNumberField(TEXT("z")), 250.0);
        }
    }

    // Case (b): NO areaClass arg — the component falls back to the UNavModifierComponent
    // constructor default (NavArea_Null). This is the load-bearing case: NavArea_Null equals
    // the CDO, so the sparse scs.get/asset.dump readback omits it and cannot confirm the set.
    // The echo must surface it, read off the template (the payload never carried this value).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BPPath);
        Payload->SetStringField(TEXT("componentName"), TEXT("NavModDefault"));
        // areaClass deliberately omitted.

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("create_nav_modifier_component handler found"),
                InvokeHandlerWithCapture(TEXT("navigation.create_nav_modifier_component"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("create_nav_modifier_component succeeded (default area)"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        if (!TestTrue(TEXT("result object present (default area)"), Capture.Result.IsValid())) return true;

        const FString ExpectedDefault = UNavArea_Null::StaticClass()->GetPathName();
        FString EchoedArea;
        if (TestTrue(TEXT("result echoes resolvedAreaClass (no-arg fallback)"),
                Capture.Result->TryGetStringField(TEXT("resolvedAreaClass"), EchoedArea)))
        {
            // The decisive assertion: the no-arg create resolves to NavArea_Null, the exact
            // equals-default value the sparse readback drops — the echo confirms it inline.
            TestEqual(TEXT("echoed resolvedAreaClass is the NavArea_Null constructor default"),
                EchoedArea, ExpectedDefault);
        }
    }

    return true;
}

// ============================================================================
// AIHandler.cpp — ai.add_mass_spawner (silent-success stub -> fail loud)
// ============================================================================
// Regression for B-add-mass-spawner-silent-noop: asserts the stub fails loud with
// NOT_IMPLEMENTED instead of the old phantom SendSuccess echo — see AIHandler.cpp
// ai.add_mass_spawner for the full rationale. A synthetic blueprintPath is safe because the
// handler rejects before loading any asset, so no fixture is required and the test never
// skips.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIAddMassSpawnerReturnsNotImplementedTest,
    "PinWright.ai.add_mass_spawner.ReturnsNotImplemented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIAddMassSpawnerReturnsNotImplementedTest::RunTest(const FString& Parameters)
{
    // The handler ignores the payload (it rejects with NOT_IMPLEMENTED before reading any
    // param), so a lone synthetic blueprintPath — never loaded — is enough.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/__PW_GatewayTests/Stub_DoesNotMatter"));

    // Delegates the honest-failure contract (handler found + bSuccess == false +
    // ErrorCode == "NOT_IMPLEMENTED") to the shared helper so it lives in one place; the old
    // stub returned bSuccess == true here.
    TestHandlerReturnsNotImplemented(*this, TEXT("ai.add_mass_spawner"), Payload);
    return true;
}

// ============================================================================
// AIHandler.cpp — ai.create_mass_entity_config / ai.configure_mass_entity
// ============================================================================
// The Mass handlers are reflection-only against the optional MassGameplay plugin
// (no MassSpawner linkage in the handler or in this test). This round-trip creates
// two config assets, parents one to the other, and verifies the reflected Parent
// write directly through FProperty — skipping gracefully when the plugin is
// disabled on the host.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIMassEntityConfigReflectionRoundTripTest,
    "PinWright.ai.configure_mass_entity.ReflectionRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIMassEntityConfigReflectionRoundTripTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BasePath = TEXT("/Game/__PW_GatewayTests/AIMass");
    const FString ParentName = FString::Printf(TEXT("MEC_Parent_%s"), *Suffix);
    const FString ChildName = FString::Printf(TEXT("MEC_Child_%s"), *Suffix);
    const FString ParentPath = BasePath / ParentName;
    const FString ChildPath = BasePath / ChildName;

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ChildPath);
        CleanupTestAsset(ParentPath);
    };

    // 1. Create the parent config. A PLUGIN_DISABLED rejection means the MassGameplay
    //    plugin is disabled on this host — skip gracefully rather than fail spuriously.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ParentName);
        Payload->SetStringField(TEXT("path"), BasePath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.create_mass_entity_config handler found"),
                InvokeHandlerWithCapture(TEXT("ai.create_mass_entity_config"), Payload, Capture))) return true;
        if (!Capture.bSuccess && Capture.ErrorCode == TEXT("PLUGIN_DISABLED"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
                TEXT("MassGameplay plugin disabled on this host; skipping Mass entity config reflection checks."));
            return true;
        }
        if (!TestTrue(TEXT("parent create_mass_entity_config succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_mass_entity_config error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // 2. Create the child config.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ChildName);
        Payload->SetStringField(TEXT("path"), BasePath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.create_mass_entity_config handler found (child)"),
                InvokeHandlerWithCapture(TEXT("ai.create_mass_entity_config"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("child create_mass_entity_config succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create_mass_entity_config error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // 3. Parent the child to the parent config and check the reported trait count.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("configPath"), ChildPath);
        Payload->SetStringField(TEXT("parentConfigPath"), ParentPath);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("ai.configure_mass_entity handler found"),
                InvokeHandlerWithCapture(TEXT("ai.configure_mass_entity"), Payload, Capture))) return true;
        if (!TestTrue(TEXT("configure_mass_entity succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("configure_mass_entity error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        double TraitCount = -1.0;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(TEXT("traitCount"), TraitCount);
        }
        TestEqual(TEXT("fresh config reports zero traits"), TraitCount, 0.0);
    }

    // 4. Verify the reflected Parent write landed, via FProperty (this test links no
    //    Mass headers either, so the readback goes through Config -> Parent reflection).
    {
        UObject* Child = LoadObject<UObject>(nullptr, *ChildPath);
        if (!TestNotNull(TEXT("child config asset loads"), Child)) return true;
        FStructProperty* ConfigProp = CastField<FStructProperty>(Child->GetClass()->FindPropertyByName(TEXT("Config")));
        if (!TestNotNull(TEXT("MassEntityConfigAsset.Config struct property resolves"), ConfigProp)) return true;
        void* ConfigPtr = ConfigProp->ContainerPtrToValuePtr<void>(Child);
        FObjectProperty* ParentProp = CastField<FObjectProperty>(ConfigProp->Struct->FindPropertyByName(TEXT("Parent")));
        if (!TestNotNull(TEXT("FMassEntityConfig.Parent object property resolves"), ParentProp)) return true;
        UObject* ParentValue = ParentProp->GetObjectPropertyValue(ParentProp->ContainerPtrToValuePtr<void>(ConfigPtr));
        if (!TestNotNull(TEXT("Parent was assigned by configure_mass_entity"), ParentValue)) return true;
        TestEqual(TEXT("Parent points at the created parent config"), ParentValue->GetName(), ParentName);
    }

    return true;
}
