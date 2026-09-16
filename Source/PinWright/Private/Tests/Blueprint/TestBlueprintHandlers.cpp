// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Blueprint domain handlers.
// Covers all 9 handler files: BlueprintCompileHandler, BlueprintComponentHandler,
// BlueprintCreationHandler, BlueprintEventHandler, BlueprintFunctionHandler,
// BlueprintGraphHandler, BlueprintInfoHandler, BlueprintPropertyHandler, SCSHandler.
// Each test exercises the handler's entry-point validation: missing required params
// produce a graceful error (no crash), and optional-only handlers accept valid input.
#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "Engine/UserDefinedEnum.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_Timeline.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintIndexHandler.h"
#include "Handlers/Blueprint/SCSTextEmitter.h"
#include "FindInBlueprintManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/Bpir/BpirGraphTestHelpers.h"
#if __has_include("K2Node_ComponentBoundEvent.h")
#include "K2Node_ComponentBoundEvent.h"
#define MCP_TEST_HAS_COMPONENT_BOUND_EVENT 1
#else
#define MCP_TEST_HAS_COMPONENT_BOUND_EVENT 0
#endif

namespace
{
    FString ToBlueprintHandlersObjectPath(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return AssetName.IsEmpty()
            ? PackagePath
            : FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    }

    FString MakeUniqueAssetPath(const FString& Prefix)
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Detach the fixture instead of force-deleting it. UEditorAssetLibrary::DeleteAsset routes
    // through ObjectTools::ForceDeleteObjects, whose referencer sweep walks every live UObject;
    // the 36 anchors in this file cost 226 s of the blueprint namespace's 262 s of suite time.
    // CleanupTestAsset accepts the package-path form directly (it derives the object path itself),
    // frees the path by renaming into /Transient, deletes any saved .uasset and notifies the
    // registry - so a same-path re-create inside one test still works.
    void CleanupAsset(const FString& PackagePath)
    {
        if (PackagePath.IsEmpty())
        {
            return;
        }

        CleanupTestAsset(PackagePath);
    }

    bool SetBlueprintCdoFloatingProperty(UBlueprint* Blueprint, const FName PropertyName, const double Value)
    {
        if (!Blueprint || !Blueprint->GeneratedClass)
        {
            return false;
        }

        UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
        if (!CDO)
        {
            return false;
        }

        FNumericProperty* Property = FindFProperty<FNumericProperty>(CDO->GetClass(), PropertyName);
        if (!Property || !Property->IsFloatingPoint())
        {
            return false;
        }

        Property->SetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(CDO), Value);
        return true;
    }

    UUserDefinedEnum* LoadUserDefinedEnum(const FString& PackagePath)
    {
        UObject* Loaded = UEditorAssetLibrary::LoadAsset(ToBlueprintHandlersObjectPath(PackagePath));
        if (!Loaded)
        {
            Loaded = UEditorAssetLibrary::LoadAsset(PackagePath);
        }
        return Cast<UUserDefinedEnum>(Loaded);
    }

    FString StripEnumScope(const FString& InName)
    {
        FString Name = InName;
        int32 ScopeSep = INDEX_NONE;
        if (Name.FindLastChar(TEXT(':'), ScopeSep) && ScopeSep >= 0 && ScopeSep + 1 < Name.Len())
        {
            Name = Name.Mid(ScopeSep + 1);
        }
        return Name;
    }

    bool EnumContainsEntry(UUserDefinedEnum* EnumAsset, const FString& EntryName)
    {
        if (!EnumAsset)
        {
            return false;
        }

        const int32 NumEntries = EnumAsset->NumEnums();
        for (int32 Index = 0; Index < NumEntries; ++Index)
        {
            const FString InternalName = EnumAsset->GetNameStringByIndex(Index);
            if (InternalName.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase))
            {
                continue;
            }

            const FString ShortInternalName = StripEnumScope(InternalName);
            const FString AuthoredName = EnumAsset->GetAuthoredNameStringByIndex(Index);
            const FString DisplayName = EnumAsset->GetDisplayNameTextByIndex(Index).ToString();
            if (EntryName.Equals(ShortInternalName, ESearchCase::IgnoreCase) ||
                EntryName.Equals(AuthoredName, ESearchCase::IgnoreCase) ||
                EntryName.Equals(DisplayName, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }

        return false;
    }

    bool TryResolveEnumLiteralValue(const UEnum* EnumType, const FString& Literal, int64& OutValue)
    {
        OutValue = INDEX_NONE;
        if (!EnumType)
        {
            return false;
        }

        const FString Token = Literal.TrimStartAndEnd();
        if (Token.IsEmpty())
        {
            return false;
        }

        const int32 NumEnums = EnumType->NumEnums();
        for (int32 Index = 0; Index < NumEnums; ++Index)
        {
            const FString InternalName = EnumType->GetNameStringByIndex(Index);
            if (InternalName.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase))
            {
                continue;
            }

            const FString ShortInternalName = StripEnumScope(InternalName);
            const FString FullName = EnumType->GetNameByIndex(Index).ToString();
            const FString AuthoredName = EnumType->GetAuthoredNameStringByIndex(Index);
            const FString DisplayName = EnumType->GetDisplayNameTextByIndex(Index).ToString();
            if (Token.Equals(InternalName, ESearchCase::IgnoreCase) ||
                Token.Equals(ShortInternalName, ESearchCase::IgnoreCase) ||
                Token.Equals(FullName, ESearchCase::IgnoreCase) ||
                Token.Equals(StripEnumScope(FullName), ESearchCase::IgnoreCase) ||
                Token.Equals(AuthoredName, ESearchCase::IgnoreCase) ||
                Token.Equals(DisplayName, ESearchCase::IgnoreCase))
            {
                OutValue = EnumType->GetValueByIndex(Index);
                return true;
            }
        }

        return false;
    }

    UEdGraph* FindMacroGraphByName(UBlueprint* Blueprint, const FString& GraphName)
    {
        if (!Blueprint)
        {
            return nullptr;
        }

        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    UK2Node_Tunnel* FindMacroTunnel(UEdGraph* Graph, const bool bEntry)
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

    UEdGraphPin* FindPinByNameDirection(
        UEdGraphNode* Node,
        const TCHAR* PinName,
        const EEdGraphPinDirection Direction)
    {
        if (!Node)
        {
            return nullptr;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinName == FName(PinName) && Pin->Direction == Direction)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    bool HasExecPin(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return false;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                return true;
            }
        }
        return false;
    }

    const FHandlerRegistration* FindRegistration(const FString& MethodName)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == MethodName)
            {
                return &Reg;
            }
        }
        return nullptr;
    }

    bool HasParam(const FHandlerRegistration* Reg, const TCHAR* ParamName)
    {
        if (!Reg)
        {
            return false;
        }

        for (const FParamSpec& Param : Reg->Params)
        {
            if (Param.Name == ParamName)
            {
                return true;
            }
        }
        return false;
    }

} // namespace

// ============================================================================
// BlueprintCompileHandler — blueprint.compile
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCompileWithPathTest,
    "PinWright.blueprint.compile.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCompileWithPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Blueprints/TestBP"));
    TestTrue(TEXT("blueprint.compile handler found"), InvokeHandler(TEXT("blueprint.compile"), Payload));
    return true;
}

// blueprint.compile is compile-only: persistence (and its integrity gate) moved
// to asset.save (E-blueprint-compile-compile-only). Regression-guard the removal:
// the live ParamSpec must carry `path` but NOT `saveAfterCompile`. Re-adding the
// param (re-coupling persistence to compile) fails this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCompileNoSaveParamTest,
    "PinWright.blueprint.compile.NoSaveAfterCompileParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCompileNoSaveParamTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Reg = FindRegistration(TEXT("blueprint.compile"));
    TestNotNull(TEXT("blueprint.compile registered"), Reg);
    TestTrue(TEXT("blueprint.compile keeps required 'path' param"),
        HasParam(Reg, TEXT("path")));
    TestFalse(TEXT("blueprint.compile no longer carries 'saveAfterCompile' param"),
        HasParam(Reg, TEXT("saveAfterCompile")));
    return true;
}

// ============================================================================
// BlueprintCreationHandler — blueprint.create
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCreateValidParamsNoCrashTest,
    "PinWright.blueprint.create.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCreateValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestBlueprint_Unit"));
    Payload->SetStringField(TEXT("savePath"), TEXT("/Game/UnitTest"));
    Payload->SetStringField(TEXT("blueprintType"), TEXT("actor"));
    TestTrue(TEXT("blueprint.create handler found"),
        InvokeHandler(TEXT("blueprint.create"), Payload));
    CleanupTestAsset(TEXT("/Game/UnitTest/TestBlueprint_Unit"));
    return true;
}

// ============================================================================
// BlueprintEventHandler — blueprint.add_event
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddEventValidParamsNoCrashTest,
    "PinWright.blueprint.add_event.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddEventValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Blueprints/TestBP"));
    Payload->SetStringField(TEXT("eventType"), TEXT("custom"));
    Payload->SetStringField(TEXT("customEventName"), TEXT("OnTestFired"));
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);
    TestTrue(TEXT("blueprint.add_event handler found"),
        InvokeHandler(TEXT("blueprint.add_event"), Payload));
    return true;
}

// ============================================================================
// BlueprintEventHandler — blueprint.add_event with parameters
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddEventWithParamsTest,
    "PinWright.blueprint.add_event.ParameterPinCreated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddEventWithParamsTest::RunTest(const FString& Parameters)
{
    // 1. Create a test blueprint
    const FString AssetPath = MakeUniqueAssetPath(TEXT("AddEventParams"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 2. Build payload with a bool parameter
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventType"), TEXT("custom"));
    Payload->SetStringField(TEXT("customEventName"), TEXT("TestEventWithParam"));
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);

    TArray<TSharedPtr<FJsonValue>> ParamsArray;
    TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
    ParamObj->SetStringField(TEXT("name"), TEXT("TestBoolParam"));
    ParamObj->SetStringField(TEXT("type"), TEXT("bool"));
    ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    Payload->SetArrayField(TEXT("parameters"), ParamsArray);

    // 3. Invoke handler
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.add_event"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    // 4. Verify the node has the parameter pin
    if (BP)
    {
        UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
        TestNotNull(TEXT("Event graph exists"), EventGraph);
        if (EventGraph)
        {
            bool bFoundParamPin = false;
            for (UEdGraphNode* Node : EventGraph->Nodes)
            {
                UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node);
                if (CustomEvent && CustomEvent->CustomFunctionName == FName(TEXT("TestEventWithParam")))
                {
                    for (UEdGraphPin* Pin : CustomEvent->Pins)
                    {
                        if (Pin->PinName == TEXT("TestBoolParam"))
                        {
                            bFoundParamPin = true;
                            TestEqual(TEXT("Pin is boolean type"),
                                Pin->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
                            break;
                        }
                    }
                    break;
                }
            }
            TestTrue(TEXT("Parameter pin exists on custom event node"), bFoundParamPin);
        }
    }

    // 5. Cleanup
    CleanupAsset(AssetPath);
    return true;
}

// ============================================================================
// BlueprintFunctionHandler — blueprint.add_function
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddMacroPureSignatureTest,
    "PinWright.blueprint.add_macro.PureSignatureCreatesTunnelPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddMacroPureSignatureTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("AddMacroPureSignature"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Inputs;
    TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
    Input->SetStringField(TEXT("name"), TEXT("Value"));
    Input->SetStringField(TEXT("type"), TEXT("float"));
    Inputs.Add(MakeShared<FJsonValueObject>(Input));

    TArray<TSharedPtr<FJsonValue>> Outputs;
    TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
    Output->SetStringField(TEXT("name"), TEXT("Result"));
    Output->SetStringField(TEXT("type"), TEXT("bool"));
    Outputs.Add(MakeShared<FJsonValueObject>(Output));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());
    Payload->SetStringField(TEXT("macroName"), TEXT("PureMacro"));
    Payload->SetArrayField(TEXT("inputs"), Inputs);
    Payload->SetArrayField(TEXT("outputs"), Outputs);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.add_macro"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    FString GraphName;
    FString EntryNodeId;
    FString ExitNodeId;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("Response has graphName"),
            Capture.Result->TryGetStringField(TEXT("graphName"), GraphName));
        TestTrue(TEXT("Response has entryNodeId"),
            Capture.Result->TryGetStringField(TEXT("entryNodeId"), EntryNodeId));
        TestTrue(TEXT("Response has exitNodeId"),
            Capture.Result->TryGetStringField(TEXT("exitNodeId"), ExitNodeId));
    }

    int32 MatchingMacroGraphs = 0;
    for (UEdGraph* Graph : BP->MacroGraphs)
    {
        if (Graph && Graph->GetName().Equals(TEXT("PureMacro"), ESearchCase::IgnoreCase))
        {
            ++MatchingMacroGraphs;
        }
    }
    TestEqual(TEXT("One MacroGraphs entry named PureMacro"), MatchingMacroGraphs, 1);

    UEdGraph* MacroGraph = FindMacroGraphByName(BP, TEXT("PureMacro"));
    UK2Node_Tunnel* EntryTunnel = FindMacroTunnel(MacroGraph, true);
    UK2Node_Tunnel* ExitTunnel = FindMacroTunnel(MacroGraph, false);
    TestNotNull(TEXT("Entry tunnel exists"), EntryTunnel);
    TestNotNull(TEXT("Exit tunnel exists"), ExitTunnel);

    if (EntryTunnel)
    {
        TestEqual(TEXT("entryNodeId matches entry tunnel"),
            EntryNodeId, EntryTunnel->NodeGuid.ToString());
        UEdGraphPin* ValuePin = FindPinByNameDirection(EntryTunnel, TEXT("Value"), EGPD_Output);
        TestTrue(TEXT("Entry tunnel has output data pin Value"),
            ValuePin && ValuePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec);
        TestFalse(TEXT("Pure macro entry has no exec pins"), HasExecPin(EntryTunnel));
    }

    if (ExitTunnel)
    {
        TestEqual(TEXT("exitNodeId matches exit tunnel"),
            ExitNodeId, ExitTunnel->NodeGuid.ToString());
        UEdGraphPin* ResultPin = FindPinByNameDirection(ExitTunnel, TEXT("Result"), EGPD_Input);
        TestTrue(TEXT("Exit tunnel has input data pin Result"),
            ResultPin && ResultPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec);
        TestFalse(TEXT("Pure macro exit has no exec pins"), HasExecPin(ExitTunnel));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddMacroMultiExitSignatureTest,
    "PinWright.blueprint.add_macro.MultiExitCreatesExecPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddMacroMultiExitSignatureTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("AddMacroMultiExitSignature"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> ExecExits;
    ExecExits.Add(MakeShared<FJsonValueString>(FString(TEXT("Completed"))));
    ExecExits.Add(MakeShared<FJsonValueString>(FString(TEXT("Failed"))));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());
    Payload->SetStringField(TEXT("name"), TEXT("MultiExitMacro"));
    Payload->SetArrayField(TEXT("execExits"), ExecExits);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.add_macro"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    UEdGraph* MacroGraph = FindMacroGraphByName(BP, TEXT("MultiExitMacro"));
    UK2Node_Tunnel* EntryTunnel = FindMacroTunnel(MacroGraph, true);
    UK2Node_Tunnel* ExitTunnel = FindMacroTunnel(MacroGraph, false);
    TestNotNull(TEXT("Entry tunnel exists"), EntryTunnel);
    TestNotNull(TEXT("Exit tunnel exists"), ExitTunnel);

    UEdGraphPin* EntryExec = FindPinByNameDirection(EntryTunnel, TEXT("execute"), EGPD_Output);
    TestTrue(TEXT("Entry tunnel has execute exec output"),
        EntryExec && EntryExec->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);

    UEdGraphPin* Completed = FindPinByNameDirection(ExitTunnel, TEXT("Completed"), EGPD_Input);
    TestTrue(TEXT("Exit tunnel has Completed exec input"),
        Completed && Completed->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);

    UEdGraphPin* Failed = FindPinByNameDirection(ExitTunnel, TEXT("Failed"), EGPD_Input);
    TestTrue(TEXT("Exit tunnel has Failed exec input"),
        Failed && Failed->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddMacroRejectsInvalidExecExitsTest,
    "PinWright.blueprint.add_macro.RejectsInvalidExecExits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddMacroRejectsInvalidExecExitsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("AddMacroInvalidExecExits"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    struct FInvalidExecExitCase
    {
        const TCHAR* MacroName;
        TSharedPtr<FJsonValue> Value;
    };

    const FInvalidExecExitCase Cases[] =
    {
        { TEXT("InvalidExecExitNonString"), MakeShared<FJsonValueNumber>(42.0) },
        { TEXT("InvalidExecExitEmptyString"), MakeShared<FJsonValueString>(FString()) },
    };

    for (const FInvalidExecExitCase& TestCase : Cases)
    {
        TArray<TSharedPtr<FJsonValue>> ExecExits;
        ExecExits.Add(TestCase.Value);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BP->GetPathName());
        Payload->SetStringField(TEXT("macroName"), TestCase.MacroName);
        Payload->SetArrayField(TEXT("execExits"), ExecExits);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.add_macro"), Payload, Capture);
        TestTrue(FString::Printf(TEXT("%s handler found"), TestCase.MacroName), bFound);
        TestTrue(FString::Printf(TEXT("%s handler responded"), TestCase.MacroName), Capture.bWasCalled);
        TestFalse(FString::Printf(TEXT("%s rejects invalid execExits"), TestCase.MacroName), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s error code"), TestCase.MacroName),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(FString::Printf(TEXT("%s macro graph was not created"), TestCase.MacroName),
            FindMacroGraphByName(BP, TestCase.MacroName) == nullptr);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddMacroGraphNameScopeCreateNodeTest,
    "PinWright.blueprint.add_macro.GraphNameScopesCreateNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddMacroGraphNameScopeCreateNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("AddMacroGraphNameScope"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    FString GraphName;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("macroName"), TEXT("ScopedMacro"));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.add_macro"), Payload, Capture);
        TestTrue(TEXT("add_macro handler found"), bFound);
        if (!TestTrue(TEXT("add_macro succeeded"), Capture.bSuccess))
        {
            return true;
        }
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("graphName"), GraphName);
        }
    }

    if (!TestFalse(TEXT("graphName returned"), GraphName.IsEmpty()))
    {
        return true;
    }

    FString NodeId;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), GraphName);
        Payload->SetStringField(TEXT("nodeType"), TEXT("Branch"));
        Payload->SetNumberField(TEXT("x"), 120.0);
        Payload->SetNumberField(TEXT("y"), 240.0);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, Capture);
        TestTrue(TEXT("create_node handler found"), bFound);
        if (!TestTrue(TEXT("create_node succeeded"), Capture.bSuccess))
        {
            return true;
        }
        if (Capture.Result.IsValid())
        {
            TestTrue(TEXT("Response has nodeId"),
                Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
        }
    }

    UEdGraph* MacroGraph = FindMacroGraphByName(BP, GraphName);
    if (!TestNotNull(TEXT("Macro graph is registered in MacroGraphs"), MacroGraph))
    {
        return true;
    }

    bool bFoundInMacro = false;
    for (UEdGraphNode* Node : MacroGraph->Nodes)
    {
        if (Node && Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
        {
            bFoundInMacro = true;
            break;
        }
    }
    TestTrue(TEXT("create_node placed the node in the macro graph"), bFoundInMacro);

    bool bFoundInEventGraph = false;
    if (BP->UbergraphPages.Num() > 0 && BP->UbergraphPages[0])
    {
        for (UEdGraphNode* Node : BP->UbergraphPages[0]->Nodes)
        {
            if (Node && Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
            {
                bFoundInEventGraph = true;
                break;
            }
        }
    }
    TestFalse(TEXT("create_node did not place the node in EventGraph"), bFoundInEventGraph);

    return true;
}

// ============================================================================
// BlueprintGraphHandler — blueprint.graph.create_node
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphCreateNodeTargetParamCallFunctionTest,
    "PinWright.blueprint.graph.create_node.TargetParamCreatesQualifiedCallFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphCreateNodeTargetParamCallFunctionTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueAssetPath(TEXT("CreateNodeTargetCallFunction"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetStringField(TEXT("nodeType"), TEXT("CallFunction"));
    Payload->SetStringField(TEXT("target"), TEXT("KismetSystemLibrary::PrintString"));
    Payload->SetNumberField(TEXT("x"), 100.0);
    Payload->SetNumberField(TEXT("y"), 200.0);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    FString NodeId;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("Response has nodeId"), Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
    }
    else
    {
        AddError(TEXT("Response result is null"));
    }

    UK2Node_CallFunction* CallNode = nullptr;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node && Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
        {
            CallNode = Cast<UK2Node_CallFunction>(Node);
            break;
        }
    }

    TestNotNull(TEXT("Created node is UK2Node_CallFunction"), CallNode);
    if (CallNode)
    {
        TestEqual(TEXT("Function member is PrintString"),
            CallNode->FunctionReference.GetMemberName(),
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
        UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass(CallNode->GetBlueprintClassFromNode());
        TestTrue(TEXT("Function parent class is UKismetSystemLibrary"),
            ParentClass == UKismetSystemLibrary::StaticClass());
    }

    CleanupAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphCreateNodeLegacyAliasCallFunctionTest,
    "PinWright.blueprint.graph.create_node.LegacyAliasCreatesQualifiedCallFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphCreateNodeLegacyAliasCallFunctionTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueAssetPath(TEXT("CreateNodeLegacyCallFunction"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetStringField(TEXT("nodeType"), TEXT("CallFunction"));
    Payload->SetStringField(TEXT("memberName"), TEXT("PrintString"));
    Payload->SetStringField(TEXT("memberClass"), TEXT("KismetSystemLibrary"));
    Payload->SetNumberField(TEXT("x"), 300.0);
    Payload->SetNumberField(TEXT("y"), 400.0);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    FString NodeId;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("Response has nodeId"), Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
    }
    else
    {
        AddError(TEXT("Response result is null"));
    }

    UK2Node_CallFunction* CallNode = nullptr;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node && Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
        {
            CallNode = Cast<UK2Node_CallFunction>(Node);
            break;
        }
    }

    TestNotNull(TEXT("Created node is UK2Node_CallFunction"), CallNode);
    if (CallNode)
    {
        TestEqual(TEXT("Function member is PrintString"),
            CallNode->FunctionReference.GetMemberName(),
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
        UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass(CallNode->GetBlueprintClassFromNode());
        TestTrue(TEXT("Function parent class is UKismetSystemLibrary"),
            ParentClass == UKismetSystemLibrary::StaticClass());
    }

    CleanupAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphCreateNodeTimelineRegistersTemplateTest,
    "PinWright.blueprint.graph.create_node.TimelineRegistersTemplate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphCreateNodeTimelineRegistersTemplateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("CreateNodeTimelineTemplate"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        return true;
    }

    const int32 InitialTimelineCount = BP->Timelines.Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetStringField(TEXT("nodeType"), TEXT("Timeline"));
    Payload->SetStringField(TEXT("target"), TEXT("IntroFade"));
    Payload->SetNumberField(TEXT("x"), 120.0);
    Payload->SetNumberField(TEXT("y"), 240.0);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.create_node"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    FString NodeId;
    FString TimelineName;
    FString TimelineTemplatePath;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("Response has nodeId"), Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
        TestTrue(TEXT("Response has timelineName"),
            Capture.Result->TryGetStringField(TEXT("timelineName"), TimelineName));
        TestTrue(TEXT("Response has timelineTemplatePath"),
            Capture.Result->TryGetStringField(TEXT("timelineTemplatePath"), TimelineTemplatePath));
        TestFalse(TEXT("timelineTemplatePath is non-empty"), TimelineTemplatePath.IsEmpty());
    }
    else
    {
        AddError(TEXT("Response result is null"));
    }

    UK2Node_Timeline* TimelineNode = nullptr;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node && Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
        {
            TimelineNode = Cast<UK2Node_Timeline>(Node);
            break;
        }
    }

    TestNotNull(TEXT("Created node is UK2Node_Timeline"), TimelineNode);
    if (TimelineNode)
    {
        TestEqual(TEXT("Node TimelineName is IntroFade"),
            TimelineNode->TimelineName, FName(TEXT("IntroFade")));
        TestNotNull(TEXT("Play pin exists"), TimelineNode->FindPin(FName(TEXT("Play"))));
        TestNotNull(TEXT("Stop pin exists"), TimelineNode->FindPin(FName(TEXT("Stop"))));
        TestNotNull(TEXT("Reverse pin exists"), TimelineNode->FindPin(FName(TEXT("Reverse"))));
        TestNotNull(TEXT("Update pin exists"), TimelineNode->FindPin(FName(TEXT("Update"))));
        TestNotNull(TEXT("Finished pin exists"), TimelineNode->FindPin(FName(TEXT("Finished"))));
        TestNotNull(TEXT("Direction pin exists"), TimelineNode->FindPin(FName(TEXT("Direction"))));
    }

    TestEqual(TEXT("Response timelineName is IntroFade"), TimelineName, FString(TEXT("IntroFade")));
    TestEqual(TEXT("Blueprint timeline count grew by one"), BP->Timelines.Num(), InitialTimelineCount + 1);
    UTimelineTemplate* TimelineTemplate =
        BP->FindTimelineTemplateByVariableName(FName(TEXT("IntroFade")));
    TestNotNull(TEXT("Registered timeline template exists"), TimelineTemplate);
    if (TimelineTemplate)
    {
        TestEqual(TEXT("Response template path matches template"),
            TimelineTemplatePath, TimelineTemplate->GetPathName());
    }

    return true;
}

