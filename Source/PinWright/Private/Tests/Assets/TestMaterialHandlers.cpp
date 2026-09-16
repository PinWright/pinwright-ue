// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Material domain handlers:
//   MaterialAuthoringHandler.cpp  (49 handlers, material.authoring.*)
//   MaterialGraphHandler.cpp      ( 8 handlers, material.graph.*)
//   MGIRCompileHandler.cpp / MGIRDecompileHandler.cpp  (2 handlers, compile/decompile MGIR)
//   TextureHandler.cpp            (27 handlers, texture.*, all RPC_NO_PARAMS)
#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "IrCore/IrTextUtils.h"
#include "MGIR/MGIRCompiler.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialFunction.h"

namespace
{
UMaterial* CreateTransientTestMaterial(const FString& NamePrefix)
{
    const FString AssetName = FString::Printf(TEXT("%s_%s"),
        *NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), FName(*AssetName), RF_Public | RF_Transient);
    if (Material)
    {
        Material->AddToRoot();
    }
    return Material;
}

UMaterialExpression* FindMaterialExpressionByGuid(UMaterial* Material, const FString& NodeId)
{
    if (!Material || NodeId.IsEmpty())
    {
        return nullptr;
    }

    for (UMaterialExpression* Expr : Material->GetExpressions())
    {
        if (Expr && Expr->MaterialExpressionGuid.ToString() == NodeId)
        {
            return Expr;
        }
    }

    return nullptr;
}

const FHandlerRegistration* FindMaterialHandlerRegistration(const FString& MethodName)
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

bool HasParamSpec(const FHandlerRegistration* Reg, const FString& Name, bool bRequired)
{
    if (!Reg)
    {
        return false;
    }

    for (const FParamSpec& Spec : Reg->Params)
    {
        if (Spec.Name == Name && Spec.bRequired == bRequired)
        {
            return true;
        }
    }

    return false;
}

struct FDispatcherValidationResult
{
    bool bCompletionFired = false;
    FString ErrorCode;
};

FDispatcherValidationResult DispatchMaterialRequestViaDispatcher(
    const FString& RequestId,
    const FString& MethodName,
    const TSharedPtr<FJsonObject>& Payload)
{
    FDispatcherValidationResult Result;
    FRpcDispatcher Dispatcher;
    // Initialize BEFORE draining so the bridge lambdas capture a live sink.
    Dispatcher.Initialize(FResponseSink(
        [&Result]
        (const FString&, bool, const FString&, const TSharedPtr<FJsonObject>&, const FString& InErrorCode)
        {
            Result.bCompletionFired = true;
            Result.ErrorCode = InErrorCode;
        }));
    Dispatcher.DrainAutoRegistrations(nullptr);

    Dispatcher.ProcessRequest(RequestId, MethodName, Payload.IsValid() ? Payload : MakeShared<FJsonObject>());
    return Result;
}
}

