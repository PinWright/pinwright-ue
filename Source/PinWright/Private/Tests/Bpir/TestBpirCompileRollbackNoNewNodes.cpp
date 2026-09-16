// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompileBpirRollbackNoNewNodesTest,
    "PinWright.blueprint.compile_bpir.RollbackNoNewNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompileBpirRollbackNoNewNodesTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BpirRollbackNoNewNodes_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return true;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* BlueprintPackage = BP->GetOutermost())
        {
            BlueprintPackage->SetDirtyFlag(false);
        }
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    };

    BP->SetFlags(RF_Transactional);

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        return true;
    }
    EventGraph->SetFlags(RF_Transactional);

    TSet<UEdGraphNode*> Before;
    Before.Reserve(EventGraph->Nodes.Num());
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node)
        {
            Before.Add(Node);
        }
    }

    const FString BpirCode = TEXT(
        "entry custom_event RollbackTest() {\n"
        "    call ThisFunctionDoesNotExistAnywhere()\n"
        "}\n");

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("code"), BpirCode);
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.compile_bpir handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
    TestTrue(TEXT("Response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("Response is a structured error"), Capture.bSuccess);
    TestEqual(TEXT("Error code is COMPILE_FAILED"),
        Capture.ErrorCode,
        FString(TEXT("COMPILE_FAILED")));

    TSet<UEdGraphNode*> After;
    After.Reserve(EventGraph->Nodes.Num());
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node)
        {
            After.Add(Node);
        }
    }

    TestEqual(TEXT("Node count unchanged after failed compile"),
        After.Num(), Before.Num());
    TestEqual(TEXT("No nodes added by failed compile"),
        After.Difference(Before).Num(), 0);
    TestEqual(TEXT("No nodes lost by failed compile"),
        Before.Difference(After).Num(), 0);

    return true;
}
