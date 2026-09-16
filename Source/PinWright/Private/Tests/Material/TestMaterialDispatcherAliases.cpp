// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-material-editor-param-name-drift.
// Verify the dispatcher accepts the shared aliases on the material asset-path slot
// (assetPath / materialPath / path), the expression-class slot (expressionClass /
// nodeType / className) and the editor toggle slot (realtime / enabled), AND that the
// handler bodies read the value through those aliases end-to-end.
//
// Each test routes through FRpcDispatcher::ProcessRequest with a real transport so it
// exercises ValidateHandlerParams (the alias machinery) and the production handler body.
// Counterfactual: removing an alias makes validation reject the request with
// MISSING_REQUIRED_PARAM / UNKNOWN_PARAMS and the body never runs.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // Create a loadable in-memory UMaterial under /Game/__PW_GatewayTests and return its path.
    FString MakeTestMaterial(UMaterial*& OutMaterial)
    {
        const FString AssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/MatAlias_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*AssetPath);
        if (!Pkg)
        {
            OutMaterial = nullptr;
            return AssetPath;
        }

        OutMaterial = NewObject<UMaterial>(
            Pkg,
            FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
            RF_Public | RF_Standalone);
        if (OutMaterial)
        {
            FAssetRegistryModule::AssetCreated(OutMaterial);
        }
        return AssetPath;
    }

    int32 CountScalarParams(UMaterial* Material)
    {
        if (!Material || !Material->GetEditorOnlyData())
        {
            return 0;
        }
        int32 Count = 0;
        for (UMaterialExpression* Expr : Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
        {
            if (Cast<UMaterialExpressionScalarParameter>(Expr))
            {
                ++Count;
            }
        }
        return Count;
    }

    int32 CountAddExpressions(UMaterial* Material)
    {
        if (!Material || !Material->GetEditorOnlyData())
        {
            return 0;
        }
        int32 Count = 0;
        for (UMaterialExpression* Expr : Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
        {
            if (Cast<UMaterialExpressionAdd>(Expr))
            {
                ++Count;
            }
        }
        return Count;
    }

    void DeleteTestMaterial(UMaterial* Material)
    {
        if (Material)
        {
            Material->ClearFlags(RF_Public | RF_Standalone);
            Material->MarkAsGarbage();
        }
    }
}

// 1. add_scalar_parameter accepts the optional `path` alias for the assetPath slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAddScalarParameterAcceptsPathAliasTest,
    "PinWright.material.aliases.AddScalarParameterAcceptsPathAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAddScalarParameterAcceptsPathAliasTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = nullptr;
    const FString AssetPath = MakeTestMaterial(Material);
    if (!TestNotNull(TEXT("Test material created"), Material))
    {
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("path"), AssetPath);
    Params->SetStringField(TEXT("parameterName"), TEXT("Roughness"));
    Params->SetNumberField(TEXT("x"), 0.0);
    Params->SetNumberField(TEXT("y"), 0.0);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("material.authoring.add_scalar_parameter"),
        TEXT("req-path-alias"), Params, bSuccess, ErrorCode);

    TestTrue(TEXT("path alias accepted (handler succeeded)"), bSuccess);
    TestEqual(TEXT("no error code"), ErrorCode, FString());
    TestEqual(TEXT("scalar parameter added via path alias"), CountScalarParams(Material), 1);

    DeleteTestMaterial(Material);
    return true;
}

// 2. add_scalar_parameter accepts the `materialPath` alias for the assetPath slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAddScalarParameterAcceptsMaterialPathAliasTest,
    "PinWright.material.aliases.AddScalarParameterAcceptsMaterialPathAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAddScalarParameterAcceptsMaterialPathAliasTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = nullptr;
    const FString AssetPath = MakeTestMaterial(Material);
    if (!TestNotNull(TEXT("Test material created"), Material))
    {
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("materialPath"), AssetPath);
    Params->SetStringField(TEXT("parameterName"), TEXT("Metallic"));
    Params->SetNumberField(TEXT("x"), 0.0);
    Params->SetNumberField(TEXT("y"), 0.0);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("material.authoring.add_scalar_parameter"),
        TEXT("req-materialpath-alias"), Params, bSuccess, ErrorCode);

    TestTrue(TEXT("materialPath alias accepted (handler succeeded)"), bSuccess);
    TestEqual(TEXT("no error code"), ErrorCode, FString());
    TestEqual(TEXT("scalar parameter added via materialPath alias"), CountScalarParams(Material), 1);

    DeleteTestMaterial(Material);
    return true;
}