// ============================================================================
// material.compile_mgir / material.decompile_mgir contract
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialMGIRHandlersRegisteredTest,
    "PinWright.material.mgir.HandlersRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialMGIRHandlersRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("compile handler registered"), IsHandlerRegistered(TEXT("material.compile_mgir")));
    TestTrue(TEXT("decompile handler registered"), IsHandlerRegistered(TEXT("material.decompile_mgir")));

    const FHandlerRegistration* CompileReg = FindMaterialHandlerRegistration(TEXT("material.compile_mgir"));
    const FHandlerRegistration* DecompileReg = FindMaterialHandlerRegistration(TEXT("material.decompile_mgir"));

    TestNotNull(TEXT("compile registration found"), CompileReg);
    TestNotNull(TEXT("decompile registration found"), DecompileReg);
    if (CompileReg)
    {
        TestEqual(TEXT("compile category"), CompileReg->Category, FString(TEXT("material")));
        TestTrue(TEXT("compile requires text"), HasParamSpec(CompileReg, TEXT("text"), true));
        TestTrue(TEXT("compile has optional mode"), HasParamSpec(CompileReg, TEXT("mode"), false));
        TestTrue(TEXT("compile has optional context"), HasParamSpec(CompileReg, TEXT("context"), false));
        TestTrue(TEXT("compile has optional runLayout"), HasParamSpec(CompileReg, TEXT("runLayout"), false));
        TestTrue(TEXT("compile has optional save"), HasParamSpec(CompileReg, TEXT("save"), false));
    }
    if (DecompileReg)
    {
        TestEqual(TEXT("decompile category"), DecompileReg->Category, FString(TEXT("material")));
        TestTrue(TEXT("decompile requires assetPath"), HasParamSpec(DecompileReg, TEXT("assetPath"), true));
        TestTrue(TEXT("decompile has optional includeReferencedFunctions"),
            HasParamSpec(DecompileReg, TEXT("includeReferencedFunctions"), false));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialMGIRCompileRejectsMissingTextViaDispatcherTest,
    "PinWright.material.compile_mgir.RejectsMissingTextViaDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialMGIRCompileRejectsMissingTextViaDispatcherTest::RunTest(const FString& Parameters)
{
    const FDispatcherValidationResult Result = DispatchMaterialRequestViaDispatcher(
        TEXT("req-mgir-missing"),
        TEXT("material.compile_mgir"),
        MakeShared<FJsonObject>());

    TestTrue(TEXT("completion fired"), Result.bCompletionFired);
    TestEqual(TEXT("missing text rejected by dispatcher"), Result.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialMGIRCompileRejectsUnknownParamViaDispatcherTest,
    "PinWright.material.compile_mgir.RejectsUnknownParamViaDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialMGIRCompileRejectsUnknownParamViaDispatcherTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), TEXT("entry material `/Game/Materials/M_Test` {}"));
    Payload->SetStringField(TEXT("unexpected"), TEXT("value"));
    const FDispatcherValidationResult Result = DispatchMaterialRequestViaDispatcher(
        TEXT("req-mgir-unknown"),
        TEXT("material.compile_mgir"),
        Payload);

    TestTrue(TEXT("completion fired"), Result.bCompletionFired);
    TestEqual(TEXT("unknown param rejected by dispatcher"), Result.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialMGIRDecompileRejectsMissingAssetPathViaDispatcherTest,
    "PinWright.material.decompile_mgir.RejectsMissingAssetPathViaDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialMGIRDecompileRejectsMissingAssetPathViaDispatcherTest::RunTest(const FString& Parameters)
{
    const FDispatcherValidationResult Result = DispatchMaterialRequestViaDispatcher(
        TEXT("req-mgir-decompile-missing"),
        TEXT("material.decompile_mgir"),
        MakeShared<FJsonObject>());

    TestTrue(TEXT("completion fired"), Result.bCompletionFired);
    TestEqual(TEXT("missing assetPath rejected by dispatcher"), Result.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialMGIRDecompileRejectsUnknownParamViaDispatcherTest,
    "PinWright.material.decompile_mgir.RejectsUnknownParamViaDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialMGIRDecompileRejectsUnknownParamViaDispatcherTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("unexpected"), TEXT("value"));
    const FDispatcherValidationResult Result = DispatchMaterialRequestViaDispatcher(
        TEXT("req-mgir-decompile-unknown"),
        TEXT("material.decompile_mgir"),
        Payload);

    TestTrue(TEXT("completion fired"), Result.bCompletionFired);
    TestEqual(TEXT("unknown param rejected by dispatcher"), Result.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialMGIRCompileFunctionCreatesFunctionEntryTest,
    "PinWright.material.compile_mgir.CompilesFunctionEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialMGIRCompileFunctionCreatesFunctionEntryTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/MF_MGIR_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    FMGIRCompileOptions Options;
    Options.bRunLayout = false;
    Options.bSave = false;

    const FString Text = FString::Printf(
        TEXT("entry function %s {\n")
        TEXT("    %%amount = call %s(InputName: \"Amount\", InputType: \"Float1\")\n")
        TEXT("    output Result: %%amount\n")
        TEXT("}"),
        *FIrTextUtils::FormatNameToken(AssetPath),
        *FIrTextUtils::FormatNameToken(UMaterialExpressionFunctionInput::StaticClass()->GetPathName()));

    FMGIRCompileResult Result = FMGIRCompiler::Compile(Text, Options);
    TestTrue(TEXT("function MGIR compile succeeds"), Result.bSuccess);
    TestEqual(TEXT("one block compiled"), Result.BlocksCompiled, 1);
    TestTrue(TEXT("compiled asset path returned"),
        Result.AssetPaths.Num() == 1 && Result.AssetPaths[0].StartsWith(AssetPath));

    UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *AssetPath);
    TestNotNull(TEXT("material function asset created"), Function);
    if (!Function)
    {
        return true;
    }

    int32 InputCount = 0;
    int32 OutputCount = 0;
    for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
    {
        if (Expression && Expression->IsA<UMaterialExpressionFunctionInput>())
        {
            ++InputCount;
        }
        else if (Expression && Expression->IsA<UMaterialExpressionFunctionOutput>())
        {
            ++OutputCount;
        }
    }

    TestEqual(TEXT("function input created"), InputCount, 1);
    TestEqual(TEXT("function output created"), OutputCount, 1);
    return true;
}

// ============================================================================
// material.authoring.create_material
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCreateMaterialValidParamsNoCrashTest,
    "PinWright.material.authoring.create_material.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringCreateMaterialValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Provide required "name"; handler will fail at asset creation (no real package system) but must not crash.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestMat"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.create_material"), Payload));
    CleanupTestAsset(TEXT("/Game/Materials/TestMat"));
    return true;
}

// ============================================================================
// material.authoring.set_blend_mode
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringSetBlendModeValidParamsNoCrashTest,
    "PinWright.material.authoring.set_blend_mode.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringSetBlendModeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("blendMode"), TEXT("Opaque"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.set_blend_mode"), Payload));
    return true;
}

// ============================================================================
// material.authoring.set_shading_model
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringSetShadingModelValidParamsNoCrashTest,
    "PinWright.material.authoring.set_shading_model.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringSetShadingModelValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("shadingModel"), TEXT("DefaultLit"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.set_shading_model"), Payload));
    return true;
}

// ============================================================================
// material.authoring.set_material_domain
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringSetMaterialDomainValidParamsNoCrashTest,
    "PinWright.material.authoring.set_material_domain.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringSetMaterialDomainValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("materialDomain"), TEXT("Surface"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.set_material_domain"), Payload));
    return true;
}

// ============================================================================
// material.authoring.add_texture_sample
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddTextureSampleValidParamsNoCrashTest,
    "PinWright.material.authoring.add_texture_sample.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddTextureSampleValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_texture_sample"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddTextureSampleParameterCreatesExpectedClassTest,
    "PinWright.material.authoring.add_texture_sample.CreatesParameterClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddTextureSampleParameterCreatesExpectedClassTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = CreateTransientTestMaterial(TEXT("M_TextureSampleParameter"));
    TestNotNull(TEXT("Transient test material created"), Material);
    if (!Material)
    {
        return true;
    }

    ON_SCOPE_EXIT
    {
        Material->RemoveFromRoot();
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Material->GetPathName());
    Payload->SetStringField(TEXT("parameterName"), TEXT("AlbedoTexture"));
    Payload->SetNumberField(TEXT("x"), 64.0);
    Payload->SetNumberField(TEXT("y"), 128.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("material.authoring.add_texture_sample"), Payload, Capture));
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return true;
    }

    FString NodeId;
    Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
    UMaterialExpressionTextureSampleParameter2D* TextureParam =
        Cast<UMaterialExpressionTextureSampleParameter2D>(FindMaterialExpressionByGuid(Material, NodeId));
    TestNotNull(TEXT("Texture sample parameter expression created"), TextureParam);
    if (TextureParam)
    {
        TestEqual(TEXT("Parameter name preserved"), TextureParam->ParameterName.ToString(), FString(TEXT("AlbedoTexture")));
    }
    return true;
}

// ============================================================================
// material.authoring.add_scalar_parameter
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddScalarParameterValidParamsNoCrashTest,
    "PinWright.material.authoring.add_scalar_parameter.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddScalarParameterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("parameterName"), TEXT("Roughness"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_scalar_parameter"), Payload));
    return true;
}

// ============================================================================
// material.authoring.add_vector_parameter
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddVectorParameterValidParamsNoCrashTest,
    "PinWright.material.authoring.add_vector_parameter.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddVectorParameterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("parameterName"), TEXT("BaseColor"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_vector_parameter"), Payload));
    return true;
}

