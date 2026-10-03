// Copyright (c) 2026 Alexander Penkin. MIT License.

// Closed-input regressions for animation creators/authoring verbs:
//   * B-animation-enum-tokens-fall-through-to-default: an unknown assetType / blendType used to
//     fall through to the default branch and report success naming a DIFFERENT created type.
//   * E-add-two-bone-ik-no-effector-location: add_two_bone_ik now authors effectorLocation /
//     jointTargetLocation and may leave its targets empty; malformed locations are refused.
// Every refusal case also asserts nothing was created, so a refusal that still mutated fails.

#include "Misc/AutomationTest.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Tests/Assets/TestAGIRFixtures.h"
#include "Tests/Gameplay/TestAnimationFixtures.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

#if __has_include("AnimGraphNode_TwoWayBlend.h") && __has_include("AnimGraphNode_LayeredBoneBlend.h")
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_TwoWayBlend.h"
#define MCP_TEST_HAS_CLOSED_TOKEN_BLEND_NODES 1
#else
#define MCP_TEST_HAS_CLOSED_TOKEN_BLEND_NODES 0
#endif

#if __has_include("AnimGraphNode_TwoBoneIK.h") && __has_include("AnimGraphNode_ModifyBone.h")
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_TwoBoneIK.h"
#define MCP_TEST_HAS_CLOSED_TOKEN_TWO_BONE_IK 1
#else
#define MCP_TEST_HAS_CLOSED_TOKEN_TWO_BONE_IK 0
#endif

namespace AnimationClosedTokenInputsTests
{
    static UAnimBlueprint* CreateAnimBlueprintFixture(
        const TCHAR* Prefix,
        PinWrightAnimationTestFixtures::FRegisteredAnimationFixture& AnimationFixture,
        FString& OutPackagePath)
    {
        if (!AnimationFixture.Create(Prefix, /*bAddFootBone=*/true))
        {
            return nullptr;
        }
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTests/ABP_%s_%s"),
            Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        return AGIRTestFixtures::CreateFreshAnimBlueprint(OutPackagePath, AnimationFixture.Skeleton);
    }

    template <typename NodeType>
    static int32 CountAnimGraphNodes(UAnimBlueprint* AnimBP, NodeType** OutLast = nullptr)
    {
        int32 Count = 0;
        if (UEdGraph* Graph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP))
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (NodeType* Typed = Cast<NodeType>(Node))
                {
                    ++Count;
                    if (OutLast)
                    {
                        *OutLast = Typed;
                    }
                }
            }
        }
        return Count;
    }

    // The value the anim compiler pushes into the compiled node for an unlinked exposed pin.
    static FString PinDefault(UEdGraphNode* Node, const TCHAR* PinName)
    {
        UEdGraphPin* Pin = Node ? Node->FindPin(FName(PinName), EGPD_Input) : nullptr;
        return Pin ? Pin->GetDefaultAsString() : FString(TEXT("<no pin>"));
    }

    static void TestPinVector(FAutomationTestBase& Test, UEdGraphNode* Node, const TCHAR* PinName, const FVector& Expected)
    {
        const FString Raw = PinDefault(Node, PinName);
        FVector Parsed = FVector::ZeroVector;
        Test.TestTrue(FString::Printf(TEXT("%s pin default '%s' parses as a vector"), PinName, *Raw),
            FDefaultValueHelper::ParseVector(Raw, Parsed));
        Test.TestTrue(FString::Printf(TEXT("%s pin default '%s' equals %s"), PinName, *Raw, *Expected.ToString()),
            Parsed.Equals(Expected, 1e-4));
    }
}