// ============================================================================
// BlueprintGraphHandler — blueprint.graph.get_nodes
// ============================================================================

// Regression: get_nodes must offer a narrowing lever so the canonical
// enumerate-to-pick call can stay inline instead of spilling. PRE-FIX FAILURE:
// namesOnly is silently ignored (every node still drags its full pins/linkedTo
// adjacency) and limit is silently ignored (all nodes returned, no
// totalCount/truncated readback). POST-FIX: namesOnly drops the pins array,
// limit caps the array, totalCount/truncated report the full count, and a
// default call still emits pins (byte-compatible).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphGetNodesProjectionAndLimitTest,
    "PinWright.blueprint.graph.get_nodes.NamesOnlyDropsPinsAndLimitCapsWithTotalCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphGetNodesProjectionAndLimitTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    // Build an EventGraph with three wired PrintString nodes, so every node has
    // pins and at least one node carries a non-empty linkedTo adjacency.
    UBlueprint* BP = CreateTransientTestBP(TEXT("GetNodesProjection"));
    if (!BP || BP->UbergraphPages.Num() == 0)
    {
        AddError(TEXT("Failed to create transient test blueprint"));
        return false;
    }
    UEdGraph* EventGraph = BP->UbergraphPages[0];

    UK2Node_CallFunction* NodeA = SpawnPrintStringCall(EventGraph, 0,   0);
    UK2Node_CallFunction* NodeB = SpawnPrintStringCall(EventGraph, 400, 0);
    UK2Node_CallFunction* NodeC = SpawnPrintStringCall(EventGraph, 800, 0);
    WireThenToExec(NodeA, NodeB);
    WireThenToExec(NodeB, NodeC);

    const int32 FullNodeCount = EventGraph->Nodes.Num();
    if (FullNodeCount < 2)
    {
        AddError(TEXT("Expected at least 2 nodes in the test graph"));
        return false;
    }

    auto CountNodesWithPins = [](const TSharedPtr<FJsonObject>& Result) -> int32
    {
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
        {
            return -1;
        }
        int32 WithPins = 0;
        for (const TSharedPtr<FJsonValue>& NodeVal : *Nodes)
        {
            const TSharedPtr<FJsonObject>* NodeObj = nullptr;
            if (!NodeVal.IsValid() || !NodeVal->TryGetObject(NodeObj) || !NodeObj) continue;
            if ((*NodeObj)->HasField(TEXT("pins"))) ++WithPins;
        }
        return WithPins;
    };

    // --- 1. Default call still emits pins on every node (backward compatible) ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());

        FTestResponseCapture Capture;
        TestTrue(TEXT("default get_nodes handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.graph.get_nodes"), Payload, Capture));
        TestTrue(TEXT("default get_nodes succeeded"), Capture.bSuccess);
        if (!Capture.Result.IsValid()) { AddError(TEXT("default result invalid")); return false; }
        TestEqual(TEXT("default get_nodes emits pins on every node"),
            CountNodesWithPins(Capture.Result), FullNodeCount);

        double TotalCount = 0.0;
        Capture.Result->TryGetNumberField(TEXT("totalNodeCount"), TotalCount);
        TestEqual(TEXT("default totalNodeCount equals full node count"),
            static_cast<int32>(TotalCount), FullNodeCount);
        bool bTruncated = true;
        Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
        TestFalse(TEXT("default response is not truncated"), bTruncated);
    }

    // --- 2. namesOnly=true drops the heavy pins/linkedTo arrays (the lever) ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());
        Payload->SetBoolField(TEXT("namesOnly"), true);

        FTestResponseCapture Capture;
        TestTrue(TEXT("namesOnly get_nodes handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.graph.get_nodes"), Payload, Capture));
        TestTrue(TEXT("namesOnly get_nodes succeeded"), Capture.bSuccess);
        // PRE-FIX: namesOnly ignored → pins present on every node → count == FullNodeCount.
        TestEqual(TEXT("namesOnly omits pins from every node"),
            CountNodesWithPins(Capture.Result), 0);

        // The light identification fields must still be present so the caller can pick.
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("nodes"), Nodes)
            && Nodes && Nodes->Num() > 0)
        {
            const TSharedPtr<FJsonObject>* First = nullptr;
            if ((*Nodes)[0]->TryGetObject(First) && First)
            {
                TestTrue(TEXT("namesOnly keeps nodeId"), (*First)->HasField(TEXT("nodeId")));
                TestTrue(TEXT("namesOnly keeps nodeTitle"), (*First)->HasField(TEXT("nodeTitle")));
            }
        }
    }

    // --- 3. limit caps the returned array and totalCount/truncated report elision ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());
        Payload->SetNumberField(TEXT("limit"), 1.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("limit get_nodes handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.graph.get_nodes"), Payload, Capture));
        TestTrue(TEXT("limit get_nodes succeeded"), Capture.bSuccess);
        if (!Capture.Result.IsValid()) { AddError(TEXT("limit result invalid")); return false; }

        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
        {
            // PRE-FIX: limit ignored → all nodes returned (Num() == FullNodeCount).
            TestEqual(TEXT("limit=1 caps nodes array to 1"), Nodes->Num(), 1);
        }
        else
        {
            AddError(TEXT("'nodes' field missing or not an array"));
        }

        double TotalCount = 0.0;
        Capture.Result->TryGetNumberField(TEXT("totalNodeCount"), TotalCount);
        TestEqual(TEXT("totalNodeCount reports the full untruncated node count"),
            static_cast<int32>(TotalCount), FullNodeCount);
        bool bTruncated = false;
        Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
        TestTrue(TEXT("truncated is true when limit caps the list"), bTruncated);
    }

    return true;
}

// ============================================================================
// BlueprintGraphHandler — blueprint.graph.get_graph_connections (filter params)
//
// These three tests FAIL on a pre-fix handler (where nodeIds/edgeType/maxEdges
// and the truncated/totalMatched response fields do not exist) and PASS after
// the fix lands.
// ============================================================================

// --- Test 1: nodeIds filter returns only edges touching the specified node ---
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphGetGraphConnectionsNodeIdsFilterTest,
    "PinWright.blueprint.graph.get_graph_connections.NodeIdsFilterReturnsBothEdgesTouchingNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphGetGraphConnectionsNodeIdsFilterTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    // Build: EventGraph with three PrintString nodes wired A.then→B.exec→C.exec
    UBlueprint* BP = CreateTransientTestBP(TEXT("ConnFilterNodeIds"));
    if (!BP || BP->UbergraphPages.Num() == 0)
    {
        AddError(TEXT("Failed to create transient test blueprint"));
        return false;
    }
    UEdGraph* EventGraph = BP->UbergraphPages[0];

    UK2Node_CallFunction* NodeA = SpawnPrintStringCall(EventGraph, 0,   0);
    UK2Node_CallFunction* NodeB = SpawnPrintStringCall(EventGraph, 400, 0);
    UK2Node_CallFunction* NodeC = SpawnPrintStringCall(EventGraph, 800, 0);
    WireThenToExec(NodeA, NodeB); // edge 1: A→B
    WireThenToExec(NodeB, NodeC); // edge 2: B→C

    // NodeGuid.ToString() uses the default format, matching what the handler emits
    // as fromNodeId/toNodeId (e.g. "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}").
    const FString NodeBId = NodeB->NodeGuid.ToString();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());
    TArray<TSharedPtr<FJsonValue>> NodeIdsArr;
    NodeIdsArr.Add(MakeShared<FJsonValueString>(NodeBId));
    Payload->SetArrayField(TEXT("nodeIds"), NodeIdsArr);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.get_graph_connections"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    // Pre-fix: nodeIds is silently ignored → returns all edges (A→B and B→C); assertion below
    // still passes in that case, but the second sub-check (no edge from unrelated node) would
    // require a broader graph.  The critical pre-fix failure is in Test 2 and Test 3.
    TestTrue(TEXT("response is success"), Capture.bSuccess);
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("result object is invalid"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
    {
        AddError(TEXT("'connections' field missing or not an array"));
        return false;
    }

    // Both edges touching NodeB must be present: A→B and B→C
    // (fromNodeId == NodeBId  OR  toNodeId == NodeBId for every returned edge)
    bool bFoundAB = false;
    bool bFoundBC = false;
    const FString NodeAId = NodeA->NodeGuid.ToString();
    const FString NodeCId = NodeC->NodeGuid.ToString();
    for (const TSharedPtr<FJsonValue>& EdgeVal : *Connections)
    {
        const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
        if (!EdgeVal.IsValid() || !EdgeVal->TryGetObject(EdgeObj) || !EdgeObj) continue;
        FString From, To;
        (*EdgeObj)->TryGetStringField(TEXT("fromNodeId"), From);
        (*EdgeObj)->TryGetStringField(TEXT("toNodeId"),   To);
        if (From == NodeAId && To == NodeBId) bFoundAB = true;
        if (From == NodeBId && To == NodeCId) bFoundBC = true;
    }
    // Pre-fix: nodeIds is silently ignored, so both edges are still present — bFoundAB and
    // bFoundBC will both be true.  However, the filter semantics are wrong: in a larger graph
    // an edge unrelated to NodeB would also appear.  For the minimal 3-node graph used here
    // the pre-fix behavior accidentally satisfies this check.  The discriminating assertion
    // is in Test 2 (maxEdges + truncated field) which definitively fails pre-fix.
    TestTrue(TEXT("A→B edge present in nodeIds-filtered result"), bFoundAB);
    TestTrue(TEXT("B→C edge present in nodeIds-filtered result"), bFoundBC);
    TestEqual(TEXT("exactly 2 connections returned (only edges touching B)"),
        Connections->Num(), 2);
    return true;
}

// --- Test 2: maxEdges cap + truncated/totalMatched response fields ---
// PRE-FIX FAILURE: the handler does not recognise maxEdges, returns all edges without
// truncated/totalMatched fields → Connections->Num() == 2 (not 1), bTruncated == false,
// totalMatched field absent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphGetGraphConnectionsMaxEdgesTest,
    "PinWright.blueprint.graph.get_graph_connections.MaxEdgesCapsResultAndSetsTruncated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphGetGraphConnectionsMaxEdgesTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    UBlueprint* BP = CreateTransientTestBP(TEXT("ConnFilterMaxEdges"));
    if (!BP || BP->UbergraphPages.Num() == 0)
    {
        AddError(TEXT("Failed to create transient test blueprint"));
        return false;
    }
    UEdGraph* EventGraph = BP->UbergraphPages[0];

    UK2Node_CallFunction* NodeA = SpawnPrintStringCall(EventGraph, 0,   0);
    UK2Node_CallFunction* NodeB = SpawnPrintStringCall(EventGraph, 400, 0);
    UK2Node_CallFunction* NodeC = SpawnPrintStringCall(EventGraph, 800, 0);
    WireThenToExec(NodeA, NodeB);
    WireThenToExec(NodeB, NodeC);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());
    Payload->SetNumberField(TEXT("maxEdges"), 1.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.get_graph_connections"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("response is success"), Capture.bSuccess);
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("result object is invalid"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
    {
        AddError(TEXT("'connections' field missing or not an array"));
        return false;
    }

    // maxEdges=1 must cap the returned array to 1 entry
    TestEqual(TEXT("connections capped to 1 by maxEdges"), Connections->Num(), 1);

    // truncated must be true (pre-fix: field absent → GetBoolField returns false)
    bool bTruncated = false;
    Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
    TestTrue(TEXT("truncated flag is true when maxEdges is hit"), bTruncated);

    // totalMatched must reflect the full count (≥2) before the cap was applied
    double TotalMatched = 0.0;
    Capture.Result->TryGetNumberField(TEXT("totalMatched"), TotalMatched);
    TestTrue(TEXT("totalMatched >= 2 (both exec edges counted)"),
        static_cast<int32>(TotalMatched) >= 2);
    return true;
}

// --- Test 3: edgeType="data" excludes exec edges ---
// PRE-FIX FAILURE: edgeType is silently ignored → exec edges are returned;
// the graph has no data wires so the filtered result must be empty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphGetGraphConnectionsEdgeTypeDataFilterTest,
    "PinWright.blueprint.graph.get_graph_connections.EdgeTypeDataFilterExcludesExecEdges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphGetGraphConnectionsEdgeTypeDataFilterTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    UBlueprint* BP = CreateTransientTestBP(TEXT("ConnFilterEdgeType"));
    if (!BP || BP->UbergraphPages.Num() == 0)
    {
        AddError(TEXT("Failed to create transient test blueprint"));
        return false;
    }
    UEdGraph* EventGraph = BP->UbergraphPages[0];

    UK2Node_CallFunction* NodeA = SpawnPrintStringCall(EventGraph, 0,   0);
    UK2Node_CallFunction* NodeB = SpawnPrintStringCall(EventGraph, 400, 0);
    UK2Node_CallFunction* NodeC = SpawnPrintStringCall(EventGraph, 800, 0);
    WireThenToExec(NodeA, NodeB);
    WireThenToExec(NodeB, NodeC);
    // No data pins are connected, so filtering to "data" must yield an empty connections array.

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());
    Payload->SetStringField(TEXT("edgeType"), TEXT("data"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.get_graph_connections"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("response is success"), Capture.bSuccess);
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("result object is invalid"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
    {
        AddError(TEXT("'connections' field missing or not an array"));
        return false;
    }

    // Only data edges are requested; this graph has none → must be empty.
    // Pre-fix: edgeType silently ignored → 2 exec edges returned → Num() == 2 → FAIL.
    TestEqual(TEXT("data-only filter yields 0 connections for exec-only graph"),
        Connections->Num(), 0);
    return true;
}

// ============================================================================
// BlueprintGraphHandler — blueprint.graph.find_nodes
// ============================================================================

// ----------------------------------------------------------------------------
// Regression test: find_nodes without graphName must search ALL graph
// collections (UbergraphPages, FunctionGraphs, MacroGraphs,
// DelegateSignatureGraphs) and tag each match with graphName / graphKind.
//
// Pre-fix behaviour: only EventGraph was searched -> matchCount == 0 for a
// node that lived in FunctionGraphs; "scope" and "graphsSearched" fields were
// absent from the response.
// Post-fix behaviour: matchCount >= 1, scope == "all", and the first match
// carries graphKind == "function" and graphName == "TestFunction".
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphFindNodesAllGraphsScopeTest,
    "PinWright.blueprint.graph.find_nodes.AllGraphsScopeFindsInFunctionGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphFindNodesAllGraphsScopeTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    // 1. Create a transient Blueprint (EventGraph will be empty).
    UBlueprint* Blueprint = CreateTransientTestBP(TEXT("FindNodesAllScope"));
    TestNotNull(TEXT("Blueprint created"), Blueprint);
    if (!Blueprint)
    {
        return true;
    }

    // 2. Create a new function graph and register it on the Blueprint.
    const FName FunctionGraphName = TEXT("TestFunction");
    UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, FunctionGraphName,
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    TestNotNull(TEXT("FuncGraph created"), FuncGraph);
    if (!FuncGraph)
    {
        return true;
    }
    Blueprint->FunctionGraphs.Add(FuncGraph);

    // 3. Spawn a PrintString call node inside the function graph.
    UK2Node_CallFunction* PrintNode = SpawnPrintStringCall(FuncGraph, 0, 0);
    TestNotNull(TEXT("PrintString node spawned"), PrintNode);
    if (!PrintNode)
    {
        return true;
    }

    // 4. Dispatch find_nodes with NO graphName -- should search all collections.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Payload->SetStringField(TEXT("query"), TEXT("PrintString"));
    // Note: graphName is intentionally omitted to trigger the all-graphs path.

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.graph.find_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("Response result is null"));
        return true;
    }

    // 5a. Pre-fix: matchCount was 0 (only EventGraph searched, which was empty).
    //     Post-fix: matchCount >= 1.
    const int32 MatchCount = static_cast<int32>(
        Capture.Result->GetNumberField(TEXT("matchCount")));
    TestTrue(TEXT("matchCount >= 1 (node found in FunctionGraphs)"), MatchCount >= 1);

    // 5b. Pre-fix: "scope" field was absent.
    //     Post-fix: scope == "all".
    FString Scope;
    const bool bHasScope = Capture.Result->TryGetStringField(TEXT("scope"), Scope);
    TestTrue(TEXT("Response has 'scope' field"), bHasScope);
    TestEqual(TEXT("scope is 'all'"), Scope, FString(TEXT("all")));

    // 5c. Pre-fix: "graphsSearched" field was absent.
    //     Post-fix: graphsSearched >= 1.
    const int32 GraphsSearched = static_cast<int32>(
        Capture.Result->GetNumberField(TEXT("graphsSearched")));
    TestTrue(TEXT("graphsSearched >= 1"), GraphsSearched >= 1);

    // 5d. Inspect first match for graphKind == "function" and graphName == "TestFunction".
    const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("matches"), Matches) && Matches && Matches->Num() > 0)
    {
        const TSharedPtr<FJsonObject>* FirstMatchPtr = nullptr;
        if ((*Matches)[0].IsValid() && (*Matches)[0]->TryGetObject(FirstMatchPtr) && FirstMatchPtr)
        {
            const TSharedPtr<FJsonObject>& FirstMatch = *FirstMatchPtr;

            // Pre-fix: graphKind was absent.  Post-fix: "function".
            FString GraphKind;
            const bool bHasGraphKind = FirstMatch->TryGetStringField(TEXT("graphKind"), GraphKind);
            TestTrue(TEXT("Match has 'graphKind' field"), bHasGraphKind);
            TestEqual(TEXT("graphKind is 'function'"), GraphKind, FString(TEXT("function")));

            // Pre-fix: graphName was absent.  Post-fix: "TestFunction".
            FString GraphNameResult;
            const bool bHasGraphName = FirstMatch->TryGetStringField(TEXT("graphName"), GraphNameResult);
            TestTrue(TEXT("Match has 'graphName' field"), bHasGraphName);
            TestEqual(TEXT("graphName matches function graph"),
                GraphNameResult, FunctionGraphName.ToString());
        }
        else
        {
            AddError(TEXT("First match entry is not a valid JSON object"));
        }
    }
    else if (MatchCount >= 1)
    {
        AddError(TEXT("matchCount >= 1 but matches array is empty or absent"));
    }

    return true;
}

// ----------------------------------------------------------------------------
// Companion test: when graphName IS provided, scope must be "single".
// This guards the existing single-graph path against regression.
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphFindNodesSingleGraphScopeTest,
    "PinWright.blueprint.graph.find_nodes.SingleGraphScopeWhenGraphNameProvided",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphFindNodesSingleGraphScopeTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    // 1. Create a transient Blueprint with a known EventGraph.
    UBlueprint* Blueprint = CreateTransientTestBP(TEXT("FindNodesSingleScope"));
    TestNotNull(TEXT("Blueprint created"), Blueprint);
    if (!Blueprint)
    {
        return true;
    }

    TestTrue(TEXT("Blueprint has at least one UbergraphPage"),
        Blueprint->UbergraphPages.Num() > 0);
    if (Blueprint->UbergraphPages.Num() == 0)
    {
        return true;
    }
    UEdGraph* EventGraph = Blueprint->UbergraphPages[0];

    // 2. Spawn a PrintString node directly in the EventGraph.
    UK2Node_CallFunction* PrintNode = SpawnPrintStringCall(EventGraph, 0, 0);
    TestNotNull(TEXT("PrintString node spawned in EventGraph"), PrintNode);
    if (!PrintNode)
    {
        return true;
    }

    // 3. Dispatch find_nodes WITH graphName set to the EventGraph name.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Payload->SetStringField(TEXT("query"), TEXT("PrintString"));
    Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.graph.find_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("Response result is null"));
        return true;
    }

    // 4a. Node is in the EventGraph, so matchCount >= 1.
    const int32 MatchCount = static_cast<int32>(
        Capture.Result->GetNumberField(TEXT("matchCount")));
    TestTrue(TEXT("matchCount >= 1"), MatchCount >= 1);

    // 4b. scope must be "single" when graphName was provided.
    FString Scope;
    const bool bHasScope = Capture.Result->TryGetStringField(TEXT("scope"), Scope);
    TestTrue(TEXT("Response has 'scope' field"), bHasScope);
    TestEqual(TEXT("scope is 'single'"), Scope, FString(TEXT("single")));

    return true;
}

