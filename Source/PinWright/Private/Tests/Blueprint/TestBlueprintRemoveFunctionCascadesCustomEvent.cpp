// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "EdGraphSchema_K2.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveFunctionCascadesCustomEventTest,
    "PinWright.blueprint.remove_function.CascadesCustomEvent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintRemoveFunctionCascadesCustomEventTest::RunTest(const FString& Parameters)
{
    // On-disk path required: LoadBlueprintAsset (called by the handler) cannot see
    // transient-package assets. Follows the FBlueprintRemoveEventComponentNameFilterTest pattern.
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/RemoveFuncCustomEvent_%s"),
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

    // Add a UK2Node_CustomEvent named "ApplyTrackInfo"
    UK2Node_CustomEvent* CustomEventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
    CustomEventNode->CustomFunctionName = FName(TEXT("ApplyTrackInfo"));
    CustomEventNode->CreateNewGuid();
    CustomEventNode->PostPlacedNewNode();
    CustomEventNode->AllocateDefaultPins();
    EventGraph->AddNode(CustomEventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    // Add one downstream exec node (ExecutionSequence) wired to the custom event's Then pin
    UK2Node_ExecutionSequence* SeqNode = NewObject<UK2Node_ExecutionSequence>(EventGraph);
    SeqNode->CreateNewGuid();
    SeqNode->PostPlacedNewNode();
    SeqNode->AllocateDefaultPins();
    EventGraph->AddNode(SeqNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    // Wire CustomEvent -> SeqNode via exec pins
    UEdGraphPin* ThenPin = CustomEventNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* ExecPin = SeqNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    if (ThenPin && ExecPin)
    {
        ThenPin->MakeLinkTo(ExecPin);
    }

    // Compile so the blueprint is valid before the handler runs.
    // No disk save needed — LoadBlueprintAsset uses FindObject which finds in-memory packages.
    FKismetEditorUtilities::CompileBlueprint(BP);

    // Verify both nodes are present before invoking the handler
    const bool bEventNodePresent = EventGraph->Nodes.Contains(CustomEventNode);
    const bool bSeqNodePresent   = EventGraph->Nodes.Contains(SeqNode);
    if (!TestTrue(TEXT("CustomEventNode present before removal"), bEventNodePresent) ||
        !TestTrue(TEXT("SeqNode present before removal"), bSeqNodePresent))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Dispatch blueprint.remove_function with the custom event's name
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"),         AssetPath);
    Payload->SetStringField(TEXT("functionName"), TEXT("ApplyTrackInfo"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.remove_function"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);

    // --- Assertions ---

    TestTrue(TEXT("Response success == true"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        // Either kind == "custom_event" OR graphKind == "event" signals the new cascade branch
        const bool bKindIsCustomEvent =
            Capture.Result->HasField(TEXT("kind")) &&
            Capture.Result->GetStringField(TEXT("kind")) == TEXT("custom_event");
        const bool bGraphKindIsEvent =
            Capture.Result->HasField(TEXT("graphKind")) &&
            Capture.Result->GetStringField(TEXT("graphKind")) == TEXT("event");

        TestTrue(TEXT("Response carries custom_event marker (kind or graphKind)"),
            bKindIsCustomEvent || bGraphKindIsEvent);

        // At minimum: the custom event root + the sequence node (2 nodes)
        if (Capture.Result->HasField(TEXT("removedNodeCount")))
        {
            const int32 RemovedCount =
                static_cast<int32>(Capture.Result->GetNumberField(TEXT("removedNodeCount")));
            TestTrue(TEXT("removedNodeCount >= 2"), RemovedCount >= 2);
        }
        else
        {
            AddError(TEXT("Response missing removedNodeCount field"));
        }
    }
    else
    {
        AddError(TEXT("Response result object is null"));
    }

    // After the handler ran, verify no matching UK2Node_CustomEvent remains in the
    // in-memory blueprint's ubergraph pages. LoadBlueprintAsset uses FindObject, so
    // the handler operated on the same in-memory object — no disk round-trip needed.
    {
        bool bCustomEventStillExists = false;
        for (UEdGraph* Page : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Page->Nodes)
            {
                if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
                {
                    if (CE->CustomFunctionName.ToString().Equals(
                            TEXT("ApplyTrackInfo"), ESearchCase::IgnoreCase))
                    {
                        bCustomEventStillExists = true;
                    }
                }
            }
        }
        TestFalse(TEXT("UK2Node_CustomEvent 'ApplyTrackInfo' no longer in ubergraph"),
            bCustomEventStillExists);
    }

    CleanupTestAsset(AssetPath);
    return true;
}