// ----------------------------------------------------------------------------
// Counterfactual: restore the open `else` sequence branch and assetType='montages' creates an
// AnimSequence with success, failing both the code assertion and the nothing-created assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCreateAssetTypeClosedSetTest,
    "PinWright.animation.CreateAnimationAssetAssetTypeClosedSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimationCreateAssetTypeClosedSetTest::RunTest(const FString& /*Parameters*/)
{
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture AnimationFixture;
    if (!TestTrue(TEXT("skeleton fixture created"), AnimationFixture.Create(TEXT("AssetTypeClosed"))))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture skeleton"), AnimationFixture.Skeleton))
    {
        return false;
    }
    const FString SkeletonPath = AnimationFixture.Skeleton->GetPathName();
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const TCHAR* Refused[] = { TEXT("montages"), TEXT("Sequences"), TEXT("blendspace") };
    for (const TCHAR* Token : Refused)
    {
        const FString Name = FString::Printf(TEXT("AS_AssetTypeRefused_%s_%s"), Token, *Guid);
        ON_SCOPE_EXIT { CleanupTestAsset(FString::Printf(TEXT("/Game/PinWrightTests/%s"), *Name)); };

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Name);
        Payload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        Payload->SetStringField(TEXT("savePath"), TEXT("/Game/PinWrightTests"));
        Payload->SetStringField(TEXT("assetType"), Token);

        FTestResponseCapture Capture;
        TestTrue(TEXT("create_animation_asset registered"),
            InvokeHandlerWithCapture(TEXT("animation.create_animation_asset"), Payload, Capture));
        TestFalse(FString::Printf(TEXT("assetType '%s' is refused"), Token), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("assetType '%s' error code"), Token),
            Capture.ErrorCode, FString(TEXT("INVALID_ASSET_TYPE")));
        TestNull(FString::Printf(TEXT("assetType '%s' created no package"), Token),
            FindPackage(nullptr, *FString::Printf(TEXT("/Game/PinWrightTests/%s"), *Name)));
    }

    // Accepted spelling is case-insensitive and still creates the requested type.
    const FString MontageName = FString::Printf(TEXT("AM_AssetTypeAccepted_%s"), *Guid);
    const FString MontagePackage = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *MontageName);
    ON_SCOPE_EXIT { CleanupTestAsset(MontagePackage); };
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), MontageName);
    Payload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    Payload->SetStringField(TEXT("savePath"), TEXT("/Game/PinWrightTests"));
    Payload->SetStringField(TEXT("assetType"), TEXT("MONTAGE"));
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("animation.create_animation_asset"), Payload, Capture);
    TestTrue(FString::Printf(TEXT("assetType 'MONTAGE' succeeds (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestNotNull(TEXT("assetType 'MONTAGE' created an AnimMontage"),
        FindObject<UAnimMontage>(nullptr, *FString::Printf(TEXT("%s.%s"), *MontagePackage, *MontageName)));
    return true;
}

// ----------------------------------------------------------------------------
// Counterfactual: restore the "Default fallback to TwoWayBlend" branch and blendType='Layered'
// adds a TwoWayBlend with success, failing the code and the unchanged-node-count assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationAddBlendNodeBlendTypeClosedSetTest,
    "PinWright.animation.authoring.AddBlendNodeBlendTypeClosedSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimationAddBlendNodeBlendTypeClosedSetTest::RunTest(const FString& /*Parameters*/)
{
#if MCP_TEST_HAS_CLOSED_TOKEN_BLEND_NODES
    using namespace AnimationClosedTokenInputsTests;

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture AnimationFixture;
    FString BlueprintPath;
    UAnimBlueprint* AnimBP = CreateAnimBlueprintFixture(TEXT("BlendTypeClosed"), AnimationFixture, BlueprintPath);
    if (!TestNotNull(TEXT("blend-type AnimBlueprint fixture created"), AnimBP))
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(BlueprintPath); };

    auto Invoke = [&](const TCHAR* BlendType, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
        if (BlendType)
        {
            Payload->SetStringField(TEXT("blendType"), BlendType);
        }
        Payload->SetNumberField(TEXT("x"), 100);
        Payload->SetNumberField(TEXT("y"), 50);
        Payload->SetBoolField(TEXT("save"), false);
        InvokeHandlerWithCapture(TEXT("animation.authoring.add_blend_node"), Payload, Capture);
    };

    const int32 TwoWayBefore = CountAnimGraphNodes<UAnimGraphNode_TwoWayBlend>(AnimBP);
    const int32 LayeredBefore = CountAnimGraphNodes<UAnimGraphNode_LayeredBoneBlend>(AnimBP);
    const TCHAR* Refused[] = { TEXT("Layered"), TEXT("TwoWay"), TEXT("BlendList") };
    for (const TCHAR* Token : Refused)
    {
        FTestResponseCapture Capture;
        Invoke(Token, Capture);
        TestFalse(FString::Printf(TEXT("blendType '%s' is refused"), Token), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("blendType '%s' error code"), Token),
            Capture.ErrorCode, FString(TEXT("UNKNOWN_NODE_TYPE")));
    }
    TestEqual(TEXT("refusals added no TwoWayBlend"),
        CountAnimGraphNodes<UAnimGraphNode_TwoWayBlend>(AnimBP), TwoWayBefore);
    TestEqual(TEXT("refusals added no LayeredBoneBlend"),
        CountAnimGraphNodes<UAnimGraphNode_LayeredBoneBlend>(AnimBP), LayeredBefore);

    // Explicit alias (case-insensitive) and omission still work.
    {
        FTestResponseCapture Capture;
        Invoke(TEXT("layeredblend"), Capture);
        TestTrue(FString::Printf(TEXT("blendType 'layeredblend' succeeds (errorCode='%s')"), *Capture.ErrorCode),
            Capture.bSuccess);
        TestEqual(TEXT("blendType 'layeredblend' added one LayeredBoneBlend"),
            CountAnimGraphNodes<UAnimGraphNode_LayeredBoneBlend>(AnimBP), LayeredBefore + 1);
    }
    {
        FTestResponseCapture Capture;
        Invoke(nullptr, Capture);
        TestTrue(FString::Printf(TEXT("omitted blendType succeeds (errorCode='%s')"), *Capture.ErrorCode),
            Capture.bSuccess);
        TestEqual(TEXT("omitted blendType added one TwoWayBlend"),
            CountAnimGraphNodes<UAnimGraphNode_TwoWayBlend>(AnimBP), TwoWayBefore + 1);
    }
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-unsupported"),
        TEXT("AnimGraph blend node headers unavailable in this build."));
