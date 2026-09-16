// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for animation.authoring setters that used to write an early field before
// validating a later field. A refusal must preserve the complete authored node state.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "Animation/AnimBlueprint.h"
#include "Curves/CurveFloat.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tests/Assets/TestAGIRFixtures.h"
#include "Tests/Gameplay/TestAnimationFixtures.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if __has_include("AnimGraphNode_StateMachine.h") && __has_include("AnimStateNode.h") && \
    __has_include("AnimStateTransitionNode.h") && __has_include("AnimationStateMachineGraph.h")
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimStateMachineTypes.h"
#include "AnimationStateMachineGraph.h"
#define MCP_TEST_HAS_ANIM_SETTER_STATE_GRAPH 1
#else
#define MCP_TEST_HAS_ANIM_SETTER_STATE_GRAPH 0
#endif

#if __has_include("AnimGraphNode_LayeredBoneBlend.h") && __has_include("AnimNodes/AnimNode_LayeredBoneBlend.h")
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "Animation/AnimData/BoneMaskFilter.h"
#define MCP_TEST_HAS_ANIM_SETTER_LAYERED_BLEND 1
#else
#define MCP_TEST_HAS_ANIM_SETTER_LAYERED_BLEND 0
#endif

namespace PinWrightAnimationSetterAtomicityTests
{
    static UAnimBlueprint* CreateFixtureAnimBlueprint(
        const TCHAR* Prefix,
        PinWrightAnimationTestFixtures::FRegisteredAnimationFixture& AnimationFixture,
        FString& OutPackagePath)
    {
        if (!AnimationFixture.Create(Prefix))
        {
            return nullptr;
        }

        OutPackagePath = FString::Printf(
            TEXT("/Game/PinWrightTests/ABP_%s_%s"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        return AGIRTestFixtures::CreateFreshAnimBlueprint(
            OutPackagePath, AnimationFixture.Skeleton);
    }

    static void AppendSnapshotProperty(
        FString& Snapshot,
        const FProperty* Property,
        const void* Container,
        const FString& PropertyPath,
        const UObject* Owner)
    {
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
        FString Exported;
        Property->ExportTextItem_Direct(
            Exported,
            ValuePtr,
            nullptr,
            const_cast<UObject*>(Owner),
            PPF_None);
        Snapshot += FString::Printf(TEXT("|%s=%s"), *PropertyPath, *Exported);

        const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
        if (!StructProperty)
        {
            return;
        }

        for (TFieldIterator<FProperty> NestedIt(StructProperty->Struct); NestedIt; ++NestedIt)
        {
            const FProperty* NestedProperty = *NestedIt;
            if (!NestedProperty->ShouldPort(PPF_None))
            {
                continue;
            }
            AppendSnapshotProperty(
                Snapshot,
                NestedProperty,
                ValuePtr,
                PropertyPath + TEXT(".") + NestedProperty->GetName(),
                Owner);
        }
    }

    // Export reflected properties in declaration order, with port-eligible struct members as
    // explicit dotted paths. This gives each test a stable byte string covering nested runtime state.
    static FString SnapshotObjectProperties(const UObject* Object)
    {
        if (!Object)
        {
            return TEXT("<null>");
        }

        FString Snapshot = Object->GetClass()->GetPathName();
        for (TFieldIterator<FProperty> PropertyIt(Object->GetClass()); PropertyIt; ++PropertyIt)
        {
            const FProperty* Property = *PropertyIt;
            AppendSnapshotProperty(Snapshot, Property, Object, Property->GetName(), Object);
        }
        return Snapshot;
    }

    static void AssertSnapshotContainsProperty(
        FAutomationTestBase& Test,
        const FString& Snapshot,
        const TCHAR* PropertyName,
        const TCHAR* CaseName)
    {
        Test.TestTrue(
            *FString::Printf(TEXT("%s snapshot includes %s"), CaseName, PropertyName),
            Snapshot.Contains(FString::Printf(TEXT("|%s="), PropertyName)));
    }

    static void AssertSnapshotHasNestedNodeKeys(
        FAutomationTestBase& Test,
        const FString& Snapshot,
        const TCHAR* CaseName)
    {
        Test.TestTrue(
            *FString::Printf(TEXT("%s snapshot key set is non-empty"), CaseName),
            Snapshot.Contains(TEXT("|")));
        Test.TestTrue(
            *FString::Printf(TEXT("%s snapshot contains nested Node prefix"), CaseName),
            Snapshot.Contains(TEXT("|Node.")));
    }

#if MCP_TEST_HAS_ANIM_SETTER_STATE_GRAPH
    static UAnimStateTransitionNode* CreateTransitionFixture(UAnimBlueprint* AnimBP)
    {
        UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
        if (!AnimGraph)
        {
            return nullptr;
        }

        UAnimGraphNode_StateMachine* StateMachine =
            AnimGraphConstructionUtils::CreateStateMachine(
                AnimBP, AnimGraph, FName(TEXT("AtomicSM")), FVector2D(100, 200));
        if (!StateMachine || !StateMachine->EditorStateMachineGraph)
        {
            return nullptr;
        }

        UAnimationStateMachineGraph* MachineGraph =
            Cast<UAnimationStateMachineGraph>(StateMachine->EditorStateMachineGraph);
        if (!MachineGraph)
        {
            return nullptr;
        }

        UAnimStateNode* Idle = AnimGraphConstructionUtils::CreateState(
            MachineGraph, FName(TEXT("Idle")), FVector2D(300, 200));
        UAnimStateNode* Run = AnimGraphConstructionUtils::CreateState(
            MachineGraph, FName(TEXT("Run")), FVector2D(500, 200));
        return AnimGraphConstructionUtils::CreateTransition(
            Idle, Run, FVector2D(400, 200));
    }
#endif

#if MCP_TEST_HAS_ANIM_SETTER_LAYERED_BLEND
    static UAnimGraphNode_LayeredBoneBlend* CreateLayeredBlendFixture(UAnimBlueprint* AnimBP)
    {
        UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
        if (!AnimGraph)
        {
            return nullptr;
        }

        UAnimGraphNode_Base* Created = AnimGraphConstructionUtils::CreateAnimNode(
            AnimGraph,
            UAnimGraphNode_LayeredBoneBlend::StaticClass(),
            FVector2D(100, 200));
        return Cast<UAnimGraphNode_LayeredBoneBlend>(Created);
    }

    static void SeedLayeredBlendState(UAnimGraphNode_LayeredBoneBlend* Node)
    {
        Node->Node.BlendMode = ELayeredBoneBlendMode::BranchFilter;
        Node->Node.BlendPoses.SetNum(2);
        Node->Node.BlendWeights.Reset();
        Node->Node.BlendWeights.Add(0.25f);
        Node->Node.BlendWeights.Add(0.75f);
        Node->Node.BlendMasks.Reset();
        Node->Node.LayerSetup.SetNum(2);

        FBranchFilter FirstFilter;
        FirstFilter.BoneName = FName(TEXT("root"));
        FirstFilter.BlendDepth = -1;
        Node->Node.LayerSetup[0].BranchFilters.Add(FirstFilter);

        FBranchFilter SecondFilter;
        SecondFilter.BoneName = FName(TEXT("root"));
        SecondFilter.BlendDepth = 2;
        Node->Node.LayerSetup[1].BranchFilters.Add(SecondFilter);

        Node->Node.LODThreshold = 3;
        Node->Node.bMeshSpaceRotationBlend = true;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        Node->Node.bRootSpaceRotationBlend = true;
#endif
        Node->Node.bMeshSpaceScaleBlend = true;
        Node->Node.CurveBlendOption = ECurveBlendOption::BlendByWeight;
        Node->Node.bBlendRootMotionBasedOnRootBone = false;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        Node->Node.bUpdateBasePoseFirst = true;
#endif
    }
#endif
}

// A valid logicType followed by an invalid blendMode used to mutate LogicType before returning
// INVALID_BLEND_MODE. A missing curve had the same defect after both logicType and blendMode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationTransitionSetterAtomicValidationTest,
    "PinWright.animation.authoring.SetTransitionSettingsAtomicValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimationTransitionSetterAtomicValidationTest::RunTest(const FString& /*Parameters*/)
{
#if MCP_TEST_HAS_ANIM_SETTER_STATE_GRAPH
    using namespace PinWrightAnimationSetterAtomicityTests;

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture AnimationFixture;
    FString BlueprintPackagePath;
    UAnimBlueprint* AnimBP = CreateFixtureAnimBlueprint(
        TEXT("SetTransitionAtomic"), AnimationFixture, BlueprintPackagePath);
    TestNotNull(TEXT("transition atomicity AnimBlueprint created"), AnimBP);
    if (!AnimBP)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(BlueprintPackagePath);
    };

    UAnimStateTransitionNode* Transition = CreateTransitionFixture(AnimBP);
    TestNotNull(TEXT("transition fixture created"), Transition);
    if (!Transition)
    {
        return false;
    }

    UCurveFloat* BaselineCurve = NewObject<UCurveFloat>(
        GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UCurveFloat::StaticClass(), TEXT("AtomicCurve")),
        RF_Transient);
    TestNotNull(TEXT("baseline curve created"), BaselineCurve);
    if (!BaselineCurve)
    {
        return false;
    }
    const TStrongObjectPtr<UCurveFloat> BaselineCurveGuard(BaselineCurve);

