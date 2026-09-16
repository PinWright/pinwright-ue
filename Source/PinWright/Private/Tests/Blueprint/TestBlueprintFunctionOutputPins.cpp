// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace BlueprintFunctionOutputPinTestUtils
{
    inline FString MakeAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline UBlueprint* MakeActorBlueprint(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal, UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
    }

    inline TSharedPtr<FJsonValue> MakePinDefinition(const FString& Name, const FString& Type)
    {
        TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
        Pin->SetStringField(TEXT("name"), Name);
        Pin->SetStringField(TEXT("type"), Type);
        return MakeShared<FJsonValueObject>(Pin);
    }

    inline int32 GetJsonArrayNum(const TSharedPtr<FJsonObject>& Object, const FString& Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values
            ? Values->Num()
            : 0;
    }

    inline UEdGraph* FindFunctionGraph(UBlueprint* Blueprint, const FString& Name)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    inline UEdGraph* FindMacroGraph(UBlueprint* Blueprint, const FString& Name)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph && Graph->GetName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    inline UK2Node_Tunnel* FindMacroTunnel(UEdGraph* Graph, bool bEntry)
    {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
            if (!Tunnel)
            {
                continue;
            }
            if (bEntry && Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs)
            {
                return Tunnel;
            }
            if (!bEntry && Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs)
            {
                return Tunnel;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddFunctionSignaturePinsAndResponseTest,
    "PinWright.blueprint.add_function.SignaturePinsAndResponseMatchGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddFunctionSignaturePinsAndResponseTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintFunctionOutputPinTestUtils;

    const FString AssetPath = MakeAssetPath(TEXT("AddFunctionSignature"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    UBlueprint* Blueprint = MakeActorBlueprint(AssetPath);
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("functionName"), TEXT("ReadSignature"));
    TArray<TSharedPtr<FJsonValue>> Inputs;
    Inputs.Add(MakePinDefinition(TEXT("  bEnabled  "), TEXT("bool")));
    Payload->SetArrayField(TEXT("inputs"), Inputs);
    TArray<TSharedPtr<FJsonValue>> Outputs;
    Outputs.Add(MakePinDefinition(TEXT("  Health01  "), TEXT("float")));
    Payload->SetArrayField(TEXT("outputs"), Outputs);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.add_function handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_function"), Payload, Capture));
    TestTrue(TEXT("blueprint.add_function responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("blueprint.add_function succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_function error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }
    bool bCompiled = false;
    TestTrue(TEXT("add_function response contains compiled"),
        Capture.Result.IsValid()
        && Capture.Result->TryGetBoolField(TEXT("compiled"), bCompiled));
    TestTrue(TEXT("add_function reports the real successful compile"), bCompiled);
    TestTrue(TEXT("add_function success matches the compile result"),
        Capture.Result.IsValid() && Capture.Result->GetBoolField(TEXT("success")) == bCompiled);

    UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, TEXT("ReadSignature"));
    if (!TestNotNull(TEXT("function graph exists"), FunctionGraph))
    {
        return true;
    }

    TArray<UK2Node_FunctionEntry*> EntryNodes;
    TArray<UK2Node_FunctionResult*> ResultNodes;
    FunctionGraph->GetNodesOfClass(EntryNodes);
    FunctionGraph->GetNodesOfClass(ResultNodes);
    TestEqual(TEXT("function graph has one entry terminator"), EntryNodes.Num(), 1);
    TestEqual(TEXT("function graph has one result terminator"), ResultNodes.Num(), 1);

    UK2Node_FunctionEntry* EntryNode = EntryNodes.Num() == 1 ? EntryNodes[0] : nullptr;
    UK2Node_FunctionResult* ResultNode = ResultNodes.Num() == 1 ? ResultNodes[0] : nullptr;
    if (EntryNode)
    {
        UEdGraphPin* InputPin = EntryNode->FindPin(TEXT("bEnabled"), EGPD_Output);
        if (TestNotNull(TEXT("logical input is an entry output pin"), InputPin))
        {
            TestEqual(TEXT("input graph type is bool"),
                InputPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
        }
        TestNull(TEXT("logical output is not stored on the entry"),
            EntryNode->FindPin(TEXT("Health01"), EGPD_Output));
    }
    if (ResultNode)
    {
        UEdGraphPin* OutputPin = ResultNode->FindPin(TEXT("Health01"), EGPD_Input);
        if (TestNotNull(TEXT("logical output is a result input pin"), OutputPin))
        {
            TestEqual(TEXT("output graph type is real"),
                OutputPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Real);
            TestEqual(TEXT("output graph subtype is float"),
                OutputPin->PinType.PinSubCategory, UEdGraphSchema_K2::PC_Float);
        }
        TestNull(TEXT("logical input is not stored on the result"),
            ResultNode->FindPin(TEXT("bEnabled"), EGPD_Input));
    }

    UFunction* GeneratedFunction = Blueprint->GeneratedClass
        ? Blueprint->GeneratedClass->FindFunctionByName(TEXT("ReadSignature"))
        : nullptr;
    if (TestNotNull(TEXT("compiled UFunction exists"), GeneratedFunction))
    {
        int32 ParamCount = 0;
        int32 InputCount = 0;
        int32 OutputCount = 0;
        bool bFoundBoolInput = false;
        bool bFoundFloatOutput = false;
        for (TFieldIterator<FProperty> It(GeneratedFunction); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm))
            {
                continue;
            }

            ++ParamCount;
            const bool bIsOutput = Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm);
            if (bIsOutput)
            {
                ++OutputCount;
            }
            else
            {
                ++InputCount;
            }

            if (Property->GetFName() == FName(TEXT("bEnabled")))
            {
                bFoundBoolInput = Property->IsA<FBoolProperty>() && !bIsOutput;
            }
            else if (Property->GetFName() == FName(TEXT("Health01")))
            {
                bFoundFloatOutput = Property->IsA<FFloatProperty>()
                    && Property->HasAnyPropertyFlags(CPF_OutParm)
                    && !Property->HasAnyPropertyFlags(CPF_ReturnParm);
            }
        }
        TestEqual(TEXT("compiled UFunction has exactly the requested parameters"), ParamCount, 2);
        TestEqual(TEXT("compiled UFunction has one input parameter"), InputCount, 1);
        TestEqual(TEXT("compiled UFunction has one output parameter"), OutputCount, 1);
        TestTrue(TEXT("compiled UFunction preserves the bool input flags and type"),
            bFoundBoolInput);
        TestTrue(TEXT("compiled UFunction preserves the named float output flags and type"),
            bFoundFloatOutput);
    }

    TestEqual(TEXT("response has one measured input"),
        GetJsonArrayNum(Capture.Result, TEXT("inputs")), 1);
    TestEqual(TEXT("response has one measured output"),
        GetJsonArrayNum(Capture.Result, TEXT("outputs")), 1);
    const TSharedPtr<FJsonObject> ResponseInput = JsonArrayFindObjectByStringField(
        Capture.Result, TEXT("inputs"), TEXT("name"), TEXT("bEnabled"));
    const TSharedPtr<FJsonObject> ResponseOutput = JsonArrayFindObjectByStringField(
        Capture.Result, TEXT("outputs"), TEXT("name"), TEXT("Health01"));
    TestTrue(TEXT("response input name is graph-normalized"), ResponseInput.IsValid());
    TestTrue(TEXT("response output name is graph-normalized"), ResponseOutput.IsValid());
    if (ResponseInput.IsValid())
    {
        TestEqual(TEXT("response input type comes from graph"),
            ResponseInput->GetStringField(TEXT("type")), FString(TEXT("bool")));
    }
    if (ResponseOutput.IsValid())
    {
        TestEqual(TEXT("response output type comes from graph"),
            ResponseOutput->GetStringField(TEXT("type")), FString(TEXT("float")));
    }
    TestFalse(TEXT("response does not echo padded input spelling"),
        JsonArrayHasObjectWithStringField(
            Capture.Result, TEXT("inputs"), TEXT("name"), TEXT("  bEnabled  ")));
    TestFalse(TEXT("response does not echo padded output spelling"),
        JsonArrayHasObjectWithStringField(
            Capture.Result, TEXT("outputs"), TEXT("name"), TEXT("  Health01  ")));

    TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
    GetPayload->SetStringField(TEXT("path"), AssetPath);
    FTestResponseCapture GetCapture;
    TestTrue(TEXT("blueprint.get handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.get"), GetPayload, GetCapture));
    if (TestTrue(TEXT("blueprint.get succeeded"), GetCapture.bSuccess))
    {
        const TSharedPtr<FJsonObject> FunctionJson = JsonArrayFindObjectByStringField(
            GetCapture.Result, TEXT("functions"), TEXT("name"), TEXT("ReadSignature"));
        if (TestTrue(TEXT("blueprint.get reports the function"), FunctionJson.IsValid()))
        {
            const TSharedPtr<FJsonObject> GetInput = JsonArrayFindObjectByStringField(
                FunctionJson, TEXT("inputs"), TEXT("name"), TEXT("bEnabled"));
            const TSharedPtr<FJsonObject> GetOutput = JsonArrayFindObjectByStringField(
                FunctionJson, TEXT("outputs"), TEXT("name"), TEXT("Health01"));
            if (TestTrue(TEXT("blueprint.get reports the graph input"), GetInput.IsValid()))
            {
                TestEqual(TEXT("blueprint.get input type comes from graph"),
                    GetInput->GetStringField(TEXT("type")), FString(TEXT("bool")));
            }
            if (TestTrue(TEXT("blueprint.get reports the graph output"), GetOutput.IsValid()))
            {
                TestEqual(TEXT("blueprint.get output type comes from graph"),
                    GetOutput->GetStringField(TEXT("type")), FString(TEXT("float")));
            }
        }
    }

    TSharedPtr<FJsonObject> CollisionPayload = MakeShared<FJsonObject>();
    CollisionPayload->SetStringField(TEXT("path"), AssetPath);
    CollisionPayload->SetStringField(TEXT("functionName"), TEXT("ExecNameCollision"));
    TArray<TSharedPtr<FJsonValue>> CollisionInputs;
    CollisionInputs.Add(MakePinDefinition(TEXT("then"), TEXT("bool")));
    CollisionPayload->SetArrayField(TEXT("inputs"), CollisionInputs);
    FTestResponseCapture CollisionCapture;
    TestTrue(TEXT("collision add_function handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_function"), CollisionPayload, CollisionCapture));
    TestFalse(TEXT("entry exec-name collision is rejected"), CollisionCapture.bSuccess);
    TestEqual(TEXT("entry exec-name collision reports pin creation failure"),
        CollisionCapture.ErrorCode, FString(TEXT("PIN_CREATION_FAILED")));
    TestNull(TEXT("entry exec-name collision rolls back the function graph"),
        FindFunctionGraph(Blueprint, TEXT("ExecNameCollision")));

    TSharedPtr<FJsonObject> DuplicatePayload = MakeShared<FJsonObject>();
    DuplicatePayload->SetStringField(TEXT("path"), AssetPath);
    DuplicatePayload->SetStringField(TEXT("functionName"), TEXT("DuplicateOutputs"));
    TArray<TSharedPtr<FJsonValue>> DuplicateOutputs;
    DuplicateOutputs.Add(MakePinDefinition(TEXT("Value"), TEXT("float")));
    DuplicateOutputs.Add(MakePinDefinition(TEXT("Value"), TEXT("float")));
    DuplicatePayload->SetArrayField(TEXT("outputs"), DuplicateOutputs);
    FTestResponseCapture DuplicateCapture;
    TestTrue(TEXT("duplicate add_function handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_function"), DuplicatePayload, DuplicateCapture));
    TestFalse(TEXT("duplicate function output names are rejected"), DuplicateCapture.bSuccess);
    TestEqual(TEXT("duplicate function outputs report pin creation failure"),
        DuplicateCapture.ErrorCode, FString(TEXT("PIN_CREATION_FAILED")));
    TestNull(TEXT("duplicate outputs roll back the function graph"),
        FindFunctionGraph(Blueprint, TEXT("DuplicateOutputs")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddMacroMeasuredPinResponseTest,
    "PinWright.blueprint.add_macro.ResponsePinsMatchGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddMacroMeasuredPinResponseTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintFunctionOutputPinTestUtils;

    const FString AssetPath = MakeAssetPath(TEXT("AddMacroMeasuredPins"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    UBlueprint* Blueprint = MakeActorBlueprint(AssetPath);
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("macroName"), TEXT("MeasuredMacro"));
    TArray<TSharedPtr<FJsonValue>> Inputs;
    Inputs.Add(MakePinDefinition(TEXT("  Value  "), TEXT("float")));
    Payload->SetArrayField(TEXT("inputs"), Inputs);
    TArray<TSharedPtr<FJsonValue>> Outputs;
    Outputs.Add(MakePinDefinition(TEXT("  Result  "), TEXT("bool")));
    Payload->SetArrayField(TEXT("outputs"), Outputs);
    TArray<TSharedPtr<FJsonValue>> ExecExits;
    ExecExits.Add(MakeShared<FJsonValueString>(FString(TEXT("Completed"))));
    ExecExits.Add(MakeShared<FJsonValueString>(FString(TEXT("Failed"))));
    Payload->SetArrayField(TEXT("execExits"), ExecExits);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.add_macro handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_macro"), Payload, Capture));
    TestTrue(TEXT("blueprint.add_macro responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("blueprint.add_macro succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_macro error: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }
    bool bCompiled = false;
    TestTrue(TEXT("add_macro response contains compiled"),
        Capture.Result.IsValid()
        && Capture.Result->TryGetBoolField(TEXT("compiled"), bCompiled));
    TestTrue(TEXT("add_macro reports the real successful compile"), bCompiled);
    TestTrue(TEXT("add_macro success matches the compile result"),
        Capture.Result.IsValid() && Capture.Result->GetBoolField(TEXT("success")) == bCompiled);

    UEdGraph* MacroGraph = FindMacroGraph(Blueprint, TEXT("MeasuredMacro"));
    UK2Node_Tunnel* EntryTunnel = FindMacroTunnel(MacroGraph, true);
    UK2Node_Tunnel* ExitTunnel = FindMacroTunnel(MacroGraph, false);
    TestNotNull(TEXT("macro graph exists"), MacroGraph);
    TestNotNull(TEXT("macro entry tunnel exists"), EntryTunnel);
    TestNotNull(TEXT("macro exit tunnel exists"), ExitTunnel);

    if (EntryTunnel)
    {
        UEdGraphPin* ValuePin = EntryTunnel->FindPin(TEXT("Value"), EGPD_Output);
        TestTrue(TEXT("input remains a data output on the entry tunnel"),
            ValuePin && ValuePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec);
        UEdGraphPin* ExecutePin = EntryTunnel->FindPin(TEXT("execute"), EGPD_Output);
        TestTrue(TEXT("entry execute pin remains separate"),
            ExecutePin && ExecutePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
    }
    if (ExitTunnel)
    {
        UEdGraphPin* ResultPin = ExitTunnel->FindPin(TEXT("Result"), EGPD_Input);
        TestTrue(TEXT("output remains a data input on the exit tunnel"),
            ResultPin && ResultPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec);
        UEdGraphPin* CompletedPin = ExitTunnel->FindPin(TEXT("Completed"), EGPD_Input);
        UEdGraphPin* FailedPin = ExitTunnel->FindPin(TEXT("Failed"), EGPD_Input);
        TestTrue(TEXT("Completed remains an exit exec pin"),
            CompletedPin && CompletedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
        TestTrue(TEXT("Failed remains an exit exec pin"),
            FailedPin && FailedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
    }

    TestEqual(TEXT("response has one measured data input"),
        GetJsonArrayNum(Capture.Result, TEXT("inputs")), 1);
    TestEqual(TEXT("response has one measured data output"),
        GetJsonArrayNum(Capture.Result, TEXT("outputs")), 1);
    TestTrue(TEXT("response input name matches the normalized tunnel pin"),
        JsonArrayHasObjectWithStringField(
            Capture.Result, TEXT("inputs"), TEXT("name"), TEXT("Value")));
    TestTrue(TEXT("response output name matches the normalized tunnel pin"),
        JsonArrayHasObjectWithStringField(
            Capture.Result, TEXT("outputs"), TEXT("name"), TEXT("Result")));
    TestFalse(TEXT("entry exec pin is absent from data inputs"),
        JsonArrayHasObjectWithStringField(
            Capture.Result, TEXT("inputs"), TEXT("name"), TEXT("execute")));
    TestFalse(TEXT("exit exec pins are absent from data outputs"),
        JsonArrayHasObjectWithStringField(
            Capture.Result, TEXT("outputs"), TEXT("name"), TEXT("Completed")));
    TestEqual(TEXT("response has two graph-derived exec exits"),
        GetJsonArrayNum(Capture.Result, TEXT("execExits")), 2);
    TestTrue(TEXT("response reports the Completed graph pin"),
        JsonStringArrayContains(Capture.Result, TEXT("execExits"), TEXT("Completed")));
    TestTrue(TEXT("response reports the Failed graph pin"),
        JsonStringArrayContains(Capture.Result, TEXT("execExits"), TEXT("Failed")));

    TSharedPtr<FJsonObject> CollisionPayload = MakeShared<FJsonObject>();
    CollisionPayload->SetStringField(TEXT("path"), AssetPath);
    CollisionPayload->SetStringField(TEXT("macroName"), TEXT("ExecNameCollision"));
    TArray<TSharedPtr<FJsonValue>> CollisionInputs;
    CollisionInputs.Add(MakePinDefinition(TEXT("execute"), TEXT("bool")));
    CollisionPayload->SetArrayField(TEXT("inputs"), CollisionInputs);
    TArray<TSharedPtr<FJsonValue>> CollisionExecExits;
    CollisionExecExits.Add(MakeShared<FJsonValueString>(FString(TEXT("Completed"))));
    CollisionPayload->SetArrayField(TEXT("execExits"), CollisionExecExits);
    FTestResponseCapture CollisionCapture;
    TestTrue(TEXT("collision add_macro handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_macro"), CollisionPayload, CollisionCapture));
    TestFalse(TEXT("macro exec-name collision is rejected"), CollisionCapture.bSuccess);
    TestEqual(TEXT("macro exec-name collision reports pin creation failure"),
        CollisionCapture.ErrorCode, FString(TEXT("PIN_CREATION_FAILED")));
    TestNull(TEXT("macro exec-name collision rolls back the graph"),
        FindMacroGraph(Blueprint, TEXT("ExecNameCollision")));

    TSharedPtr<FJsonObject> DuplicatePayload = MakeShared<FJsonObject>();
    DuplicatePayload->SetStringField(TEXT("path"), AssetPath);
    DuplicatePayload->SetStringField(TEXT("macroName"), TEXT("DuplicateOutputs"));
    TArray<TSharedPtr<FJsonValue>> DuplicateOutputs;
    DuplicateOutputs.Add(MakePinDefinition(TEXT("Value"), TEXT("float")));
    DuplicateOutputs.Add(MakePinDefinition(TEXT("Value"), TEXT("float")));
    DuplicatePayload->SetArrayField(TEXT("outputs"), DuplicateOutputs);
    FTestResponseCapture DuplicateCapture;
    TestTrue(TEXT("duplicate add_macro handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_macro"), DuplicatePayload, DuplicateCapture));
    TestFalse(TEXT("duplicate macro output names are rejected"), DuplicateCapture.bSuccess);
    TestEqual(TEXT("duplicate macro outputs report pin creation failure"),
        DuplicateCapture.ErrorCode, FString(TEXT("PIN_CREATION_FAILED")));
    TestNull(TEXT("duplicate outputs roll back the macro graph"),
        FindMacroGraph(Blueprint, TEXT("DuplicateOutputs")));

    return true;
}
