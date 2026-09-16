// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionResult.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

namespace BlueprintInterfaceOutputOverrideTestUtils
{
    FString MakeUniqueAssetPath(const TCHAR* Prefix)
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    bool CreateBlueprintAsset(
        FAutomationTestBase& Test,
        const FString& AssetPath,
        const TCHAR* BlueprintType)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(AssetPath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(AssetPath));
        Payload->SetStringField(TEXT("blueprintType"), BlueprintType);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("blueprint.create handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture));
        if (!Test.TestTrue(TEXT("blueprint.create succeeded"), Capture.bSuccess))
        {
            Test.AddError(FString::Printf(
                TEXT("blueprint.create failed: %s %s"),
                *Capture.ErrorCode,
                *Capture.Message));
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonValue> MakePinDefinition(const TCHAR* Name, const TCHAR* Type)
    {
        TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
        Pin->SetStringField(TEXT("name"), Name);
        Pin->SetStringField(TEXT("type"), Type);
        return MakeShared<FJsonValueObject>(Pin);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInterfaceOutputOverrideUsesFunctionGraphTest,
    "PinWright.blueprint.interface.OutputOverrideUsesFunctionGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintInterfaceOutputOverrideUsesFunctionGraphTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintInterfaceOutputOverrideTestUtils;

    const FString InterfacePath = MakeUniqueAssetPath(TEXT("BPI_OutputOverride"));
    const FString TargetPath = MakeUniqueAssetPath(TEXT("BP_OutputOverride"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
        CleanupTestAsset(InterfacePath);
    };

    if (!CreateBlueprintAsset(*this, InterfacePath, TEXT("interface"))
        || !CreateBlueprintAsset(*this, TargetPath, TEXT("actor")))
    {
        return true;
    }

    const FString FunctionName = TEXT("GetHealth01");
    TSharedPtr<FJsonObject> AddFunctionPayload = MakeShared<FJsonObject>();
    AddFunctionPayload->SetStringField(TEXT("path"), InterfacePath);
    AddFunctionPayload->SetStringField(TEXT("functionName"), FunctionName);
    TArray<TSharedPtr<FJsonValue>> Outputs;
    Outputs.Add(MakePinDefinition(TEXT("Health01"), TEXT("float")));
    AddFunctionPayload->SetArrayField(TEXT("outputs"), Outputs);

    FTestResponseCapture AddFunctionCapture;
    TestTrue(TEXT("interface blueprint.add_function handler found"),
        InvokeHandlerWithCapture(
            TEXT("blueprint.add_function"),
            AddFunctionPayload,
            AddFunctionCapture));
    if (!TestTrue(TEXT("interface getter creation succeeded"), AddFunctionCapture.bSuccess))
    {
        AddError(FString::Printf(
            TEXT("interface add_function failed: %s %s"),
            *AddFunctionCapture.ErrorCode,
            *AddFunctionCapture.Message));
        return true;
    }

    UBlueprint* InterfaceBlueprint = Cast<UBlueprint>(
        UEditorAssetLibrary::LoadAsset(ToObjectPath(InterfacePath)));
    UBlueprint* TargetBlueprint = Cast<UBlueprint>(
        UEditorAssetLibrary::LoadAsset(ToObjectPath(TargetPath)));
    if (!TestNotNull(TEXT("interface Blueprint loads"), InterfaceBlueprint)
        || !TestNotNull(TEXT("target Blueprint loads"), TargetBlueprint)
        || !TestNotNull(TEXT("interface generated class exists"),
            InterfaceBlueprint ? InterfaceBlueprint->GeneratedClass.Get() : nullptr))
    {
        return true;
    }

    TSharedPtr<FJsonObject> AddInterfacePayload = MakeShared<FJsonObject>();
    AddInterfacePayload->SetStringField(TEXT("path"), TargetPath);
    AddInterfacePayload->SetStringField(
        TEXT("interfaceClass"),
        InterfaceBlueprint->GeneratedClass->GetPathName());
    AddInterfacePayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture AddInterfaceCapture;
    TestTrue(TEXT("blueprint.add_interface handler found"),
        InvokeHandlerWithCapture(
            TEXT("blueprint.add_interface"),
            AddInterfacePayload,
            AddInterfaceCapture));
    if (!TestTrue(TEXT("interface implementation creation succeeded"),
        AddInterfaceCapture.bSuccess))
    {
        AddError(FString::Printf(
            TEXT("add_interface failed: %s %s"),
            *AddInterfaceCapture.ErrorCode,
            *AddInterfaceCapture.Message));
        return true;
    }

    UEdGraph* OriginalInterfaceGraph = FindImplementedInterfaceGraphByName(
        TargetBlueprint,
        FunctionName);
    if (!TestNotNull(TEXT("output interface owns a function graph"), OriginalInterfaceGraph))
    {
        return true;
    }

    TSharedPtr<FJsonObject> OverridePayload = MakeShared<FJsonObject>();
    OverridePayload->SetStringField(TEXT("path"), TargetPath);
    OverridePayload->SetStringField(TEXT("functionName"), FunctionName);
    OverridePayload->SetBoolField(TEXT("override"), true);

    FTestResponseCapture OverrideCapture;
    TestTrue(TEXT("override blueprint.add_function handler found"),
        InvokeHandlerWithCapture(
            TEXT("blueprint.add_function"),
            OverridePayload,
            OverrideCapture));
    if (!TestTrue(TEXT("output interface override succeeds"), OverrideCapture.bSuccess))
    {
        AddError(FString::Printf(
            TEXT("override add_function failed: %s %s"),
            *OverrideCapture.ErrorCode,
            *OverrideCapture.Message));
        return true;
    }

    TestTrue(TEXT("the interface-owned graph object is reused"),
        FindImplementedInterfaceGraphByName(TargetBlueprint, FunctionName) == OriginalInterfaceGraph);
    TestFalse(TEXT("no duplicate ordinary function graph is created"),
        HasOrdinaryFunctionGraphByName(TargetBlueprint, FunctionName));
    TestFalse(TEXT("no output-less override event is created"),
        HasOverrideEventByName(TargetBlueprint, FunctionName));

    bool bEventOverride = true;
    TestTrue(TEXT("override response includes the graph/event discriminator"),
        OverrideCapture.Result.IsValid()
        && OverrideCapture.Result->TryGetBoolField(TEXT("eventOverride"), bEventOverride));
    TestFalse(TEXT("interface getter response reports a function graph"), bEventOverride);

    TArray<UK2Node_FunctionResult*> ResultNodes;
    OriginalInterfaceGraph->GetNodesOfClass(ResultNodes);
    TestEqual(TEXT("interface getter retains one result terminator"), ResultNodes.Num(), 1);
    if (ResultNodes.Num() == 1)
    {
        UEdGraphPin* HealthPin = ResultNodes[0]->FindPin(TEXT("Health01"), EGPD_Input);
        if (TestNotNull(TEXT("interface getter retains the named output"), HealthPin))
        {
            TestEqual(TEXT("named output remains a real pin"),
                HealthPin->PinType.PinCategory,
                UEdGraphSchema_K2::PC_Real);
            TestEqual(TEXT("named output remains float"),
                HealthPin->PinType.PinSubCategory,
                UEdGraphSchema_K2::PC_Float);
        }
    }

    return true;
}