    Transition->LogicType = TEnumAsByte<ETransitionLogicType::Type>(
        ETransitionLogicType::TLT_StandardBlend);
    Transition->BlendMode = EAlphaBlendOption::Linear;
    Transition->CustomBlendCurve = BaselineCurve;
    Transition->PriorityOrder = 7;
    Transition->CrossfadeDuration = 0.75f;
    Transition->Bidirectional = true;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    Transition->bDisabled = false;
#endif

    {
        const FString Before = SnapshotObjectProperties(Transition);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AnimBP->GetPathName());
        Payload->SetStringField(TEXT("stateMachineName"), TEXT("AtomicSM"));
        Payload->SetStringField(TEXT("fromState"), TEXT("Idle"));
        Payload->SetStringField(TEXT("toState"), TEXT("Run"));
        Payload->SetStringField(TEXT("logicType"), TEXT("Custom"));
        Payload->SetStringField(TEXT("blendMode"), TEXT("NotARealBlendMode"));
        Payload->SetBoolField(TEXT("disabled"), true);

        FTestResponseCapture Capture;
        TestTrue(TEXT("transition invalid-blend handler found"),
            InvokeHandlerWithCapture(
                TEXT("animation.authoring.set_transition_settings"), Payload, Capture));
        TestFalse(TEXT("invalid blendMode is rejected"), Capture.bSuccess);
        TestEqual(TEXT("invalid blendMode error code"),
            Capture.ErrorCode, FString(TEXT("INVALID_BLEND_MODE")));
        AssertSnapshotContainsProperty(*this, Before, TEXT("LogicType"), TEXT("invalid blendMode transition"));
        AssertSnapshotContainsProperty(*this, Before, TEXT("BlendMode"), TEXT("invalid blendMode transition"));
        TestEqual(TEXT("invalid blendMode leaves transition byte-identical"),
            SnapshotObjectProperties(Transition), Before);
    }

    {
        const FString Before = SnapshotObjectProperties(Transition);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AnimBP->GetPathName());
        Payload->SetStringField(TEXT("stateMachineName"), TEXT("AtomicSM"));
        Payload->SetStringField(TEXT("fromState"), TEXT("Idle"));
        Payload->SetStringField(TEXT("toState"), TEXT("Run"));
        Payload->SetStringField(TEXT("logicType"), TEXT("Inertialization"));
        Payload->SetStringField(TEXT("blendMode"), TEXT("Cubic"));
        Payload->SetStringField(
            TEXT("blendCurvePath"),
            TEXT("/Game/PinWrightTests/MissingAtomicCurve.MissingAtomicCurve"));
        Payload->SetBoolField(TEXT("disabled"), true);

        FTestResponseCapture Capture;
        TestTrue(TEXT("transition missing-curve handler found"),
            InvokeHandlerWithCapture(
                TEXT("animation.authoring.set_transition_settings"), Payload, Capture));
        TestFalse(TEXT("missing blend curve is rejected"), Capture.bSuccess);
        TestEqual(TEXT("missing blend curve error code"),
            Capture.ErrorCode, FString(TEXT("BLEND_CURVE_NOT_FOUND")));
        AssertSnapshotContainsProperty(*this, Before, TEXT("LogicType"), TEXT("missing blend curve transition"));
        AssertSnapshotContainsProperty(*this, Before, TEXT("BlendMode"), TEXT("missing blend curve transition"));
        TestEqual(TEXT("missing blend curve leaves transition byte-identical"),
            SnapshotObjectProperties(Transition), Before);
    }

    return true;