// ============================================================================
// material.authoring.add_static_switch_parameter
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddStaticSwitchParameterValidParamsNoCrashTest,
    "PinWright.material.authoring.add_static_switch_parameter.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddStaticSwitchParameterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("parameterName"), TEXT("UseDetail"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_static_switch_parameter"), Payload));
    return true;
}

// ============================================================================
// material.authoring.add_math_node
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddMathNodeValidParamsNoCrashTest,
    "PinWright.material.authoring.add_math_node.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddMathNodeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("operation"), TEXT("Multiply"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_math_node"), Payload));
    return true;
}

// ============================================================================
// material.authoring.add_custom_expression
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddCustomExpressionMissingRequiredParamTest,
    "PinWright.material.authoring.add_custom_expression.MissingRequiredParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddCustomExpressionMissingRequiredParamTest::RunTest(const FString& Parameters)
{
    // Required "assetPath", "code", and "inputs" are all absent.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("material.authoring.add_custom_expression"), Payload, Capture));
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error when inputs are omitted"), Capture.bSuccess);
    TestEqual(TEXT("Error code is INVALID_PARAMS"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddCustomExpressionValidParamsNoCrashTest,
    "PinWright.material.authoring.add_custom_expression.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddCustomExpressionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = CreateTransientTestMaterial(TEXT("M_CustomNoInputs"));
    TestNotNull(TEXT("Transient test material created"), Material);
    if (!Material)
    {
        return true;
    }

    ON_SCOPE_EXIT
    {
        Material->RemoveFromRoot();
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Material->GetPathName());
    Payload->SetStringField(TEXT("code"), TEXT("return 1.0f;"));
    Payload->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("material.authoring.add_custom_expression"), Payload, Capture));
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded with explicit empty inputs"), Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return true;
    }

    FString CustomNodeId;
    Capture.Result->TryGetStringField(TEXT("nodeId"), CustomNodeId);
    TestFalse(TEXT("Custom expression node ID returned"), CustomNodeId.IsEmpty());

    double InputCount = -1.0;
    Capture.Result->TryGetNumberField(TEXT("inputCount"), InputCount);
    TestEqual(TEXT("Response reports zero configured inputs"), InputCount, 0.0);

    const TArray<TSharedPtr<FJsonValue>>* ResponseInputs = nullptr;
    TestTrue(TEXT("Response includes inputs array"), Capture.Result->TryGetArrayField(TEXT("inputs"), ResponseInputs));
    if (ResponseInputs)
    {
        TestEqual(TEXT("Response inputs array is empty"), ResponseInputs->Num(), 0);
    }

    UMaterialExpressionCustom* CustomExpr =
        Cast<UMaterialExpressionCustom>(FindMaterialExpressionByGuid(Material, CustomNodeId));
    TestNotNull(TEXT("Created custom expression found in material"), CustomExpr);
    if (CustomExpr)
    {
        TestEqual(TEXT("Created custom expression has no leftover default inputs"), CustomExpr->Inputs.Num(), 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddCustomExpressionSupportsMultipleInputsTest,
    "PinWright.material.authoring.add_custom_expression.SupportsMultipleInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddCustomExpressionSupportsMultipleInputsTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = CreateTransientTestMaterial(TEXT("M_CustomInputs"));
    TestNotNull(TEXT("Transient test material created"), Material);
    if (!Material)
    {
        return true;
    }

    ON_SCOPE_EXIT
    {
        Material->RemoveFromRoot();
    };

    const FString AssetPath = Material->GetPathName();

    TSharedPtr<FJsonObject> CustomPayload = MakeShared<FJsonObject>();
    CustomPayload->SetStringField(TEXT("assetPath"), AssetPath);
    CustomPayload->SetStringField(TEXT("code"), TEXT("return First + Second;"));
    CustomPayload->SetNumberField(TEXT("x"), 0.0);
    CustomPayload->SetNumberField(TEXT("y"), 0.0);
    TArray<TSharedPtr<FJsonValue>> Inputs;
    TSharedPtr<FJsonObject> FirstInput = MakeShared<FJsonObject>();
    FirstInput->SetStringField(TEXT("name"), TEXT("First"));
    Inputs.Add(MakeShared<FJsonValueObject>(FirstInput));
    Inputs.Add(MakeShared<FJsonValueString>(TEXT("Second")));
    CustomPayload->SetArrayField(TEXT("inputs"), Inputs);

    FTestResponseCapture CustomCapture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("material.authoring.add_custom_expression"), CustomPayload, CustomCapture));
    TestTrue(TEXT("Custom expression handler responded"), CustomCapture.bWasCalled);
    TestTrue(TEXT("Custom expression handler succeeded"), CustomCapture.bSuccess);
    TestTrue(TEXT("Custom expression result returned"), CustomCapture.Result.IsValid());
    if (!CustomCapture.Result.IsValid())
    {
        return true;
    }

    FString CustomNodeId;
    CustomCapture.Result->TryGetStringField(TEXT("nodeId"), CustomNodeId);
    TestFalse(TEXT("Custom expression node ID returned"), CustomNodeId.IsEmpty());

    UMaterialExpressionCustom* CustomExpr =
        Cast<UMaterialExpressionCustom>(FindMaterialExpressionByGuid(Material, CustomNodeId));
    TestNotNull(TEXT("Created custom expression found in material"), CustomExpr);
    if (!CustomExpr)
    {
        return true;
    }

    double InputCount = -1.0;
    CustomCapture.Result->TryGetNumberField(TEXT("inputCount"), InputCount);
    TestEqual(TEXT("Response reports two configured inputs"), InputCount, 2.0);

    const TArray<TSharedPtr<FJsonValue>>* ResponseInputs = nullptr;
    TestTrue(TEXT("Response includes configured input names"),
        CustomCapture.Result->TryGetArrayField(TEXT("inputs"), ResponseInputs));
    if (ResponseInputs)
    {
        TestEqual(TEXT("Response input array count"), ResponseInputs->Num(), 2);
        if (ResponseInputs->Num() == 2)
        {
            TestEqual(TEXT("Response first input name"), (*ResponseInputs)[0]->AsString(), FString(TEXT("First")));
            TestEqual(TEXT("Response second input name"), (*ResponseInputs)[1]->AsString(), FString(TEXT("Second")));
        }
    }

    TestEqual(TEXT("Custom expression input count"), CustomExpr->Inputs.Num(), 2);
    if (CustomExpr->Inputs.Num() == 2)
    {
        TestEqual(TEXT("First custom input name"), CustomExpr->Inputs[0].InputName.ToString(), FString(TEXT("First")));
        TestEqual(TEXT("Second custom input name"), CustomExpr->Inputs[1].InputName.ToString(), FString(TEXT("Second")));
    }

    TSharedPtr<FJsonObject> ScalarPayload = MakeShared<FJsonObject>();
    ScalarPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ScalarPayload->SetStringField(TEXT("parameterName"), TEXT("InputScalar"));
    ScalarPayload->SetNumberField(TEXT("defaultValue"), 1.0);
    ScalarPayload->SetNumberField(TEXT("x"), 300.0);
    ScalarPayload->SetNumberField(TEXT("y"), 0.0);

    FTestResponseCapture ScalarCapture;
    TestTrue(TEXT("Scalar parameter handler found"),
        InvokeHandlerWithCapture(TEXT("material.authoring.add_scalar_parameter"), ScalarPayload, ScalarCapture));
    TestTrue(TEXT("Scalar parameter handler responded"), ScalarCapture.bWasCalled);
    TestTrue(TEXT("Scalar parameter handler succeeded"), ScalarCapture.bSuccess);
    TestTrue(TEXT("Scalar parameter result returned"), ScalarCapture.Result.IsValid());
    if (!ScalarCapture.Result.IsValid())
    {
        return true;
    }

    FString ScalarNodeId;
    ScalarCapture.Result->TryGetStringField(TEXT("nodeId"), ScalarNodeId);
    TestFalse(TEXT("Scalar parameter node ID returned"), ScalarNodeId.IsEmpty());

    TSharedPtr<FJsonObject> ConnectPayload = MakeShared<FJsonObject>();
    ConnectPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ConnectPayload->SetStringField(TEXT("sourceNodeId"), ScalarNodeId);
    ConnectPayload->SetStringField(TEXT("targetNodeId"), CustomNodeId);
    ConnectPayload->SetStringField(TEXT("inputName"), TEXT("Second"));

    FTestResponseCapture ConnectCapture;
    TestTrue(TEXT("Connect handler found"),
        InvokeHandlerWithCapture(TEXT("material.authoring.connect_nodes"), ConnectPayload, ConnectCapture));
    TestTrue(TEXT("Connect handler responded"), ConnectCapture.bWasCalled);
    TestTrue(TEXT("Connect handler succeeded"), ConnectCapture.bSuccess);

    UMaterialExpressionScalarParameter* ScalarExpr =
        Cast<UMaterialExpressionScalarParameter>(FindMaterialExpressionByGuid(Material, ScalarNodeId));
    TestNotNull(TEXT("Created scalar parameter found in material"), ScalarExpr);
    if (ScalarExpr && CustomExpr->Inputs.Num() == 2)
    {
        TestTrue(TEXT("Dynamic custom input pin connected to scalar parameter"),
            CustomExpr->Inputs[1].Input.Expression == ScalarExpr);
    }
    return true;
}

