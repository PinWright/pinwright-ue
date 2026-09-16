// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Interface.h"

namespace
{
    FString ToBlueprintCreateInterfaceObjectPath(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return AssetName.IsEmpty()
            ? PackagePath
            : FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    }

    FString MakeUniqueInterfacePath()
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BPI_CreateInterface_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCreateInterfaceAssetTest,
    "PinWright.blueprint.create.InterfaceAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCreateInterfaceAssetTest::RunTest(const FString& Parameters)
{
    const FString InterfacePath = MakeUniqueInterfacePath();
    const FString InvalidParentPath = MakeUniqueInterfacePath();
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InterfacePath);
        CleanupTestAsset(InvalidParentPath);
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(InterfacePath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(InterfacePath));
        Payload->SetStringField(TEXT("blueprintType"), TEXT("interface"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.create handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture));
        TestTrue(TEXT("blueprint.create responded"), Capture.bWasCalled);
        if (!TestTrue(TEXT("blueprint.create interface succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("create error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    UBlueprint* InterfaceBlueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ToBlueprintCreateInterfaceObjectPath(InterfacePath)));
    if (!TestNotNull(TEXT("created interface asset loads as UBlueprint"), InterfaceBlueprint))
    {
        return true;
    }
    TestEqual(TEXT("created asset is a Blueprint Interface"),
        InterfaceBlueprint->BlueprintType, EBlueprintType::BPTYPE_Interface);
    TestTrue(TEXT("interface parent defaults to UInterface"),
        InterfaceBlueprint->ParentClass
        && InterfaceBlueprint->ParentClass->IsChildOf(UInterface::StaticClass()));

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), InterfacePath);
        Payload->SetStringField(TEXT("functionName"), TEXT("ComputeValue"));

        TArray<TSharedPtr<FJsonValue>> Inputs;
        {
            TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
            Input->SetStringField(TEXT("name"), TEXT("Input"));
            Input->SetStringField(TEXT("type"), TEXT("int"));
            Inputs.Add(MakeShared<FJsonValueObject>(Input));
        }
        Payload->SetArrayField(TEXT("inputs"), Inputs);

        TArray<TSharedPtr<FJsonValue>> Outputs;
        {
            TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
            Output->SetStringField(TEXT("name"), TEXT("Result"));
            Output->SetStringField(TEXT("type"), TEXT("int"));
            Outputs.Add(MakeShared<FJsonValueObject>(Output));
        }
        Payload->SetArrayField(TEXT("outputs"), Outputs);

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.add_function handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_function"), Payload, Capture));
        TestTrue(TEXT("blueprint.add_function responded"), Capture.bWasCalled);
        if (!TestTrue(TEXT("blueprint.add_function on interface succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_function error: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        if (Capture.Result.IsValid())
        {
            TestTrue(TEXT("interface function response is public"),
                Capture.Result->GetBoolField(TEXT("public")));
        }
    }

    UEdGraph* FunctionGraph = nullptr;
    for (UEdGraph* Graph : InterfaceBlueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetName().Equals(TEXT("ComputeValue"), ESearchCase::IgnoreCase))
        {
            FunctionGraph = Graph;
            break;
        }
    }
    if (!TestNotNull(TEXT("interface function graph exists"), FunctionGraph))
    {
        return true;
    }

    UK2Node_FunctionEntry* EntryNode = nullptr;
    int32 NonTerminatorNodeCount = 0;
    for (UEdGraphNode* Node : FunctionGraph->Nodes)
    {
        if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            EntryNode = Entry;
            continue;
        }
        if (Cast<UK2Node_FunctionResult>(Node))
        {
            continue;
        }
        ++NonTerminatorNodeCount;
    }
    TestNotNull(TEXT("interface function has an entry terminator"), EntryNode);
    TestEqual(TEXT("interface function has no body implementation nodes"), NonTerminatorNodeCount, 0);
    if (EntryNode)
    {
        TestTrue(TEXT("interface function is public"),
            (EntryNode->GetExtraFlags() & FUNC_Public) != 0);
        TestFalse(TEXT("interface function is not private"),
            (EntryNode->GetExtraFlags() & FUNC_Private) != 0);
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InterfacePath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.inspect handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.inspect"), Payload, Capture));
        TestTrue(TEXT("blueprint.inspect responded"), Capture.bWasCalled);
        if (TestTrue(TEXT("blueprint.inspect succeeded"), Capture.bSuccess) && Capture.Result.IsValid())
        {
            FString BlueprintType;
            Capture.Result->TryGetStringField(TEXT("blueprintType"), BlueprintType);
            TestEqual(TEXT("inspect reports blueprintType Interface"),
                BlueprintType, FString(TEXT("Interface")));
            TestFalse(TEXT("inspect does not add kind field"),
                Capture.Result->HasField(TEXT("kind")));
        }
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(InvalidParentPath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(InvalidParentPath));
        Payload->SetStringField(TEXT("blueprintType"), TEXT("interface"));
        Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.create invalid parent handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture));
        TestTrue(TEXT("blueprint.create invalid parent responded"), Capture.bWasCalled);
        TestFalse(TEXT("blueprint.create rejects Actor parent for interface"), Capture.bSuccess);
        TestEqual(TEXT("invalid interface parent error code"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    return true;
}
