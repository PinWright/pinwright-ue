// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "K2Node_CustomEvent.h"
#if defined(__has_include) && (__has_include("BlueprintGraph/K2Node_CreateDelegate.h") || __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h") || __has_include("K2Node_CreateDelegate.h"))
#if __has_include("BlueprintGraph/K2Node_CreateDelegate.h")
#include "BlueprintGraph/K2Node_CreateDelegate.h"
#elif __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h")
#include "BlueprintGraph/Classes/K2Node_CreateDelegate.h"
#else
#include "K2Node_CreateDelegate.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCascadeCreateDelegateCleanupTest,
    "PinWright.blueprint.remove_event.CascadeCreateDelegateCleanup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCascadeCreateDelegateCleanupTest::RunTest(const FString& Parameters)
{
    // On-disk path required: LoadBlueprintAsset (called by the handler) cannot see
    // transient-package assets. Follows the FBlueprintRemoveEventComponentNameFilterTest pattern.
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/CascadeCreateDelegate_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Locate the ubergraph (EventGraph)
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("EventGraph exists"), EventGraph))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Create a UK2Node_CustomEvent named "MyEvent"
    UK2Node_CustomEvent* CustomEventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
    CustomEventNode->CustomFunctionName = FName(TEXT("MyEvent"));
    CustomEventNode->CreateNewGuid();
    EventGraph->AddNode(CustomEventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    // Create a UK2Node_CreateDelegate whose SelectedFunctionName references the same event
    UK2Node_CreateDelegate* CreateDelegateNode = NewObject<UK2Node_CreateDelegate>(EventGraph);
    CreateDelegateNode->SelectedFunctionName = FName(TEXT("MyEvent"));
    CreateDelegateNode->CreateNewGuid();
    EventGraph->AddNode(CreateDelegateNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    // Capture the CreateDelegate node's GUID for later lookup
    const FGuid CreateDelegateGuid = CreateDelegateNode->NodeGuid;

    // Verify both nodes are present before invoking the handler
    if (!TestTrue(TEXT("CustomEvent present before removal"),
            EventGraph->Nodes.Contains(CustomEventNode)) ||
        !TestTrue(TEXT("CreateDelegate present before removal"),
            EventGraph->Nodes.Contains(CreateDelegateNode)))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Invoke blueprint.remove_event targeting "MyEvent"
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"),      AssetPath);
    Payload->SetStringField(TEXT("eventName"), TEXT("MyEvent"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.remove_event"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);

    // The CustomEvent node must be gone (existing behavior)
    TestTrue(TEXT("CustomEvent was removed"), !EventGraph->Nodes.Contains(CustomEventNode));

    // The CreateDelegate node must also be gone (new cascade behavior).
    // Verify by GUID lookup — the node pointer may be dangling after RemoveNode.
    UEdGraphNode* FoundByGuid = FBlueprintEditorUtils::GetNodeByGUID(BP, CreateDelegateGuid);
    TestNull(TEXT("CreateDelegate was cascade-removed (not found by GUID)"), FoundByGuid);

    CleanupTestAsset(AssetPath);
    return true;
}

#endif