// 3. material.graph.add_expression accepts the `assetPath` alias for the materialPath
//    slot plus the `nodeType` alias for the expressionClass slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphAddExpressionAcceptsNodeTypeAliasTest,
    "PinWright.material.aliases.GraphAddExpressionAcceptsNodeTypeAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphAddExpressionAcceptsNodeTypeAliasTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = nullptr;
    const FString AssetPath = MakeTestMaterial(Material);
    if (!TestNotNull(TEXT("Test material created"), Material))
    {
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Canonical asset-path alias for this RPC (`assetPath`) + nodeType for the class slot.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("assetPath"), AssetPath);
    Params->SetStringField(TEXT("nodeType"), TEXT("Add"));
    Params->SetNumberField(TEXT("x"), 0.0);
    Params->SetNumberField(TEXT("y"), 0.0);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("material.graph.add_expression"),
        TEXT("req-nodetype-alias"), Params, bSuccess, ErrorCode);

    TestTrue(TEXT("assetPath + nodeType aliases accepted (handler succeeded)"), bSuccess);
    TestEqual(TEXT("no error code"), ErrorCode, FString());
    TestEqual(TEXT("Add expression created via aliases"), CountAddExpressions(Material), 1);

    DeleteTestMaterial(Material);
    return true;
}

// 4. material.graph.add_expression accepts the canonical materialPath + expressionClass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphAddExpressionAcceptsExpressionClassAliasTest,
    "PinWright.material.aliases.GraphAddExpressionAcceptsExpressionClassAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialGraphAddExpressionAcceptsExpressionClassAliasTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = nullptr;
    const FString AssetPath = MakeTestMaterial(Material);
    if (!TestNotNull(TEXT("Test material created"), Material))
    {
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("materialPath"), AssetPath);
    Params->SetStringField(TEXT("expressionClass"), TEXT("MaterialExpressionAdd"));
    Params->SetNumberField(TEXT("x"), 0.0);
    Params->SetNumberField(TEXT("y"), 0.0);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("material.graph.add_expression"),
        TEXT("req-expressionclass-alias"), Params, bSuccess, ErrorCode);

    TestTrue(TEXT("materialPath + expressionClass accepted (handler succeeded)"), bSuccess);
    TestEqual(TEXT("no error code"), ErrorCode, FString());
    TestEqual(TEXT("Add expression created via canonical class slot"), CountAddExpressions(Material), 1);

    DeleteTestMaterial(Material);
    return true;
}

// 5. editor.set_viewport_realtime accepts the `enabled` alias for the realtime slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetViewportRealtimeAcceptsEnabledAliasTest,
    "PinWright.editor.aliases.SetViewportRealtimeAcceptsEnabledAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetViewportRealtimeAcceptsEnabledAliasTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetBoolField(TEXT("enabled"), true);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("editor.set_viewport_realtime"),
        TEXT("req-realtime-enabled"), Params, bSuccess, ErrorCode);

    // Counterfactual: without the alias the param is rejected as UNKNOWN_PARAMS [enabled].
    TestTrue(TEXT("enabled alias accepted (handler succeeded)"), bSuccess);
    TestEqual(TEXT("no error code"), ErrorCode, FString());

    return true;
}

// 6. editor.set_game_view accepts the `realtime` alias for the enabled slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetGameViewAcceptsRealtimeAliasTest,
    "PinWright.editor.aliases.SetGameViewAcceptsRealtimeAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetGameViewAcceptsRealtimeAliasTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetBoolField(TEXT("realtime"), false);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("editor.set_game_view"),
        TEXT("req-gameview-realtime"), Params, bSuccess, ErrorCode);

    // Counterfactual: without the alias the param is rejected as UNKNOWN_PARAMS [realtime].
    TestTrue(TEXT("realtime alias accepted (handler succeeded)"), bSuccess);
    TestEqual(TEXT("no error code"), ErrorCode, FString());

    return true;
}
