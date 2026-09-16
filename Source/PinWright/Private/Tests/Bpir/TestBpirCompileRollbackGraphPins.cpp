// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "BpirGraphTestHelpers.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"


namespace
{
    UEdGraphPin* FindExecOutput(UEdGraphNode* Node)
    {
        return Node ? Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
    }

    UEdGraphPin* FindExecInput(UEdGraphNode* Node)
    {
        return Node ? Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
    }

    int32 CountLinksTo(const UEdGraphPin* Source, const UEdGraphPin* Target)
    {
        int32 Count = 0;
        if (!Source || !Target)
        {
            return Count;
        }

        for (const UEdGraphPin* LinkedPin : Source->LinkedTo)
        {
            if (LinkedPin == Target)
            {
                ++Count;
            }
        }
        return Count;
    }

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompileBpirRollbackGraphPinsTest,
    "PinWright.blueprint.compile_bpir.RollbackGraphPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCompileBpirRollbackGraphPinsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BpirRollbackGraphPins_%s"),
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

    UK2Node_CustomEvent* ExistingEvent = NewObject<UK2Node_CustomEvent>(
        EventGraph, NAME_None, RF_Transactional);
    ExistingEvent->CustomFunctionName = FName(TEXT("RollbackGraphPinsEvent"));
    ExistingEvent->CreateNewGuid();
    ExistingEvent->PostPlacedNewNode();
    ExistingEvent->AllocateDefaultPins();
    EventGraph->AddNode(ExistingEvent, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    UK2Node_CallFunction* DownstreamNode = BpirGraphTestHelpers::AddPrintStringNode(EventGraph, /*bTransactional=*/true);
    TestNotNull(TEXT("Downstream PrintString node created"), DownstreamNode);
    if (!DownstreamNode)
    {
        return true;
    }

    UEdGraphPin* OriginalEventThen = FindExecOutput(ExistingEvent);
    UEdGraphPin* OriginalDownstreamExec = FindExecInput(DownstreamNode);
    TestNotNull(TEXT("Custom event has exec output"), OriginalEventThen);
    TestNotNull(TEXT("Downstream node has exec input"), OriginalDownstreamExec);
    if (!OriginalEventThen || !OriginalDownstreamExec)
    {
        return true;
    }

    const UEdGraphSchema* Schema = OriginalEventThen->GetSchema();
    const bool bConnected = Schema
        ? Schema->TryCreateConnection(OriginalEventThen, OriginalDownstreamExec)
        : (OriginalEventThen->MakeLinkTo(OriginalDownstreamExec), true);
    TestTrue(TEXT("Original exec link created"), bConnected);
    TestEqual(TEXT("Original event pin links once"), OriginalEventThen->LinkedTo.Num(), 1);
    TestEqual(TEXT("Original downstream pin links once"), OriginalDownstreamExec->LinkedTo.Num(), 1);

    const FGuid OriginalEventGuid = ExistingEvent->NodeGuid;
    const FGuid OriginalDownstreamGuid = DownstreamNode->NodeGuid;

    const FString BpirCode = TEXT(
        "entry custom_event RollbackGraphPinsEvent() {\n"
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

    UEdGraphNode* RestoredEventNode = FBlueprintEditorUtils::GetNodeByGUID(BP, OriginalEventGuid);
    UEdGraphNode* RestoredDownstreamNode = FBlueprintEditorUtils::GetNodeByGUID(BP, OriginalDownstreamGuid);
    TestNotNull(TEXT("Original custom event still resolves after rollback"), RestoredEventNode);
    TestNotNull(TEXT("Original downstream node still resolves after rollback"), RestoredDownstreamNode);
    if (!RestoredEventNode || !RestoredDownstreamNode)
    {
        return true;
    }

    UEdGraphPin* RestoredEventThen = FindExecOutput(RestoredEventNode);
    UEdGraphPin* RestoredDownstreamExec = FindExecInput(RestoredDownstreamNode);
    TestNotNull(TEXT("Restored custom event exec output exists"), RestoredEventThen);
    TestNotNull(TEXT("Restored downstream exec input exists"), RestoredDownstreamExec);
    if (!RestoredEventThen || !RestoredDownstreamExec)
    {
        return true;
    }

    TestEqual(TEXT("Restored custom event exec output has exactly one link"),
        RestoredEventThen->LinkedTo.Num(),
        1);
    TestEqual(TEXT("Restored downstream exec input has exactly one link"),
        RestoredDownstreamExec->LinkedTo.Num(),
        1);
    TestEqual(TEXT("Restored custom event links to downstream exactly once"),
        CountLinksTo(RestoredEventThen, RestoredDownstreamExec),
        1);
    TestEqual(TEXT("Restored downstream links back to custom event exactly once"),
        CountLinksTo(RestoredDownstreamExec, RestoredEventThen),
        1);

    return true;
}