// ============================================================================
// BlueprintGraphHandler — blueprint.graph.set_pin_default_value
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphSetPinDefaultValueEnumAppliesRequestedLiteralTest,
    "PinWright.blueprint.graph.set_pin_default_value.EnumAppliesRequestedLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphSetPinDefaultValueEnumAppliesRequestedLiteralTest::RunTest(const FString& Parameters)
{
    const FString EnumPath = MakeUniqueAssetPath(TEXT("E_PinDefault"));
    const FString BlueprintPath = MakeUniqueAssetPath(TEXT("BP_PinDefault"));
    CleanupAsset(EnumPath);
    CleanupAsset(BlueprintPath);

    TSharedPtr<FJsonObject> CreateEnumPayload = MakeShared<FJsonObject>();
    CreateEnumPayload->SetStringField(TEXT("path"), EnumPath);
    TArray<TSharedPtr<FJsonValue>> EnumEntries;
    EnumEntries.Add(MakeShared<FJsonValueString>(TEXT("Idle")));
    EnumEntries.Add(MakeShared<FJsonValueString>(TEXT("Scanning")));
    CreateEnumPayload->SetArrayField(TEXT("entries"), EnumEntries);
    TestTrue(TEXT("blueprint.create_enum handler found"),
        InvokeHandler(TEXT("blueprint.create_enum"), CreateEnumPayload));

    UUserDefinedEnum* EnumAsset = LoadUserDefinedEnum(EnumPath);
    TestNotNull(TEXT("Created enum is loadable"), EnumAsset);
    if (!EnumAsset)
    {
        CleanupAsset(EnumPath);
        CleanupAsset(BlueprintPath);
        return true;
    }

    const FString BlueprintAssetName = FPackageName::GetLongPackageAssetName(BlueprintPath);
    UPackage* BlueprintPackage = CreatePackage(*BlueprintPath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        BlueprintPackage,
        FName(*BlueprintAssetName),
        EBlueprintType::BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("PinWrightTests")));
    TestNotNull(TEXT("Test blueprint created"), Blueprint);
    if (!Blueprint)
    {
        CleanupAsset(EnumPath);
        CleanupAsset(BlueprintPath);
        return true;
    }

    FEdGraphPinType EnumPinType;
    EnumPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    EnumPinType.PinSubCategoryObject = EnumAsset;
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("CurrentState"), EnumPinType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    UEdGraph* EventGraph = (Blueprint->UbergraphPages.Num() > 0) ? Blueprint->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Blueprint has an event graph"), EventGraph);
    if (!EventGraph)
    {
        CleanupAsset(EnumPath);
        CleanupAsset(BlueprintPath);
        return true;
    }

    UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(EventGraph);
    SetNode->VariableReference.SetSelfMember(TEXT("CurrentState"));
    EventGraph->AddNode(SetNode, true, false);
    SetNode->CreateNewGuid();
    SetNode->PostPlacedNewNode();
    SetNode->AllocateDefaultPins();
    SetNode->ReconstructNode();

    UEdGraphPin* StatePin = SetNode->FindPin(TEXT("CurrentState"));
    TestNotNull(TEXT("Set node contains CurrentState pin"), StatePin);
    if (!StatePin)
    {
        CleanupAsset(EnumPath);
        CleanupAsset(BlueprintPath);
        return true;
    }

    TSharedPtr<FJsonObject> SetDefaultPayload = MakeShared<FJsonObject>();
    SetDefaultPayload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    SetDefaultPayload->SetStringField(TEXT("graphName"), EventGraph->GetName());
    SetDefaultPayload->SetStringField(TEXT("nodeId"), SetNode->NodeGuid.ToString());
    SetDefaultPayload->SetStringField(TEXT("pinName"), TEXT("CurrentState"));
    SetDefaultPayload->SetStringField(TEXT("value"), TEXT("Scanning"));
    TestTrue(TEXT("blueprint.graph.set_pin_default_value handler found"),
        InvokeHandler(TEXT("blueprint.graph.set_pin_default_value"), SetDefaultPayload));

    StatePin = SetNode->FindPin(TEXT("CurrentState"));
    TestNotNull(TEXT("CurrentState pin still valid"), StatePin);
    if (StatePin)
    {
        int64 AppliedValue = INDEX_NONE;
        int64 ExpectedValue = INDEX_NONE;
        const bool bHasAppliedValue = TryResolveEnumLiteralValue(EnumAsset, StatePin->DefaultValue, AppliedValue);
        const bool bHasExpectedValue = TryResolveEnumLiteralValue(EnumAsset, TEXT("Scanning"), ExpectedValue);
        TestTrue(TEXT("Applied pin default resolves to enum value"), bHasAppliedValue);
        TestTrue(TEXT("Expected literal resolves to enum value"), bHasExpectedValue);
        if (bHasAppliedValue && bHasExpectedValue)
        {
            TestEqual(TEXT("Applied enum pin default equals requested enum value"), AppliedValue, ExpectedValue);
        }
    }

    CleanupAsset(BlueprintPath);
    CleanupAsset(EnumPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphSetPinDefaultValueTextRequiresIdentityTest,
    "PinWright.blueprint.graph.set_pin_default_value.TextRequiresIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphSetPinDefaultValueTextRequiresIdentityTest::RunTest(const FString& Parameters)
{
    const FString BlueprintPath = MakeUniqueAssetPath(TEXT("BP_TextPinDefault"));
    CleanupAsset(BlueprintPath);

    const FString BlueprintAssetName = FPackageName::GetLongPackageAssetName(BlueprintPath);
    UPackage* BlueprintPackage = CreatePackage(*BlueprintPath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        BlueprintPackage,
        FName(*BlueprintAssetName),
        EBlueprintType::BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("PinWrightTests")));
    TestNotNull(TEXT("Test blueprint created"), Blueprint);
    if (!Blueprint)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    UEdGraph* EventGraph = (Blueprint->UbergraphPages.Num() > 0) ? Blueprint->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Blueprint has an event graph"), EventGraph);
    if (!EventGraph)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(EventGraph);
    CallNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText),
        UKismetSystemLibrary::StaticClass());
    EventGraph->AddNode(CallNode, true, false);
    CallNode->CreateNewGuid();
    CallNode->PostPlacedNewNode();
    CallNode->AllocateDefaultPins();
    CallNode->ReconstructNode();

    UEdGraphPin* TextPin = CallNode->FindPin(TEXT("InText"));
    TestNotNull(TEXT("PrintText node contains InText pin"), TextPin);
    if (!TextPin)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    auto MakeSetTextPayload = [&]() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
        Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());
        Payload->SetStringField(TEXT("nodeId"), CallNode->NodeGuid.ToString());
        Payload->SetStringField(TEXT("pinName"), TEXT("InText"));
        return Payload;
    };

    TSharedPtr<FJsonObject> PlainPayload = MakeSetTextPayload();
    PlainPayload->SetStringField(TEXT("value"), TEXT("Plain"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.graph.set_pin_default_value handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.set_pin_default_value"), PlainPayload, Capture));
    TestTrue(TEXT("Plain update produced a response"), Capture.bWasCalled);
    TestFalse(TEXT("Plain update failed"), Capture.bSuccess);
    TestEqual(TEXT("Plain update error code"),
        Capture.ErrorCode, FString(TEXT("INVALID_TEXT_LOCALIZATION_IDENTITY")));

    TSharedPtr<FJsonObject> LocalizedPayload = MakeSetTextPayload();
    LocalizedPayload->SetStringField(TEXT("value"), TEXT("NSLOCTEXT(\"Blueprint\", \"TextPin\", \"Hello\")"));
    Capture.Reset();
    TestTrue(TEXT("blueprint.graph.set_pin_default_value handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.set_pin_default_value"), LocalizedPayload, Capture));
    TestTrue(TEXT("Localized update succeeded"), Capture.bSuccess);

    const TOptional<FString> InitialNamespace = FTextInspector::GetNamespace(TextPin->DefaultTextValue);
    const TOptional<FString> InitialKey = FTextInspector::GetKey(TextPin->DefaultTextValue);
    const FString* InitialSource = FTextInspector::GetSourceString(TextPin->DefaultTextValue);
    TestTrue(TEXT("Initial namespace set"), InitialNamespace.IsSet());
    TestTrue(TEXT("Initial key set"), InitialKey.IsSet());
    if (InitialNamespace.IsSet())
    {
        TestEqual(TEXT("Initial namespace value"), InitialNamespace.GetValue(), FString(TEXT("Blueprint")));
    }
    if (InitialKey.IsSet())
    {
        TestEqual(TEXT("Initial key value"), InitialKey.GetValue(), FString(TEXT("TextPin")));
    }
    TestNotNull(TEXT("Initial source string set"), InitialSource);
    if (InitialSource)
    {
        TestEqual(TEXT("Initial source string value"), *InitialSource, FString(TEXT("Hello")));
    }

    TSharedPtr<FJsonObject> ExistingIdentityPayload = MakeSetTextPayload();
    ExistingIdentityPayload->SetStringField(TEXT("value"), TEXT("Goodbye"));
    Capture.Reset();
    TestTrue(TEXT("blueprint.graph.set_pin_default_value handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.set_pin_default_value"), ExistingIdentityPayload, Capture));
    TestTrue(TEXT("Plain update with existing identity succeeded"), Capture.bSuccess);

    const TOptional<FString> UpdatedNamespace = FTextInspector::GetNamespace(TextPin->DefaultTextValue);
    const TOptional<FString> UpdatedKey = FTextInspector::GetKey(TextPin->DefaultTextValue);
    const FString* UpdatedSource = FTextInspector::GetSourceString(TextPin->DefaultTextValue);
    TestTrue(TEXT("Updated namespace set"), UpdatedNamespace.IsSet());
    TestTrue(TEXT("Updated key set"), UpdatedKey.IsSet());
    if (UpdatedNamespace.IsSet())
    {
        TestEqual(TEXT("Updated namespace preserved"), UpdatedNamespace.GetValue(), FString(TEXT("Blueprint")));
    }
    if (UpdatedKey.IsSet())
    {
        TestEqual(TEXT("Updated key preserved"), UpdatedKey.GetValue(), FString(TEXT("TextPin")));
    }
    TestNotNull(TEXT("Updated source string set"), UpdatedSource);
    if (UpdatedSource)
    {
        TestEqual(TEXT("Updated source string value"), *UpdatedSource, FString(TEXT("Goodbye")));
    }

    CleanupAsset(BlueprintPath);
    return true;
}

// ============================================================================
// BlueprintGraphHandler — blueprint.graph.set_pin_default_values (batch)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphSetPinDefaultValuesBatchSuccessTest,
    "PinWright.blueprint.graph.set_pin_default_values.BatchSuccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphSetPinDefaultValuesBatchSuccessTest::RunTest(const FString& Parameters)
{
    const FString BlueprintPath = MakeUniqueAssetPath(TEXT("BP_BatchPin"));
    CleanupAsset(BlueprintPath);

    const FString BlueprintAssetName = FPackageName::GetLongPackageAssetName(BlueprintPath);
    UPackage* BlueprintPackage = CreatePackage(*BlueprintPath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        BlueprintPackage,
        FName(*BlueprintAssetName),
        EBlueprintType::BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("PinWrightTests")));
    TestNotNull(TEXT("Test blueprint created"), Blueprint);
    if (!Blueprint)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    UEdGraph* EventGraph = (Blueprint->UbergraphPages.Num() > 0) ? Blueprint->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Blueprint has an event graph"), EventGraph);
    if (!EventGraph)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    // Create three CallFunction nodes targeting KismetSystemLibrary::PrintString
    // Each has a "InString" input pin we can set default values on.
    TArray<UK2Node_CallFunction*> Nodes;
    TArray<FString> NodeGuids;
    for (int32 i = 0; i < 3; ++i)
    {
        UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(EventGraph);
        CallNode->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
            UKismetSystemLibrary::StaticClass());
        EventGraph->AddNode(CallNode, true, false);
        CallNode->CreateNewGuid();
        CallNode->PostPlacedNewNode();
        CallNode->AllocateDefaultPins();
        CallNode->ReconstructNode();
        Nodes.Add(CallNode);
        NodeGuids.Add(CallNode->NodeGuid.ToString());
    }

    // Build batch payload
    TArray<TSharedPtr<FJsonValue>> Updates;
    TArray<FString> ExpectedValues = { TEXT("Hello"), TEXT("World"), TEXT("Batch") };
    for (int32 i = 0; i < 3; ++i)
    {
        TSharedPtr<FJsonObject> Update = MakeShared<FJsonObject>();
        Update->SetStringField(TEXT("nodeId"), NodeGuids[i]);
        Update->SetStringField(TEXT("pinName"), TEXT("InString"));
        Update->SetStringField(TEXT("value"), ExpectedValues[i]);
        Updates.Add(MakeShared<FJsonValueObject>(Update));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());
    Payload->SetArrayField(TEXT("updates"), Updates);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.graph.set_pin_default_values handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.set_pin_default_values"), Payload, Capture));
    TestTrue(TEXT("Handler was called"), Capture.bWasCalled);
    TestTrue(TEXT("Batch succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        TestEqual(TEXT("totalUpdates is 3"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("totalUpdates"))), 3);
        TestEqual(TEXT("successCount is 3"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("successCount"))), 3);
        TestEqual(TEXT("failureCount is 0"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("failureCount"))), 0);

        const TArray<TSharedPtr<FJsonValue>>* ResultsArr = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("results"), ResultsArr) && ResultsArr)
        {
            TestEqual(TEXT("results array has 3 entries"), ResultsArr->Num(), 3);
            for (int32 i = 0; i < FMath::Min(ResultsArr->Num(), 3); ++i)
            {
                const TSharedPtr<FJsonObject>& ItemObj = (*ResultsArr)[i]->AsObject();
                TestTrue(TEXT("Item reports success"), ItemObj->GetBoolField(TEXT("success")));
            }
        }
    }

    // Verify actual pin state
    for (int32 i = 0; i < 3; ++i)
    {
        UEdGraphPin* Pin = Nodes[i]->FindPin(TEXT("InString"));
        TestNotNull(TEXT("InString pin exists"), Pin);
        if (Pin)
        {
            TestEqual(
                *FString::Printf(TEXT("Pin %d default value matches"), i),
                Pin->DefaultValue, ExpectedValues[i]);
        }
    }

    CleanupAsset(BlueprintPath);
    return true;
}

// Regression: object-reference input pins store their value in Pin->DefaultObject,
// not the string Pin->DefaultValue. The verify branch of ApplyPinDefaultValueCore
// once only inspected DefaultValue/DefaultTextValue, so a successful object-pin write
// (DefaultValue stays "") tripped a false VERIFICATION_FAILED. This test sets an
// object pin to a real object path and asserts the call succeeds and DefaultObject
// resolves to the requested object. Reverting the fix makes it fail with
// VERIFICATION_FAILED.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphSetPinDefaultValueObjectPinVerifiesTest,
    "PinWright.blueprint.graph.set_pin_default_value.ObjectPinVerifies",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphSetPinDefaultValueObjectPinVerifiesTest::RunTest(const FString& Parameters)
{
    // Use an always-loadable engine asset as the object value (same asset other
    // graph tests load) so the pin's DefaultObject resolves deterministically.
    UObject* TargetObject = LoadObject<UBlueprint>(
        nullptr,
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
    TestNotNull(TEXT("Engine object value is loadable"), TargetObject);
    if (!TargetObject)
    {
        return true;
    }
    const FString TargetObjectPath = TargetObject->GetPathName();

    const FString BlueprintPath = MakeUniqueAssetPath(TEXT("BP_ObjectPin"));
    CleanupAsset(BlueprintPath);

    const FString BlueprintAssetName = FPackageName::GetLongPackageAssetName(BlueprintPath);
    UPackage* BlueprintPackage = CreatePackage(*BlueprintPath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        BlueprintPackage,
        FName(*BlueprintAssetName),
        EBlueprintType::BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("PinWrightTests")));
    TestNotNull(TEXT("Test blueprint created"), Blueprint);
    if (!Blueprint)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    // Member variable typed as an object reference (PC_Object). Its VariableSet node
    // exposes an object-reference input pin whose value lives in Pin->DefaultObject.
    FEdGraphPinType ObjectPinType;
    ObjectPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ObjectPinType.PinSubCategoryObject = UObject::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("AssetRef"), ObjectPinType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    UEdGraph* EventGraph = (Blueprint->UbergraphPages.Num() > 0) ? Blueprint->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Blueprint has an event graph"), EventGraph);
    if (!EventGraph)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(EventGraph);
    SetNode->VariableReference.SetSelfMember(TEXT("AssetRef"));
    EventGraph->AddNode(SetNode, true, false);
    SetNode->CreateNewGuid();
    SetNode->PostPlacedNewNode();
    SetNode->AllocateDefaultPins();
    SetNode->ReconstructNode();

    UEdGraphPin* AssetPin = SetNode->FindPin(TEXT("AssetRef"));
    TestNotNull(TEXT("Set node contains AssetRef pin"), AssetPin);
    if (!AssetPin)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }
    TestEqual(TEXT("AssetRef pin is an object pin"),
        AssetPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Object);
    // The defect's precondition: object pins leave the string DefaultValue empty.
    TestTrue(TEXT("Object pin DefaultValue starts empty"), AssetPin->DefaultValue.IsEmpty());

    TSharedPtr<FJsonObject> SetDefaultPayload = MakeShared<FJsonObject>();
    SetDefaultPayload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    SetDefaultPayload->SetStringField(TEXT("graphName"), EventGraph->GetName());
    SetDefaultPayload->SetStringField(TEXT("nodeId"), SetNode->NodeGuid.ToString());
    SetDefaultPayload->SetStringField(TEXT("pinName"), TEXT("AssetRef"));
    SetDefaultPayload->SetStringField(TEXT("value"), TargetObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.graph.set_pin_default_value handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.set_pin_default_value"), SetDefaultPayload, Capture));
    TestTrue(TEXT("Handler was called"), Capture.bWasCalled);
    // Core regression assertion: the write to an object pin must NOT report a false failure.
    TestTrue(TEXT("Object-pin set succeeds (no false VERIFICATION_FAILED)"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        TestEqual(TEXT("Object-pin set did not fail with VERIFICATION_FAILED"),
            Capture.ErrorCode, FString(TEXT("")));
    }

    // The actual production-code edit must have landed in DefaultObject.
    AssetPin = SetNode->FindPin(TEXT("AssetRef"));
    TestNotNull(TEXT("AssetRef pin still valid"), AssetPin);
    if (AssetPin)
    {
        // TestSamePtr was added in UE 5.5; on older versions compare the pointers directly.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        TestSamePtr(TEXT("Pin DefaultObject resolves to requested object"),
            AssetPin->DefaultObject.Get(), TargetObject);
#else
        TestTrue(TEXT("Pin DefaultObject resolves to requested object"),
            AssetPin->DefaultObject.Get() == TargetObject);
#endif
    }

    // The success result should surface the resolved object path.
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ReportedPath;
        TestTrue(TEXT("Result reports defaultObjectPath"),
            Capture.Result->TryGetStringField(TEXT("defaultObjectPath"), ReportedPath));
        TestEqual(TEXT("Reported defaultObjectPath matches requested"),
            ReportedPath, TargetObjectPath);
    }

    CleanupAsset(BlueprintPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphSetPinDefaultValuesBatchPartialFailureTest,
    "PinWright.blueprint.graph.set_pin_default_values.BatchPartialFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphSetPinDefaultValuesBatchPartialFailureTest::RunTest(const FString& Parameters)
{
    const FString BlueprintPath = MakeUniqueAssetPath(TEXT("BP_BatchPinFail"));
    CleanupAsset(BlueprintPath);

    const FString BlueprintAssetName = FPackageName::GetLongPackageAssetName(BlueprintPath);
    UPackage* BlueprintPackage = CreatePackage(*BlueprintPath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        BlueprintPackage,
        FName(*BlueprintAssetName),
        EBlueprintType::BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("PinWrightTests")));
    TestNotNull(TEXT("Test blueprint created"), Blueprint);
    if (!Blueprint)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    UEdGraph* EventGraph = (Blueprint->UbergraphPages.Num() > 0) ? Blueprint->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Blueprint has an event graph"), EventGraph);
    if (!EventGraph)
    {
        CleanupAsset(BlueprintPath);
        return true;
    }

    // Create one valid CallFunction node
    UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(EventGraph);
    CallNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    EventGraph->AddNode(CallNode, true, false);
    CallNode->CreateNewGuid();
    CallNode->PostPlacedNewNode();
    CallNode->AllocateDefaultPins();
    CallNode->ReconstructNode();

    // Build batch with 3 updates: 1 valid, 1 bad nodeId, 1 bad pinName
    TArray<TSharedPtr<FJsonValue>> Updates;

    // Valid update
    TSharedPtr<FJsonObject> ValidUpdate = MakeShared<FJsonObject>();
    ValidUpdate->SetStringField(TEXT("nodeId"), CallNode->NodeGuid.ToString());
    ValidUpdate->SetStringField(TEXT("pinName"), TEXT("InString"));
    ValidUpdate->SetStringField(TEXT("value"), TEXT("ValidValue"));
    Updates.Add(MakeShared<FJsonValueObject>(ValidUpdate));

    // Invalid nodeId
    TSharedPtr<FJsonObject> BadNodeUpdate = MakeShared<FJsonObject>();
    BadNodeUpdate->SetStringField(TEXT("nodeId"), TEXT("NonExistentNodeId_12345"));
    BadNodeUpdate->SetStringField(TEXT("pinName"), TEXT("InString"));
    BadNodeUpdate->SetStringField(TEXT("value"), TEXT("Ignored"));
    Updates.Add(MakeShared<FJsonValueObject>(BadNodeUpdate));

    // Invalid pinName
    TSharedPtr<FJsonObject> BadPinUpdate = MakeShared<FJsonObject>();
    BadPinUpdate->SetStringField(TEXT("nodeId"), CallNode->NodeGuid.ToString());
    BadPinUpdate->SetStringField(TEXT("pinName"), TEXT("NonExistentPin"));
    BadPinUpdate->SetStringField(TEXT("value"), TEXT("Ignored"));
    Updates.Add(MakeShared<FJsonValueObject>(BadPinUpdate));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Payload->SetStringField(TEXT("graphName"), EventGraph->GetName());
    Payload->SetArrayField(TEXT("updates"), Updates);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.graph.set_pin_default_values handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.set_pin_default_values"), Payload, Capture));
    TestTrue(TEXT("Handler was called"), Capture.bWasCalled);
    TestTrue(TEXT("Batch overall succeeded (partial failures are non-fatal)"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        TestEqual(TEXT("totalUpdates is 3"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("totalUpdates"))), 3);
        TestEqual(TEXT("successCount is 1"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("successCount"))), 1);
        TestEqual(TEXT("failureCount is 2"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("failureCount"))), 2);

        const TArray<TSharedPtr<FJsonValue>>* ResultsArr = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("results"), ResultsArr) && ResultsArr)
        {
            TestEqual(TEXT("results array has 3 entries"), ResultsArr->Num(), 3);

            // First item: success
            if (ResultsArr->Num() > 0)
            {
                const TSharedPtr<FJsonObject>& Item0 = (*ResultsArr)[0]->AsObject();
                TestTrue(TEXT("Item 0 succeeded"), Item0->GetBoolField(TEXT("success")));
            }
            // Second item: failure (bad nodeId)
            if (ResultsArr->Num() > 1)
            {
                const TSharedPtr<FJsonObject>& Item1 = (*ResultsArr)[1]->AsObject();
                TestFalse(TEXT("Item 1 failed"), Item1->GetBoolField(TEXT("success")));
                TestEqual(TEXT("Item 1 error is NODE_NOT_FOUND"),
                    Item1->GetStringField(TEXT("error")), FString(TEXT("NODE_NOT_FOUND")));
            }
            // Third item: failure (bad pinName)
            if (ResultsArr->Num() > 2)
            {
                const TSharedPtr<FJsonObject>& Item2 = (*ResultsArr)[2]->AsObject();
                TestFalse(TEXT("Item 2 failed"), Item2->GetBoolField(TEXT("success")));
                TestEqual(TEXT("Item 2 error is PIN_NOT_FOUND"),
                    Item2->GetStringField(TEXT("error")), FString(TEXT("PIN_NOT_FOUND")));
            }
        }
    }

    // Verify the valid update actually applied
    UEdGraphPin* Pin = CallNode->FindPin(TEXT("InString"));
    TestNotNull(TEXT("InString pin exists"), Pin);
    if (Pin)
    {
        TestEqual(TEXT("Valid update applied correctly"), Pin->DefaultValue, FString(TEXT("ValidValue")));
    }

    CleanupAsset(BlueprintPath);
    return true;
}

// ============================================================================
// BlueprintInfoHandler — blueprint.exists
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintExistsValidPathNoCrashTest,
    "PinWright.blueprint.exists.ValidPathNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintExistsValidPathNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Blueprints/TestBP"));
    TestTrue(TEXT("blueprint.exists handler found"),
        InvokeHandler(TEXT("blueprint.exists"), Payload));
    return true;
}

// ============================================================================
// BlueprintPropertyHandler — blueprint.add_variable
// ============================================================================

