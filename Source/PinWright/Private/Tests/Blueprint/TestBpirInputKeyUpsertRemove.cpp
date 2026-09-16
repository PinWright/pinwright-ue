// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "K2Node_InputKey.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace
{
    FString MakeInputKeyTestAssetPath()
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BpirInputKeyUpsert_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    TArray<UK2Node_InputKey*> FindInputKeyNodes(UBlueprint* Blueprint, const FKey& Key)
    {
        TArray<UK2Node_InputKey*> Nodes;
        if (!Blueprint)
        {
            return Nodes;
        }

        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(Node))
                {
                    if (InputKeyNode->InputKey == Key)
                    {
                        Nodes.Add(InputKeyNode);
                    }
                }
            }
        }
        return Nodes;
    }

    bool CompileInputKeyBpir(
        FAutomationTestBase& Test,
        const FString& AssetPath,
        const FString& PrintStringValue)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("mode"), TEXT("append"));
        Payload->SetStringField(TEXT("code"), FString::Printf(
            TEXT("entry key_pressed Tab() {\n")
            TEXT("    call PrintString(InString: \"%s\")\n")
            TEXT("}\n"),
            *PrintStringValue));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("blueprint.compile_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
        Test.TestTrue(TEXT("compile_bpir response was sent"), Capture.bWasCalled);
        Test.TestTrue(TEXT("compile_bpir succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            Test.AddError(FString::Printf(
                TEXT("compile_bpir failed: code=%s message=%s"),
                *Capture.ErrorCode,
                *Capture.Message));
        }
        return Capture.bSuccess;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyUpsertAndRemoveEventTest,
    "PinWright.blueprint.compile_bpir.InputKeyUpsertAndRemoveEvent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyUpsertAndRemoveEventTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeInputKeyTestAssetPath();
    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* BlueprintPackage = Blueprint->GetOutermost())
        {
            BlueprintPackage->SetDirtyFlag(false);
        }
        CleanupTestAsset(AssetPath);
    };

    if (!CompileInputKeyBpir(*this, AssetPath, TEXT("first")))
    {
        return false;
    }
    if (!CompileInputKeyBpir(*this, AssetPath, TEXT("second")))
    {
        return false;
    }

    TArray<UK2Node_InputKey*> TabNodes = FindInputKeyNodes(Blueprint, EKeys::Tab);
    TestEqual(TEXT("Exactly one Tab UK2Node_InputKey remains after double compile"), TabNodes.Num(), 1);
    if (TabNodes.Num() != 1)
    {
        return false;
    }

    UK2Node_InputKey* TabNode = TabNodes[0];
    UEdGraphPin* PressedPin = TabNode->FindPin(TEXT("Pressed"), EGPD_Output);
    TestTrue(TEXT("Tab InputKey Pressed exec pin is linked"),
        PressedPin && PressedPin->LinkedTo.Num() > 0);

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("path"), AssetPath);
    RemovePayload->SetStringField(TEXT("eventName"), TEXT("Tab"));
    RemovePayload->SetStringField(TEXT("nodeId"), TabNode->NodeGuid.ToString());

    FTestResponseCapture RemoveCapture;
    TestTrue(TEXT("blueprint.remove_event handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.remove_event"), RemovePayload, RemoveCapture));
    TestTrue(TEXT("remove_event response was sent"), RemoveCapture.bWasCalled);
    TestTrue(TEXT("remove_event succeeded"), RemoveCapture.bSuccess);
    if (!RemoveCapture.bSuccess)
    {
        AddError(FString::Printf(
            TEXT("remove_event failed: code=%s message=%s"),
            *RemoveCapture.ErrorCode,
            *RemoveCapture.Message));
        return false;
    }

    double RemovedNodeCount = 0.0;
    TestTrue(TEXT("remove_event response has removedNodeCount"),
        RemoveCapture.Result.IsValid()
        && RemoveCapture.Result->TryGetNumberField(TEXT("removedNodeCount"), RemovedNodeCount));
    TestTrue(TEXT("remove_event removed at least one node"), RemovedNodeCount > 0.0);

    TabNodes = FindInputKeyNodes(Blueprint, EKeys::Tab);
    TestEqual(TEXT("No Tab UK2Node_InputKey nodes remain after remove_event"), TabNodes.Num(), 0);

    return true;
}