// ============================================================================
// material.authoring.connect_nodes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringConnectNodesValidParamsNoCrashTest,
    "PinWright.material.authoring.connect_nodes.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringConnectNodesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("sourceNodeId"), TEXT("NodeA"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.connect_nodes"), Payload));
    return true;
}

// ============================================================================
// material.authoring.create_material_function
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCreateMaterialFunctionValidParamsNoCrashTest,
    "PinWright.material.authoring.create_material_function.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringCreateMaterialFunctionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("MF_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.create_material_function"), Payload));
    CleanupTestAsset(TEXT("/Game/Materials/Functions/MF_Test"));
    return true;
}

// ============================================================================
// material.authoring.add_function_input
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddFunctionInputValidParamsNoCrashTest,
    "PinWright.material.authoring.add_function_input.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddFunctionInputValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/Functions/MF_Test"));
    Payload->SetStringField(TEXT("inputName"), TEXT("Color"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_function_input"), Payload));
    CleanupTestAsset(TEXT("/Game/Materials/Functions/MF_Test"));
    return true;
}

namespace
{
// Finds the single UMaterialExpressionFunctionInput on a function whose GUID matches
// the nodeId the handler returned, so the test inspects the exact node it just created.
UMaterialExpressionFunctionInput* FindFunctionInputByNodeId(UMaterialFunction* Function, const FString& NodeId)
{
    if (!Function || NodeId.IsEmpty())
    {
        return nullptr;
    }
    for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
    {
        UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression);
        if (Input && Input->MaterialExpressionGuid.ToString() == NodeId)
        {
            return Input;
        }
    }
    return nullptr;
}
}