// Regression for B-add-variable-default-value-ignored: blueprint.add_variable
// documents a defaultValue param but historically read-then-discarded it — the
// new FBPVariableDescription was built by hand and never assigned DefaultValue,
// so after the compile the CDO read back the type's zero value. This drives the
// real registered handler through the dispatcher with defaultValue:"50.0" and
// asserts the compiled CDO actually holds 50.0, plus that the response echoes the
// applied default and carries no warning. Reverting the NewVar.DefaultValue
// assignment makes the CDO read 0.0 and fails the core assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddVariableDefaultValueLandsOnCdoTest,
    "PinWright.blueprint.add_variable.DefaultValueLandsOnCdo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddVariableDefaultValueLandsOnCdoTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("AddVarDefaultValue"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());
    Payload->SetStringField(TEXT("variableName"), TEXT("Health"));
    Payload->SetStringField(TEXT("variableType"), TEXT("float"));
    Payload->SetStringField(TEXT("defaultValue"), TEXT("50.0"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.add_variable"), Payload, Capture);
    TestTrue(TEXT("blueprint.add_variable handler found"), bFound);
    TestTrue(TEXT("add_variable with defaultValue succeeds"), Capture.bSuccess);

    // Core assertion: the supplied default actually landed on the compiled CDO.
    // Pre-fix this read back 0.0 because NewVar.DefaultValue was never assigned.
    if (TestNotNull(TEXT("GeneratedClass exists after compile"), BP->GeneratedClass.Get()))
    {
        UObject* CDO = BP->GeneratedClass->GetDefaultObject();
        if (TestNotNull(TEXT("CDO exists"), CDO))
        {
            FProperty* HealthProp = FindFProperty<FProperty>(CDO->GetClass(), TEXT("Health"));
            if (TestNotNull(TEXT("Health property exists on CDO"), HealthProp))
            {
                FNumericProperty* NumProp = CastField<FNumericProperty>(HealthProp);
                if (TestNotNull(TEXT("Health is a numeric property"), NumProp))
                {
                    const double CdoValue = NumProp->GetFloatingPointPropertyValue(
                        NumProp->ContainerPtrToValuePtr<void>(CDO));
                    TestEqual(TEXT("CDO default for Health is 50.0, not the zero value"),
                        CdoValue, 50.0);
                }
            }
        }
    }

    // The response must echo the applied default in-band and carry no warning,
    // since "50.0" is fully coercible to a float.
    if (TestTrue(TEXT("response object present"), Capture.Result.IsValid()))
    {
        double EchoedDefault = 0.0;
        TestTrue(TEXT("response echoes a defaultValue field"),
            Capture.Result->TryGetNumberField(TEXT("defaultValue"), EchoedDefault));
        TestEqual(TEXT("echoed defaultValue is 50.0"), EchoedDefault, 50.0);
        TestFalse(TEXT("no warning for a coercible default"),
            Capture.Result->HasField(TEXT("warning")));
    }

    return true;
}

// ============================================================================
// Regression: blueprint.get's `defaults` map must surface each member
// variable's CDO default, not the always-empty registry object
// (ticket E-blueprint-get-defaults-always-empty). Pre-fix the snapshot never
// emitted `defaults`, so the handler folded in the registry's always-empty
// `defaults:{}` and a caller could never read back a default through the verb
// that names a `defaults` field. This adds a variable with a non-zero default
// (which lands on the compiled CDO), then asserts blueprint.get's `defaults`
// echoes that value. Reverting BuildBlueprintDefaults fails it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGetDefaultsReflectCdoTest,
    "PinWright.blueprint.get.DefaultsReflectCdo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGetDefaultsReflectCdoTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("BlueprintGetDefaults"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    // Add a float variable with a non-zero default; blueprint.add_variable lands
    // it on the compiled CDO (covered by FBlueprintAddVariableDefaultValueLandsOnCdoTest).
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), BP->GetPathName());
    AddPayload->SetStringField(TEXT("variableName"), TEXT("Brightness"));
    AddPayload->SetStringField(TEXT("variableType"), TEXT("float"));
    AddPayload->SetStringField(TEXT("defaultValue"), TEXT("1.5"));

    FTestResponseCapture AddCapture;
    const bool bAddFound = InvokeHandlerWithCapture(TEXT("blueprint.add_variable"), AddPayload, AddCapture);
    TestTrue(TEXT("blueprint.add_variable handler found"), bAddFound);
    TestTrue(TEXT("add_variable with defaultValue succeeds"), AddCapture.bSuccess);

    // Read it back through the verb that advertises a `defaults` field.
    TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
    GetPayload->SetStringField(TEXT("path"), BP->GetPathName());

    FTestResponseCapture GetCapture;
    const bool bGetFound = InvokeHandlerWithCapture(TEXT("blueprint.get"), GetPayload, GetCapture);
    TestTrue(TEXT("blueprint.get handler found"), bGetFound);
    TestTrue(TEXT("blueprint.get succeeded"), GetCapture.bSuccess);

    if (GetCapture.bSuccess && GetCapture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
        const bool bHasDefaults =
            GetCapture.Result->TryGetObjectField(TEXT("defaults"), DefaultsObj);
        // Core assertion: `defaults` is present AND populated with the variable's
        // CDO value — pre-fix it was the always-empty registry object.
        if (TestTrue(TEXT("blueprint.get response carries a defaults object"), bHasDefaults)
            && DefaultsObj != nullptr && (*DefaultsObj).IsValid())
        {
            double Brightness = 0.0;
            const bool bHasBrightness =
                (*DefaultsObj)->TryGetNumberField(TEXT("Brightness"), Brightness);
            TestTrue(TEXT("defaults map surfaces the Brightness member variable"), bHasBrightness);
            TestEqual(TEXT("defaults.Brightness reflects the 1.5 CDO default, not empty/zero"),
                Brightness, 1.5);
        }
    }

    return true;
}

// ============================================================================
// BlueprintPropertyHandler — blueprint.set_default
// ============================================================================

// Regression for B-blueprint-set-default-not-persisted: blueprint.set_default
// historically wrote the value onto the live CDO, called only the non-structural
// MarkBlueprintAsModified, never compiled, and routed the save through the
// throttled helper whose skip path reports true with the return ignored — so the
// override lived only in RAM (readback from the same live CDO always looked
// correct) and silently reverted on editor restart. The fixed handler mirrors
// the SCS finalize sequence: structural mark + compile, re-apply to the rebuilt
// post-compile CDO, mark-for-save via McpSafeAssetSave, and report `saved`
// in-band. This drives the real handler on a transient test BP with a bool
// variable and locks in that contract: success, saved:true (pre-fix the
// response had no saved field at all), no dirty residue on the transient
// package, and the value surviving a fresh compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetDefaultPersistsThroughCompileTest,
    "PinWright.blueprint.set_default.PersistsThroughCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetDefaultPersistsThroughCompileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("SetDefaultPersist"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    // Seed a bool variable through the real add_variable handler (compiles the
    // BP and lands the variable on the CDO with its zero default, false).
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("path"), BP->GetPathName());
        AddPayload->SetStringField(TEXT("variableName"), TEXT("bPersistFlag"));
        AddPayload->SetStringField(TEXT("variableType"), TEXT("bool"));

        FTestResponseCapture AddCapture;
        TestTrue(TEXT("blueprint.add_variable handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_variable"), AddPayload, AddCapture));
        if (!TestTrue(TEXT("add_variable succeeds"), AddCapture.bSuccess))
        {
            return true;
        }
    }

    // Flip the default through blueprint.set_default.
    FTestResponseCapture Capture;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BP->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), TEXT("bPersistFlag"));
        Payload->SetField(TEXT("value"), MakeShared<FJsonValueBoolean>(true));
        TestTrue(TEXT("blueprint.set_default handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.set_default"), Payload, Capture));
    }

    if (!TestTrue(TEXT("set_default succeeds"), Capture.bSuccess))
    {
        return true;
    }

    // The save verdict must be reported in-band, and it must be the TRUTH. The handler
    // deliberately only marks the package dirty (an immediate SaveLoadedAsset here risks
    // the recursive-FlushRenderingCommands crash), so nothing it does persists the CDO
    // edit. This assertion used to be `saved is true`, sourced from McpSafeAssetSave's
    // constant-true bool — i.e. the response asserted durability on the exact path that
    // never writes, which is the shape B-blueprint-set-default-not-persisted describes.
    // The honest contract is markedForSave:true + saved:false + pendingFlush:true; a
    // caller who needs durability follows up with asset.save.
    if (TestTrue(TEXT("response object present"), Capture.Result.IsValid()))
    {
        bool bSavedReported = true;
        TestTrue(TEXT("response carries a saved field"),
            Capture.Result->TryGetBoolField(TEXT("saved"), bSavedReported));
        TestFalse(TEXT("a mark-dirty-only set_default reports saved:false"), bSavedReported);

        bool bMarkedForSave = false;
        TestTrue(TEXT("response carries markedForSave"),
            Capture.Result->TryGetBoolField(TEXT("markedForSave"), bMarkedForSave));
        TestTrue(TEXT("the package was marked for a later save"), bMarkedForSave);

        bool bPendingFlush = false;
        TestTrue(TEXT("response carries pendingFlush"),
            Capture.Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush));
        TestTrue(TEXT("pendingFlush tells the caller the edit is not durable yet"), bPendingFlush);
    }

    // No dirty residue: the transient package can never be dirtied
    // (UPackage::SetDirtyFlag no-ops on GetTransientPackage), so a dirty flag
    // here would mean the handler wrote through some other, wrong package.
    TestFalse(TEXT("package is not dirty after the call"),
        BP->GetOutermost()->IsDirty());

    // The persistence contract: the value must survive a fresh compile — the
    // handler re-applies the value to the post-compile CDO, so another compile
    // must still read it back as true.
    FKismetEditorUtilities::CompileBlueprint(BP);
    if (TestNotNull(TEXT("GeneratedClass exists after recompile"), BP->GeneratedClass.Get()))
    {
        UObject* CDO = BP->GeneratedClass->GetDefaultObject();
        if (TestNotNull(TEXT("CDO exists after recompile"), CDO))
        {
            FBoolProperty* BoolProp =
                FindFProperty<FBoolProperty>(CDO->GetClass(), TEXT("bPersistFlag"));
            if (TestNotNull(TEXT("bPersistFlag property exists on CDO"), BoolProp))
            {
                TestTrue(TEXT("bPersistFlag default survives a fresh compile"),
                    BoolProp->GetPropertyValue(BoolProp->ContainerPtrToValuePtr<void>(CDO)));
            }
        }
    }

    return true;
}

// ============================================================================
// SCSHandler — blueprint.scs.get
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsGetComponentFilterParamsRegisteredTest,
    "PinWright.blueprint.scs.get.ComponentFilterParamsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintScsGetComponentFilterParamsRegisteredTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* ScsGetReg = FindRegistration(TEXT("blueprint.scs.get"));
    TestNotNull(TEXT("blueprint.scs.get registration exists"), ScsGetReg);
    TestTrue(TEXT("blueprint.scs.get registers nameMatch"),
        HasParam(ScsGetReg, TEXT("nameMatch")));
    TestTrue(TEXT("blueprint.scs.get registers name_match alias"),
        HasParam(ScsGetReg, TEXT("name_match")));
    TestTrue(TEXT("blueprint.scs.get registers componentClass"),
        HasParam(ScsGetReg, TEXT("componentClass")));
    TestTrue(TEXT("blueprint.scs.get registers component_class alias"),
        HasParam(ScsGetReg, TEXT("component_class")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsTextEmitterNestedChildrenTest,
    "PinWright.blueprint.scs_text.NestedChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintScsTextEmitterNestedChildrenTest::RunTest(const FString& Parameters)
{
    auto MakeComponent = [](const TCHAR* Name, const TCHAR* ClassName, const TCHAR* Source,
                            const TCHAR* Parent, const TCHAR* InheritedFrom) -> TSharedPtr<FJsonValue>
    {
        TSharedPtr<FJsonObject> Component = MakeShared<FJsonObject>();
        Component->SetStringField(TEXT("name"), Name);
        Component->SetStringField(TEXT("class"), ClassName);
        Component->SetStringField(TEXT("source"), Source);
        if (Parent)
        {
            Component->SetStringField(TEXT("parent"), Parent);
        }
        if (InheritedFrom)
        {
            Component->SetStringField(TEXT("inheritedFrom"), InheritedFrom);
        }
        return MakeShared<FJsonValueObject>(Component);
    };

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Components;
    Components.Add(MakeComponent(
        TEXT("ChildMesh"),
        TEXT("StaticMeshComponent"),
        TEXT("scs"),
        TEXT("DefaultSceneRoot"),
        nullptr));
    Components.Add(MakeComponent(
        TEXT("DefaultSceneRoot"),
        TEXT("SceneComponent"),
        TEXT("inherited-scs"),
        nullptr,
        TEXT("/Game/BP_Parent.BP_Parent")));
    Components.Add(MakeComponent(
        TEXT("Orphan"),
        TEXT("SceneComponent"),
        TEXT("scs"),
        TEXT("MissingParent"),
        nullptr));
    Root->SetArrayField(TEXT("components"), Components);

    const FString Text = SCSTextEmitter::BuildText(Root);
    TestTrue(TEXT("Text contains inherited root component"),
        Text.Contains(TEXT("component(DefaultSceneRoot) {")));
    TestTrue(TEXT("Text contains child component"),
        Text.Contains(TEXT("component(ChildMesh) {")));
    TestTrue(TEXT("Text nests resolved children"),
        Text.Contains(TEXT("children {\n    component(ChildMesh) {")));
    TestTrue(TEXT("Text preserves inherited source"),
        Text.Contains(TEXT("source: inherited-scs")));
    TestTrue(TEXT("Text preserves inherited origin"),
        Text.Contains(TEXT("inherits: /Game/BP_Parent.BP_Parent")));
    TestTrue(TEXT("Text preserves unresolved parent metadata"),
        Text.Contains(TEXT("parent: MissingParent")));

    const int32 RootIndex = Text.Find(TEXT("component(DefaultSceneRoot) {"));
    const int32 ChildIndex = Text.Find(TEXT("component(ChildMesh) {"));
    TestTrue(TEXT("Resolved parent renders before nested child"),
        RootIndex != INDEX_NONE && ChildIndex != INDEX_NONE && RootIndex < ChildIndex);

    return true;
}

// ============================================================================
// SCSHandler — blueprint.scs.add_component
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsAddComponentValidParamsNoCrashTest,
    "PinWright.blueprint.scs.add_component.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintScsAddComponentValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Blueprints/TestBP"));
    Payload->SetStringField(TEXT("componentClass"), TEXT("StaticMeshComponent"));
    Payload->SetStringField(TEXT("componentName"), TEXT("MyMesh"));
    TestTrue(TEXT("blueprint.scs.add_component handler found"),
        InvokeHandler(TEXT("blueprint.scs.add_component"), Payload));
    return true;
}

// ============================================================================
// SCSHandler — blueprint.scs.set_property (CDO default subobject fallback)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsSetPropertyCdoSubobjectTest,
    "PinWright.blueprint.scs.set_property.CdoDefaultSubobject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintScsSetPropertyCdoSubobjectTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeUniqueAssetPath(TEXT("BP_ScsSetPropCdo"));
    ON_SCOPE_EXIT { CleanupAsset(BPPath); };

    // Create a Character BP — CharacterMovement is a C++ default subobject on
    // ACharacter that is NOT in the SCS, so set_property must fall through to
    // the CDO default subobject lookup.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(BPPath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(BPPath));
        Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Character"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture);
        if (!TestTrue(TEXT("Blueprint created"), Capture.bSuccess)) return true;
    }

    // Set MaxWalkSpeed on CharMoveComp — this is a CDO default subobject.
    // ACharacter creates its CharacterMovement component via
    // CreateDefaultSubobject(ACharacter::CharacterMovementComponentName, ...)
    // where CharacterMovementComponentName == "CharMoveComp" (see Character.cpp),
    // so GetName() returns "CharMoveComp" — not the C++ member "CharacterMovement".
    // The SCS lookup's CDO-fallback branch compares against SubObj->GetName(),
    // so the ComponentName param has to match the subobject's UObject name.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BPPath);
        Payload->SetStringField(TEXT("componentName"), TEXT("CharMoveComp"));
        Payload->SetStringField(TEXT("propertyName"), TEXT("MaxWalkSpeed"));
        Payload->SetField(TEXT("propertyValue"),
            MakeShared<FJsonValueNumber>(450.0));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.scs.set_property"), Payload, Capture);
        TestTrue(TEXT("set_property on CDO subobject succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            FString Source;
            Capture.Result->TryGetStringField(TEXT("source"), Source);
            TestEqual(TEXT("source is default_subobject"), Source,
                FString(TEXT("default_subobject")));
        }
    }

    return true;
}

