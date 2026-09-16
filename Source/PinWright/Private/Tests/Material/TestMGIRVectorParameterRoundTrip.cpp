// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "UObject/Package.h"

namespace
{
UMaterial* CreateVectorParameterRoundTripScratchMaterial(const IrTest::FScratchAsset& Scratch)
{
    UPackage* Package = CreatePackage(*Scratch.PackagePath);
    if (!Package)
    {
        return nullptr;
    }

    UMaterial* Material = NewObject<UMaterial>(
        Package,
        FName(*Scratch.AssetName),
        RF_Public | RF_Standalone);
    if (Material)
    {
        FAssetRegistryModule::AssetCreated(Material);
    }
    return Material;
}

UMaterialExpressionVectorParameter* FindVectorParameter(UMaterial* Material)
{
    if (!Material)
    {
        return nullptr;
    }

    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (UMaterialExpressionVectorParameter* VectorParameter =
            Cast<UMaterialExpressionVectorParameter>(Expression))
        {
            return VectorParameter;
        }
    }
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRVectorParameterDefaultRoundTripTest,
    "PinWright.material.mgir.VectorParameter.DefaultRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRVectorParameterDefaultRoundTripTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset SourceScratch(TEXT("M_MGIRVectorParameterSource"));
    UMaterial* SourceMaterial = CreateVectorParameterRoundTripScratchMaterial(SourceScratch);
    TestNotNull(TEXT("source material created"), SourceMaterial);
    if (!SourceMaterial || !SourceMaterial->GetEditorOnlyData())
    {
        return false;
    }

    UMaterialExpressionVectorParameter* SourceParameter =
        NewObject<UMaterialExpressionVectorParameter>(SourceMaterial, NAME_None, RF_Transactional);
    TestNotNull(TEXT("source vector parameter created"), SourceParameter);
    if (!SourceParameter)
    {
        return false;
    }

    SourceParameter->ParameterName = TEXT("Tint");
    SourceParameter->DefaultValue = FLinearColor(0.125f, 0.25f, 0.5f, 0.75f);
    SourceMaterial->GetEditorOnlyData()->ExpressionCollection.AddExpression(SourceParameter);

    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(SourceMaterial);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    TestTrue(TEXT("decompile emits the vector default as an array"),
        DecompileResult.MGIRText.Contains(TEXT("DefaultValue: [0.125, 0.25, 0.5, 0.75]")));
    if (!DecompileResult.bSuccess)
    {
        return false;
    }

    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRVectorParameterTarget"));
    const FString TargetMGIR = IrTest::ReplaceScratchAssetName(
        DecompileResult.MGIRText,
        SourceScratch,
        TargetScratch);

    FMGIRCompileOptions CompileOptions;
    CompileOptions.bRunLayout = false;
    CompileOptions.bSave = false;
    const FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(TargetMGIR, CompileOptions);
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode,
        *CompileResult.ErrorMessage),
        CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    UMaterialExpressionVectorParameter* TargetParameter = FindVectorParameter(TargetMaterial);
    TestNotNull(TEXT("compiled vector parameter exists"), TargetParameter);
    if (!TargetParameter)
    {
        return false;
    }

    TestEqual(TEXT("parameter name round-trips"), TargetParameter->ParameterName, FName(TEXT("Tint")));
    TestEqual(TEXT("default R round-trips"), TargetParameter->DefaultValue.R, 0.125f);
    TestEqual(TEXT("default G round-trips"), TargetParameter->DefaultValue.G, 0.25f);
    TestEqual(TEXT("default B round-trips"), TargetParameter->DefaultValue.B, 0.5f);
    TestEqual(TEXT("default A round-trips"), TargetParameter->DefaultValue.A, 0.75f);
    return true;
}
