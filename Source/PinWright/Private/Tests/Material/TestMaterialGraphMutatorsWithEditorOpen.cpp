// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-material-graph-mutators-bypass-editor-open-guard.
//
// material.graph mutators must reject edits while FMaterialEditor owns the graph
// working copy; otherwise direct LoadObject mutations can be clobbered on editor sync/save.
//
// Counterfactual: if the shared guard or any graph call site is reverted, the
// corresponding mutator returns success and changes graph state while the editor is open.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Handlers/Material/MainInputBindings.h"
#include "Handlers/Material/MaterialFinders.h"
#include "Tests/Material/MaterialEditorOpenTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Material/MaterialExpressionFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Engine/Texture.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
    constexpr const TCHAR* DefaultTexturePath = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");

    TSharedPtr<FJsonObject> MakeAddNodePayload(const FString& ObjectPath, double X, double Y)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("nodeType"), TEXT("Constant"));
        Payload->SetNumberField(TEXT("x"), X);
        Payload->SetNumberField(TEXT("y"), Y);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeRemoveNodePayload(const FString& ObjectPath, const FString& NodeId)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("nodeId"), NodeId);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeConnectNodesPayload(const FString& ObjectPath, const FString& SourceNodeId)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("Roughness"));
        Payload->SetNumberField(TEXT("sourceOutputIndex"), 0.0);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeBreakConnectionsPayload(const FString& ObjectPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("nodeId"), TEXT("Main"));
        Payload->SetStringField(TEXT("pinName"), TEXT("BaseColor"));
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeAddTextureSamplePayload(const FString& ObjectPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("materialPath"), ObjectPath);
        Payload->SetStringField(TEXT("texturePath"), DefaultTexturePath);
        Payload->SetNumberField(TEXT("x"), 300.0);
        Payload->SetNumberField(TEXT("y"), 300.0);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeAddExpressionPayload(const FString& ObjectPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("materialPath"), ObjectPath);
        Payload->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
        Payload->SetNumberField(TEXT("x"), 400.0);
        Payload->SetNumberField(TEXT("y"), 400.0);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeCreateNodesPayload(const FString& ObjectPath)
    {
        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("type"), TEXT("Constant"));
        Node->SetNumberField(TEXT("x"), 500.0);
        Node->SetNumberField(TEXT("y"), 500.0);

        TArray<TSharedPtr<FJsonValue>> Nodes;
        Nodes.Add(MakeShared<FJsonValueObject>(Node));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("materialPath"), ObjectPath);
        Payload->SetArrayField(TEXT("nodes"), Nodes);
        return Payload;
    }

    using MaterialEditorOpenTestHelpers::ExpressionCount;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphMutatorsRejectEditorOpenTest,
    "PinWright.material.graph.mutators.reject-editor-open",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphMutatorsRejectEditorOpenTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // On UE 5.4, opening the Material Editor via editor.open_asset in a headless
    // (-RenderOffscreen) run crashes a background compile worker
    // (ensure SkeletonCompiledBlueprints.Num() == 1, BlueprintCompilationManager.cpp:361 →
    // EXCEPTION_ACCESS_VIOLATION). The test requires the editor to open; skip on 5.4.
    // 5.5+ is stable. See TestMaterialGraphEditWithEditorOpen.cpp for the same guard.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipped on UE 5.4: opening the Material Editor headlessly crashes a background compile worker."));
    return true;
