// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace NetworkingRpcSignatureTests
{
    static UEdGraph* FindFunctionGraph(UBlueprint* Blueprint, const FName FunctionName)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetFName() == FunctionName)
            {
                return Graph;
            }
        }
        return nullptr;
    }

    static UK2Node_FunctionEntry* FindEntryNode(UEdGraph* Graph)
    {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
            {
                return EntryNode;
            }
        }
        return nullptr;
    }

    static TSharedPtr<FJsonObject> MakeCreatePayload(const FString& BlueprintPath,
        const FString& FunctionName, const FString& RpcType, bool bReliable)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
        Payload->SetStringField(TEXT("functionName"), FunctionName);
        Payload->SetStringField(TEXT("rpcType"), RpcType);
        Payload->SetBoolField(TEXT("reliable"), bReliable);

        TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
        Input->SetStringField(TEXT("name"), TEXT("PayloadValue"));
        Input->SetStringField(TEXT("type"), TEXT("integer"));
        TArray<TSharedPtr<FJsonValue>> Inputs;
        Inputs.Add(MakeShared<FJsonValueObject>(Input));
        Payload->SetArrayField(TEXT("inputs"), Inputs);
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingCreateRpcDirectionReliabilityMatrixTest,
    "PinWright.networking.create_rpc_function.DirectionReliabilityMatrix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingCreateRpcDirectionReliabilityMatrixTest::RunTest(const FString& Parameters)
{
    using namespace NetworkingRpcSignatureTests;

    const TStrongObjectPtr<UBlueprint> BlueprintGuard(
        CompilerTestUtils::CreateTransientTestBP(TEXT("RpcSignatureMatrix")));
    UBlueprint* Blueprint = BlueprintGuard.Get();
    if (!TestNotNull(TEXT("Transient Blueprint created"), Blueprint))
    {
        return true;
    }

    struct FRpcCase
    {
        const TCHAR* FunctionName;
        const TCHAR* RpcType;
        int32 DirectionFlag;
        bool bReliable;
    };
    const FRpcCase Cases[] = {
        { TEXT("ServerReliable"), TEXT("Server"), FUNC_NetServer, true },
        { TEXT("ServerUnreliable"), TEXT("Server"), FUNC_NetServer, false },
        { TEXT("ClientReliable"), TEXT("Client"), FUNC_NetClient, true },
        { TEXT("ClientUnreliable"), TEXT("Client"), FUNC_NetClient, false },
        { TEXT("MulticastReliable"), TEXT("NetMulticast"), FUNC_NetMulticast, true },
        { TEXT("MulticastUnreliable"), TEXT("NetMulticast"), FUNC_NetMulticast, false },
    };
    const int32 DirectionMask = FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast;

    for (const FRpcCase& Case : Cases)
    {
        FTestResponseCapture Capture;
        const TSharedPtr<FJsonObject> Payload = MakeCreatePayload(
            Blueprint->GetPathName(), Case.FunctionName, Case.RpcType, Case.bReliable);
        TestTrue(*FString::Printf(TEXT("%s handler found"), Case.FunctionName),
            InvokeHandlerWithCapture(TEXT("networking.create_rpc_function"), Payload, Capture));
        TestTrue(*FString::Printf(TEXT("%s creation succeeded"), Case.FunctionName), Capture.bSuccess);

        UEdGraph* Graph = FindFunctionGraph(Blueprint, FName(Case.FunctionName));
        UK2Node_FunctionEntry* EntryNode = FindEntryNode(Graph);
        if (!TestNotNull(*FString::Printf(TEXT("%s entry node exists"), Case.FunctionName), EntryNode))
        {
            continue;
        }

        const int32 GraphFlags = EntryNode->GetExtraFlags();
        TestTrue(*FString::Printf(TEXT("%s has FUNC_Net"), Case.FunctionName),
            (GraphFlags & FUNC_Net) != 0);
        TestEqual(*FString::Printf(TEXT("%s has exactly its requested direction"), Case.FunctionName),
            GraphFlags & DirectionMask, Case.DirectionFlag);
        TestEqual(*FString::Printf(TEXT("%s reliability matches request"), Case.FunctionName),
            (GraphFlags & FUNC_NetReliable) != 0, Case.bReliable);
        TestFalse(*FString::Printf(TEXT("%s has no validation flag by default"), Case.FunctionName),
            (GraphFlags & FUNC_NetValidate) != 0);

        UEdGraphPin* InputPin = EntryNode->FindPin(FName(TEXT("PayloadValue")), EGPD_Output);
        TestNotNull(*FString::Printf(TEXT("%s input is an entry-node output pin"), Case.FunctionName),
            InputPin);
        TestNull(*FString::Printf(TEXT("%s has no reversed entry-node input pin"), Case.FunctionName),
            EntryNode->FindPin(FName(TEXT("PayloadValue")), EGPD_Input));

        UFunction* Function = Blueprint->GeneratedClass
            ? Blueprint->GeneratedClass->FindFunctionByName(FName(Case.FunctionName))
            : nullptr;
        if (TestNotNull(*FString::Printf(TEXT("%s compiled UFunction exists"), Case.FunctionName), Function))
        {
            const int32 CompiledFlags = static_cast<int32>(Function->FunctionFlags);
            TestEqual(*FString::Printf(TEXT("%s compiled direction matches"), Case.FunctionName),
                CompiledFlags & DirectionMask, Case.DirectionFlag);
            TestEqual(*FString::Printf(TEXT("%s compiled reliability matches"), Case.FunctionName),
                (CompiledFlags & FUNC_NetReliable) != 0, Case.bReliable);

            FProperty* InputProperty = Function->FindPropertyByName(FName(TEXT("PayloadValue")));
            if (TestNotNull(*FString::Printf(TEXT("%s compiled input exists"), Case.FunctionName),
                    InputProperty))
            {
                TestTrue(*FString::Printf(TEXT("%s compiled input is a parameter"), Case.FunctionName),
                    InputProperty->HasAnyPropertyFlags(CPF_Parm));
                TestFalse(*FString::Printf(TEXT("%s compiled input is not an out parameter"), Case.FunctionName),
                    InputProperty->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm));
            }
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingRpcInvalidConfigurationRefusedTest,
    "PinWright.networking.create_rpc_function.InvalidConfigurationRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingRpcInvalidConfigurationRefusedTest::RunTest(const FString& Parameters)
{
    using namespace NetworkingRpcSignatureTests;

    const TStrongObjectPtr<UBlueprint> BlueprintGuard(
        CompilerTestUtils::CreateTransientTestBP(TEXT("RpcInvalidConfiguration")));
    UBlueprint* Blueprint = BlueprintGuard.Get();
    if (!TestNotNull(TEXT("Transient Blueprint created"), Blueprint))
    {
        return true;
    }
    const FString BlueprintPath = Blueprint->GetPathName();
    const int32 InitialGraphCount = Blueprint->FunctionGraphs.Num();

    {
        FTestResponseCapture Capture;
        const TSharedPtr<FJsonObject> Payload = MakeCreatePayload(
            BlueprintPath, TEXT("BadDirection"), TEXT("Broadcast"), true);
        TestTrue(TEXT("create_rpc_function handler found for bad direction"),
            InvokeHandlerWithCapture(TEXT("networking.create_rpc_function"), Payload, Capture));
        TestFalse(TEXT("unknown RPC direction is refused"), Capture.bSuccess);
        TestEqual(TEXT("unknown direction has typed error"), Capture.ErrorCode,
            FString(TEXT("INVALID_RPC_CONFIGURATION")));
        TestEqual(TEXT("unknown direction creates no graph"),
            Blueprint->FunctionGraphs.Num(), InitialGraphCount);
    }

    {
        FTestResponseCapture Capture;
        const TSharedPtr<FJsonObject> Payload = MakeCreatePayload(
            BlueprintPath, TEXT("HasReturnValue"), TEXT("Server"), true);
        TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
        Output->SetStringField(TEXT("name"), TEXT("ReturnValue"));
        Output->SetStringField(TEXT("type"), TEXT("boolean"));
        TArray<TSharedPtr<FJsonValue>> Outputs;
        Outputs.Add(MakeShared<FJsonValueObject>(Output));
        Payload->SetArrayField(TEXT("outputs"), Outputs);
        TestTrue(TEXT("create_rpc_function handler found for output signature"),
            InvokeHandlerWithCapture(TEXT("networking.create_rpc_function"), Payload, Capture));
        TestFalse(TEXT("RPC return value is refused"), Capture.bSuccess);
        TestEqual(TEXT("RPC return value has typed error"), Capture.ErrorCode,
            FString(TEXT("INVALID_RPC_CONFIGURATION")));
        TestEqual(TEXT("RPC return refusal creates no graph"),
            Blueprint->FunctionGraphs.Num(), InitialGraphCount);
    }

#if WITH_DEV_AUTOMATION_TESTS
    {
        const int32 GraphCountBeforePinFailure = Blueprint->FunctionGraphs.Num();
        BlueprintHandlerUtils::SetForceParsedPinCreationFailureForTests(true);

        FTestResponseCapture Capture;
        const TSharedPtr<FJsonObject> Payload = MakeCreatePayload(
            BlueprintPath, TEXT("InjectedPinFailure"), TEXT("Server"), true);
        TestTrue(TEXT("create_rpc_function handler found for injected pin failure"),
            InvokeHandlerWithCapture(TEXT("networking.create_rpc_function"), Payload, Capture));

        BlueprintHandlerUtils::SetForceParsedPinCreationFailureForTests(false);
        TestFalse(TEXT("Post-graph pin creation failure is reported"), Capture.bSuccess);
        TestEqual(TEXT("Pin creation failure has typed error"), Capture.ErrorCode,
            FString(TEXT("PIN_CREATION_FAILED")));
        TestEqual(TEXT("Pin creation failure restores function graph count"),
            Blueprint->FunctionGraphs.Num(), GraphCountBeforePinFailure);
        TestNull(TEXT("Pin creation failure leaves no function graph"),
            FindFunctionGraph(Blueprint, FName(TEXT("InjectedPinFailure"))));
        TestNull(TEXT("Pin creation failure leaves no dangling entry node"),
            FindEntryNode(FindFunctionGraph(Blueprint, FName(TEXT("InjectedPinFailure")))));
    }
#endif

    {
        FTestResponseCapture CreateCapture;
        const TSharedPtr<FJsonObject> CreatePayload = MakeCreatePayload(
            BlueprintPath, TEXT("ServerCannotValidate"), TEXT("Server"), false);
        TestTrue(TEXT("Server RPC handler found"), InvokeHandlerWithCapture(
            TEXT("networking.create_rpc_function"), CreatePayload, CreateCapture));
        TestTrue(TEXT("Server RPC created"), CreateCapture.bSuccess);

        FTestResponseCapture ValidationCapture;
        TSharedPtr<FJsonObject> ValidationPayload = MakeShared<FJsonObject>();
        ValidationPayload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
        ValidationPayload->SetStringField(TEXT("functionName"), TEXT("ServerCannotValidate"));
        ValidationPayload->SetBoolField(TEXT("withValidation"), true);
        TestTrue(TEXT("configure_rpc_validation handler found"), InvokeHandlerWithCapture(
            TEXT("networking.configure_rpc_validation"), ValidationPayload, ValidationCapture));
        TestFalse(TEXT("Blueprint RPC validation is refused"), ValidationCapture.bSuccess);
        TestEqual(TEXT("Blueprint validation has typed error"), ValidationCapture.ErrorCode,
            FString(TEXT("UNSUPPORTED")));

        UK2Node_FunctionEntry* EntryNode = FindEntryNode(
            FindFunctionGraph(Blueprint, FName(TEXT("ServerCannotValidate"))));
        if (TestNotNull(TEXT("Server RPC entry still exists"), EntryNode))
        {
            TestFalse(TEXT("refused validation does not mutate flags"),
                (EntryNode->GetExtraFlags() & FUNC_NetValidate) != 0);

            EntryNode->AddExtraFlags(FUNC_NetValidate);
            const int32 FlagsWithUnsupportedValidation = EntryNode->GetExtraFlags();

            FTestResponseCapture ReliabilityCapture;
            TSharedPtr<FJsonObject> ReliabilityPayload = MakeShared<FJsonObject>();
            ReliabilityPayload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
            ReliabilityPayload->SetStringField(
                TEXT("functionName"), TEXT("ServerCannotValidate"));
            ReliabilityPayload->SetBoolField(TEXT("reliable"), true);
            TestTrue(TEXT("set_rpc_reliability finds stale validation fixture"),
                InvokeHandlerWithCapture(TEXT("networking.set_rpc_reliability"),
                    ReliabilityPayload, ReliabilityCapture));
            TestFalse(TEXT("Reliability change on unsupported validation is refused"),
                ReliabilityCapture.bSuccess);
            TestEqual(TEXT("Unsupported validation reliability has typed error"),
                ReliabilityCapture.ErrorCode, FString(TEXT("INVALID_RPC_CONFIGURATION")));
            TestEqual(TEXT("Refused reliability preserves the existing flags"),
                EntryNode->GetExtraFlags(), FlagsWithUnsupportedValidation);

            FTestResponseCapture ClearCapture;
            ValidationPayload->SetBoolField(TEXT("withValidation"), false);
            TestTrue(TEXT("configure_rpc_validation finds stale validation fixture"),
                InvokeHandlerWithCapture(TEXT("networking.configure_rpc_validation"),
                    ValidationPayload, ClearCapture));
            TestTrue(TEXT("Unsupported validation flag can be cleared"), ClearCapture.bSuccess);
            TestFalse(TEXT("Validation clear removes FUNC_NetValidate"),
                (EntryNode->GetExtraFlags() & FUNC_NetValidate) != 0);
        }
    }

    {
        UEdGraph* PlainFunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
            Blueprint, FName(TEXT("NotAnRpc")), UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UFunction>(
            Blueprint, PlainFunctionGraph, false, static_cast<UFunction*>(nullptr));
        UK2Node_FunctionEntry* EntryNode = FindEntryNode(PlainFunctionGraph);
        if (TestNotNull(TEXT("Plain function entry exists"), EntryNode))
        {
            const int32 FlagsBeforeRequest = EntryNode->GetExtraFlags();

            FTestResponseCapture ReliabilityCapture;
            TSharedPtr<FJsonObject> ReliabilityPayload = MakeShared<FJsonObject>();
            ReliabilityPayload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
            ReliabilityPayload->SetStringField(TEXT("functionName"), TEXT("NotAnRpc"));
            ReliabilityPayload->SetBoolField(TEXT("reliable"), true);
            TestTrue(TEXT("set_rpc_reliability handler found"), InvokeHandlerWithCapture(
                TEXT("networking.set_rpc_reliability"), ReliabilityPayload, ReliabilityCapture));
            TestFalse(TEXT("Reliability on a non-RPC function is refused"),
                ReliabilityCapture.bSuccess);
            TestEqual(TEXT("Non-RPC reliability has typed error"), ReliabilityCapture.ErrorCode,
                FString(TEXT("INVALID_RPC_CONFIGURATION")));
            TestEqual(TEXT("Refused reliability does not mutate flags"),
                EntryNode->GetExtraFlags(), FlagsBeforeRequest);
        }
    }

    return true;
}
