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
#if __has_include("K2Node_ComponentBoundEvent.h")
#include "K2Node_ComponentBoundEvent.h"
#define MCP_TEST_HAS_CBE 1
#else
#define MCP_TEST_HAS_CBE 0
#endif

#if MCP_TEST_HAS_CBE

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveEventComponentNameFilterTest,
    "PinWright.blueprint.remove_event.ComponentNameFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintRemoveEventComponentNameFilterTest::RunTest(const FString& Parameters)
{
    // On-disk path required: LoadBlueprintAsset (called by the handler) cannot see
    // transient-package assets. Follows the FBlueprintAddEventWithParamsTest pattern.
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/RemoveEventDedup_%s"),
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

    // Two ComponentBoundEvent nodes sharing DelegatePropertyName but bound to
    // different components — the disambiguation scenario the filter exists for.
    const FName SharedDelegate(TEXT("OnClicked"));

    UK2Node_ComponentBoundEvent* NodeA = NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
    NodeA->ComponentPropertyName = FName(TEXT("BT_A"));
    NodeA->DelegatePropertyName  = SharedDelegate;
    // DelegateOwnerClass must be non-null to avoid downstream null deref during remove.
    NodeA->DelegateOwnerClass    = AActor::StaticClass();
    NodeA->CreateNewGuid();
    EventGraph->AddNode(NodeA, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    // AllocateDefaultPins creates the OutputDelegate pin. Without it, the recompile
    // triggered by remove_event crashes in UK2Node_Event::UpdateDelegatePin via
    // FindPinChecked → null Pin → access violation in ResolveSimpleMemberReference.
    NodeA->AllocateDefaultPins();

    UK2Node_ComponentBoundEvent* NodeB = NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
    NodeB->ComponentPropertyName = FName(TEXT("BT_B"));
    NodeB->DelegatePropertyName  = SharedDelegate;
    NodeB->DelegateOwnerClass    = AActor::StaticClass();
    NodeB->CreateNewGuid();
    EventGraph->AddNode(NodeB, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    NodeB->AllocateDefaultPins();

    // Verify both nodes are in the graph before invoking the handler
    bool bNodeAPresent = EventGraph->Nodes.Contains(NodeA);
    bool bNodeBPresent = EventGraph->Nodes.Contains(NodeB);
    if (!TestTrue(TEXT("NodeA present before removal"), bNodeAPresent) ||
        !TestTrue(TEXT("NodeB present before removal"), bNodeBPresent))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"),          AssetPath);
    Payload->SetStringField(TEXT("eventName"),     SharedDelegate.ToString());
    Payload->SetStringField(TEXT("componentName"), TEXT("BT_A"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.remove_event"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);

    bool bNodeAGone = !EventGraph->Nodes.Contains(NodeA);
    bool bNodeBStillPresent = EventGraph->Nodes.Contains(NodeB);

    TestTrue(TEXT("NodeA (BT_A) was removed by componentName filter"), bNodeAGone);
    TestTrue(TEXT("NodeB (BT_B) was NOT removed (unrelated component)"), bNodeBStillPresent);

    CleanupTestAsset(AssetPath);
    return true;
}

#endif