// ============================================================================
// SCSHandler — blueprint.scs.set_property (local SCS regression)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsSetPropertyLocalScsTest,
    "PinWright.blueprint.scs.set_property.LocalScsComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintScsSetPropertyLocalScsTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeUniqueAssetPath(TEXT("BP_ScsSetPropLocal"));
    ON_SCOPE_EXIT { CleanupAsset(BPPath); };

    // Create blueprint
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(BPPath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(BPPath));
        Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture);
        if (!TestTrue(TEXT("Blueprint created"), Capture.bSuccess)) return true;
    }

    // Add an SCS component
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BPPath);
        Payload->SetStringField(TEXT("componentClass"), TEXT("SceneComponent"));
        Payload->SetStringField(TEXT("componentName"), TEXT("TestScene"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), Payload, Capture);
        if (!TestTrue(TEXT("Component added"), Capture.bSuccess)) return true;
    }

    // Set bVisible on the local SCS component
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BPPath);
        Payload->SetStringField(TEXT("componentName"), TEXT("TestScene"));
        Payload->SetStringField(TEXT("propertyName"), TEXT("bVisible"));
        Payload->SetField(TEXT("propertyValue"),
            MakeShared<FJsonValueBoolean>(false));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.scs.set_property"), Payload, Capture);
        TestTrue(TEXT("set_property on local SCS component succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            FString Source;
            Capture.Result->TryGetStringField(TEXT("source"), Source);
            TestEqual(TEXT("source is local"), Source, FString(TEXT("local")));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsSetPropertyInheritedScsChildOverrideTest,
    "PinWright.blueprint.scs.set_property.InheritedScsChildOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintScsSetPropertyInheritedScsChildOverrideTest::RunTest(const FString& Parameters)
{
    const FString ParentPath = MakeUniqueAssetPath(TEXT("BP_ScsSetPropInheritedParent"));
    const FString ChildPath = MakeUniqueAssetPath(TEXT("BP_ScsSetPropInheritedChild"));
    ON_SCOPE_EXIT
    {
        CleanupAsset(ChildPath);
        CleanupAsset(ParentPath);
    };

    UPackage* ParentPackage = CreatePackage(*ParentPath);
    UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), ParentPackage,
        FName(*FPackageName::GetLongPackageAssetName(ParentPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Parent Blueprint created"), ParentBP))
    {
        return true;
    }
    if (!TestNotNull(TEXT("Parent SCS exists"), ParentBP->SimpleConstructionScript.Get()))
    {
        return true;
    }

    USCS_Node* ParentNode = ParentBP->SimpleConstructionScript->CreateNode(
        USceneComponent::StaticClass(), FName(TEXT("InheritedScene")));
    if (!TestNotNull(TEXT("Parent SCS node created"), ParentNode))
    {
        return true;
    }
    ParentBP->SimpleConstructionScript->AddNode(ParentNode);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ParentBP);
    FKismetEditorUtilities::CompileBlueprint(ParentBP);

    ParentNode = ParentBP->SimpleConstructionScript->FindSCSNode(FName(TEXT("InheritedScene")));
    if (!TestNotNull(TEXT("Parent SCS node still exists after compile"), ParentNode))
    {
        return true;
    }
    UActorComponent* ParentTemplate = ParentNode->ComponentTemplate;
    if (!TestNotNull(TEXT("Parent component template exists"), ParentTemplate))
    {
        return true;
    }
    if (!TestTrue(TEXT("Parent GeneratedClass exists"), ParentBP->GeneratedClass != nullptr))
    {
        return true;
    }

    UPackage* ChildPackage = CreatePackage(*ChildPath);
    UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
        ParentBP->GeneratedClass, ChildPackage,
        FName(*FPackageName::GetLongPackageAssetName(ChildPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Child Blueprint created"), ChildBP))
    {
        return true;
    }
    FKismetEditorUtilities::CompileBlueprint(ChildBP);

    TArray<TSharedPtr<FJsonValue>> Tags;
    Tags.Add(MakeShared<FJsonValueString>(FString(TEXT("ChildOnlyTag"))));

    TSharedPtr<FJsonObject> InvalidPathPayload = MakeShared<FJsonObject>();
    InvalidPathPayload->SetStringField(TEXT("blueprintPath"), ChildPath);
    InvalidPathPayload->SetStringField(TEXT("componentName"), TEXT("InheritedScene"));
    InvalidPathPayload->SetStringField(TEXT("propertyName"), TEXT("ComponentTags.NotAField"));
    InvalidPathPayload->SetArrayField(TEXT("propertyValue"), Tags);

    FTestResponseCapture InvalidPathCapture;
    TestTrue(TEXT("Handler is registered for invalid inherited property path"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.set_property"), InvalidPathPayload, InvalidPathCapture));
    TestFalse(TEXT("Invalid inherited property path fails"), InvalidPathCapture.bSuccess);

    UInheritableComponentHandler* ChildHandlerAfterInvalidPath =
        ChildBP->GetInheritableComponentHandler(false);
    UActorComponent* OverrideAfterInvalidPath = ChildHandlerAfterInvalidPath
        ? ChildHandlerAfterInvalidPath->GetOverridenComponentTemplate(FComponentKey(ParentNode))
        : nullptr;
    TestNull(TEXT("Invalid inherited property path does not create child override"),
        OverrideAfterInvalidPath);

    TSharedPtr<FJsonObject> InvalidValuePayload = MakeShared<FJsonObject>();
    InvalidValuePayload->SetStringField(TEXT("blueprintPath"), ChildPath);
    InvalidValuePayload->SetStringField(TEXT("componentName"), TEXT("InheritedScene"));
    InvalidValuePayload->SetStringField(TEXT("propertyName"), TEXT("ComponentTags"));
    InvalidValuePayload->SetField(TEXT("propertyValue"), MakeShared<FJsonValueNumber>(123.0));

    FTestResponseCapture InvalidValueCapture;
    TestTrue(TEXT("Handler is registered for invalid inherited property value"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.set_property"), InvalidValuePayload, InvalidValueCapture));
    TestFalse(TEXT("Invalid inherited property value fails"), InvalidValueCapture.bSuccess);

    UInheritableComponentHandler* ChildHandlerAfterInvalidValue =
        ChildBP->GetInheritableComponentHandler(false);
    UActorComponent* OverrideAfterInvalidValue = ChildHandlerAfterInvalidValue
        ? ChildHandlerAfterInvalidValue->GetOverridenComponentTemplate(FComponentKey(ParentNode))
        : nullptr;
    TestNull(TEXT("Invalid inherited property value does not create child override"),
        OverrideAfterInvalidValue);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), ChildPath);
    Payload->SetStringField(TEXT("componentName"), TEXT("InheritedScene"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("ComponentTags"));
    Payload->SetArrayField(TEXT("propertyValue"), Tags);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.scs.set_property"), Payload, Capture);
    TestTrue(TEXT("Handler is registered"), bFound);
    TestTrue(TEXT("Handler reports success"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        FString Source;
        Capture.Result->TryGetStringField(TEXT("source"), Source);
        TestEqual(TEXT("source is inherited_scs"), Source, FString(TEXT("inherited_scs")));
    }

    const FName ChildOnlyTag(TEXT("ChildOnlyTag"));
    TestFalse(TEXT("Parent SCS template was not mutated"),
        ParentTemplate->ComponentTags.Contains(ChildOnlyTag));

    UInheritableComponentHandler* ChildHandler = ChildBP->GetInheritableComponentHandler(false);
    TestNotNull(TEXT("Child inheritable component handler exists"), ChildHandler);

    UActorComponent* OverrideTemplate = ChildHandler
        ? ChildHandler->GetOverridenComponentTemplate(FComponentKey(ParentNode))
        : nullptr;
    TestNotNull(TEXT("Child ICH override template exists"), OverrideTemplate);
    TestTrue(TEXT("Child override template has ChildOnlyTag"),
        OverrideTemplate && OverrideTemplate->ComponentTags.Contains(ChildOnlyTag));
    // If the fix is reverted, RPC success is not enough: the child override is absent or lacks this tag.

    return true;
}

// ============================================================================
// BlueprintTypeDefinitionHandler — blueprint.create_enum / set_enum_entries
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCreateEnumValidParamsNoCrashTest,
    "PinWright.blueprint.create_enum.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCreateEnumValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Enums/E_TestGatewayEnum"));
    TArray<TSharedPtr<FJsonValue>> Entries;
    Entries.Add(MakeShared<FJsonValueString>(TEXT("EntryA")));
    Entries.Add(MakeShared<FJsonValueString>(TEXT("EntryB")));
    Payload->SetArrayField(TEXT("entries"), Entries);
    TestTrue(TEXT("blueprint.create_enum handler found"),
        InvokeHandler(TEXT("blueprint.create_enum"), Payload));
    CleanupTestAsset(TEXT("/Game/Enums/E_TestGatewayEnum"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCreateEnumEntriesAppliedTest,
    "PinWright.blueprint.create_enum.EntriesApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCreateEnumEntriesAppliedTest::RunTest(const FString& Parameters)
{
    const FString EnumPath = MakeUniqueAssetPath(TEXT("E_EntriesApplied"));
    CleanupAsset(EnumPath);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), EnumPath);
    TArray<TSharedPtr<FJsonValue>> Entries;
    Entries.Add(MakeShared<FJsonValueString>(TEXT("Idle")));
    Entries.Add(MakeShared<FJsonValueString>(TEXT("Scanning")));
    Entries.Add(MakeShared<FJsonValueString>(TEXT("Complete")));
    Payload->SetArrayField(TEXT("entries"), Entries);

    TestTrue(TEXT("blueprint.create_enum handler found"),
        InvokeHandler(TEXT("blueprint.create_enum"), Payload));

    UUserDefinedEnum* EnumAsset = LoadUserDefinedEnum(EnumPath);
    TestNotNull(TEXT("Created enum is loadable"), EnumAsset);
    if (EnumAsset)
    {
        TestTrue(TEXT("Enum contains Idle"), EnumContainsEntry(EnumAsset, TEXT("Idle")));
        TestTrue(TEXT("Enum contains Scanning"), EnumContainsEntry(EnumAsset, TEXT("Scanning")));
        TestTrue(TEXT("Enum contains Complete"), EnumContainsEntry(EnumAsset, TEXT("Complete")));
    }

    CleanupAsset(EnumPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetEnumEntriesUpdatesAssetTest,
    "PinWright.blueprint.set_enum_entries.UpdatesAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetEnumEntriesUpdatesAssetTest::RunTest(const FString& Parameters)
{
    const FString EnumPath = MakeUniqueAssetPath(TEXT("E_SetEntries"));
    CleanupAsset(EnumPath);

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), EnumPath);
    TArray<TSharedPtr<FJsonValue>> InitialEntries;
    InitialEntries.Add(MakeShared<FJsonValueString>(TEXT("Idle")));
    CreatePayload->SetArrayField(TEXT("entries"), InitialEntries);
    TestTrue(TEXT("blueprint.create_enum handler found"),
        InvokeHandler(TEXT("blueprint.create_enum"), CreatePayload));

    TSharedPtr<FJsonObject> UpdatePayload = MakeShared<FJsonObject>();
    UpdatePayload->SetStringField(TEXT("path"), EnumPath);
    TArray<TSharedPtr<FJsonValue>> UpdatedEntries;
    UpdatedEntries.Add(MakeShared<FJsonValueString>(TEXT("Scanning")));
    UpdatedEntries.Add(MakeShared<FJsonValueString>(TEXT("Complete")));
    UpdatePayload->SetArrayField(TEXT("entries"), UpdatedEntries);
    TestTrue(TEXT("blueprint.set_enum_entries handler found"),
        InvokeHandler(TEXT("blueprint.set_enum_entries"), UpdatePayload));

    UUserDefinedEnum* EnumAsset = LoadUserDefinedEnum(EnumPath);
    TestNotNull(TEXT("Updated enum is loadable"), EnumAsset);
    if (EnumAsset)
    {
        TestFalse(TEXT("Enum no longer contains Idle"), EnumContainsEntry(EnumAsset, TEXT("Idle")));
        TestTrue(TEXT("Enum contains Scanning"), EnumContainsEntry(EnumAsset, TEXT("Scanning")));
        TestTrue(TEXT("Enum contains Complete"), EnumContainsEntry(EnumAsset, TEXT("Complete")));
    }

    CleanupAsset(EnumPath);
    return true;
}

// ============================================================================
// BlueprintTypeDefinitionHandler — blueprint.create_struct / struct field edits
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCreateStructValidParamsNoCrashTest,
    "PinWright.blueprint.create_struct.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCreateStructValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Structs/S_TestGatewayStruct"));
    TestTrue(TEXT("blueprint.create_struct handler found"),
        InvokeHandler(TEXT("blueprint.create_struct"), Payload));
    CleanupTestAsset(TEXT("/Game/Structs/S_TestGatewayStruct"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintListStructFieldsRichUserDefinedStructSchemaTest,
    "PinWright.blueprint.list_struct_fields.RichUserDefinedStructSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintListStructFieldsRichUserDefinedStructSchemaTest::RunTest(const FString& Parameters)
{
    const FString StructPath = MakeUniqueAssetPath(TEXT("S_RichStructFields"));
    CleanupAsset(StructPath);

    auto MakeFieldValue = [](const TCHAR* Name, const TCHAR* Type) -> TSharedPtr<FJsonValue>
    {
        TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
        Field->SetStringField(TEXT("name"), Name);
        Field->SetStringField(TEXT("type"), Type);
        return MakeShared<FJsonValueObject>(Field);
    };

    TArray<TSharedPtr<FJsonValue>> Fields;
    Fields.Add(MakeFieldValue(TEXT("Label"), TEXT("string")));
    Fields.Add(MakeFieldValue(TEXT("HistoryIds"), TEXT("array<int>")));

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), StructPath);
    CreatePayload->SetArrayField(TEXT("fields"), Fields);

    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("blueprint.create_struct handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.create_struct"), CreatePayload, CreateCapture));
    TestTrue(TEXT("blueprint.create_struct responded"), CreateCapture.bWasCalled);
    TestTrue(TEXT("blueprint.create_struct succeeded"), CreateCapture.bSuccess);

    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), StructPath);

    FTestResponseCapture ListCapture;
    TestTrue(TEXT("blueprint.list_struct_fields handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.list_struct_fields"), ListPayload, ListCapture));
    TestTrue(TEXT("blueprint.list_struct_fields responded"), ListCapture.bWasCalled);
    TestTrue(TEXT("blueprint.list_struct_fields succeeded"), ListCapture.bSuccess);
    TestTrue(TEXT("blueprint.list_struct_fields returned result"), ListCapture.Result.IsValid());

    if (ListCapture.Result.IsValid())
    {
        FString StructGuid;
        TestTrue(TEXT("response has non-empty struct guid"),
            ListCapture.Result->TryGetStringField(TEXT("guid"), StructGuid) && !StructGuid.IsEmpty());

        const TArray<TSharedPtr<FJsonValue>>* ResultFields = nullptr;
        TestTrue(TEXT("response has fields array"),
            ListCapture.Result->TryGetArrayField(TEXT("fields"), ResultFields) && ResultFields);
        if (ResultFields)
        {
            TestEqual(TEXT("count matches fields.Num()"),
                static_cast<int32>(ListCapture.Result->GetNumberField(TEXT("count"))),
                ResultFields->Num());
            TestEqual(TEXT("count matches authored fields.Num()"),
                static_cast<int32>(ListCapture.Result->GetNumberField(TEXT("count"))),
                Fields.Num());
            TestEqual(TEXT("field count matches authored fields"), ResultFields->Num(), Fields.Num());

            bool bSawRichField = false;
            bool bSawArrayFieldWithCollapsedType = false;
            for (const TSharedPtr<FJsonValue>& FieldValue : *ResultFields)
            {
                const TSharedPtr<FJsonObject> FieldObj = FieldValue.IsValid() ? FieldValue->AsObject() : nullptr;
                if (!TestTrue(TEXT("field is object"), FieldObj.IsValid()))
                {
                    continue;
                }

                FString Type;
                TestTrue(TEXT("field has collapsed type"), FieldObj->TryGetStringField(TEXT("type"), Type) && !Type.IsEmpty());

                FString ContainerType;
                FieldObj->TryGetStringField(TEXT("containerType"), ContainerType);
                if (ContainerType == TEXT("Array"))
                {
                    bSawArrayFieldWithCollapsedType = Type == TEXT("Array<int>");
                }

                // metaData is only dumped on UE 5.5+ (FStructVariableDescription::MetaData
                // does not exist on 5.3/5.4); every other rich key is engine-agnostic and is
                // still required on all supported engines.
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
                const bool bHasMetaDataKey = FieldObj->HasField(TEXT("metaData"));
#else
                const bool bHasMetaDataKey = true;
#endif
                bSawRichField = bSawRichField
                    || (FieldObj->HasField(TEXT("displayName"))
                        && FieldObj->HasField(TEXT("rawType"))
                        && FieldObj->HasField(TEXT("subType"))
                        && FieldObj->HasField(TEXT("containerType"))
                        && FieldObj->HasField(TEXT("defaultValue"))
                        && FieldObj->HasField(TEXT("currentDefaultValue"))
                        && FieldObj->HasField(TEXT("tooltip"))
                        && FieldObj->HasField(TEXT("flags"))
                        && bHasMetaDataKey);
            }
            TestTrue(TEXT("array field preserves collapsed DescribePinType type"), bSawArrayFieldWithCollapsedType);
            TestTrue(TEXT("at least one field exposes rich dump-parity keys"), bSawRichField);
        }
    }

    CleanupAsset(StructPath);
    return true;
}