// Regression test for E-material-function-input-optional-default: add_function_input must
// default to a REQUIRED input (bUsePreviewValueAsDefault=false), and the new optional /
// defaultValue knobs must toggle bUsePreviewValueAsDefault and populate PreviewValue on the
// production handler. This exercises the real handler against an on-disk UMaterialFunction
// and reads back the engine fields, so it fails if the optionality plumbing were reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddFunctionInputOptionalDefaultTest,
    "PinWright.material.authoring.add_function_input.OptionalAndDefaultValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddFunctionInputOptionalDefaultTest::RunTest(const FString& Parameters)
{
    const FString FunctionPath = FString::Printf(
        TEXT("/Game/PinWrightTests/MF_OptInput_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(FunctionPath);
    };

    // Create a real on-disk material function so the handler's LoadObject<UMaterialFunction> resolves.
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(FunctionPath));
        CreatePayload->SetStringField(TEXT("path"), FPackageName::GetLongPackagePath(FunctionPath));
        CreatePayload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("create_material_function handler found"),
            InvokeHandler(TEXT("material.authoring.create_material_function"), CreatePayload));
    }

    UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *FunctionPath);
    TestNotNull(TEXT("material function asset created"), Function);
    if (!Function)
    {
        return true;
    }

    // Shared invoker: adds one input and returns the FunctionInput node the handler created.
    auto AddInput = [&](const FString& Name, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Decorate)
        -> UMaterialExpressionFunctionInput*
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FunctionPath);
        Payload->SetStringField(TEXT("inputName"), Name);
        Payload->SetStringField(TEXT("inputType"), TEXT("Vector3"));
        Payload->SetNumberField(TEXT("x"), 0.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        Decorate(Payload);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.add_function_input"), Payload, Capture);
        TestTrue(TEXT("add_function_input handler found"), bFound);
        TestTrue(*FString::Printf(TEXT("add_function_input '%s' succeeded"), *Name), Capture.bSuccess);
        FString NodeId;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
        }
        return FindFunctionInputByNodeId(Function, NodeId);
    };

    // 1. Default — required (bUsePreviewValueAsDefault stays false). This is the bug's baseline.
    UMaterialExpressionFunctionInput* Required = AddInput(TEXT("Required"),
        [](const TSharedPtr<FJsonObject>&) {});
    TestNotNull(TEXT("required input node created"), Required);
    if (Required)
    {
        TestFalse(TEXT("default input is required (bUsePreviewValueAsDefault==false)"),
            Required->bUsePreviewValueAsDefault != 0);
    }

    // 2. optional:true — must flip bUsePreviewValueAsDefault to true.
    UMaterialExpressionFunctionInput* Optional = AddInput(TEXT("Optional"),
        [](const TSharedPtr<FJsonObject>& P) { P->SetBoolField(TEXT("optional"), true); });
    TestNotNull(TEXT("optional input node created"), Optional);
    if (Optional)
    {
        TestTrue(TEXT("optional:true sets bUsePreviewValueAsDefault"),
            Optional->bUsePreviewValueAsDefault != 0);
    }

    // 3. defaultValue array — implies optional AND populates PreviewValue.
    UMaterialExpressionFunctionInput* Defaulted = AddInput(TEXT("Defaulted"),
        [](const TSharedPtr<FJsonObject>& P)
        {
            TArray<TSharedPtr<FJsonValue>> Components;
            Components.Add(MakeShared<FJsonValueNumber>(0.25));
            Components.Add(MakeShared<FJsonValueNumber>(0.5));
            Components.Add(MakeShared<FJsonValueNumber>(0.75));
            P->SetArrayField(TEXT("defaultValue"), Components);
        });
    TestNotNull(TEXT("defaultValue input node created"), Defaulted);
    if (Defaulted)
    {
        TestTrue(TEXT("defaultValue implies bUsePreviewValueAsDefault"),
            Defaulted->bUsePreviewValueAsDefault != 0);
        TestEqual(TEXT("PreviewValue.X from defaultValue[0]"), Defaulted->PreviewValue.X, 0.25f);
        TestEqual(TEXT("PreviewValue.Y from defaultValue[1]"), Defaulted->PreviewValue.Y, 0.5f);
        TestEqual(TEXT("PreviewValue.Z from defaultValue[2]"), Defaulted->PreviewValue.Z, 0.75f);
    }

    return true;
}

// ============================================================================
// material.authoring.add_function_output
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddFunctionOutputValidParamsNoCrashTest,
    "PinWright.material.authoring.add_function_output.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddFunctionOutputValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/Functions/MF_Test"));
    Payload->SetStringField(TEXT("inputName"), TEXT("Result"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_function_output"), Payload));
    return true;
}

// ============================================================================
// material.authoring.use_material_function
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringUseMaterialFunctionValidParamsNoCrashTest,
    "PinWright.material.authoring.use_material_function.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringUseMaterialFunctionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("functionPath"), TEXT("/Game/Materials/Functions/MF_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.use_material_function"), Payload));
    return true;
}

// ============================================================================
// material.authoring.create_material_instance
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCreateMaterialInstanceValidParamsNoCrashTest,
    "PinWright.material.authoring.create_material_instance.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringCreateMaterialInstanceValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("MI_Test"));
    Payload->SetStringField(TEXT("parentMaterial"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.create_material_instance"), Payload));
    CleanupTestAsset(TEXT("/Game/Materials/MI_Test"));
    return true;
}

// ============================================================================
// material.authoring.set_scalar_parameter_value
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringSetScalarParameterValueValidParamsNoCrashTest,
    "PinWright.material.authoring.set_scalar_parameter_value.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringSetScalarParameterValueValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/MI_Test"));
    Payload->SetStringField(TEXT("parameterName"), TEXT("Roughness"));
    Payload->SetNumberField(TEXT("value"), 0.5);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.set_scalar_parameter_value"), Payload));
    return true;
}

// ============================================================================
// material.authoring.set_vector_parameter_value
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringSetVectorParameterValueValidParamsNoCrashTest,
    "PinWright.material.authoring.set_vector_parameter_value.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringSetVectorParameterValueValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/MI_Test"));
    Payload->SetStringField(TEXT("parameterName"), TEXT("BaseColor"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.set_vector_parameter_value"), Payload));
    return true;
}

// ============================================================================
// material.authoring.set_texture_parameter_value
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringSetTextureParameterValueValidParamsNoCrashTest,
    "PinWright.material.authoring.set_texture_parameter_value.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringSetTextureParameterValueValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/MI_Test"));
    Payload->SetStringField(TEXT("parameterName"), TEXT("DiffuseMap"));
    Payload->SetStringField(TEXT("texturePath"), TEXT("/Game/Textures/T_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.set_texture_parameter_value"), Payload));
    return true;
}

// ============================================================================
// material.authoring.create_landscape_material
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCreateLandscapeMaterialValidParamsNoCrashTest,
    "PinWright.material.authoring.create_landscape_material.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringCreateLandscapeMaterialValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("M_Landscape"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.create_landscape_material"), Payload));
    CleanupTestAsset(TEXT("/Game/Materials/M_Landscape"));
    return true;
}

// ============================================================================
// material.authoring.create_decal_material
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCreateDecalMaterialValidParamsNoCrashTest,
    "PinWright.material.authoring.create_decal_material.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringCreateDecalMaterialValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("M_Decal"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.create_decal_material"), Payload));
    CleanupTestAsset(TEXT("/Game/Materials/M_Decal"));
    return true;
}

// ============================================================================
// material.authoring.create_post_process_material
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCreatePostProcessMaterialValidParamsNoCrashTest,
    "PinWright.material.authoring.create_post_process_material.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringCreatePostProcessMaterialValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("M_PostProcess"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.create_post_process_material"), Payload));
    CleanupTestAsset(TEXT("/Game/Materials/M_PostProcess"));
    return true;
}