#endif
    return true;
}

// ----------------------------------------------------------------------------
// Routed through the real dispatcher so the declared nested {x,y,z} schema is enforced too.
// Counterfactual: drop the effectorLocation/jointTargetLocation params and the success case
// fails with UNKNOWN_PARAMS; make the target bones required again and it fails with
// MISSING_PARAMETERS; drop the strict axis read and the {x,y} / string-z cases succeed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationAddTwoBoneIkLocationsTest,
    "PinWright.animation.authoring.AddTwoBoneIkLocationsAndEmptyTargets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimationAddTwoBoneIkLocationsTest::RunTest(const FString& /*Parameters*/)
{
#if MCP_TEST_HAS_CLOSED_TOKEN_TWO_BONE_IK
    using namespace AnimationClosedTokenInputsTests;

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture AnimationFixture;
    FString BlueprintPath;
    UAnimBlueprint* AnimBP = CreateAnimBlueprintFixture(TEXT("TwoBoneIkLoc"), AnimationFixture, BlueprintPath);
    if (!TestNotNull(TEXT("two-bone-ik AnimBlueprint fixture created"), AnimBP))
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(BlueprintPath); };

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    auto MakeVec = [](double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    };
    auto MakePayload = [&]()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
        Payload->SetStringField(TEXT("ikBone"), TEXT("foot_l"));
        Payload->SetNumberField(TEXT("x"), 300);
        Payload->SetNumberField(TEXT("y"), 120);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    };
    auto Run = [&](const TCHAR* CaseName, const TSharedPtr<FJsonObject>& Payload,
                   bool& bOutSuccess, FString& OutCode, TSharedPtr<FJsonObject>& OutResult)
    {
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("animation.authoring.add_two_bone_ik"),
            CaseName, Payload, bOutSuccess, OutResult, OutCode);
    };

    struct FRefusal { const TCHAR* Name; TSharedPtr<FJsonObject> Payload; const TCHAR* Code; };
    TArray<FRefusal> Refusals;
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), 1.0);
        Loc->SetNumberField(TEXT("y"), 2.0);
        P->SetObjectField(TEXT("effectorLocation"), Loc);
        Refusals.Add({TEXT("effectorLocation missing z"), P, TEXT("INVALID_TRANSFORM_PAYLOAD")});
    }
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        TSharedPtr<FJsonObject> Loc = MakeVec(1.0, 2.0, 0.0);
        Loc->SetStringField(TEXT("z"), TEXT("high"));
        P->SetObjectField(TEXT("jointTargetLocation"), Loc);
        Refusals.Add({TEXT("jointTargetLocation string z"), P, TEXT("INVALID_TRANSFORM_PAYLOAD")});
    }
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        TSharedPtr<FJsonObject> Loc = MakeVec(1.0, 2.0, 3.0);
        Loc->SetNumberField(TEXT("w"), 4.0);
        P->SetObjectField(TEXT("effectorLocation"), Loc);
        Refusals.Add({TEXT("effectorLocation extra key"), P, TEXT("UNKNOWN_NESTED_PARAMS")});
    }
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        P->SetStringField(TEXT("effectorLocationSpace"), TEXT("BCS_BoneSpace"));
        Refusals.Add({TEXT("bone space without effectorBone"), P, TEXT("MISSING_PARAMETERS")});
    }
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        P->SetStringField(TEXT("effectorBone"), TEXT("root"));
        P->SetStringField(TEXT("jointTargetLocationSpace"), TEXT("BCS_ParentBoneSpace"));
        Refusals.Add({TEXT("parent-bone space without jointTargetBone"), P, TEXT("MISSING_PARAMETERS")});
    }

    const int32 NodesBefore = CountAnimGraphNodes<UAnimGraphNode_TwoBoneIK>(AnimBP);
    for (const FRefusal& Refusal : Refusals)
    {
        bool bSuccess = true;
        FString Code;
        TSharedPtr<FJsonObject> Result;
        Run(Refusal.Name, Refusal.Payload, bSuccess, Code, Result);
        TestFalse(FString::Printf(TEXT("%s is refused"), Refusal.Name), bSuccess);
        TestEqual(FString::Printf(TEXT("%s error code"), Refusal.Name), Code, FString(Refusal.Code));
    }
    TestEqual(TEXT("refusals created no TwoBoneIK node"),
        CountAnimGraphNodes<UAnimGraphNode_TwoBoneIK>(AnimBP), NodesBefore);

    // Pure component-space IK: no target bones, explicit locations.
    TSharedPtr<FJsonObject> Payload = MakePayload();
    Payload->SetStringField(TEXT("effectorLocationSpace"), TEXT("BCS_ComponentSpace"));
    Payload->SetObjectField(TEXT("effectorLocation"), MakeVec(20.55, 11.02, 8.48));
    Payload->SetObjectField(TEXT("jointTargetLocation"), MakeVec(10.0, 40.0, 50.0));
    Payload->SetNumberField(TEXT("alpha"), 0.75);
    bool bSuccess = false;
    FString Code;
    TSharedPtr<FJsonObject> Result;
    Run(TEXT("component-space locations"), Payload, bSuccess, Code, Result);
    TestTrue(FString::Printf(TEXT("component-space add_two_bone_ik succeeds (errorCode='%s')"), *Code), bSuccess);

    UAnimGraphNode_TwoBoneIK* Node = nullptr;
    TestEqual(TEXT("success created one TwoBoneIK node"),
        CountAnimGraphNodes<UAnimGraphNode_TwoBoneIK>(AnimBP, &Node), NodesBefore + 1);
    if (!Node)
    {
        return false;
    }
    TestEqual(TEXT("EffectorLocation authored"), Node->Node.EffectorLocation, FVector(20.55, 11.02, 8.48));
    TestEqual(TEXT("JointTargetLocation authored"), Node->Node.JointTargetLocation, FVector(10.0, 40.0, 50.0));
    // The pins, not the struct, decide what compiles: a struct-only write leaves them at the
    // autogenerated (0,0,0) / 1.0 and the compiled node gets those.
    TestPinVector(*this, Node, TEXT("EffectorLocation"), FVector(20.55, 11.02, 8.48));
    TestPinVector(*this, Node, TEXT("JointTargetLocation"), FVector(10.0, 40.0, 50.0));
    TestTrue(FString::Printf(TEXT("Alpha pin default '%s' is 0.75"), *PinDefault(Node, TEXT("Alpha"))),
        FMath::IsNearlyEqual(FCString::Atof(*PinDefault(Node, TEXT("Alpha"))), 0.75f, 1e-4f));
    TestEqual(TEXT("EffectorTarget left empty"), Node->Node.EffectorTarget.BoneReference.BoneName, FName(NAME_None));
    TestEqual(TEXT("JointTarget left empty"), Node->Node.JointTarget.BoneReference.BoneName, FName(NAME_None));
    const TSharedPtr<FJsonObject>* EchoedLocation = nullptr;
    if (TestTrue(TEXT("response echoes effectorLocation"),
            Result.IsValid() && Result->TryGetObjectField(TEXT("effectorLocation"), EchoedLocation)))
    {
        TestEqual(TEXT("echoed effectorLocation.z"), (*EchoedLocation)->GetNumberField(TEXT("z")), 8.48);
    }
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-unsupported"),
        TEXT("AnimGraphNode_TwoBoneIK header unavailable in this build."));