// ============================================================================
// BlueprintInspectHandler — blueprint.inspect
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectValidParamsNoCrashTest,
    "PinWright.blueprint.inspect.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintInspectValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NonExistentBP"));
    TestTrue(TEXT("blueprint.inspect found"), InvokeHandler(TEXT("blueprint.inspect"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectReferencesDisabledTest,
    "PinWright.blueprint.inspect.ReferencesDisabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintInspectReferencesDisabledTest::RunTest(const FString& Parameters)
{
    // Test that includeReferences=false is accepted without crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NonExistentBP"));
    Payload->SetBoolField(TEXT("includeReferences"), false);
    TestTrue(TEXT("blueprint.inspect with references disabled"), InvokeHandler(TEXT("blueprint.inspect"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectScriptRefsEnabledTest,
    "PinWright.blueprint.inspect.ScriptRefsEnabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintInspectScriptRefsEnabledTest::RunTest(const FString& Parameters)
{
    // includeScriptRefs=true to test the filter opt-in path
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NonExistentBP"));
    Payload->SetBoolField(TEXT("includeScriptRefs"), true);
    TestTrue(TEXT("blueprint.inspect with script refs"), InvokeHandler(TEXT("blueprint.inspect"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectEmptyAssetPathTest,
    "PinWright.blueprint.inspect.EmptyAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintInspectEmptyAssetPathTest::RunTest(const FString& Parameters)
{
    // Empty string for required param — should get validation error, not crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT(""));
    TestTrue(TEXT("blueprint.inspect empty path"), InvokeHandler(TEXT("blueprint.inspect"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectParamSpecTest,
    "PinWright.blueprint.inspect.ParamSpecRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintInspectParamSpecTest::RunTest(const FString& Parameters)
{
    // Verify the handler is registered with the correct param count
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == TEXT("blueprint.inspect"))
        {
            const int32 ParamCount = Reg.Params.Num();
            TestEqual(TEXT("param count"), ParamCount, 4);
            if (ParamCount < 4)
            {
                return true;
            }
            TestEqual(TEXT("category"), Reg.Category, TEXT("blueprint"));
            TestTrue(TEXT("first param is required"), Reg.Params[0].bRequired);
            TestFalse(TEXT("second param is optional"), Reg.Params[1].bRequired);
            TestEqual(TEXT("fourth param name"), Reg.Params[3].Name, FString(TEXT("includeProperties")));
            TestFalse(TEXT("includeProperties is optional"), Reg.Params[3].bRequired);
            TestEqual(TEXT("includeProperties default is false"), Reg.Params[3].Default, FString(TEXT("false")));
            return true;
        }
    }
    AddError(TEXT("blueprint.inspect not found in registrations"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectIncludePropertiesSparseInheritanceTest,
    "PinWright.blueprint.inspect.IncludePropertiesSparseInheritance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintInspectIncludePropertiesSparseInheritanceTest::RunTest(const FString& Parameters)
{
    const FString ParentPath = MakeUniqueAssetPath(TEXT("BP_InspectPropsParent"));
    const FString ChildPath = MakeUniqueAssetPath(TEXT("BP_InspectPropsChild"));
    ON_SCOPE_EXIT
    {
        CleanupAsset(ChildPath);
        CleanupAsset(ParentPath);
    };

    UPackage* ParentPackage = CreatePackage(*ParentPath);
    UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        ParentPackage,
        FName(*FPackageName::GetLongPackageAssetName(ParentPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Parent blueprint created"), ParentBP))
    {
        return true;
    }

    const FName ParentOnlyPropertyName(TEXT("InitialLifeSpan"));
    const FName ChildOverridePropertyName(TEXT("NetUpdateFrequency"));
    if (!TestTrue(TEXT("Parent-only native property changed"),
        SetBlueprintCdoFloatingProperty(ParentBP, ParentOnlyPropertyName, 7.0)))
    {
        return true;
    }

    UPackage* ChildPackage = CreatePackage(*ChildPath);
    UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
        ParentBP->GeneratedClass,
        ChildPackage,
        FName(*FPackageName::GetLongPackageAssetName(ChildPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Child blueprint created"), ChildBP))
    {
        return true;
    }

    TestTrue(TEXT("Child keeps inherited parent-only value"),
        SetBlueprintCdoFloatingProperty(ChildBP, ParentOnlyPropertyName, 7.0));
    if (!TestTrue(TEXT("Child override native property changed"),
        SetBlueprintCdoFloatingProperty(ChildBP, ChildOverridePropertyName, 42.0)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ChildPath);
    Payload->SetBoolField(TEXT("includeReferences"), false);
    Payload->SetBoolField(TEXT("includeProperties"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.inspect"), Payload, Capture);
    TestTrue(TEXT("blueprint.inspect handler found"), bFound);
    TestTrue(TEXT("blueprint.inspect succeeded"), Capture.bSuccess);
    TestTrue(TEXT("blueprint.inspect returned result"), Capture.Result.IsValid());

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* Properties = nullptr;
    TestTrue(TEXT("properties object present"),
        Capture.Result->TryGetObjectField(TEXT("properties"), Properties) && Properties && Properties->IsValid());
    if (!Properties || !Properties->IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* ChildOverride = nullptr;
    TestTrue(TEXT("changed inherited property emitted"),
        (*Properties)->TryGetObjectField(ChildOverridePropertyName.ToString(), ChildOverride) && ChildOverride && ChildOverride->IsValid());
    if (ChildOverride && ChildOverride->IsValid())
    {
        bool bIsOverriddenLocally = false;
        TestTrue(TEXT("is_overridden_locally present"),
            (*ChildOverride)->TryGetBoolField(TEXT("is_overridden_locally"), bIsOverriddenLocally));
        TestTrue(TEXT("is_overridden_locally true"), bIsOverriddenLocally);

        FString InheritedFrom;
        TestTrue(TEXT("inherited_from present"),
            (*ChildOverride)->TryGetStringField(TEXT("inherited_from"), InheritedFrom));
        TestFalse(TEXT("inherited_from non-empty"), InheritedFrom.IsEmpty());
    }

    TestFalse(TEXT("unchanged inherited parent value omitted"),
        (*Properties)->HasField(ParentOnlyPropertyName.ToString()));
    return true;
}

// ============================================================================
// BlueprintIndexHandler — blueprint.search
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchHandlerExistsTest,
    "PinWright.blueprint.search.HandlerExists",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchHandlerExistsTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("blueprint.search is registered"), IsHandlerRegistered(TEXT("blueprint.search")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchAutoIndexTaskQueueSafeTest,
    "PinWright.blueprint.search.AutoIndexTaskQueueSafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchAutoIndexTaskQueueSafeTest::RunTest(const FString& Parameters)
{
    struct FTaskState
    {
        bool bCompleted = false;
        bool bRanOnGameThread = false;
    };

    const TSharedRef<FTaskState, ESPMode::ThreadSafe> State =
        MakeShared<FTaskState, ESPMode::ThreadSafe>();

    // blueprint.search RPCs execute from this same named-thread queue. The old
    // immediate EnsureCompletion call recursively pumped the queue here and hit
    // TaskGraph's RecursionGuard assertion.
    AsyncTask(ENamedThreads::GameThread, [State]()
    {
        State->bRanOnGameThread = IsInGameThread();

        FStreamSearchOptions Options;
        const TSharedRef<FStreamSearch> TriggerSearch =
            MakeShared<FStreamSearch>(TEXT(""), Options);
        PinWright::BlueprintIndexHandler::StopAndJoinStreamSearch(TriggerSearch);
        State->bCompleted = true;
    });

    FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);

    TestTrue(TEXT("auto-index trigger ran on the game thread"), State->bRanOnGameThread);
    TestTrue(TEXT("auto-index trigger completed without re-entering the task queue"), State->bCompleted);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchBasicQueryTest,
    "PinWright.blueprint.search.BasicQuery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchBasicQueryTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("test"));
    // Skip the project-wide FiB index pump (60s+) and scope the AssetRegistry filter to an
    // empty path. The test only verifies response shape (results array + totalMatches field),
    // not that results are non-empty — a project-wide index build is unnecessary here and is
    // exercised separately by handlers that run in a real workflow context.
    Payload->SetBoolField(TEXT("autoIndex"), false);
    Payload->SetStringField(TEXT("path"), TEXT("/Game/__PinWright_NoSuchPath__"));

    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.search"), Payload, Capture);
    TestTrue(TEXT("blueprint.search handler found"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("Response is success"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("Response has results array"), Capture.Result->HasField(TEXT("results")));
        TestTrue(TEXT("Response has totalMatches field"), Capture.Result->HasField(TEXT("totalMatches")));
    }
    else
    {
        AddError(TEXT("Capture result was null"));
    }

    return true;
}

// ============================================================================
// BlueprintApiIndexHandler — blueprint.build_api_index
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBuildApiIndexNoParamNoCrashTest,
    "PinWright.blueprint.build_api_index.NoParamNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintBuildApiIndexNoParamNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("blueprint.build_api_index found"), InvokeHandler(TEXT("blueprint.build_api_index"), MakeShared<FJsonObject>()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBuildApiIndexWithClassFilterTest,
    "PinWright.blueprint.build_api_index.WithClassFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintBuildApiIndexWithClassFilterTest::RunTest(const FString& Parameters)
{
    // Filter to specific classes to test the filter path
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> FilterClasses;
    FilterClasses.Add(MakeShared<FJsonValueString>(TEXT("AActor")));
    FilterClasses.Add(MakeShared<FJsonValueString>(TEXT("UActorComponent")));
    Payload->SetArrayField(TEXT("classFilter"), FilterClasses);
    TestTrue(TEXT("blueprint.build_api_index with filter"), InvokeHandler(TEXT("blueprint.build_api_index"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBuildApiIndexEmptyFilterTest,
    "PinWright.blueprint.build_api_index.EmptyFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintBuildApiIndexEmptyFilterTest::RunTest(const FString& Parameters)
{
    // Empty filter array — should behave same as no filter
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("classFilter"), TArray<TSharedPtr<FJsonValue>>());
    TestTrue(TEXT("blueprint.build_api_index empty filter"), InvokeHandler(TEXT("blueprint.build_api_index"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBuildApiIndexNonExistentClassFilterTest,
    "PinWright.blueprint.build_api_index.NonExistentClassFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintBuildApiIndexNonExistentClassFilterTest::RunTest(const FString& Parameters)
{
    // Filter to non-existent class — should produce index with 0 classes, not crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> FilterClasses;
    FilterClasses.Add(MakeShared<FJsonValueString>(TEXT("ThisClassDoesNotExist_XYZ")));
    Payload->SetArrayField(TEXT("classFilter"), FilterClasses);
    TestTrue(TEXT("blueprint.build_api_index non-existent class"), InvokeHandler(TEXT("blueprint.build_api_index"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBuildApiIndexParamSpecTest,
    "PinWright.blueprint.build_api_index.ParamSpecRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintBuildApiIndexParamSpecTest::RunTest(const FString& Parameters)
{
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == TEXT("blueprint.build_api_index"))
        {
            TestEqual(TEXT("param count"), Reg.Params.Num(), 1);
            TestFalse(TEXT("classFilter is optional"), Reg.Params[0].bRequired);
            return true;
        }
    }
    AddError(TEXT("blueprint.build_api_index not found in registrations"));
    return true;
}

// ============================================================================
// BlueprintApiIndexHandler — blueprint.search_api
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchApiValidParamsNoCrashTest,
    "PinWright.blueprint.search_api.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchApiValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("get actor location"));
    TestTrue(TEXT("blueprint.search_api found"), InvokeHandler(TEXT("blueprint.search_api"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchApiCustomLimitTest,
    "PinWright.blueprint.search_api.CustomLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchApiCustomLimitTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("spawn actor"));
    Payload->SetNumberField(TEXT("limit"), 10);
    TestTrue(TEXT("blueprint.search_api with limit"), InvokeHandler(TEXT("blueprint.search_api"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchApiZeroLimitTest,
    "PinWright.blueprint.search_api.ZeroLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchApiZeroLimitTest::RunTest(const FString& Parameters)
{
    // Zero limit should be clamped to a positive value, not crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("set material"));
    Payload->SetNumberField(TEXT("limit"), 0);
    TestTrue(TEXT("blueprint.search_api zero limit"), InvokeHandler(TEXT("blueprint.search_api"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchApiNegativeLimitTest,
    "PinWright.blueprint.search_api.NegativeLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchApiNegativeLimitTest::RunTest(const FString& Parameters)
{
    // Negative limit should be clamped, not crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("destroy component"));
    Payload->SetNumberField(TEXT("limit"), -5);
    TestTrue(TEXT("blueprint.search_api negative limit"), InvokeHandler(TEXT("blueprint.search_api"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchApiEmptyQueryTest,
    "PinWright.blueprint.search_api.EmptyQuery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchApiEmptyQueryTest::RunTest(const FString& Parameters)
{
    // Empty query string should get validation error, not crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT(""));
    TestTrue(TEXT("blueprint.search_api empty query"), InvokeHandler(TEXT("blueprint.search_api"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchApiMultiWordQueryTest,
    "PinWright.blueprint.search_api.MultiWordQuery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchApiMultiWordQueryTest::RunTest(const FString& Parameters)
{
    // Multi-word query to exercise tokenization
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("character movement jump velocity"));
    TestTrue(TEXT("blueprint.search_api multi-word"), InvokeHandler(TEXT("blueprint.search_api"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchApiParamSpecTest,
    "PinWright.blueprint.search_api.ParamSpecRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchApiParamSpecTest::RunTest(const FString& Parameters)
{
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == TEXT("blueprint.search_api"))
        {
            TestEqual(TEXT("param count"), Reg.Params.Num(), 2);
            TestTrue(TEXT("query param is required"), Reg.Params[0].bRequired);
            TestFalse(TEXT("limit param is optional"), Reg.Params[1].bRequired);
            return true;
        }
    }
    AddError(TEXT("blueprint.search_api not found in registrations"));
    return true;
}

// ============================================================================
// Response Capture: blueprint.exists with missing required path param
// Demonstrates InvokeHandlerWithCapture() pattern for verifying error shape.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintExistsCaptureErrorTest,
    "PinWright.blueprint.exists.CaptureErrorResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintExistsCaptureErrorTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Omit "path" to trigger the handler's INVALID_BLUEPRINT_PATH error.
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.exists"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error (missing path)"), Capture.bSuccess);
    TestEqual(TEXT("Error code is INVALID_BLUEPRINT_PATH"),
        Capture.ErrorCode, FString(TEXT("INVALID_BLUEPRINT_PATH")));
    return true;
}

// ============================================================================
// BlueprintApiIndexHandler — blueprint.build_api_index (merge + prefix)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBuildApiIndexPrefixedClassFilterTest,
    "PinWright.blueprint.build_api_index.PrefixedClassFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintBuildApiIndexPrefixedClassFilterTest::RunTest(const FString& Parameters)
{
    // "AActor" should now work — prefix stripping maps it to "Actor"
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> FilterClasses;
    FilterClasses.Add(MakeShared<FJsonValueString>(TEXT("AActor")));
    Payload->SetArrayField(TEXT("classFilter"), FilterClasses);
    TestTrue(TEXT("blueprint.build_api_index with AActor prefix"), InvokeHandler(TEXT("blueprint.build_api_index"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBuildApiIndexUPrefixedClassFilterTest,
    "PinWright.blueprint.build_api_index.UPrefixedClassFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintBuildApiIndexUPrefixedClassFilterTest::RunTest(const FString& Parameters)
{
    // "UActorComponent" should map to "ActorComponent"
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> FilterClasses;
    FilterClasses.Add(MakeShared<FJsonValueString>(TEXT("UActorComponent")));
    Payload->SetArrayField(TEXT("classFilter"), FilterClasses);
    TestTrue(TEXT("blueprint.build_api_index with UActorComponent prefix"), InvokeHandler(TEXT("blueprint.build_api_index"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBuildApiIndexMergeNoCrashTest,
    "PinWright.blueprint.build_api_index.MergeNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintBuildApiIndexMergeNoCrashTest::RunTest(const FString& Parameters)
{
    // Two consecutive builds with different filters should merge
    TSharedPtr<FJsonObject> Payload1 = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Filter1;
    Filter1.Add(MakeShared<FJsonValueString>(TEXT("Actor")));
    Payload1->SetArrayField(TEXT("classFilter"), Filter1);
    InvokeHandler(TEXT("blueprint.build_api_index"), Payload1);

    TSharedPtr<FJsonObject> Payload2 = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Filter2;
    Filter2.Add(MakeShared<FJsonValueString>(TEXT("ActorComponent")));
    Payload2->SetArrayField(TEXT("classFilter"), Filter2);
    TestTrue(TEXT("blueprint.build_api_index second call merges"), InvokeHandler(TEXT("blueprint.build_api_index"), Payload2));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSearchApiAfterRebuildNoCrashTest,
    "PinWright.blueprint.search_api.AfterRebuildNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintSearchApiAfterRebuildNoCrashTest::RunTest(const FString& Parameters)
{
    // Build then immediately search — cache invalidation should work
    TSharedPtr<FJsonObject> BuildPayload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Filter;
    Filter.Add(MakeShared<FJsonValueString>(TEXT("Actor")));
    BuildPayload->SetArrayField(TEXT("classFilter"), Filter);
    InvokeHandler(TEXT("blueprint.build_api_index"), BuildPayload);

    TSharedPtr<FJsonObject> SearchPayload = MakeShared<FJsonObject>();
    SearchPayload->SetStringField(TEXT("query"), TEXT("get location"));
    TestTrue(TEXT("blueprint.search_api after rebuild"), InvokeHandler(TEXT("blueprint.search_api"), SearchPayload));
    return true;
}

// ============================================================================
// BlueprintReparentHandler — blueprint.reparent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReparentRegisteredTest,
    "PinWright.blueprint.reparent.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReparentRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("blueprint.reparent is registered"), IsHandlerRegistered(TEXT("blueprint.reparent")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReparentMissingPathTest,
    "PinWright.blueprint.reparent.MissingRequiredPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReparentMissingPathTest::RunTest(const FString& Parameters)
{
    // Omit required "path" — handler must reject without crashing
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("newParentClass"), TEXT("Actor"));
    TestTrue(TEXT("blueprint.reparent handler found"), InvokeHandler(TEXT("blueprint.reparent"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReparentMissingNewParentClassTest,
    "PinWright.blueprint.reparent.MissingRequiredNewParentClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReparentMissingNewParentClassTest::RunTest(const FString& Parameters)
{
    // Omit required "newParentClass" — handler must reject without crashing
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Blueprints/TestBP"));
    TestTrue(TEXT("blueprint.reparent handler found"), InvokeHandler(TEXT("blueprint.reparent"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReparentValidParamsNoCrashTest,
    "PinWright.blueprint.reparent.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReparentValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Blueprints/TestBP"));
    Payload->SetStringField(TEXT("newParentClass"), TEXT("Pawn"));
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);
    TestTrue(TEXT("blueprint.reparent handler found"), InvokeHandler(TEXT("blueprint.reparent"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReparentNotFoundBlueprintTest,
    "PinWright.blueprint.reparent.NotFoundBlueprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReparentNotFoundBlueprintTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/__NonExistent_Reparent_Test_BP"));
    Payload->SetStringField(TEXT("newParentClass"), TEXT("Actor"));
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(TEXT("blueprint.reparent"), Payload, Capture));
    TestTrue(TEXT("should fail for missing BP"), !Capture.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReparentInvalidClassTest,
    "PinWright.blueprint.reparent.InvalidParentClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReparentInvalidClassTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Blueprints/TestBP"));
    Payload->SetStringField(TEXT("newParentClass"), TEXT("CompletelyBogusClassName_XYZ_12345"));
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(TEXT("blueprint.reparent"), Payload, Capture));
    // Should fail — either BP not found or class not found, but no crash
    TestTrue(TEXT("should fail"), !Capture.bSuccess);
    return true;
}

// ============================================================================
// BlueprintFunctionHandler — blueprint.remove_function
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveFunctionWithPathTest,
    "PinWright.blueprint.remove_function.WithValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRemoveFunctionWithPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Blueprints/BP_TestActor"));
    Payload->SetStringField(TEXT("functionName"), TEXT("TestFunction"));
    bSuppressLogErrors = true;
    TestTrue(TEXT("blueprint.remove_function handler is registered"),
        InvokeHandler(TEXT("blueprint.remove_function"), Payload));
    return true;
}

// ============================================================================
// BlueprintVariableCleanupHandler — blueprint.delete_unused_variables
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDeleteUnusedVarsDryRunTest,
    "PinWright.blueprint.delete_unused_variables.DryRunWithPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintDeleteUnusedVarsDryRunTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Blueprints/BP_TestActor"));
    Payload->SetBoolField(TEXT("dryRun"), true);
    bSuppressLogErrors = true;
    TestTrue(TEXT("blueprint.delete_unused_variables handler is registered"),
        InvokeHandler(TEXT("blueprint.delete_unused_variables"), Payload));
    return true;
}

// ============================================================================
// Integration: blueprint.add_function → blueprint.remove_function round-trip
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddRemoveFunctionRoundTripTest,
    "PinWright.blueprint.remove_function.Integration_AddThenRemove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddRemoveFunctionRoundTripTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeUniqueAssetPath(TEXT("BP_RemoveFuncTest"));
    ON_SCOPE_EXIT { CleanupAsset(BPPath); };

    // Create blueprint
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(BPPath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(BPPath));
        Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture);
        if (!TestTrue(TEXT("Blueprint created"), Capture.bSuccess)) return true;
    }

    // Add function
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BPPath);
        Payload->SetStringField(TEXT("functionName"), TEXT("TestFunc"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.add_function"), Payload, Capture);
        if (!TestTrue(TEXT("Function added"), Capture.bSuccess)) return true;
    }

    // Remove function
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BPPath);
        Payload->SetStringField(TEXT("functionName"), TEXT("TestFunc"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.remove_function"), Payload, Capture);
        TestTrue(TEXT("Function removed successfully"), Capture.bSuccess);
    }

    // Verify function is gone (remove again — should be idempotent)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BPPath);
        Payload->SetStringField(TEXT("functionName"), TEXT("TestFunc"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.remove_function"), Payload, Capture);
        TestTrue(TEXT("Remove idempotent — still succeeds"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            FString Note;
            Capture.Result->TryGetStringField(TEXT("note"), Note);
            TestTrue(TEXT("Note indicates function already removed"), !Note.IsEmpty());
        }
    }

    return true;
}

// ============================================================================
// Integration: blueprint.delete_unused_variables round-trip
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDeleteUnusedVarsIntegrationTest,
    "PinWright.blueprint.delete_unused_variables.Integration_CreateAndClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintDeleteUnusedVarsIntegrationTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeUniqueAssetPath(TEXT("BP_UnusedVarTest"));
    ON_SCOPE_EXIT { CleanupAsset(BPPath); };

    // Create blueprint
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(BPPath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(BPPath));
        Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture);
        if (!TestTrue(TEXT("Blueprint created"), Capture.bSuccess)) return true;
    }

    // Add an unused variable
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BPPath);
        Payload->SetStringField(TEXT("variableName"), TEXT("UnusedTestVar"));
        Payload->SetStringField(TEXT("variableType"), TEXT("bool"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.add_variable"), Payload, Capture);
        if (!TestTrue(TEXT("Variable added"), Capture.bSuccess)) return true;
    }

    // Dry run — should find the unused variable
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BPPath);
        Payload->SetBoolField(TEXT("dryRun"), true);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.delete_unused_variables"), Payload, Capture);
        TestTrue(TEXT("Dry run succeeded"), Capture.bSuccess);
        // Unguarded on purpose: the report array must be PRESENT and must NAME the
        // variable this test added. Wrapping these in `if (TryGetArrayField(...))`
        // would let a handler that stopped emitting unusedVariables — the whole point
        // of dryRun — pass with no assertion running at all.
        const TArray<TSharedPtr<FJsonValue>>* UnusedArr = nullptr;
        TestTrue(TEXT("Dry run response carries an unusedVariables array"),
            Capture.Result.IsValid()
            && Capture.Result->TryGetArrayField(TEXT("unusedVariables"), UnusedArr)
            && UnusedArr != nullptr);
        TestTrue(TEXT("unusedVariables names the variable this test added"),
            JsonStringArrayContains(Capture.Result, TEXT("unusedVariables"), TEXT("UnusedTestVar")));
    }

    // Actual delete
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BPPath);
        Payload->SetBoolField(TEXT("dryRun"), false);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.delete_unused_variables"), Payload, Capture);
        TestTrue(TEXT("Delete succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            double Deleted = 0;
            Capture.Result->TryGetNumberField(TEXT("deletedCount"), Deleted);
            TestTrue(TEXT("At least one variable deleted"), Deleted > 0);
        }
    }

    return true;
}

// ============================================================================
// BpirCompilerHandler — blueprint.get_node_connections (data pins)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGetNodeConnectionsDataPinsTest,
    "PinWright.blueprint.get_node_connections.DataPinsIncluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGetNodeConnectionsDataPinsTest::RunTest(const FString& Parameters)
{
    // 1. Create a temporary blueprint
    const FString AssetPath = MakeUniqueAssetPath(TEXT("GetNodeConnsData"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0)
        ? BP->UbergraphPages[0] : nullptr;
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 2. Add a CustomEvent node
    UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
    EventNode->CustomFunctionName = TEXT("TestDataPinEvent");
    EventNode->CreateNewGuid();
    EventNode->PostPlacedNewNode();
    EventNode->AllocateDefaultPins();
    EventGraph->AddNode(EventNode, true, false);

    // 3. Add a PrintString call function node (has both exec and data pins)
    UK2Node_CallFunction* PrintNode = NewObject<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintNode->CreateNewGuid();
    PrintNode->PostPlacedNewNode();
    PrintNode->AllocateDefaultPins();
    EventGraph->AddNode(PrintNode, true, false);
    PrintNode->ReconstructNode();

    // 4. Wire exec: CustomEvent.Then -> PrintString.Execute
    UEdGraphPin* EventThenPin = EventNode->FindPin(
        UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* PrintExecPin = PrintNode->FindPin(
        UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    if (TestNotNull(TEXT("Event Then pin exists"), EventThenPin)
        && TestNotNull(TEXT("Print Execute pin exists"), PrintExecPin))
    {
        EventThenPin->MakeLinkTo(PrintExecPin);
    }

    // 5. Wire data: create a GetName pure node and connect its ReturnValue
    //    to PrintString's InString input pin.
    UK2Node_CallFunction* GetNameNode = NewObject<UK2Node_CallFunction>(EventGraph);
    GetNameNode->FunctionReference.SetExternalMember(
        FName(TEXT("GetName")),
        UObject::StaticClass());
    GetNameNode->CreateNewGuid();
    GetNameNode->PostPlacedNewNode();
    GetNameNode->AllocateDefaultPins();
    EventGraph->AddNode(GetNameNode, true, false);
    GetNameNode->ReconstructNode();

    UEdGraphPin* GetNameReturnPin = GetNameNode->FindPin(
        UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    UEdGraphPin* InStringPin = PrintNode->FindPin(TEXT("InString"), EGPD_Input);

    bool bDataWired = false;
    if (TestNotNull(TEXT("GetName ReturnValue pin exists"), GetNameReturnPin)
        && TestNotNull(TEXT("PrintString InString pin exists"), InStringPin))
    {
        GetNameReturnPin->MakeLinkTo(InStringPin);
        bDataWired = true;
    }

    // 6. Call get_node_connections on PrintNode with default params (data included)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeId"), PrintNode->NodeGuid.ToString());

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("blueprint.get_node_connections"), Payload, Capture);
        TestTrue(TEXT("Handler found"), bFound);
        TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            // Backward compat: execInputs and execOutputs must always be present
            const TArray<TSharedPtr<FJsonValue>>* ExecInputs = nullptr;
            Capture.Result->TryGetArrayField(TEXT("execInputs"), ExecInputs);
            const TArray<TSharedPtr<FJsonValue>>* ExecOutputs = nullptr;
            Capture.Result->TryGetArrayField(TEXT("execOutputs"), ExecOutputs);
            TestNotNull(TEXT("execInputs array present"), ExecInputs);
            TestNotNull(TEXT("execOutputs array present"), ExecOutputs);

            if (ExecInputs)
            {
                TestTrue(TEXT("execInputs has at least one entry"),
                    ExecInputs->Num() > 0);
                if (ExecInputs->Num() > 0)
                {
                    const TSharedPtr<FJsonObject>& Entry =
                        (*ExecInputs)[0]->AsObject();
                    if (TestTrue(TEXT("exec entry has pinCategory"),
                        Entry.IsValid() && Entry->HasField(TEXT("pinCategory"))))
                    {
                        FString Category;
                        Entry->TryGetStringField(TEXT("pinCategory"), Category);
                        TestEqual(TEXT("exec pinCategory is 'exec'"),
                            Category, FString(TEXT("exec")));
                    }
                }
            }

            // Data pins: dataInputs and dataOutputs must be present
            const TArray<TSharedPtr<FJsonValue>>* DataInputs = nullptr;
            Capture.Result->TryGetArrayField(TEXT("dataInputs"), DataInputs);
            const TArray<TSharedPtr<FJsonValue>>* DataOutputs = nullptr;
            Capture.Result->TryGetArrayField(TEXT("dataOutputs"), DataOutputs);
            TestNotNull(TEXT("dataInputs array present"), DataInputs);
            TestNotNull(TEXT("dataOutputs array present"), DataOutputs);

            if (bDataWired && DataInputs)
            {
                TestTrue(TEXT("dataInputs has at least one entry"),
                    DataInputs->Num() > 0);
                if (DataInputs->Num() > 0)
                {
                    const TSharedPtr<FJsonObject>& Entry =
                        (*DataInputs)[0]->AsObject();
                    if (TestTrue(TEXT("data entry has pinCategory"),
                        Entry.IsValid() && Entry->HasField(TEXT("pinCategory"))))
                    {
                        FString PinName;
                        Entry->TryGetStringField(TEXT("pinName"), PinName);
                        TestEqual(TEXT("data input pinName is InString"),
                            PinName, FString(TEXT("InString")));

                        FString Category;
                        Entry->TryGetStringField(TEXT("pinCategory"), Category);
                        TestTrue(TEXT("data pinCategory is not 'exec'"),
                            Category != TEXT("exec"));
                    }
                }
            }
        }
    }

    // 7. Call with includeDataPins=false — verify no data arrays in response
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeId"), PrintNode->NodeGuid.ToString());
        Payload->SetBoolField(TEXT("includeDataPins"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("blueprint.get_node_connections"), Payload, Capture);
        TestTrue(TEXT("Handler found (no data)"), bFound);
        TestTrue(TEXT("Handler succeeded (no data)"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            TestTrue(TEXT("execInputs present when data disabled"),
                Capture.Result->HasField(TEXT("execInputs")));
            TestTrue(TEXT("execOutputs present when data disabled"),
                Capture.Result->HasField(TEXT("execOutputs")));
            TestFalse(TEXT("dataInputs absent when data disabled"),
                Capture.Result->HasField(TEXT("dataInputs")));
            TestFalse(TEXT("dataOutputs absent when data disabled"),
                Capture.Result->HasField(TEXT("dataOutputs")));
        }
    }

    // 8. Cleanup
    CleanupAsset(AssetPath);
    return true;
}

// ============================================================================
// BlueprintEventHandler — blueprint.remove_event (standard UK2Node_Event)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveStandardEventTest,
    "PinWright.blueprint.remove_event.StandardEventRemoved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRemoveStandardEventTest::RunTest(const FString& Parameters)
{
    // 1. Create a test blueprint
    const FString AssetPath = MakeUniqueAssetPath(TEXT("RemoveStdEvent"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 2. Add a ReceiveBeginPlay event node (UK2Node_Event, not CustomEvent)
    UFunction* BeginPlayFunc = AActor::StaticClass()->FindFunctionByName(
        FName(TEXT("ReceiveBeginPlay")));
    if (!TestNotNull(TEXT("ReceiveBeginPlay function found"), BeginPlayFunc))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    FGraphNodeCreator<UK2Node_Event> NodeCreator(*EventGraph);
    UK2Node_Event* EventNode = NodeCreator.CreateNode();
    EventNode->EventReference.SetExternalMember(
        BeginPlayFunc->GetFName(), AActor::StaticClass());
    EventNode->bOverrideFunction = true;
    NodeCreator.Finalize();

    // Verify node was added
    bool bFoundBefore = false;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (UK2Node_Event* Evt = Cast<UK2Node_Event>(Node))
        {
            if (Evt->EventReference.GetMemberName().ToString().Equals(
                    TEXT("ReceiveBeginPlay"), ESearchCase::IgnoreCase))
            {
                bFoundBefore = true;
                break;
            }
        }
    }
    TestTrue(TEXT("ReceiveBeginPlay node exists before removal"), bFoundBefore);

    // 3. Invoke remove_event
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventName"), TEXT("ReceiveBeginPlay"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.remove_event"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    // 4. Verify removedNodeCount >= 1
    if (Capture.Result.IsValid())
    {
        const int32 RemovedCount = static_cast<int32>(
            Capture.Result->GetNumberField(TEXT("removedNodeCount")));
        TestTrue(TEXT("removedNodeCount >= 1"), RemovedCount >= 1);
    }

    // 5. Verify node is gone
    bool bFoundAfter = false;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (UK2Node_Event* Evt = Cast<UK2Node_Event>(Node))
        {
            if (Evt->EventReference.GetMemberName().ToString().Equals(
                    TEXT("ReceiveBeginPlay"), ESearchCase::IgnoreCase))
            {
                bFoundAfter = true;
                break;
            }
        }
    }
    TestFalse(TEXT("ReceiveBeginPlay node removed after removal"), bFoundAfter);

    // 6. Cleanup
    CleanupAsset(AssetPath);
    return true;
}

// ============================================================================
// BlueprintEventHandler — blueprint.remove_event (custom event regression)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveCustomEventTest,
    "PinWright.blueprint.remove_event.CustomEventRemoved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRemoveCustomEventTest::RunTest(const FString& Parameters)
{
    // 1. Create a test blueprint
    const FString AssetPath = MakeUniqueAssetPath(TEXT("RemoveCustomEvt"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 2. Add a custom event node
    UK2Node_CustomEvent* CustomEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
    CustomEvent->CustomFunctionName = TEXT("MyTestCustomEvent");
    CustomEvent->CreateNewGuid();
    CustomEvent->PostPlacedNewNode();
    CustomEvent->AllocateDefaultPins();
    EventGraph->AddNode(CustomEvent, true, false);

    // Verify node was added
    bool bFoundBefore = false;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
        {
            if (CE->CustomFunctionName.ToString().Equals(
                    TEXT("MyTestCustomEvent"), ESearchCase::IgnoreCase))
            {
                bFoundBefore = true;
                break;
            }
        }
    }
    TestTrue(TEXT("Custom event exists before removal"), bFoundBefore);

    // 3. Invoke remove_event
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventName"), TEXT("MyTestCustomEvent"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.remove_event"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    // 4. Verify removedNodeCount >= 1
    if (Capture.Result.IsValid())
    {
        const int32 RemovedCount = static_cast<int32>(
            Capture.Result->GetNumberField(TEXT("removedNodeCount")));
        TestTrue(TEXT("removedNodeCount >= 1"), RemovedCount >= 1);
    }

    // 5. Verify node is gone
    bool bFoundAfter = false;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
        {
            if (CE->CustomFunctionName.ToString().Equals(
                    TEXT("MyTestCustomEvent"), ESearchCase::IgnoreCase))
            {
                bFoundAfter = true;
                break;
            }
        }
    }
    TestFalse(TEXT("Custom event node removed after removal"), bFoundAfter);

    // 6. Cleanup
    CleanupAsset(AssetPath);
    return true;
}

// ============================================================================
// BlueprintEventHandler — blueprint.remove_event (idempotent no-op)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveEventIdempotentTest,
    "PinWright.blueprint.remove_event.IdempotentNonExistent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRemoveEventIdempotentTest::RunTest(const FString& Parameters)
{
    // 1. Create a test blueprint with no events
    const FString AssetPath = MakeUniqueAssetPath(TEXT("RemoveEvtIdemp"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 2. Call remove_event for a non-existent event
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventName"), TEXT("DoesNotExist"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.remove_event"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded (idempotent)"), Capture.bSuccess);

    // 3. Verify the "not present" note and removedNodeCount == 0
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("Response has 'note' field"),
            Capture.Result->HasField(TEXT("note")));
        const int32 RemovedCount = static_cast<int32>(
            Capture.Result->GetNumberField(TEXT("removedNodeCount")));
        TestEqual(TEXT("removedNodeCount is 0"), RemovedCount, 0);
    }

    // 4. Cleanup
    CleanupAsset(AssetPath);
    return true;
}

// ============================================================================
// BlueprintEventHandler — blueprint.remove_event (ComponentBoundEvent, bare name)
//
// Regression test for the pre-fix bug: the ubergraph loop only matched
// UK2Node_ComponentBoundEvent via EventReference.GetMemberName(), which is the
// delegate-signature FName (e.g. "OnClicked__DelegateSignature"), not the user-
// visible delegate property name ("OnClicked").  Callers supply the bare
// DelegatePropertyName or a display-title form — both were silently skipped,
// returning removedNodeCount=0 while the node remained in the graph.
//
// These two tests prove the fix by:
//   BareDelegateName  — eventName="OnClicked"
//   RebuiltTitle      — eventName="OnClicked (TestButton)"
// Both must match the node and produce removedNodeCount >= 1.
// ============================================================================

#if MCP_TEST_HAS_COMPONENT_BOUND_EVENT

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveComponentBoundEventBareName,
    "PinWright.blueprint.remove_event.BareDelegateName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRemoveComponentBoundEventBareName::RunTest(const FString& Parameters)
{
    // 1. Create a transient-package-style BP accessible to LoadBlueprintAsset via FindObject.
    const FString AssetPath = MakeUniqueAssetPath(TEXT("RemoveCompBoundEvt"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 2. Manually plant a UK2Node_ComponentBoundEvent in the ubergraph.
    //    We don't bind a real component — we just set the FName fields that the
    //    matcher inspects so the test is self-contained and fast.
    UK2Node_ComponentBoundEvent* BoundEventNode =
        NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
    BoundEventNode->ComponentPropertyName = FName(TEXT("TestButton"));
    BoundEventNode->DelegatePropertyName  = FName(TEXT("OnClicked"));
    // CustomFunctionName is the mangled internal name the engine generates.
    BoundEventNode->CustomFunctionName    =
        FName(TEXT("BndEvt__TestBP_TestButton_K2Node_ComponentBoundEvent_0_OnClicked"));
    BoundEventNode->CreateNewGuid();
    BoundEventNode->PostPlacedNewNode();
    BoundEventNode->AllocateDefaultPins();
    EventGraph->AddNode(BoundEventNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);

    // Verify the node was planted.
    bool bFoundBefore = false;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (UK2Node_ComponentBoundEvent* CBE = Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            if (CBE->DelegatePropertyName == FName(TEXT("OnClicked")))
            {
                bFoundBefore = true;
                break;
            }
        }
    }
    if (!TestTrue(TEXT("ComponentBoundEvent node planted before removal"), bFoundBefore))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 3. Invoke blueprint.remove_event with the bare DelegatePropertyName.
    //    Pre-fix: the handler only checked EventReference.GetMemberName(), so
    //    "OnClicked" would not match → removedNodeCount=0 and node survived.
    //    Post-fix: DelegatePropertyName is checked and the node is removed.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventName"), TEXT("OnClicked"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.remove_event"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    // 4. Assert removedNodeCount >= 1.
    //    Pre-fix this would be 0 (false-idempotent no-op path).
    if (Capture.Result.IsValid())
    {
        const int32 RemovedCount = static_cast<int32>(
            Capture.Result->GetNumberField(TEXT("removedNodeCount")));
        TestTrue(TEXT("removedNodeCount >= 1 (bare delegate name matched)"),
            RemovedCount >= 1);
    }

    // 5. Assert the node is no longer in the graph.
    bool bFoundAfter = false;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (UK2Node_ComponentBoundEvent* CBE = Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            if (CBE->DelegatePropertyName == FName(TEXT("OnClicked")))
            {
                bFoundAfter = true;
                break;
            }
        }
    }
    TestFalse(TEXT("ComponentBoundEvent node removed from graph"), bFoundAfter);

    // 6. Cleanup.
    CleanupAsset(AssetPath);
    return true;
}

// ============================================================================
// Same regression, display-title form: "OnClicked (TestButton)"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveComponentBoundEventRebuiltTitle,
    "PinWright.blueprint.remove_event.RebuiltDisplayTitle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRemoveComponentBoundEventRebuiltTitle::RunTest(const FString& Parameters)
{
    // 1. Create BP.
    const FString AssetPath = MakeUniqueAssetPath(TEXT("RemoveCompBoundEvt2"));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 2. Plant the same ComponentBoundEvent node.
    UK2Node_ComponentBoundEvent* BoundEventNode =
        NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
    BoundEventNode->ComponentPropertyName = FName(TEXT("TestButton"));
    BoundEventNode->DelegatePropertyName  = FName(TEXT("OnClicked"));
    BoundEventNode->CustomFunctionName    =
        FName(TEXT("BndEvt__TestBP_TestButton_K2Node_ComponentBoundEvent_0_OnClicked"));
    BoundEventNode->CreateNewGuid();
    BoundEventNode->PostPlacedNewNode();
    BoundEventNode->AllocateDefaultPins();
    EventGraph->AddNode(BoundEventNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);

    // Verify planted.
    bool bFoundBefore = false;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            bFoundBefore = true;
            break;
        }
    }
    if (!TestTrue(TEXT("ComponentBoundEvent node planted before removal"), bFoundBefore))
    {
        CleanupAsset(AssetPath);
        return true;
    }

    // 3. Invoke with the rebuilt "DelegateDisplay (ComponentProp)" title form.
    //    The handler rebuilds this from GetTargetDelegateDisplayName() + ComponentPropertyName;
    //    the test passes the equivalent string directly to verify the rebuilt-title branch.
    //    Pre-fix: this form was not checked at all → removedNodeCount=0.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("eventName"), TEXT("OnClicked (TestButton)"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.remove_event"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    // 4. Assert removedNodeCount >= 1.
    if (Capture.Result.IsValid())
    {
        const int32 RemovedCount = static_cast<int32>(
            Capture.Result->GetNumberField(TEXT("removedNodeCount")));
        TestTrue(TEXT("removedNodeCount >= 1 (rebuilt display title matched)"),
            RemovedCount >= 1);
    }

    // 5. Assert node is gone.
    bool bFoundAfter = false;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            bFoundAfter = true;
            break;
        }
    }
    TestFalse(TEXT("ComponentBoundEvent node removed from graph"), bFoundAfter);

    // 6. Cleanup.
    CleanupAsset(AssetPath);
    return true;
}

#endif // MCP_TEST_HAS_COMPONENT_BOUND_EVENT

// ============================================================================
// blueprint.remove_function — macro graph variant
//
// Pre-fix behaviour: FindBlueprintFunctionGraph only walked FunctionGraphs, so
// passing a macro name returned the idempotent "not found" success with a
// "note" field and NO "graphKind" field.  The graph would still be present in
// MacroGraphs, so the bMacroGraphGone assertion would FAIL on pre-fix code.
//
// Post-fix behaviour: FindBlueprintFunctionGraph also walks MacroGraphs.
// The handler removes the graph and returns graphKind: "macro".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveFunctionMacroGraphTest,
    "PinWright.blueprint.remove_function.MacroGraph_RemovedAndGraphKindReturned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRemoveFunctionMacroGraphTest::RunTest(const FString& Parameters)
{
    // 1. Create a transient Blueprint.
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("RemoveMacroTest"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    // 2. Add a macro graph directly so we can test that the remove handler finds
    //    it in MacroGraphs.  Pre-fix: FindBlueprintFunctionGraph never searched
    //    MacroGraphs, so the handler would return an idempotent "not found" note
    //    and leave the graph untouched.
    UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP,
        FName(TEXT("TestMacro")),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());

    if (!TestNotNull(TEXT("Macro graph created"), MacroGraph))
    {
        return true;
    }

    BP->MacroGraphs.Add(MacroGraph);

    // Sanity: confirm the macro is present before removal.
    TestTrue(TEXT("MacroGraphs contains TestMacro before removal"),
        BP->MacroGraphs.Contains(MacroGraph));

    // 3. Dispatch blueprint.remove_function with the macro's name.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());
    Payload->SetStringField(TEXT("functionName"), TEXT("TestMacro"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.remove_function"), Payload, Capture);

    TestTrue(TEXT("Handler is registered"), bFound);

    // 4. Assert success.
    //    Pre-fix: Capture.bSuccess is also true (the idempotent path still
    //    returns success), so this assertion alone does NOT catch the bug.
    TestTrue(TEXT("Handler reports success"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        // 5. Assert graphKind == "macro".
        //    PRE-FIX FAILURE POINT: this field is absent because the handler
        //    never found the graph and took the early "not found" return path.
        FString GraphKind;
        const bool bHasGraphKind =
            Capture.Result->TryGetStringField(TEXT("graphKind"), GraphKind);
        TestTrue(TEXT("Response contains 'graphKind' field"), bHasGraphKind);
        TestEqual(TEXT("graphKind is 'macro'"), GraphKind, FString(TEXT("macro")));

        // A "note" field signals the idempotent early-return (pre-fix behaviour).
        TestFalse(TEXT("Response does NOT have an idempotent 'note' field"),
            Capture.Result->HasField(TEXT("note")));
    }

    // 6. Assert the macro graph is gone from MacroGraphs.
    //    PRE-FIX FAILURE POINT: the graph was never touched, so Contains()
    //    still returns true and this assertion fails.
    const bool bMacroGraphGone = !BP->MacroGraphs.Contains(MacroGraph);
    TestTrue(TEXT("MacroGraphs no longer contains the removed graph"), bMacroGraphGone);

    return true;
}

// ============================================================================
// blueprint.remove_function — function graph variant (regression guard)
//
// Ensures the existing function-removal path was not broken by the macro fix.
// Creates a function graph via blueprint.add_function, removes it via the
// handler, and asserts graphKind == "function".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRemoveFunctionGraphKindFunctionTest,
    "PinWright.blueprint.remove_function.FunctionGraph_GraphKindFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRemoveFunctionGraphKindFunctionTest::RunTest(const FString& Parameters)
{
    // 1. Create a transient Blueprint.
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("RemoveFuncKindTest"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    // 2. Add a function graph via the add_function handler (standard path).
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("path"), BP->GetPathName());
        AddPayload->SetStringField(TEXT("functionName"), TEXT("TestFunc"));

        FTestResponseCapture AddCapture;
        InvokeHandlerWithCapture(TEXT("blueprint.add_function"), AddPayload, AddCapture);
        if (!TestTrue(TEXT("Function added successfully"), AddCapture.bSuccess))
        {
            return true;
        }
    }

    // Verify the function graph was created in FunctionGraphs.
    UEdGraph* FuncGraph = nullptr;
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (G && G->GetName().Equals(TEXT("TestFunc"), ESearchCase::IgnoreCase))
        {
            FuncGraph = G;
            break;
        }
    }
    if (!TestNotNull(TEXT("FunctionGraphs contains TestFunc"), FuncGraph))
    {
        return true;
    }

    // 3. Dispatch blueprint.remove_function with the function's name.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());
    Payload->SetStringField(TEXT("functionName"), TEXT("TestFunc"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.remove_function"), Payload, Capture);

    TestTrue(TEXT("Handler is registered"), bFound);

    // 4. Assert success.
    TestTrue(TEXT("Handler reports success"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        // 5. Assert graphKind == "function".
        FString GraphKind;
        const bool bHasGraphKind =
            Capture.Result->TryGetStringField(TEXT("graphKind"), GraphKind);
        TestTrue(TEXT("Response contains 'graphKind' field"), bHasGraphKind);
        TestEqual(TEXT("graphKind is 'function'"), GraphKind, FString(TEXT("function")));

        TestFalse(TEXT("Response does NOT have an idempotent 'note' field"),
            Capture.Result->HasField(TEXT("note")));
    }

    // 6. Assert the function graph is gone from FunctionGraphs.
    const bool bFuncGraphGone = !BP->FunctionGraphs.Contains(FuncGraph);
    TestTrue(TEXT("FunctionGraphs no longer contains the removed graph"), bFuncGraphGone);

    return true;
}

// ============================================================================
// BlueprintHandlersList — blueprint.list (short class name must not fire ensure)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintListShortClassNameNoEnsureTest,
    "PinWright.blueprint.list.ShortClassNameNoEnsure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintListShortClassNameNoEnsureTest::RunTest(const FString& Parameters)
{
    // blueprint.list must route short class names through ResolveUClass so the
    // FTopLevelAssetPath ensure never fires; if it did fire, the automation
    // framework would surface it as an error automatically.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
    Filter->SetStringField(TEXT("class"), TEXT("Character"));
    Filter->SetStringField(TEXT("pathStartsWith"), TEXT("/Engine"));
    Payload->SetObjectField(TEXT("filter"), Filter);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(TEXT("blueprint.list"), Payload, Capture);
    TestTrue(TEXT("blueprint.list handler found"), bInvoked);
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("response reports success"), Capture.bSuccess);
    return true;
}

// ============================================================================
// BlueprintTypeDefinitionHandler — F-enum-struct-rich-metadata
// Rich object-shape enum entries, rich struct field optionals, and the two
// post-add struct field edit RPCs (set_struct_field_default,
// set_struct_field_metadata).
// ============================================================================

namespace
{
    // Test-local helper: locates a field object in a list_struct_fields response
    // by friendly name. Returns nullptr if absent.
    TSharedPtr<FJsonObject> FindStructFieldInListResponse(const TSharedPtr<FJsonObject>& ListResult, const FString& FieldName)
    {
        if (!ListResult.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
        if (!ListResult->TryGetArrayField(TEXT("fields"), FieldsArr) || !FieldsArr)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& V : *FieldsArr)
        {
            const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
            if (!Obj.IsValid())
            {
                continue;
            }
            FString Name;
            FString DisplayName;
            Obj->TryGetStringField(TEXT("name"), Name);
            Obj->TryGetStringField(TEXT("displayName"), DisplayName);
            if (Name.Equals(FieldName, ESearchCase::IgnoreCase) ||
                DisplayName.Equals(FieldName, ESearchCase::IgnoreCase))
            {
                return Obj;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetEnumEntriesRichEntriesRoundTripTest,
    "PinWright.blueprint.set_enum_entries.RichEntriesRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetEnumEntriesRichEntriesRoundTripTest::RunTest(const FString& Parameters)
{
    const FString EnumPath = MakeUniqueAssetPath(TEXT("E_RichEnum"));
    CleanupAsset(EnumPath);
    ON_SCOPE_EXIT { CleanupAsset(EnumPath); };

    // Build entries: first with explicit displayName/tooltip, second with hidden=true.
    auto MakeObjectEntry = [](const TCHAR* Name, const TCHAR* DisplayName, const TCHAR* Tooltip, TOptional<bool> Hidden) -> TSharedPtr<FJsonValue>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        if (DisplayName)
        {
            Obj->SetStringField(TEXT("displayName"), DisplayName);
        }
        if (Tooltip)
        {
            Obj->SetStringField(TEXT("tooltip"), Tooltip);
        }
        if (Hidden.IsSet())
        {
            Obj->SetBoolField(TEXT("hidden"), Hidden.GetValue());
        }
        return MakeShared<FJsonValueObject>(Obj);
    };

    TArray<TSharedPtr<FJsonValue>> Entries;
    Entries.Add(MakeObjectEntry(TEXT("Idle"), TEXT("Idle State"), TEXT("Waiting for input"), {}));
    Entries.Add(MakeObjectEntry(TEXT("Hidden"), nullptr, nullptr, TOptional<bool>(true)));

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), EnumPath);
    CreatePayload->SetArrayField(TEXT("entries"), Entries);

    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("blueprint.create_enum handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.create_enum"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_enum succeeded"), CreateCapture.bSuccess);

    UUserDefinedEnum* EnumAsset = LoadUserDefinedEnum(EnumPath);
    if (!TestNotNull(TEXT("Enum is loadable"), EnumAsset))
    {
        return true;
    }

    TestEqual(TEXT("Idle display name matches"),
        EnumAsset->GetDisplayNameTextByIndex(0).ToString(), FString(TEXT("Idle State")));
    TestEqual(TEXT("Idle tooltip metadata stored"),
        EnumAsset->GetMetaData(TEXT("ToolTip"), 0), FString(TEXT("Waiting for input")));
    TestTrue(TEXT("Hidden flag stored on index 1"),
        EnumAsset->HasMetaData(TEXT("Hidden"), 1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetEnumEntriesStringFormBackCompatTest,
    "PinWright.blueprint.set_enum_entries.StringFormBackCompat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetEnumEntriesStringFormBackCompatTest::RunTest(const FString& Parameters)
{
    const FString EnumPath = MakeUniqueAssetPath(TEXT("E_LegacyStrings"));
    CleanupAsset(EnumPath);
    ON_SCOPE_EXIT { CleanupAsset(EnumPath); };

    TArray<TSharedPtr<FJsonValue>> Entries;
    Entries.Add(MakeShared<FJsonValueString>(TEXT("Idle")));
    Entries.Add(MakeShared<FJsonValueString>(TEXT("Hidden")));

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), EnumPath);
    CreatePayload->SetArrayField(TEXT("entries"), Entries);

    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("blueprint.create_enum handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.create_enum"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_enum succeeded"), CreateCapture.bSuccess);

    UUserDefinedEnum* EnumAsset = LoadUserDefinedEnum(EnumPath);
    if (!TestNotNull(TEXT("Enum is loadable"), EnumAsset))
    {
        return true;
    }

    // Tooltip optional unset → metadata absent. Hidden optional unset → key absent.
    TestEqual(TEXT("No tooltip set for legacy string entry"),
        EnumAsset->GetMetaData(TEXT("ToolTip"), 0), FString());
    TestFalse(TEXT("Hidden flag absent on legacy string entry"),
        EnumAsset->HasMetaData(TEXT("Hidden"), 1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddStructFieldRichFieldRoundTripTest,
    "PinWright.blueprint.add_struct_field.RichFieldRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddStructFieldRichFieldRoundTripTest::RunTest(const FString& Parameters)
{
    const FString StructPath = MakeUniqueAssetPath(TEXT("S_RichField"));
    CleanupAsset(StructPath);
    ON_SCOPE_EXIT { CleanupAsset(StructPath); };

    // create_struct with path only -> default seeded field, no fields array.
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), StructPath);
    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("blueprint.create_struct handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.create_struct"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_struct succeeded"), CreateCapture.bSuccess);

    // add_struct_field with full rich payload: defaultValue, tooltip, metadata + flags.
    TSharedPtr<FJsonObject> MetaData = MakeShared<FJsonObject>();
    MetaData->SetStringField(TEXT("ClampMin"), TEXT("0"));
    MetaData->SetStringField(TEXT("ClampMax"), TEXT("100"));
    TSharedPtr<FJsonObject> Flags = MakeShared<FJsonObject>();
    Flags->SetBoolField(TEXT("dontEditOnInstance"), true);
    Flags->SetBoolField(TEXT("enableSaveGame"), true);

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), StructPath);
    AddPayload->SetStringField(TEXT("fieldName"), TEXT("Score"));
    AddPayload->SetStringField(TEXT("fieldType"), TEXT("int"));
    AddPayload->SetStringField(TEXT("defaultValue"), TEXT("42"));
    AddPayload->SetStringField(TEXT("tooltip"), TEXT("Player score"));
    AddPayload->SetObjectField(TEXT("metaData"), MetaData);
    AddPayload->SetObjectField(TEXT("flags"), Flags);

    FTestResponseCapture AddCapture;
    TestTrue(TEXT("blueprint.add_struct_field handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_struct_field"), AddPayload, AddCapture));
    TestTrue(TEXT("add_struct_field succeeded"), AddCapture.bSuccess);

    // Read back via blueprint.list_struct_fields and assert all rich aspects landed.
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), StructPath);
    FTestResponseCapture ListCapture;
    TestTrue(TEXT("blueprint.list_struct_fields handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.list_struct_fields"), ListPayload, ListCapture));
    TestTrue(TEXT("list_struct_fields succeeded"), ListCapture.bSuccess);

    TSharedPtr<FJsonObject> Field = FindStructFieldInListResponse(ListCapture.Result, TEXT("Score"));
    if (!TestTrue(TEXT("Score field present in list response"), Field.IsValid()))
    {
        return true;
    }

    FString DefaultValue;
    Field->TryGetStringField(TEXT("defaultValue"), DefaultValue);
    TestEqual(TEXT("defaultValue is 42"), DefaultValue, FString(TEXT("42")));

    FString Tooltip;
    Field->TryGetStringField(TEXT("tooltip"), Tooltip);
    TestEqual(TEXT("tooltip matches"), Tooltip, FString(TEXT("Player score")));

#if !UE_VERSION_OLDER_THAN(5, 5, 0)
    // Per-field struct metadata storage (FStructVariableDescription::MetaData) and the
    // FStructureEditorUtils::SetMetaData API that writes it both arrived in UE 5.5. On 5.3/5.4
    // the engine has no place to store field metadata, so add_struct_field silently skips it
    // and list_struct_fields omits the metaData object. The defaultValue/tooltip/flags
    // assertions above exercise APIs that exist on every supported engine and still run.
    const TSharedPtr<FJsonObject>* MetaObj = nullptr;
    if (TestTrue(TEXT("metaData object present"),
        Field->TryGetObjectField(TEXT("metaData"), MetaObj) && MetaObj && (*MetaObj).IsValid()))
    {
        FString MetaVal;
        (*MetaObj)->TryGetStringField(TEXT("ClampMin"), MetaVal);
        TestEqual(TEXT("metaData ClampMin"), MetaVal, FString(TEXT("0")));
        (*MetaObj)->TryGetStringField(TEXT("ClampMax"), MetaVal);
        TestEqual(TEXT("metaData ClampMax"), MetaVal, FString(TEXT("100")));
    }
#endif

    const TSharedPtr<FJsonObject>* FlagsObj = nullptr;
    if (TestTrue(TEXT("flags object present"),
        Field->TryGetObjectField(TEXT("flags"), FlagsObj) && FlagsObj && (*FlagsObj).IsValid()))
    {
        bool bFlag = false;
        (*FlagsObj)->TryGetBoolField(TEXT("dontEditOnInstance"), bFlag);
        TestTrue(TEXT("dontEditOnInstance flag true"), bFlag);
        (*FlagsObj)->TryGetBoolField(TEXT("enableSaveGame"), bFlag);
        TestTrue(TEXT("enableSaveGame flag true"), bFlag);
    }

    return true;
}

// Regression for E-create-struct-orphan-seed-field: the engine seeds every new
// user-defined struct with a stray default bool MemberVar_0 (UE forbids a
// zero-field struct). The "create empty, then add_struct_field" workflow must
// reclaim that seed on the FIRST add instead of appending — otherwise N adds
// leave N+1 fields with a phantom MemberVar_0. Without the seed-reuse branch in
// AddStructField this test sees count == 4 and a leftover MemberVar_0 and fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddStructFieldReclaimsSeedTest,
    "PinWright.blueprint.add_struct_field.ReclaimsOrphanSeed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddStructFieldReclaimsSeedTest::RunTest(const FString& Parameters)
{
    const FString StructPath = MakeUniqueAssetPath(TEXT("S_SeedReclaim"));
    CleanupAsset(StructPath);
    ON_SCOPE_EXIT { CleanupAsset(StructPath); };

    // create_struct with no fields array -> engine seeds the lone MemberVar_0.
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), StructPath);
    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("blueprint.create_struct handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.create_struct"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_struct succeeded"), CreateCapture.bSuccess);

    // Add three distinct fields one at a time via add_struct_field.
    const TCHAR* FieldNames[] = { TEXT("ItemName"), TEXT("StackCount"), TEXT("Weight") };
    const TCHAR* FieldTypes[] = { TEXT("string"), TEXT("int"), TEXT("float") };
    for (int32 Index = 0; Index < 3; ++Index)
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("path"), StructPath);
        AddPayload->SetStringField(TEXT("fieldName"), FieldNames[Index]);
        AddPayload->SetStringField(TEXT("fieldType"), FieldTypes[Index]);
        FTestResponseCapture AddCapture;
        TestTrue(TEXT("blueprint.add_struct_field handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_struct_field"), AddPayload, AddCapture));
        TestTrue(FString::Printf(TEXT("add_struct_field '%s' succeeded"), FieldNames[Index]), AddCapture.bSuccess);
    }

    // list_struct_fields must show exactly the three fields added — the seed was
    // reclaimed by the first add, not left dangling as a fourth field.
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), StructPath);
    FTestResponseCapture ListCapture;
    TestTrue(TEXT("blueprint.list_struct_fields handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.list_struct_fields"), ListPayload, ListCapture));
    TestTrue(TEXT("list_struct_fields succeeded"), ListCapture.bSuccess);

    double Count = -1.0;
    if (ListCapture.Result.IsValid())
    {
        ListCapture.Result->TryGetNumberField(TEXT("count"), Count);
    }
    TestEqual(TEXT("struct has exactly 3 fields (seed reclaimed, not orphaned)"), Count, 3.0);

    // Every requested field is present...
    for (int32 Index = 0; Index < 3; ++Index)
    {
        TSharedPtr<FJsonObject> Field = FindStructFieldInListResponse(ListCapture.Result, FieldNames[Index]);
        TestTrue(FString::Printf(TEXT("field '%s' present"), FieldNames[Index]), Field.IsValid());
    }

    // ...and the stray engine seed is gone (reclaimed into the first field).
    TSharedPtr<FJsonObject> Seed = FindStructFieldInListResponse(ListCapture.Result, TEXT("MemberVar_0"));
    TestFalse(TEXT("no orphan MemberVar_0 seed remains"), Seed.IsValid());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetStructFieldDefaultAndMetadataTest,
    "PinWright.blueprint.set_struct_field_default_and_metadata.SetAndClear",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetStructFieldDefaultAndMetadataTest::RunTest(const FString& Parameters)
{
    const FString StructPath = MakeUniqueAssetPath(TEXT("S_PostAddEdit"));
    CleanupAsset(StructPath);
    ON_SCOPE_EXIT { CleanupAsset(StructPath); };

    // Seed struct with a single int field.
    TSharedPtr<FJsonObject> FieldObj = MakeShared<FJsonObject>();
    FieldObj->SetStringField(TEXT("name"), TEXT("Score"));
    FieldObj->SetStringField(TEXT("type"), TEXT("int"));
    TArray<TSharedPtr<FJsonValue>> FieldArr;
    FieldArr.Add(MakeShared<FJsonValueObject>(FieldObj));

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), StructPath);
    CreatePayload->SetArrayField(TEXT("fields"), FieldArr);
    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("blueprint.create_struct handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.create_struct"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_struct succeeded"), CreateCapture.bSuccess);

    // set_struct_field_default value="7" → list shows defaultValue=="7".
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("path"), StructPath);
        P->SetStringField(TEXT("fieldName"), TEXT("Score"));
        P->SetStringField(TEXT("value"), TEXT("7"));
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_struct_field_default handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.set_struct_field_default"), P, Cap));
        TestTrue(TEXT("set_struct_field_default succeeded"), Cap.bSuccess);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("path"), StructPath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("list_struct_fields handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.list_struct_fields"), P, Cap));
        TSharedPtr<FJsonObject> Field = FindStructFieldInListResponse(Cap.Result, TEXT("Score"));
        if (TestTrue(TEXT("Score field present"), Field.IsValid()))
        {
            FString DV;
            Field->TryGetStringField(TEXT("defaultValue"), DV);
            TestEqual(TEXT("defaultValue is 7"), DV, FString(TEXT("7")));
        }
    }

#if !UE_VERSION_OLDER_THAN(5, 5, 0)
    // blueprint.set_struct_field_metadata (and the FStructureEditorUtils::SetMetaData it wraps)
    // only exists on UE 5.5+. On 5.3/5.4 the handler returns UNSUPPORTED_ENGINE_VERSION because
    // FStructVariableDescription has no MetaData storage, so the set/clear round-trip below is
    // unrunnable. The set_struct_field_default round-trip above uses ChangeVariableDefaultValue,
    // which exists on every supported engine, and still runs.

    // set_struct_field_metadata key=ClampMin value=0 → present.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("path"), StructPath);
        P->SetStringField(TEXT("fieldName"), TEXT("Score"));
        P->SetStringField(TEXT("key"), TEXT("ClampMin"));
        P->SetStringField(TEXT("value"), TEXT("0"));
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_struct_field_metadata handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.set_struct_field_metadata"), P, Cap));
        TestTrue(TEXT("set_struct_field_metadata succeeded"), Cap.bSuccess);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("path"), StructPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("blueprint.list_struct_fields"), P, Cap);
        TSharedPtr<FJsonObject> Field = FindStructFieldInListResponse(Cap.Result, TEXT("Score"));
        if (TestTrue(TEXT("Score field present after metadata set"), Field.IsValid()))
        {
            const TSharedPtr<FJsonObject>* MetaObj = nullptr;
            if (TestTrue(TEXT("metaData object present"),
                Field->TryGetObjectField(TEXT("metaData"), MetaObj) && MetaObj && (*MetaObj).IsValid()))
            {
                FString MetaVal;
                (*MetaObj)->TryGetStringField(TEXT("ClampMin"), MetaVal);
                TestEqual(TEXT("ClampMin set to 0"), MetaVal, FString(TEXT("0")));
            }
        }
    }

    // Clear by passing empty value -> ClampMin absent.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("path"), StructPath);
        P->SetStringField(TEXT("fieldName"), TEXT("Score"));
        P->SetStringField(TEXT("key"), TEXT("ClampMin"));
        P->SetStringField(TEXT("value"), TEXT(""));
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_struct_field_metadata clear handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.set_struct_field_metadata"), P, Cap));
        TestTrue(TEXT("set_struct_field_metadata clear succeeded"), Cap.bSuccess);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("path"), StructPath);
        FTestResponseCapture Cap;
        InvokeHandlerWithCapture(TEXT("blueprint.list_struct_fields"), P, Cap);
        TSharedPtr<FJsonObject> Field = FindStructFieldInListResponse(Cap.Result, TEXT("Score"));
        if (TestTrue(TEXT("Score field present after metadata clear"), Field.IsValid()))
        {
            const TSharedPtr<FJsonObject>* MetaObj = nullptr;
            if (Field->TryGetObjectField(TEXT("metaData"), MetaObj) && MetaObj && (*MetaObj).IsValid())
            {
                TestFalse(TEXT("ClampMin cleared"),
                    (*MetaObj)->HasField(TEXT("ClampMin")));
            }
        }
    }
#endif

    return true;
}

// ============================================================================
// Regression: B-variable-category-ftext-localization-error
// A variable/function category is an editor-only organizational label, NOT
// persisted localizable game/UI text. The F-require-ftext-localization-identity
// gate (CoerceStringToPersistedFText) over-caught it and rejected a plain-string
// `category` with INVALID_TEXT_LOCALIZATION_IDENTITY. These tests pin the fix:
// add_variable / set_variable_settings / set_function_settings must accept a
// plain string and store it verbatim. They drive the real registered handlers
// through the dispatcher, so reverting the fix (restoring the coercion gate)
// makes the handler SendError instead of SendSuccess and fails Capture.bSuccess.
// The by-name NewVariables lookup is CompilerTestUtils::FindNewVariableByName.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddVariablePlainStringCategoryTest,
    "PinWright.blueprint.add_variable.PlainStringCategoryAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddVariablePlainStringCategoryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("AddVarPlainCategory"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());
    Payload->SetStringField(TEXT("variableName"), TEXT("TargetMarkerStyle"));
    Payload->SetStringField(TEXT("variableType"), TEXT("int"));
    // Plain string with no NSLOCTEXT namespace/key — the exact repro input.
    Payload->SetStringField(TEXT("category"), TEXT("Minimap"));
    Payload->SetBoolField(TEXT("isPublic"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.add_variable"), Payload, Capture);
    TestTrue(TEXT("blueprint.add_variable handler found"), bFound);
    // Pre-fix this SendError'd INVALID_TEXT_LOCALIZATION_IDENTITY.
    TestTrue(TEXT("add_variable with plain-string category succeeds"), Capture.bSuccess);
    TestNotEqual(TEXT("error code is not the localization-identity rejection"),
        Capture.ErrorCode, FString(TEXT("INVALID_TEXT_LOCALIZATION_IDENTITY")));

    const FBPVariableDescription* Var = CompilerTestUtils::FindNewVariableByName(BP, TEXT("TargetMarkerStyle"));
    if (TestNotNull(TEXT("variable was actually created"), Var))
    {
        TestEqual(TEXT("category FText carries the plain string verbatim"),
            Var->Category.ToString(), FString(TEXT("Minimap")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetVariableSettingsPlainStringCategoryTest,
    "PinWright.blueprint.set_variable_settings.PlainStringCategoryAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetVariableSettingsPlainStringCategoryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("SetVarSettingsPlainCategory"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    // Seed a variable WITHOUT a category, then set its category as a plain string.
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("path"), BP->GetPathName());
        AddPayload->SetStringField(TEXT("variableName"), TEXT("HealthRestored"));
        AddPayload->SetStringField(TEXT("variableType"), TEXT("int"));
        FTestResponseCapture AddCapture;
        TestTrue(TEXT("seed add_variable handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_variable"), AddPayload, AddCapture));
        TestTrue(TEXT("seed add_variable succeeded"), AddCapture.bSuccess);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());
    Payload->SetStringField(TEXT("variableName"), TEXT("HealthRestored"));
    Payload->SetStringField(TEXT("category"), TEXT("Pickup"));
    Payload->SetBoolField(TEXT("isInstanceEditable"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.set_variable_settings"), Payload, Capture);
    TestTrue(TEXT("blueprint.set_variable_settings handler found"), bFound);
    // Pre-fix this SendError'd INVALID_TEXT_LOCALIZATION_IDENTITY.
    TestTrue(TEXT("set_variable_settings with plain-string category succeeds"), Capture.bSuccess);
    TestNotEqual(TEXT("error code is not the localization-identity rejection"),
        Capture.ErrorCode, FString(TEXT("INVALID_TEXT_LOCALIZATION_IDENTITY")));

    const FBPVariableDescription* Var = CompilerTestUtils::FindNewVariableByName(BP, TEXT("HealthRestored"));
    if (TestNotNull(TEXT("variable still present"), Var))
    {
        TestEqual(TEXT("category FText carries the plain string verbatim"),
            Var->Category.ToString(), FString(TEXT("Pickup")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetFunctionSettingsPlainStringCategoryTest,
    "PinWright.blueprint.set_function_settings.PlainStringCategoryAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetFunctionSettingsPlainStringCategoryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("SetFuncSettingsPlainCategory"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    // Seed a function, then set its category as a plain string.
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("path"), BP->GetPathName());
        AddPayload->SetStringField(TEXT("functionName"), TEXT("DoThing"));
        FTestResponseCapture AddCapture;
        TestTrue(TEXT("seed add_function handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_function"), AddPayload, AddCapture));
        TestTrue(TEXT("seed add_function succeeded"), AddCapture.bSuccess);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());
    Payload->SetStringField(TEXT("functionName"), TEXT("DoThing"));
    Payload->SetStringField(TEXT("category"), TEXT("Gameplay"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.set_function_settings"), Payload, Capture);
    TestTrue(TEXT("blueprint.set_function_settings handler found"), bFound);
    // Pre-fix the parallel function-category path SendError'd INVALID_TEXT_LOCALIZATION_IDENTITY.
    TestTrue(TEXT("set_function_settings with plain-string category succeeds"), Capture.bSuccess);
    TestNotEqual(TEXT("error code is not the localization-identity rejection"),
        Capture.ErrorCode, FString(TEXT("INVALID_TEXT_LOCALIZATION_IDENTITY")));

    return true;
}

// ============================================================================
// Regression: blueprint.get's doc-contract must not advertise a `components`
// field it never emits (ticket E-blueprint-get-omits-components-readback-guidance).
// The registration summary previously listed "components" while the handler
// (BuildBlueprintSnapshot + the registry merge) only ever produces
// variables/functions/events. This locks both halves of the contract:
//   (1) the registered Summary string must not promise components, and
//   (2) the live handler response must contain no `components` field.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGetDoesNotAdvertiseOrEmitComponentsTest,
    "PinWright.blueprint.get.NoComponentsContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGetDoesNotAdvertiseOrEmitComponentsTest::RunTest(const FString& Parameters)
{
    // --- (1) Doc-contract guard: the registered summary must not promise a
    //         `components` field that blueprint.get never returns. Assert the
    //         *property* the ticket is about — "components, if mentioned, is a
    //         negative routing note, never a returned-field promise" — not the
    //         exact retired prose (matching the convention in
    //         TestAssetReferenceDirection.cpp). Any 'components' mention must
    //         co-occur with the "not include components" disclaimer and route
    //         the caller to blueprint.scs.get; a benign rewording of the field
    //         list that still disclaims components keeps passing. ---
    const FString Summary = GetRegisteredSummary(TEXT("blueprint.get"));
    TestFalse(TEXT("blueprint.get is registered"), Summary.IsEmpty());

    if (Summary.Contains(TEXT("components"), ESearchCase::IgnoreCase))
    {
        TestTrue(
            TEXT("blueprint.get summary disclaims components ('does NOT include' / 'not include')"),
            Summary.Contains(TEXT("not include components"), ESearchCase::IgnoreCase));
        TestTrue(
            TEXT("blueprint.get summary routes component readback to blueprint.scs.get"),
            Summary.Contains(TEXT("blueprint.scs.get")));
    }

    // --- (2) Behavior guard: a live blueprint.get response carries no
    //         `components` field, but does carry variables/functions/events. ---
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("BlueprintGetNoComponents"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), BP->GetPathName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.get"), Payload, Capture);
    TestTrue(TEXT("blueprint.get handler found"), bFound);
    TestTrue(TEXT("blueprint.get succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        TestFalse(TEXT("blueprint.get response has no 'components' field"),
            Capture.Result->HasField(TEXT("components")));
        TestTrue(TEXT("blueprint.get response has 'variables'"),
            Capture.Result->HasField(TEXT("variables")));
        TestTrue(TEXT("blueprint.get response has 'functions'"),
            Capture.Result->HasField(TEXT("functions")));
        TestTrue(TEXT("blueprint.get response has 'events'"),
            Capture.Result->HasField(TEXT("events")));
    }

    return true;
}

// ============================================================================
// Regression: E-inspect-events-omits-disabled-stub-flag.
// blueprint.inspect / blueprint.get build events[] via CollectBlueprintEvents,
// whose AppendEvent lambda previously emitted only {name,eventType,parameters}.
// A disabled event stub (the inert ReceiveTick / ReceiveActorBeginOverlap default
// ghosts a fresh Actor BP carries) was therefore indistinguishable from a live
// authored handler. The fix adds an `enabled` boolean (UEdGraphNode::IsNodeEnabled)
// to each entry, mirroring the structured nodeState.isEnabled the graph-inspection
// family already exposes. This seeds one enabled event (ReceiveBeginPlay) and one
// disabled event (ReceiveTick), drives blueprint.inspect, and asserts every
// events[] entry carries `enabled` with the disabled stub reading false and the
// live handler true. Reverting the SetBoolField in CollectBlueprintEvents drops
// the field and fails the presence assertions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectEventsCarryEnabledFlagTest,
    "PinWright.blueprint.inspect.EventsCarryEnabledFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintInspectEventsCarryEnabledFlagTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("InspectEventsEnabledFlag"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }
    if (!TestTrue(TEXT("Blueprint has an ubergraph page"), BP->UbergraphPages.Num() > 0))
    {
        return true;
    }
    UEdGraph* EventGraph = BP->UbergraphPages[0];

    // Enabled live handler. A fresh Actor BP seeds ReceiveBeginPlay as one of the
    // *disabled* ghost default stubs the ticket describes, so EnsureBeginPlayNode
    // returns that inert ghost rather than a fresh enabled node. Promote it to the
    // enabled state an authored handler carries so this entry exercises the live
    // (enabled=true) side of the flag, distinct from the disabled stub below.
    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    if (TestNotNull(TEXT("BeginPlay event node created"), BeginPlayNode))
    {
        BeginPlayNode->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/true);
    }

    // Disabled inert stub, mirroring the default Actor ghost the ticket describes.
    UK2Node_Event* TickNode = BpirGraphTestHelpers::EnsureEventNode(EventGraph, TEXT("ReceiveTick"));
    if (TestNotNull(TEXT("Tick event node created"), TickNode))
    {
        TickNode->SetEnabledState(ENodeEnabledState::Disabled, /*bUserAction*/true);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.inspect handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.inspect"), Payload, Capture));
    TestTrue(TEXT("blueprint.inspect succeeded"), Capture.bSuccess);

    TSharedPtr<FJsonObject> LiveEvent = JsonArrayFindObjectByStringField(
        Capture.Result, TEXT("events"), TEXT("name"), TEXT("ReceiveBeginPlay"));
    if (TestTrue(TEXT("events[] contains the live ReceiveBeginPlay handler"), LiveEvent.IsValid()))
    {
        bool bEnabled = false;
        TestTrue(TEXT("live event entry carries an 'enabled' field"),
            LiveEvent->TryGetBoolField(TEXT("enabled"), bEnabled));
        TestTrue(TEXT("live ReceiveBeginPlay handler reports enabled=true"), bEnabled);
    }

    TSharedPtr<FJsonObject> StubEvent = JsonArrayFindObjectByStringField(
        Capture.Result, TEXT("events"), TEXT("name"), TEXT("ReceiveTick"));
    if (TestTrue(TEXT("events[] contains the disabled ReceiveTick stub"), StubEvent.IsValid()))
    {
        bool bEnabled = true;
        TestTrue(TEXT("stub event entry carries an 'enabled' field"),
            StubEvent->TryGetBoolField(TEXT("enabled"), bEnabled));
        TestFalse(TEXT("disabled ReceiveTick stub reports enabled=false"), bEnabled);
    }

    return true;
}

// ============================================================================
// Regression: E-blueprint-get-omits-function-category.
// blueprint.set_function_settings persists a function's `category` into the
// function entry's MetaData.Category (FBlueprintEditorUtils::
// SetBlueprintFunctionOrMacroCategory) — the same MetaData struct that already
// surfaces bCallInEditor — yet CollectBlueprintFunctions read back every other
// set_function_settings knob (public/protected/private/pure/const/callInEditor)
// and silently dropped `category`. This pins the fix: after setting a function
// category, BOTH summary readback verbs (blueprint.get and blueprint.inspect)
// emit the `category` field on the matching function entry. Reverting the
// CollectBlueprintFunctions emit (the shared builder behind both verbs) drops
// the field and fails both halves.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFunctionCategoryReadbackTest,
    "PinWright.blueprint.get.FunctionCategoryReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintFunctionCategoryReadbackTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("FunctionCategoryReadback"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    const FString FunctionName = TEXT("WeighItems");
    const FString ExpectedCategory = TEXT("Inventory|Weight");

    // Seed a function, then assign it a category via set_function_settings.
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("path"), BP->GetPathName());
        AddPayload->SetStringField(TEXT("functionName"), FunctionName);
        FTestResponseCapture AddCapture;
        TestTrue(TEXT("seed add_function handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_function"), AddPayload, AddCapture));
        TestTrue(TEXT("seed add_function succeeded"), AddCapture.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("path"), BP->GetPathName());
        SetPayload->SetStringField(TEXT("functionName"), FunctionName);
        SetPayload->SetStringField(TEXT("category"), ExpectedCategory);
        FTestResponseCapture SetCapture;
        TestTrue(TEXT("set_function_settings handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.set_function_settings"), SetPayload, SetCapture));
        TestTrue(TEXT("set_function_settings (category) succeeded"), SetCapture.bSuccess);
    }

    // --- blueprint.get must emit the category on the matching function entry. ---
    {
        TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
        GetPayload->SetStringField(TEXT("path"), BP->GetPathName());
        FTestResponseCapture GetCapture;
        TestTrue(TEXT("blueprint.get handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.get"), GetPayload, GetCapture));
        TestTrue(TEXT("blueprint.get succeeded"), GetCapture.bSuccess);

        TSharedPtr<FJsonObject> Fn = JsonArrayFindObjectByStringField(
            GetCapture.Result, TEXT("functions"), TEXT("name"), FunctionName);
        if (TestTrue(TEXT("blueprint.get returns the WeighItems function entry"), Fn.IsValid()))
        {
            FString ReadCategory;
            TestTrue(TEXT("blueprint.get function entry carries a 'category' field"),
                Fn->TryGetStringField(TEXT("category"), ReadCategory));
            TestEqual(TEXT("blueprint.get function category round-trips set_function_settings"),
                ReadCategory, ExpectedCategory);
        }
    }

    // --- blueprint.inspect shares CollectBlueprintFunctions, so it too must emit it. ---
    {
        TSharedPtr<FJsonObject> InspectPayload = MakeShared<FJsonObject>();
        InspectPayload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        FTestResponseCapture InspectCapture;
        TestTrue(TEXT("blueprint.inspect handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.inspect"), InspectPayload, InspectCapture));
        TestTrue(TEXT("blueprint.inspect succeeded"), InspectCapture.bSuccess);

        TSharedPtr<FJsonObject> Fn = JsonArrayFindObjectByStringField(
            InspectCapture.Result, TEXT("functions"), TEXT("name"), FunctionName);
        if (TestTrue(TEXT("blueprint.inspect returns the WeighItems function entry"), Fn.IsValid()))
        {
            FString ReadCategory;
            TestTrue(TEXT("blueprint.inspect function entry carries a 'category' field"),
                Fn->TryGetStringField(TEXT("category"), ReadCategory));
            TestEqual(TEXT("blueprint.inspect function category round-trips set_function_settings"),
                ReadCategory, ExpectedCategory);
        }
    }

    return true;
}

// ============================================================================
// Regression: blueprint.get / blueprint.inspect must emit a function's
// `category` on the function entry (ticket E-blueprint-get-omits-function-category).
// CollectBlueprintFunctions previously read the function flags + bCallInEditor
// but never EntryNode->MetaData.Category, so the one set_function_settings knob
// that couldn't round-trip through the summary verbs was the category. This
// seeds a function, sets its category through the real handler, then drives the
// two readback verbs and asserts each function entry carries category=<value>.
// Reverting the emit (dropping the SetStringField in CollectBlueprintFunctions)
// fails the get assertion; reverting the formatter line fails the inspect one.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReadbackEmitsFunctionCategoryTest,
    "PinWright.blueprint.get.FunctionCategoryRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReadbackEmitsFunctionCategoryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("FunctionCategoryRoundTrips"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }

    // Seed a function, then set its category through the real handler — exactly
    // the write path set_function_settings exercises.
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("path"), BP->GetPathName());
        AddPayload->SetStringField(TEXT("functionName"), TEXT("CalculateTotalWeight"));
        FTestResponseCapture AddCapture;
        TestTrue(TEXT("seed add_function handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_function"), AddPayload, AddCapture));
        TestTrue(TEXT("seed add_function succeeded"), AddCapture.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("path"), BP->GetPathName());
        SetPayload->SetStringField(TEXT("functionName"), TEXT("CalculateTotalWeight"));
        SetPayload->SetStringField(TEXT("category"), TEXT("Inventory|Weight"));
        FTestResponseCapture SetCapture;
        TestTrue(TEXT("set_function_settings handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.set_function_settings"), SetPayload, SetCapture));
        TestTrue(TEXT("set_function_settings succeeded"), SetCapture.bSuccess);
    }

    // Both summary verbs must surface the function's category on its entry. They
    // take the asset path under different keys (blueprint.get: "path",
    // blueprint.inspect: "assetPath") but must read it back identically.
    auto AssertCategoryRoundTrips = [&](const TCHAR* Method, const TCHAR* PathKey)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(PathKey, BP->GetPathName());
        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("%s handler found"), Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        TestTrue(*FString::Printf(TEXT("%s succeeded"), Method), Capture.bSuccess);

        const TSharedPtr<FJsonObject> Fn = JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("functions"), TEXT("name"), TEXT("CalculateTotalWeight"));
        if (TestTrue(*FString::Printf(TEXT("%s returned the seeded function entry"), Method), Fn.IsValid()))
        {
            FString Category;
            TestTrue(*FString::Printf(TEXT("%s function entry has a 'category' field"), Method),
                Fn->TryGetStringField(TEXT("category"), Category));
            TestEqual(*FString::Printf(TEXT("%s function category round-trips verbatim"), Method),
                Category, FString(TEXT("Inventory|Weight")));
        }
    };

    AssertCategoryRoundTrips(TEXT("blueprint.get"), TEXT("path"));
    AssertCategoryRoundTrips(TEXT("blueprint.inspect"), TEXT("assetPath"));

    return true;
}