// ============================================================================
// material.authoring.add_landscape_layer
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddLandscapeLayerValidParamsNoCrashTest,
    "PinWright.material.authoring.add_landscape_layer.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddLandscapeLayerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("layerName"), TEXT("Grass"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_landscape_layer"), Payload));
    // The handler saves the layer info asset to disk; remove it so the run leaves no artifact.
    CleanupTestAsset(TEXT("/Game/Landscape/Layers/Grass"));
    return true;
}

// ============================================================================
// material.authoring.configure_layer_blend
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringConfigureLayerBlendValidParamsNoCrashTest,
    "PinWright.material.authoring.configure_layer_blend.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringConfigureLayerBlendValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Landscape"));
    // "layers" is a required array; supply an empty array to exercise the handler path.
    TArray<TSharedPtr<FJsonValue>> EmptyLayers;
    Payload->SetArrayField(TEXT("layers"), EmptyLayers);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.configure_layer_blend"), Payload));
    return true;
}

// ============================================================================
// material.authoring.compile_material  (single required param: assetPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCompileMaterialValidParamsNoCrashTest,
    "PinWright.material.authoring.compile_material.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringCompileMaterialValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.compile_material"), Payload));
    return true;
}

// ============================================================================
// material.authoring.get_material_info
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringGetMaterialInfoValidParamsNoCrashTest,
    "PinWright.material.authoring.get_material_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringGetMaterialInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.get_material_info"), Payload));
    return true;
}

// ============================================================================
// material.authoring.get_material_node_details
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringGetMaterialNodeDetailsValidParamsNoCrashTest,
    "PinWright.material.authoring.get_material_node_details.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringGetMaterialNodeDetailsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("nodeId"), TEXT("some-guid"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.get_material_node_details"), Payload));
    return true;
}

// ============================================================================
// material.authoring.set_two_sided
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringSetTwoSidedValidParamsNoCrashTest,
    "PinWright.material.authoring.set_two_sided.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringSetTwoSidedValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetBoolField(TEXT("twoSided"), true);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.set_two_sided"), Payload));
    return true;
}

// ============================================================================
// Authoring — no-required-param node helpers (single required: assetPath)
// add_texture_coordinate, add_world_position, add_vertex_normal, add_pixel_depth,
// add_fresnel, add_reflection_vector, add_panner, add_rotator,
// add_noise, add_voronoi, add_if, add_switch, add_component_mask,
// add_dot_product, add_cross_product, add_desaturation, add_append
// Each has one MissingRequiredParam test (empty payload) and one ValidParamsNoCrash.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddTextureCoordinateValidParamsNoCrashTest,
    "PinWright.material.authoring.add_texture_coordinate.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddTextureCoordinateValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_texture_coordinate"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddWorldPositionValidParamsNoCrashTest,
    "PinWright.material.authoring.add_world_position.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddWorldPositionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_world_position"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddVertexNormalValidParamsNoCrashTest,
    "PinWright.material.authoring.add_vertex_normal.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddVertexNormalValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_vertex_normal"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddPixelDepthValidParamsNoCrashTest,
    "PinWright.material.authoring.add_pixel_depth.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddPixelDepthValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_pixel_depth"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddFresnelValidParamsNoCrashTest,
    "PinWright.material.authoring.add_fresnel.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddFresnelValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_fresnel"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddFresnelCreatesExpectedClassTest,
    "PinWright.material.authoring.add_fresnel.CreatesExpectedClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddFresnelCreatesExpectedClassTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = CreateTransientTestMaterial(TEXT("M_FresnelShim"));
    TestNotNull(TEXT("Transient test material created"), Material);
    if (!Material)
    {
        return true;
    }

    ON_SCOPE_EXIT
    {
        Material->RemoveFromRoot();
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Material->GetPathName());
    Payload->SetNumberField(TEXT("x"), 128.0);
    Payload->SetNumberField(TEXT("y"), 256.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("material.authoring.add_fresnel"), Payload, Capture));
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return true;
    }

    FString NodeId;
    Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
    UMaterialExpressionFresnel* Fresnel =
        Cast<UMaterialExpressionFresnel>(FindMaterialExpressionByGuid(Material, NodeId));
    TestNotNull(TEXT("Fresnel expression created"), Fresnel);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddReflectionVectorValidParamsNoCrashTest,
    "PinWright.material.authoring.add_reflection_vector.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddReflectionVectorValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_reflection_vector"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddPannerValidParamsNoCrashTest,
    "PinWright.material.authoring.add_panner.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddPannerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_panner"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddRotatorValidParamsNoCrashTest,
    "PinWright.material.authoring.add_rotator.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddRotatorValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_rotator"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddNoiseValidParamsNoCrashTest,
    "PinWright.material.authoring.add_noise.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddNoiseValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_noise"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddVoronoiValidParamsNoCrashTest,
    "PinWright.material.authoring.add_voronoi.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddVoronoiValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_voronoi"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddIfValidParamsNoCrashTest,
    "PinWright.material.authoring.add_if.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddIfValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_if"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddSwitchValidParamsNoCrashTest,
    "PinWright.material.authoring.add_switch.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddSwitchValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_switch"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddComponentMaskValidParamsNoCrashTest,
    "PinWright.material.authoring.add_component_mask.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddComponentMaskValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_component_mask"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddDotProductValidParamsNoCrashTest,
    "PinWright.material.authoring.add_dot_product.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddDotProductValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_dot_product"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddCrossProductValidParamsNoCrashTest,
    "PinWright.material.authoring.add_cross_product.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddCrossProductValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_cross_product"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddDesaturationValidParamsNoCrashTest,
    "PinWright.material.authoring.add_desaturation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddDesaturationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_desaturation"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAddAppendValidParamsNoCrashTest,
    "PinWright.material.authoring.add_append.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringAddAppendValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.authoring.add_append"), Payload));
    return true;
}

// ============================================================================
// material.graph.add_node  (required: assetPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphAddNodeValidParamsNoCrashTest,
    "PinWright.material.graph.add_node.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphAddNodeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("nodeType"), TEXT("Add"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.graph.add_node"), Payload));
    return true;
}