#endif
    return true;
}

// ----------------------------------------------------------------------------
// Same pin-ownership defect on add_modify_bone: Translation/Rotation/Scale/Alpha are
// PinShownByDefault. Counterfactual: drop SyncSkeletalControlPinDefaultsFromStruct from
// add_modify_bone and the pins stay at their autogenerated defaults, failing every pin check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationAddModifyBonePinDefaultsTest,
    "PinWright.animation.authoring.AddModifyBoneWritesExposedPinDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimationAddModifyBonePinDefaultsTest::RunTest(const FString& /*Parameters*/)
{
#if MCP_TEST_HAS_CLOSED_TOKEN_TWO_BONE_IK
    using namespace AnimationClosedTokenInputsTests;

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture AnimationFixture;
    FString BlueprintPath;
    UAnimBlueprint* AnimBP = CreateAnimBlueprintFixture(TEXT("ModifyBonePins"), AnimationFixture, BlueprintPath);
    if (!TestNotNull(TEXT("modify-bone AnimBlueprint fixture created"), AnimBP))
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(BlueprintPath); };

    auto MakeObj = [](const TCHAR* A, double VA, const TCHAR* B, double VB, const TCHAR* C, double VC)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(A, VA);
        Obj->SetNumberField(B, VB);
        Obj->SetNumberField(C, VC);
        return Obj;
    };
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Payload->SetStringField(TEXT("boneName"), TEXT("foot_l"));
    Payload->SetObjectField(TEXT("translation"), MakeObj(TEXT("x"), 1.0, TEXT("y"), 2.0, TEXT("z"), 3.0));
    Payload->SetObjectField(TEXT("rotation"), MakeObj(TEXT("pitch"), 10.0, TEXT("yaw"), 20.0, TEXT("roll"), 30.0));
    Payload->SetObjectField(TEXT("scale"), MakeObj(TEXT("x"), 1.25, TEXT("y"), 1.5, TEXT("z"), 1.75));
    Payload->SetNumberField(TEXT("alpha"), 0.5);
    Payload->SetNumberField(TEXT("x"), 200);
    Payload->SetNumberField(TEXT("y"), 80);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("animation.authoring.add_modify_bone"), Payload, Capture);
    TestTrue(FString::Printf(TEXT("add_modify_bone succeeds (errorCode='%s')"), *Capture.ErrorCode), Capture.bSuccess);

    UAnimGraphNode_ModifyBone* Node = nullptr;
    CountAnimGraphNodes<UAnimGraphNode_ModifyBone>(AnimBP, &Node);
    if (!TestNotNull(TEXT("ModifyBone node created"), Node))
    {
        return false;
    }
    TestPinVector(*this, Node, TEXT("Translation"), FVector(1.0, 2.0, 3.0));
    TestPinVector(*this, Node, TEXT("Scale"), FVector(1.25, 1.5, 1.75));
    FRotator Rotation = FRotator::ZeroRotator;
    const FString RotationRaw = PinDefault(Node, TEXT("Rotation"));
    TestTrue(FString::Printf(TEXT("Rotation pin default '%s' is (10,20,30)"), *RotationRaw),
        FDefaultValueHelper::ParseRotator(RotationRaw, Rotation)
        && Rotation.Equals(FRotator(10.0, 20.0, 30.0), 1e-3));
    TestTrue(FString::Printf(TEXT("Alpha pin default '%s' is 0.5"), *PinDefault(Node, TEXT("Alpha"))),
        FMath::IsNearlyEqual(FCString::Atof(*PinDefault(Node, TEXT("Alpha"))), 0.5f, 1e-4f));
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-unsupported"),
        TEXT("AnimGraphNode_ModifyBone header unavailable in this build."));
#endif
    return true;
}
