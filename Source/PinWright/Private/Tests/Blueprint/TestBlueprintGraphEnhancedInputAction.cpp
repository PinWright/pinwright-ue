// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/EnhancedInputTestUtils.h"

#include "Compiler/CodeNodeEmitter.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/NoExportTypes.h"
#include "UObject/Package.h"

namespace
{
    static FString MakeEnhancedInputTestPath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    static UBlueprint* CreateActorBlueprint(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        return Package
            ? FKismetEditorUtilities::CreateBlueprint(
                AActor::StaticClass(), Package,
                FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
                BPTYPE_Normal, UBlueprint::StaticClass(),
                UBlueprintGeneratedClass::StaticClass())
            : nullptr;
    }

}

using namespace EnhancedInputTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphCreateEnhancedInputActionTest,
    "PinWright.blueprint.graph.create_node.EnhancedInputActionBoundBeforePins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphCreateEnhancedInputActionTest::RunTest(const FString& Parameters)
{
    const FString BlueprintPath = MakeEnhancedInputTestPath(TEXT("BP_CreateEnhancedInput"));
    const FString ActionPath = MakeEnhancedInputTestPath(TEXT("IA_CreateEnhancedInput"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(BlueprintPath);
        CleanupTestAsset(ActionPath);
    };

    UBlueprint* Blueprint = CreateActorBlueprint(BlueprintPath);
    UInputAction* Action = CreateAxis2DInputAction(ActionPath);
    if (!TestNotNull(TEXT("Blueprint fixture created"), Blueprint)
        || !TestNotNull(TEXT("Axis2D UInputAction fixture created"), Action))
    {
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Payload->SetStringField(TEXT("nodeType"), TEXT("K2Node_EnhancedInputAction"));
    Payload->SetStringField(TEXT("inputAction"), Action->GetPathName());
    Payload->SetNumberField(TEXT("x"), 125.0);
    Payload->SetNumberField(TEXT("y"), 250.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("create_node handler is registered"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, Capture));
    TestTrue(TEXT("Bound Enhanced Input node creation succeeds"), Capture.bSuccess);

    FString NodeId;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("create_node returns nodeId"),
            Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
    }
    else
    {
        AddError(TEXT("create_node did not return a result object"));
    }

    UEdGraphNode* CreatedNode = BlueprintGraphHelpers::FindNodeByIdOrName(EventGraph, NodeId);
    TestAxis2DNodeShape(*this, CreatedNode, Action);

    const int32 NodeCountBeforeWrongClass = EventGraph->Nodes.Num();
    Blueprint->GetOutermost()->SetDirtyFlag(false);
    TSharedPtr<FJsonObject> WrongClassPayload = MakeShared<FJsonObject>();
    WrongClassPayload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    WrongClassPayload->SetStringField(TEXT("nodeType"), TEXT("K2Node_EnhancedInputAction"));
    WrongClassPayload->SetStringField(TEXT("inputAction"), Blueprint->GetPathName());
    WrongClassPayload->SetNumberField(TEXT("x"), 500.0);
    WrongClassPayload->SetNumberField(TEXT("y"), 250.0);

    FTestResponseCapture WrongClassCapture;
    TestTrue(TEXT("create_node handler remains registered for wrong-class case"),
        InvokeHandlerWithCapture(
            TEXT("blueprint.graph.create_node"), WrongClassPayload, WrongClassCapture));
    TestFalse(TEXT("Wrong-class inputAction is rejected"), WrongClassCapture.bSuccess);
    TestEqual(TEXT("Wrong-class inputAction reports INVALID_ARGUMENT"),
        WrongClassCapture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestEqual(TEXT("Wrong-class rejection leaves node count unchanged"),
        EventGraph->Nodes.Num(), NodeCountBeforeWrongClass);
    TestFalse(TEXT("Wrong-class rejection leaves Blueprint package clean"),
        Blueprint->GetOutermost()->IsDirty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphReconstructEnhancedInputActionTest,
    "PinWright.blueprint.graph.reconstruct_node.EnhancedInputActionRefreshesPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphReconstructEnhancedInputActionTest::RunTest(const FString& Parameters)
{
    const FString BlueprintPath = MakeEnhancedInputTestPath(TEXT("BP_ReconstructEnhancedInput"));
    const FString ActionPath = MakeEnhancedInputTestPath(TEXT("IA_ReconstructEnhancedInput"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(BlueprintPath);
        CleanupTestAsset(ActionPath);
    };

    UBlueprint* Blueprint = CreateActorBlueprint(BlueprintPath);
    UInputAction* Action = CreateAxis2DInputAction(ActionPath);
    if (!TestNotNull(TEXT("Blueprint fixture created"), Blueprint)
        || !TestNotNull(TEXT("Axis2D UInputAction fixture created"), Action))
    {
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
    UClass* NodeClass = FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass();
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph)
        || !TestNotNull(TEXT("Dynamic Enhanced Input node class resolves"), NodeClass))
    {
        return true;
    }

    FGraphNodeCreator<UK2Node> NodeCreator(*EventGraph);
    UK2Node* InputNode = NodeCreator.CreateNode(false, NodeClass);
    NodeCreator.Finalize();
    if (!TestNotNull(TEXT("Unbound Enhanced Input node created"), InputNode))
    {
        return true;
    }

    UEdGraphPin* UnboundValue = InputNode->FindPin(TEXT("ActionValue"), EGPD_Output);
    TestNotNull(TEXT("Unbound ActionValue output exists"), UnboundValue);
    if (UnboundValue)
    {
        TestEqual(TEXT("Unbound ActionValue starts as bool"),
            UnboundValue->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    }
    TestNull(TEXT("Unbound node has no InputAction output"),
        InputNode->FindPin(TEXT("InputAction"), EGPD_Output));

    TestTrue(TEXT("Production reflected property path accepts Axis2D fixture"),
        FCodeNodeEmitter::SetEnhancedInputAction(InputNode, Action));
    const FGuid OriginalGuid = InputNode->NodeGuid;
    Blueprint->GetOutermost()->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Payload->SetStringField(TEXT("nodeId"), OriginalGuid.ToString());

    FTestResponseCapture Capture;
    TestTrue(TEXT("reconstruct_node handler is registered"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.reconstruct_node"), Payload, Capture));
    TestTrue(TEXT("Enhanced Input node reconstruction succeeds"), Capture.bSuccess);
    TestEqual(TEXT("Reconstruction preserves node GUID"), InputNode->NodeGuid, OriginalGuid);
    TestTrue(TEXT("Reconstruction marks Blueprint package dirty"),
        Blueprint->GetOutermost()->IsDirty());
    if (Capture.Result.IsValid())
    {
        bool bGuidPreserved = false;
        TestTrue(TEXT("Response includes guidPreserved"),
            Capture.Result->TryGetBoolField(TEXT("guidPreserved"), bGuidPreserved));
        TestTrue(TEXT("Response confirms GUID preservation"), bGuidPreserved);
    }
    else
    {
        AddError(TEXT("reconstruct_node did not return a result object"));
    }
    TestAxis2DNodeShape(*this, InputNode, Action);

    FGraphNodeCreator<UEdGraphNode_Comment> CommentCreator(*EventGraph);
    UEdGraphNode_Comment* CommentNode = CommentCreator.CreateNode(false);
    CommentCreator.Finalize();
    CommentNode->NodeComment = TEXT("must remain unchanged");
    const FGuid CommentGuid = CommentNode->NodeGuid;
    const int32 CommentPinCount = CommentNode->Pins.Num();
    Blueprint->GetOutermost()->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> UnsupportedPayload = MakeShared<FJsonObject>();
    UnsupportedPayload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    UnsupportedPayload->SetStringField(TEXT("nodeId"), CommentGuid.ToString());

    FTestResponseCapture UnsupportedCapture;
    TestTrue(TEXT("reconstruct_node handler remains registered for non-K2 case"),
        InvokeHandlerWithCapture(
            TEXT("blueprint.graph.reconstruct_node"), UnsupportedPayload, UnsupportedCapture));
    TestFalse(TEXT("Non-K2 node reconstruction is rejected"), UnsupportedCapture.bSuccess);
    TestEqual(TEXT("Non-K2 rejection reports INVALID_NODE_TYPE"),
        UnsupportedCapture.ErrorCode, FString(TEXT("INVALID_NODE_TYPE")));
    TestEqual(TEXT("Non-K2 rejection preserves GUID"), CommentNode->NodeGuid, CommentGuid);
    TestEqual(TEXT("Non-K2 rejection preserves pin count"),
        CommentNode->Pins.Num(), CommentPinCount);
    TestEqual(TEXT("Non-K2 rejection preserves node data"),
        CommentNode->NodeComment, FString(TEXT("must remain unchanged")));
    TestFalse(TEXT("Non-K2 rejection does not dirty the Blueprint package"),
        Blueprint->GetOutermost()->IsDirty());

    return true;
}