// ============================================================================
// material.graph.remove_node  (required: assetPath, nodeId)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphRemoveNodeValidParamsNoCrashTest,
    "PinWright.material.graph.remove_node.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphRemoveNodeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("nodeId"), TEXT("some-guid"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.graph.remove_node"), Payload));
    return true;
}

// ============================================================================
// material.graph.connect_nodes  (required: assetPath, sourceNodeId, inputName)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphConnectNodesValidParamsNoCrashTest,
    "PinWright.material.graph.connect_nodes.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphConnectNodesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("sourceNodeId"), TEXT("NodeA"));
    Payload->SetStringField(TEXT("inputName"), TEXT("BaseColor"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.graph.connect_nodes"), Payload));
    return true;
}

// ============================================================================
// material.graph.break_connections  (required: assetPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphBreakConnectionsValidParamsNoCrashTest,
    "PinWright.material.graph.break_connections.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphBreakConnectionsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.graph.break_connections"), Payload));
    return true;
}

// ============================================================================
// material.graph.get_node_details  (required: assetPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphGetNodeDetailsValidParamsNoCrashTest,
    "PinWright.material.graph.get_node_details.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphGetNodeDetailsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Materials/M_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.graph.get_node_details"), Payload));
    return true;
}

// ============================================================================
// material.graph.add_texture_sample  (required: materialPath, texturePath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphAddTextureSampleValidParamsNoCrashTest,
    "PinWright.material.graph.add_texture_sample.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphAddTextureSampleValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("materialPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("texturePath"), TEXT("/Game/Textures/T_Test"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.graph.add_texture_sample"), Payload));
    return true;
}

// ============================================================================
// material.graph.add_expression  (required: materialPath, expressionClass)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphAddExpressionValidParamsNoCrashTest,
    "PinWright.material.graph.add_expression.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphAddExpressionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("materialPath"), TEXT("/Game/Materials/M_Test"));
    Payload->SetStringField(TEXT("expressionClass"), TEXT("Add"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.graph.add_expression"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphAddExpressionAppliesPropertiesTest,
    "PinWright.material.graph.add_expression.AppliesProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphAddExpressionAppliesPropertiesTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = CreateTransientTestMaterial(TEXT("M_GraphExpressionProperties"));
    TestNotNull(TEXT("Transient test material created"), Material);
    if (!Material)
    {
        return true;
    }

    ON_SCOPE_EXIT
    {
        Material->RemoveFromRoot();
    };

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("ParameterName"), TEXT("FactoryScalar"));
    Properties->SetNumberField(TEXT("DefaultValue"), 0.25);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("materialPath"), Material->GetPathName());
    Payload->SetStringField(TEXT("expressionClass"), TEXT("ScalarParameter"));
    Payload->SetObjectField(TEXT("properties"), Properties);
    Payload->SetNumberField(TEXT("x"), 128.0);
    Payload->SetNumberField(TEXT("y"), 256.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("material.graph.add_expression"), Payload, Capture));
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return true;
    }

    FString NodeId;
    Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
    TestFalse(TEXT("Expression node ID returned"), NodeId.IsEmpty());

    UMaterialExpressionScalarParameter* ScalarExpr =
        Cast<UMaterialExpressionScalarParameter>(FindMaterialExpressionByGuid(Material, NodeId));
    TestNotNull(TEXT("Created scalar parameter found in material"), ScalarExpr);
    if (ScalarExpr)
    {
        TestEqual(TEXT("ParameterName property applied"),
            ScalarExpr->ParameterName.ToString(), FString(TEXT("FactoryScalar")));
        TestEqual(TEXT("DefaultValue property applied"), ScalarExpr->DefaultValue, 0.25f);
        TestEqual(TEXT("Graph X position applied"), ScalarExpr->MaterialExpressionEditorX, 128);
        TestEqual(TEXT("Graph Y position applied"), ScalarExpr->MaterialExpressionEditorY, 256);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphAddExpressionRejectsUnknownPropertyTest,
    "PinWright.material.graph.add_expression.RejectsUnknownProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphAddExpressionRejectsUnknownPropertyTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = CreateTransientTestMaterial(TEXT("M_GraphExpressionBadProperty"));
    TestNotNull(TEXT("Transient test material created"), Material);
    if (!Material)
    {
        return true;
    }

    ON_SCOPE_EXIT
    {
        Material->RemoveFromRoot();
    };

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("NotARealProperty"), TEXT("value"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("materialPath"), Material->GetPathName());
    Payload->SetStringField(TEXT("expressionClass"), TEXT("ScalarParameter"));
    Payload->SetObjectField(TEXT("properties"), Properties);
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("material.graph.add_expression"), Payload, Capture));
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("Handler rejected unknown property"), Capture.bSuccess);
    TestEqual(TEXT("Error code is PROPERTY_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("PROPERTY_NOT_FOUND")));
    TestEqual(TEXT("No expression added after property failure"), Material->GetExpressions().Num(), 0);
    return true;
}

// ============================================================================
// material.graph.create_nodes  (required: materialPath, nodes)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphCreateNodesValidParamsNoCrashTest,
    "PinWright.material.graph.create_nodes.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphCreateNodesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("materialPath"), TEXT("/Game/Materials/M_Test"));
    // Provide a valid (empty) array for "nodes" to reach deeper handler logic.
    TArray<TSharedPtr<FJsonValue>> EmptyNodes;
    Payload->SetArrayField(TEXT("nodes"), EmptyNodes);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("material.graph.create_nodes"), Payload));
    return true;
}