#else
    const FString AssetName = FString::Printf(
        TEXT("GraphMutatorsWhileOpen_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    UPackage* Pkg = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UMaterial* Material = NewObject<UMaterial>(
        Pkg, FName(*AssetName), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(PackagePath);
        return true;
    }

    auto CreateConstant = [this, Material](const TCHAR* Label, float X, float Y) -> UMaterialExpression*
    {
        FCreateResult Result = FMaterialExpressionFactory::Create(
            Material, UMaterialExpressionConstant::StaticClass(), nullptr, FVector2D(X, Y));
        TestTrue(FString::Printf(TEXT("%s fixture expression created"), Label), Result.IsSuccess());
        TestNotNull(FString::Printf(TEXT("%s fixture expression valid"), Label), Result.Expression);
        return Result.Expression;
    };

    UMaterialExpression* RemoveExpr = CreateConstant(TEXT("remove"), 0.0f, 0.0f);
    UMaterialExpression* SourceExpr = CreateConstant(TEXT("connect"), 100.0f, 0.0f);
    UMaterialExpression* BreakExpr = CreateConstant(TEXT("break"), 200.0f, 0.0f);
    if (!RemoveExpr || !SourceExpr || !BreakExpr)
    {
        CleanupTestAsset(PackagePath);
        return true;
    }

    FExpressionInput* BaseColorInput =
        PinWright::Material::ResolveMainInput(Material->GetEditorOnlyData(), TEXT("BaseColor"));
    FExpressionInput* RoughnessInput =
        PinWright::Material::ResolveMainInput(Material->GetEditorOnlyData(), TEXT("Roughness"));
    if (!TestNotNull(TEXT("BaseColor input resolved"), BaseColorInput)
        || !TestNotNull(TEXT("Roughness input resolved"), RoughnessInput))
    {
        CleanupTestAsset(PackagePath);
        return true;
    }

    BaseColorInput->Expression = BreakExpr;
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    const FString PackageFile = FPackageName::LongPackageNameToFilename(
        PackagePath, FPackageName::GetAssetPackageExtension());
    Pkg->MarkPackageDirty();
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!TestTrue(TEXT("Material saved to disk"),
            UPackage::SavePackage(Pkg, Material, *PackageFile, SaveArgs)))
    {
        CleanupTestAsset(PackagePath);
        return true;
    }

    if (!TestNotNull(TEXT("DefaultTexture available"),
            LoadObject<UTexture>(nullptr, DefaultTexturePath)))
    {
        CleanupTestAsset(PackagePath);
        return true;
    }

    const int32 BaselineCount = ExpressionCount(Material);

    TSharedPtr<FJsonObject> OpenPayload = MakeShared<FJsonObject>();
    OpenPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture OpenCapture;
    TestTrue(TEXT("editor.open_asset handler found"),
        InvokeHandlerWithCapture(TEXT("editor.open_asset"), OpenPayload, OpenCapture));

    if (TestTrue(TEXT("Material Editor is registered open after editor.open_asset"),
            PinWright::Material::IsMaterialEditorOpen(Material)))
    {
        auto ExpectEditorOpen = [this, Material, BaselineCount](
            const TCHAR* MethodName, const TSharedPtr<FJsonObject>& Payload)
        {
            FTestResponseCapture Capture;
            TestTrue(FString::Printf(TEXT("%s handler found"), MethodName),
                InvokeHandlerWithCapture(MethodName, Payload, Capture));
            TestTrue(FString::Printf(TEXT("%s responded"), MethodName), Capture.bWasCalled);
            TestFalse(FString::Printf(TEXT("%s rejected while editor open"), MethodName), Capture.bSuccess);
            TestEqual(FString::Printf(TEXT("%s error code is EDITOR_OPEN"), MethodName),
                Capture.ErrorCode, FString(TEXT("EDITOR_OPEN")));
            TestEqual(FString::Printf(TEXT("%s left expression count unchanged"), MethodName),
                ExpressionCount(Material), BaselineCount);
        };

        ExpectEditorOpen(
            TEXT("material.graph.add_node"),
            MakeAddNodePayload(ObjectPath, 10.0, 10.0));
        ExpectEditorOpen(
            TEXT("material.graph.remove_node"),
            MakeRemoveNodePayload(ObjectPath, RemoveExpr->MaterialExpressionGuid.ToString()));
        ExpectEditorOpen(
            TEXT("material.graph.connect_nodes"),
            MakeConnectNodesPayload(ObjectPath, SourceExpr->MaterialExpressionGuid.ToString()));
        TestTrue(TEXT("connect_nodes left Roughness disconnected"),
            RoughnessInput->Expression == nullptr);
        ExpectEditorOpen(
            TEXT("material.graph.break_connections"),
            MakeBreakConnectionsPayload(ObjectPath));
        TestTrue(TEXT("break_connections left BaseColor connected"),
            BaseColorInput->Expression == BreakExpr);
        ExpectEditorOpen(
            TEXT("material.graph.add_texture_sample"),
            MakeAddTextureSamplePayload(ObjectPath));
        ExpectEditorOpen(
            TEXT("material.graph.add_expression"),
            MakeAddExpressionPayload(ObjectPath));
        ExpectEditorOpen(
            TEXT("material.graph.create_nodes"),
            MakeCreateNodesPayload(ObjectPath));

        TestEqual(TEXT("no graph mutator changed expression count while editor open"),
            ExpressionCount(Material), BaselineCount);
    }

    TSharedPtr<FJsonObject> ClosePayload = MakeShared<FJsonObject>();
    ClosePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture CloseCapture;
    TestTrue(TEXT("editor.close_asset handler found"),
        InvokeHandlerWithCapture(TEXT("editor.close_asset"), ClosePayload, CloseCapture));
    TestFalse(TEXT("editor reports closed"),
        PinWright::Material::IsMaterialEditorOpen(Material));

    FTestResponseCapture SuccessCapture;
    TestTrue(TEXT("add_node handler found after closing editor"),
        InvokeHandlerWithCapture(
            TEXT("material.graph.add_node"),
            MakeAddNodePayload(ObjectPath, 600.0, 600.0),
            SuccessCapture));
    if (TestTrue(TEXT("editor-closed add_node succeeds"), SuccessCapture.bSuccess)
        && TestTrue(TEXT("success response has a result"), SuccessCapture.Result.IsValid()))
    {
        FString NodeId;
        TestTrue(TEXT("response carries a nodeId"),
            SuccessCapture.Result->TryGetStringField(TEXT("nodeId"), NodeId));
        TestFalse(TEXT("nodeId is non-empty"), NodeId.IsEmpty());
    }
    TestEqual(TEXT("expression count increased by one after editor close"),
        ExpressionCount(Material), BaselineCount + 1);

    CleanupTestAsset(PackagePath);
    return true;
#endif
}
