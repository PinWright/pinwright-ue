// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBreakMaterialAttributes.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "MaterialExpressionIO.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialConnectSourcePinResolvesOutputIndexTest,
    "PinWright.material.connect_nodes.SourcePinResolvesOutputIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialConnectSourcePinResolvesOutputIndexTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/ConnectSourcePin_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UMaterial* Material = NewObject<UMaterial>(
        Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);

    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // BreakMaterialAttributes has many named outputs (BaseColor, Metallic, Specular, Roughness, ...).
    UMaterialExpressionBreakMaterialAttributes* BreakExpr =
        NewObject<UMaterialExpressionBreakMaterialAttributes>(Material);
    if (!TestNotNull(TEXT("BreakMaterialAttributes created"), BreakExpr))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }
    if (!BreakExpr->MaterialExpressionGuid.IsValid())
    {
        BreakExpr->MaterialExpressionGuid = FGuid::NewGuid();
    }
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(BreakExpr);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    // Look up the dynamic index of the "Roughness" output (proves the test isn't trivially 0).
    int32 RoughnessIndex = INDEX_NONE;
    const TArray<FExpressionOutput>& Outputs = BreakExpr->GetOutputs();
    for (int32 i = 0; i < Outputs.Num(); ++i)
    {
        if (Outputs[i].OutputName.ToString().Equals(TEXT("Roughness"), ESearchCase::IgnoreCase))
        {
            RoughnessIndex = i;
            break;
        }
    }
    TestTrue(TEXT("Roughness output found on BreakMaterialAttributes"), RoughnessIndex != INDEX_NONE);
    TestTrue(TEXT("Roughness output index is non-zero (so OutputIndex=0 default would fail)"), RoughnessIndex > 0);

    const FString SourceGuid = BreakExpr->MaterialExpressionGuid.ToString();

    // Test 1: sourcePin "Roughness" resolves to RoughnessIndex.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceGuid);
        Payload->SetStringField(TEXT("targetNodeId"), FString());
        Payload->SetStringField(TEXT("inputName"), TEXT("Roughness"));
        Payload->SetStringField(TEXT("sourcePin"), TEXT("Roughness"));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.connect_nodes"), Payload, Capture);
        TestTrue(TEXT("Handler found for material.authoring.connect_nodes"), bFound);
        TestTrue(TEXT("connect_nodes succeeded with sourcePin"), Capture.bSuccess);
        TestEqual(TEXT("Roughness.OutputIndex matches dynamic RoughnessIndex"),
            Material->GetEditorOnlyData()->Roughness.OutputIndex, RoughnessIndex);
    }

    // Test 2: sourceOutputIndex fallback (no sourcePin) sets OutputIndex directly.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceGuid);
        Payload->SetStringField(TEXT("targetNodeId"), FString());
        Payload->SetStringField(TEXT("inputName"), TEXT("Metallic"));
        Payload->SetNumberField(TEXT("sourceOutputIndex"), 2);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.connect_nodes"), Payload, Capture);
        TestTrue(TEXT("Handler found for material.authoring.connect_nodes (index fallback)"), bFound);
        TestTrue(TEXT("connect_nodes succeeded with sourceOutputIndex"), Capture.bSuccess);
        TestEqual(TEXT("Metallic.OutputIndex matches sourceOutputIndex=2"),
            Material->GetEditorOnlyData()->Metallic.OutputIndex, 2);
    }

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialConnectStaticSwitchInputAliasesTest,
    "PinWright.material.authoring.connect_nodes.StaticSwitchInputAliases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialConnectStaticSwitchInputAliasesTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Temp/PinWrightTests/ConnectStaticSwitch_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Transient package created"), Package)) return true;

    UMaterial* Material = NewObject<UMaterial>(
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Transient material created"), Material)) return true;

    UMaterialExpressionConstant* FirstSource = NewObject<UMaterialExpressionConstant>(Material);
    UMaterialExpressionConstant* SecondSource = NewObject<UMaterialExpressionConstant>(Material);
    UMaterialExpressionStaticSwitchParameter* Switch =
        NewObject<UMaterialExpressionStaticSwitchParameter>(Material);
    if (!TestNotNull(TEXT("First source created"), FirstSource)
        || !TestNotNull(TEXT("Second source created"), SecondSource)
        || !TestNotNull(TEXT("Static switch created"), Switch))
    {
        return true;
    }

    FirstSource->MaterialExpressionGuid = FGuid::NewGuid();
    SecondSource->MaterialExpressionGuid = FGuid::NewGuid();
    Switch->MaterialExpressionGuid = FGuid::NewGuid();
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(FirstSource);
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(SecondSource);
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Switch);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    auto Connect = [&](UMaterialExpression* Source, const FString& InputName, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), Source->MaterialExpressionGuid.ToString());
        Payload->SetStringField(TEXT("targetNodeId"), Switch->MaterialExpressionGuid.ToString());
        Payload->SetStringField(TEXT("inputName"), InputName);
        return InvokeHandlerWithCapture(TEXT("material.authoring.connect_nodes"), Payload, Capture);
    };

    FTestResponseCapture InvalidCapture;
    TestTrue(TEXT("connect_nodes handler found"), Connect(FirstSource, TEXT("UnknownInput"), InvalidCapture));
    TestFalse(TEXT("Unknown input is refused"), InvalidCapture.bSuccess);
    TestEqual(TEXT("Unknown input uses PIN_NOT_FOUND"),
        InvalidCapture.ErrorCode, FString(TEXT("PIN_NOT_FOUND")));
    TestNull(TEXT("Refusal does not mutate A"), Switch->A.Expression);
    TestNull(TEXT("Refusal does not mutate B"), Switch->B.Expression);

    const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
    if (TestTrue(TEXT("PIN_NOT_FOUND includes candidates"),
        InvalidCapture.Result.IsValid()
            && InvalidCapture.Result->TryGetArrayField(TEXT("candidates"), Candidates)
            && Candidates))
    {
        TArray<FString> CandidateNames;
        for (const TSharedPtr<FJsonValue>& Candidate : *Candidates)
        {
            FString CandidateName;
            if (Candidate.IsValid() && Candidate->TryGetString(CandidateName))
            {
                CandidateNames.Add(CandidateName);
            }
        }
        TestTrue(TEXT("Candidates include display name True"), CandidateNames.Contains(TEXT("True")));
        TestTrue(TEXT("Candidates include internal name A"), CandidateNames.Contains(TEXT("A")));
        TestTrue(TEXT("Candidates include display name False"), CandidateNames.Contains(TEXT("False")));
        TestTrue(TEXT("Candidates include internal name B"), CandidateNames.Contains(TEXT("B")));
    }

    FTestResponseCapture TrueCapture;
    Connect(FirstSource, TEXT("True"), TrueCapture);
    TestTrue(TEXT("Display name True succeeds"), TrueCapture.bSuccess);
    TestTrue(TEXT("True read-back is A"), Switch->A.Expression == FirstSource);

    FTestResponseCapture ACapture;
    Connect(SecondSource, TEXT("A"), ACapture);
    TestTrue(TEXT("Internal name A succeeds"), ACapture.bSuccess);
    TestTrue(TEXT("A read-back uses the requested source"), Switch->A.Expression == SecondSource);

    FTestResponseCapture FalseCapture;
    Connect(FirstSource, TEXT("False"), FalseCapture);
    TestTrue(TEXT("Display name False succeeds"), FalseCapture.bSuccess);
    TestTrue(TEXT("False read-back is B"), Switch->B.Expression == FirstSource);

    FTestResponseCapture BCapture;
    Connect(SecondSource, TEXT("B"), BCapture);
    TestTrue(TEXT("Internal name B succeeds"), BCapture.bSuccess);
    TestTrue(TEXT("B read-back uses the requested source"), Switch->B.Expression == SecondSource);

    TSharedPtr<FJsonObject> DetailsPayload = MakeShared<FJsonObject>();
    DetailsPayload->SetStringField(TEXT("assetPath"), AssetPath);
    DetailsPayload->SetStringField(TEXT("nodeId"), Switch->MaterialExpressionGuid.ToString());

    FTestResponseCapture DetailsCapture;
    const bool bDetailsFound = InvokeHandlerWithCapture(
        TEXT("material.authoring.get_material_node_details"), DetailsPayload, DetailsCapture);
    TestTrue(TEXT("get_material_node_details handler found"), bDetailsFound);
    TestTrue(TEXT("get_material_node_details succeeded"), DetailsCapture.bSuccess);
    if (DetailsCapture.bSuccess && DetailsCapture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject> TrueInput = JsonArrayFindObjectByStringField(
            DetailsCapture.Result, TEXT("inputs"), TEXT("name"), TEXT("True"));
        TestTrue(TEXT("Static switch details expose True"), TrueInput.IsValid());
        if (TrueInput.IsValid())
        {
            FString InternalName;
            TestTrue(TEXT("True details expose internalName"),
                TrueInput->TryGetStringField(TEXT("internalName"), InternalName));
            TestEqual(TEXT("True details internalName is A"), InternalName, FString(TEXT("A")));
        }

        const TSharedPtr<FJsonObject> FalseInput = JsonArrayFindObjectByStringField(
            DetailsCapture.Result, TEXT("inputs"), TEXT("name"), TEXT("False"));
        TestTrue(TEXT("Static switch details expose False"), FalseInput.IsValid());
        if (FalseInput.IsValid())
        {
            FString InternalName;
            TestTrue(TEXT("False details expose internalName"),
                FalseInput->TryGetStringField(TEXT("internalName"), InternalName));
            TestEqual(TEXT("False details internalName is B"), InternalName, FString(TEXT("B")));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialConnectTextureObjectInputFallbackTest,
    "PinWright.material.authoring.connect_nodes.TextureObjectReflectedFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialConnectTextureObjectInputFallbackTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Temp/PinWrightTests/ConnectTextureObject_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Transient package created"), Package)) return true;

    UMaterial* Material = NewObject<UMaterial>(
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Transient material created"), Material)) return true;

    UMaterialExpressionTextureObject* Source =
        NewObject<UMaterialExpressionTextureObject>(Material);
    UMaterialExpressionTextureSampleParameter2D* Target =
        NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
    if (!TestNotNull(TEXT("TextureObject source created"), Source)
        || !TestNotNull(TEXT("TextureSampleParameter2D target created"), Target))
    {
        return true;
    }

    Source->MaterialExpressionGuid = FGuid::NewGuid();
    Target->MaterialExpressionGuid = FGuid::NewGuid();
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Source);
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Target);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    // UE 5.8's TextureSampleParameter constructor sets bShowTextureInputPin=false, so
    // GetInput/GetInputsView omit TextureObject even though the reflected field is valid.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("sourceNodeId"), Source->MaterialExpressionGuid.ToString());
    Payload->SetStringField(TEXT("targetNodeId"), Target->MaterialExpressionGuid.ToString());
    Payload->SetStringField(TEXT("inputName"), TEXT("TextureObject"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("material.authoring.connect_nodes"), Payload, Capture);
    TestTrue(TEXT("connect_nodes handler found for TextureObject"), bFound);
    TestTrue(TEXT("TextureObject reflected fallback succeeds"), Capture.bSuccess);
    TestEqual(TEXT("TextureObject fallback emits no error"), Capture.ErrorCode, FString());
    TestTrue(TEXT("TextureObject retains the source expression"),
        Target->TextureObject.Expression == Source);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialConnectMainInputReadBackTest,
    "PinWright.material.authoring.connect_nodes.MainInputReadBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialConnectMainInputReadBackTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Temp/PinWrightTests/ConnectMainInput_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Transient package created"), Package)) return true;

    UMaterial* Material = NewObject<UMaterial>(
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Transient material created"), Material)) return true;

    UMaterialExpressionConstant* Source = NewObject<UMaterialExpressionConstant>(Material);
    if (!TestNotNull(TEXT("Main input source created"), Source)) return true;

    Source->MaterialExpressionGuid = FGuid::NewGuid();
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Source);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("sourceNodeId"), Source->MaterialExpressionGuid.ToString());
    Payload->SetStringField(TEXT("targetNodeId"), TEXT("Main"));
    Payload->SetStringField(TEXT("inputName"), TEXT("BaseColor"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("material.authoring.connect_nodes"), Payload, Capture);
    TestTrue(TEXT("connect_nodes handler found for Main"), bFound);
    TestTrue(TEXT("Main connection succeeds after read-back"), Capture.bSuccess);
    TestEqual(TEXT("Main connection emits no error"), Capture.ErrorCode, FString());
    TestTrue(TEXT("Main BaseColor retains the source expression"),
        Material->GetEditorOnlyData()->BaseColor.Expression == Source);

    return true;
}