// ============================================================================
// TextureHandler — all 27 handlers use RPC_NO_PARAMS, so one ValidParamsNoCrash each.
// A single representative group of tests covers their uniform dispatch path.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateNoiseTextureValidParamsNoCrashTest,
    "PinWright.texture.create_noise_texture.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateNoiseTextureValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.create_noise_texture"), Payload));
    // Empty payload defaults the asset name to "Asset" and saves it; clean it up.
    CleanupTestAsset(TEXT("/Game/Textures/Asset"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateGradientTextureValidParamsNoCrashTest,
    "PinWright.texture.create_gradient_texture.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateGradientTextureValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.create_gradient_texture"), Payload));
    // Empty payload defaults the asset name to "Asset" and saves it; clean it up.
    CleanupTestAsset(TEXT("/Game/Textures/Asset"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreatePatternTextureValidParamsNoCrashTest,
    "PinWright.texture.create_pattern_texture.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreatePatternTextureValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.create_pattern_texture"), Payload));
    // Empty payload defaults the asset name to "Asset" and saves it; clean it up.
    CleanupTestAsset(TEXT("/Game/Textures/Asset"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateNormalFromHeightValidParamsNoCrashTest,
    "PinWright.texture.create_normal_from_height.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateNormalFromHeightValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.create_normal_from_height"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetCompressionSettingsValidParamsNoCrashTest,
    "PinWright.texture.set_compression_settings.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetCompressionSettingsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.set_compression_settings"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetTextureGroupValidParamsNoCrashTest,
    "PinWright.texture.set_texture_group.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetTextureGroupValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.set_texture_group"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetLodBiasValidParamsNoCrashTest,
    "PinWright.texture.set_lod_bias.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetLodBiasValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.set_lod_bias"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureConfigureVirtualTextureValidParamsNoCrashTest,
    "PinWright.texture.configure_virtual_texture.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureConfigureVirtualTextureValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.configure_virtual_texture"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetStreamingPriorityValidParamsNoCrashTest,
    "PinWright.texture.set_streaming_priority.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetStreamingPriorityValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.set_streaming_priority"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureGetTextureInfoValidParamsNoCrashTest,
    "PinWright.texture.get_texture_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureGetTextureInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.get_texture_info"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureResizeTextureValidParamsNoCrashTest,
    "PinWright.texture.resize_texture.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureResizeTextureValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.resize_texture"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureInvertValidParamsNoCrashTest,
    "PinWright.texture.invert.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureInvertValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.invert"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureDesaturateValidParamsNoCrashTest,
    "PinWright.texture.desaturate.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureDesaturateValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.desaturate"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureAdjustLevelsValidParamsNoCrashTest,
    "PinWright.texture.adjust_levels.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureAdjustLevelsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.adjust_levels"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureBlurValidParamsNoCrashTest,
    "PinWright.texture.blur.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureBlurValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.blur"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSharpenValidParamsNoCrashTest,
    "PinWright.texture.sharpen.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSharpenValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.sharpen"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureChannelPackValidParamsNoCrashTest,
    "PinWright.texture.channel_pack.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureChannelPackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.channel_pack"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCombineTexturesValidParamsNoCrashTest,
    "PinWright.texture.combine_textures.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCombineTexturesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.combine_textures"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureAdjustCurvesValidParamsNoCrashTest,
    "PinWright.texture.adjust_curves.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureAdjustCurvesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.adjust_curves"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureChannelExtractValidParamsNoCrashTest,
    "PinWright.texture.channel_extract.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureChannelExtractValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.channel_extract"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetTextureFilterValidParamsNoCrashTest,
    "PinWright.texture.set_texture_filter.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetTextureFilterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.set_texture_filter"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetTextureWrapValidParamsNoCrashTest,
    "PinWright.texture.set_texture_wrap.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetTextureWrapValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.set_texture_wrap"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateRenderTargetValidParamsNoCrashTest,
    "PinWright.texture.create_render_target.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateRenderTargetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("texture.create_render_target"), Payload));
    return true;
}

// Regression for B-create-render-target-false-collision: texture.create_render_target
// must accept a brand-new, never-existing path and actually create the asset. The old
// collision guard probed for a prior asset with StaticLoadObject followed by
// FindPackage(nullptr, *FullPath); the failed load left an empty in-memory UPackage that
// FindPackage then tripped on, so EVERY new path was rejected with "Asset with this name
// already exists" and nothing was created. The fix replaces that double-guard with a
// registry-only UEditorAssetLibrary::DoesAssetExist check (no load side effect).
// Reverting the fix makes this test fail: the create returns an error (bSuccess=false)
// for a path that does not exist, and DoesAssetExist stays false afterward.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateRenderTargetNewPathSucceedsTest,
    "PinWright.texture.create_render_target.NewPathSucceeds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateRenderTargetNewPathSucceedsTest::RunTest(const FString& Parameters)
{
    const FString Folder = TEXT("/Game/PinWrightTests/RenderTargets");
    const FString AssetName = FString::Printf(TEXT("RT_NewPath_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString FullPath = Folder / AssetName;

    // Clean up the asset this test writes to disk regardless of outcome (the GUID name
    // already guarantees a brand-new path, so no pre-create cleanup is needed).
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    TestFalse(TEXT("target path is absent before create"),
        UEditorAssetLibrary::DoesAssetExist(FullPath));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetNumberField(TEXT("width"), 64);
    Payload->SetNumberField(TEXT("height"), 64);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("texture.create_render_target"), Payload, Capture));

    // The self-inflicted false collision rejected brand-new paths; the fix must let a
    // never-existing path through to a real create.
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("create_render_target succeeds on a brand-new path — rejected new path %s: [%s] %s"),
            *FullPath, *Capture.ErrorCode, *Capture.Message));
    }
    else if (Capture.Result.IsValid())
    {
        FString AssetPath;
        TestTrue(TEXT("response carries assetPath"),
            Capture.Result->TryGetStringField(TEXT("assetPath"), AssetPath));
    }

    // The render target must actually exist after a successful create.
    TestTrue(TEXT("render target exists after create"),
        UEditorAssetLibrary::DoesAssetExist(FullPath));

    return true;
}

// ============================================================================
// Response Capture: material.authoring.create_material with missing "name"
// Demonstrates InvokeHandlerWithCapture() pattern for verifying error shape.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateMaterialCaptureErrorTest,
    "PinWright.material.authoring.create_material.CaptureErrorResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialCreateMaterialCaptureErrorTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Omit every name source (name / assetName / assetPath) so the create-verb's
    // destination resolver (MaterialCreatePathParamUtils::ResolveCreateNameAndFolder)
    // hits its no-name-source failure path. It emits MISSING_REQUIRED_PARAM — the same
    // vocabulary the dispatcher's ValidateHandlerParams emits for this missing required
    // slot on the wire (E-material-create-combined-assetpath-split). This bypasses the
    // dispatcher, so the captured code is the handler body's own, not the wire-level one.
    bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.create_material"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error (missing name)"), Capture.bSuccess);
    TestEqual(TEXT("Error code is MISSING_REQUIRED_PARAM"),
        Capture.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    return true;
}