#else
    TestTrue(TEXT("transition setter handler registered"),
        IsHandlerRegistered(TEXT("animation.authoring.set_transition_settings")));
    return true;
#endif
}

// A missing blend-mask asset and malformed layer JSON used to leave a changed BlendMode behind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationLayeredBlendSetterAtomicValidationTest,
    "PinWright.animation.authoring.SetLayeredBlendLayersAtomicValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimationLayeredBlendSetterAtomicValidationTest::RunTest(const FString& /*Parameters*/)
{
#if MCP_TEST_HAS_ANIM_SETTER_STATE_GRAPH && MCP_TEST_HAS_ANIM_SETTER_LAYERED_BLEND
    using namespace PinWrightAnimationSetterAtomicityTests;

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture AnimationFixture;
    FString BlueprintPackagePath;
    UAnimBlueprint* AnimBP = CreateFixtureAnimBlueprint(
        TEXT("SetLayeredBlendAtomic"), AnimationFixture, BlueprintPackagePath);
    TestNotNull(TEXT("layered blend atomicity AnimBlueprint created"), AnimBP);
    if (!AnimBP)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(BlueprintPackagePath);
    };

    UAnimGraphNode_LayeredBoneBlend* Node = CreateLayeredBlendFixture(AnimBP);
    TestNotNull(TEXT("layered blend fixture created"), Node);
    if (!Node)
    {
        return false;
    }
    SeedLayeredBlendState(Node);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    const FString NodeName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

    {
        const FString Before = SnapshotObjectProperties(Node);
        TSharedPtr<FJsonObject> Layer = MakeShared<FJsonObject>();
        Layer->SetArrayField(TEXT("branchFilters"), TArray<TSharedPtr<FJsonValue>>());
        TArray<TSharedPtr<FJsonValue>> Layers;
        Layers.Add(MakeShared<FJsonValueObject>(Layer));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AnimBP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), TEXT("AnimGraph"));
        Payload->SetStringField(TEXT("nodeName"), NodeName);
        Payload->SetArrayField(TEXT("layers"), Layers);
        Payload->SetStringField(TEXT("blendMode"), TEXT("BlendMask"));
        Payload->SetStringField(
            TEXT("blendMaskPath"),
            TEXT("/Game/PinWrightTests/MissingAtomicBlendProfile.MissingAtomicBlendProfile"));
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("layered missing-mask handler found"),
            InvokeHandlerWithCapture(
                TEXT("animation.authoring.set_layered_blend_layers"), Payload, Capture));
        TestFalse(TEXT("missing blend mask is rejected"), Capture.bSuccess);
        TestEqual(TEXT("missing blend mask error code"),
            Capture.ErrorCode, FString(TEXT("BONE_MASK_NOT_FOUND")));
        AssertSnapshotHasNestedNodeKeys(*this, Before, TEXT("missing blend mask layered"));
        AssertSnapshotContainsProperty(*this, Before, TEXT("Node.BlendMode"), TEXT("missing blend mask layered"));
        AssertSnapshotContainsProperty(*this, Before, TEXT("Node"), TEXT("missing blend mask layered"));
        TestEqual(TEXT("missing blend mask leaves node byte-identical"),
            SnapshotObjectProperties(Node), Before);
    }

    {
        const FString Before = SnapshotObjectProperties(Node);
        TArray<TSharedPtr<FJsonValue>> Layers;
        Layers.Add(MakeShared<FJsonValueString>(TEXT("not-a-layer-object")));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AnimBP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), TEXT("AnimGraph"));
        Payload->SetStringField(TEXT("nodeName"), NodeName);
        Payload->SetArrayField(TEXT("layers"), Layers);
        Payload->SetStringField(TEXT("blendMode"), TEXT("BlendMask"));
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("layered invalid-layers handler found"),
            InvokeHandlerWithCapture(
                TEXT("animation.authoring.set_layered_blend_layers"), Payload, Capture));
        TestFalse(TEXT("malformed layers are rejected"), Capture.bSuccess);
        TestEqual(TEXT("malformed layers error code"),
            Capture.ErrorCode, FString(TEXT("INVALID_LAYERS")));
        AssertSnapshotHasNestedNodeKeys(*this, Before, TEXT("malformed layers layered"));
        AssertSnapshotContainsProperty(*this, Before, TEXT("Node.BlendMode"), TEXT("malformed layers layered"));
        AssertSnapshotContainsProperty(*this, Before, TEXT("Node"), TEXT("malformed layers layered"));
        TestEqual(TEXT("malformed layers leave node byte-identical"),
            SnapshotObjectProperties(Node), Before);
    }

    {
        const FString Before = SnapshotObjectProperties(Node);
        TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
        Filter->SetStringField(TEXT("boneName"), TEXT("root"));
        Filter->SetNumberField(TEXT("blendDepth"), 1);
        TArray<TSharedPtr<FJsonValue>> Filters;
        Filters.Add(MakeShared<FJsonValueObject>(Filter));

        TSharedPtr<FJsonObject> Layer = MakeShared<FJsonObject>();
        Layer->SetArrayField(TEXT("branchFilters"), Filters);
        TArray<TSharedPtr<FJsonValue>> Layers;
        Layers.Add(MakeShared<FJsonValueObject>(Layer));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AnimBP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), TEXT("AnimGraph"));
        Payload->SetStringField(TEXT("nodeName"), NodeName);
        Payload->SetArrayField(TEXT("layers"), Layers);
        Payload->SetStringField(TEXT("blendMode"), TEXT("BranchFilter"));
        Payload->SetBoolField(TEXT("meshSpaceRotationBlend"), false);
        Payload->SetBoolField(TEXT("meshSpaceScaleBlend"), false);
        Payload->SetStringField(TEXT("curveBlendOption"), TEXT("NotARealCurveBlendOption"));
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("layered invalid-curve-option handler found"),
            InvokeHandlerWithCapture(
                TEXT("animation.authoring.set_layered_blend_layers"), Payload, Capture));
        TestFalse(TEXT("invalid curveBlendOption is rejected"), Capture.bSuccess);
        TestEqual(TEXT("invalid curveBlendOption error code"),
            Capture.ErrorCode, FString(TEXT("PROPERTY_SET_FAILED")));
        TestTrue(TEXT("invalid curveBlendOption identifies the reflected field"),
            Capture.Message.Contains(TEXT("curveBlendOption")));
        AssertSnapshotHasNestedNodeKeys(*this, Before, TEXT("invalid curveBlendOption layered"));
        AssertSnapshotContainsProperty(*this, Before, TEXT("Node.BlendMode"), TEXT("invalid curveBlendOption layered"));
        AssertSnapshotContainsProperty(*this, Before, TEXT("Node"), TEXT("invalid curveBlendOption layered"));
        TestEqual(TEXT("invalid curveBlendOption leaves node byte-identical"),
            SnapshotObjectProperties(Node), Before);
    }

    return true;
#else
    TestTrue(TEXT("layered setter handler registered"),
        IsHandlerRegistered(TEXT("animation.authoring.set_layered_blend_layers")));
    return true;
#endif
}
