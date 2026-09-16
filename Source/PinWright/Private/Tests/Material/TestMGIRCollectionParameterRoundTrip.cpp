// Copyright (c) 2026 Alexander Penkin. MIT License.

// Counterfactual: without the collection-parameter normalization in
// FMGIRExpressionEmitter::EmitExpression, the round-trip compile succeeds but leaves the
// target node's ParameterId invalid, while unknown-name and missing-collection calls are
// accepted as dead nodes.
#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Handlers/ErrorCodes.h"
#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialParameterCollection.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "UObject/Package.h"

namespace MGIRCollectionParameterTest
{
UMaterial* CreateScratchMaterial(const IrTest::FScratchAsset& Scratch)
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

UMaterialParameterCollection* CreateScratchCollection(
    const IrTest::FScratchAsset& Scratch,
    FName ParameterName,
    const FGuid& ParameterId)
{
    UPackage* Package = CreatePackage(*Scratch.PackagePath);
    if (!Package)
    {
        return nullptr;
    }

    UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(
        Package,
        FName(*Scratch.AssetName),
        RF_Public | RF_Standalone);
    if (!Collection)
    {
        return nullptr;
    }

    FCollectionScalarParameter Parameter;
    Parameter.ParameterName = ParameterName;
    Parameter.Id = ParameterId;
    Parameter.DefaultValue = 1.0f;
    Collection->ScalarParameters.Add(Parameter);
    Collection->PostEditChange();
    FAssetRegistryModule::AssetCreated(Collection);
    return Collection;
}

UMaterialExpressionCollectionParameter* FindCollectionParameter(UMaterial* Material)
{
    if (!Material)
    {
        return nullptr;
    }

    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (UMaterialExpressionCollectionParameter* CollectionParameter =
            Cast<UMaterialExpressionCollectionParameter>(Expression))
        {
            return CollectionParameter;
        }
    }
    return nullptr;
}
} // namespace MGIRCollectionParameterTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRCollectionParameterIdRoundTripTest,
    "PinWright.material.mgir.CollectionParameter.RoundTripPreservesParameterId",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRCollectionParameterIdRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace MGIRCollectionParameterTest;

    const FName ParameterName(TEXT("ViewmodelFOVScale"));
    const FGuid ParameterId = FGuid::NewGuid();
    IrTest::FScratchAsset CollectionScratch(TEXT("MPC_MGIRCollectionParameter"));
    UMaterialParameterCollection* Collection =
        CreateScratchCollection(CollectionScratch, ParameterName, ParameterId);
    TestNotNull(TEXT("parameter collection created"), Collection);
    if (!Collection)
    {
        return false;
    }
    TestEqual(TEXT("collection fixture resolves the parameter id"),
        Collection->GetParameterId(ParameterName), ParameterId);

    IrTest::FScratchAsset SourceScratch(TEXT("M_MGIRCollectionParameterSource"));
    UMaterial* SourceMaterial = CreateScratchMaterial(SourceScratch);
    TestNotNull(TEXT("source material created"), SourceMaterial);
    if (!SourceMaterial || !SourceMaterial->GetEditorOnlyData())
    {
        return false;
    }

    UMaterialExpressionCollectionParameter* SourceParameter =
        NewObject<UMaterialExpressionCollectionParameter>(SourceMaterial, NAME_None, RF_Transactional);
    TestNotNull(TEXT("source collection parameter created"), SourceParameter);
    if (!SourceParameter)
    {
        return false;
    }

    SourceParameter->Collection = Collection;
    SourceParameter->ParameterName = ParameterName;
    SourceParameter->ParameterId = ParameterId;
    SourceMaterial->GetEditorOnlyData()->ExpressionCollection.AddExpression(SourceParameter);

    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(SourceMaterial);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    TestTrue(TEXT("decompile emits the collection path"),
        DecompileResult.MGIRText.Contains(Collection->GetPathName()));
    TestTrue(TEXT("decompile emits the parameter name"),
        DecompileResult.MGIRText.Contains(ParameterName.ToString()));
    if (!DecompileResult.bSuccess)
    {
        return false;
    }

    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRCollectionParameterTarget"));
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
    UMaterialExpressionCollectionParameter* TargetParameter =
        FindCollectionParameter(TargetMaterial);
    TestNotNull(TEXT("compiled collection parameter exists"), TargetParameter);
    if (!TargetParameter)
    {
        return false;
    }

    TestEqual(TEXT("collection reference round-trips"),
        TargetParameter->Collection.Get(), Collection);
    TestEqual(TEXT("parameter name round-trips"),
        TargetParameter->ParameterName, ParameterName);
    TestEqual(TEXT("parameter id is resolved from the collection"),
        TargetParameter->ParameterId, ParameterId);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRCollectionParameterRejectsUnknownNameTest,
    "PinWright.material.mgir.CollectionParameter.RejectsUnknownName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRCollectionParameterRejectsUnknownNameTest::RunTest(const FString& Parameters)
{
    using namespace MGIRCollectionParameterTest;

    const FName ExistingParameterName(TEXT("ExistingScalar"));
    IrTest::FScratchAsset CollectionScratch(TEXT("MPC_MGIRCollectionParameterInvalid"));
    UMaterialParameterCollection* Collection = CreateScratchCollection(
        CollectionScratch,
        ExistingParameterName,
        FGuid::NewGuid());
    TestNotNull(TEXT("parameter collection created"), Collection);
    if (!Collection)
    {
        return false;
    }

    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRCollectionParameterInvalid"));
    const FString InvalidMGIR = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%collection = call `/Script/Engine.MaterialExpressionCollectionParameter`")
        TEXT("(Collection: \"%s\", ParameterName: \"MissingScalar\") @(0, 0)\n")
        TEXT("}\n"),
        *TargetScratch.PackagePath,
        *Collection->GetPathName());

    FMGIRCompileOptions CompileOptions;
    CompileOptions.bRunLayout = false;
    CompileOptions.bSave = false;
    const FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(InvalidMGIR, CompileOptions);
    TestFalse(TEXT("unknown collection parameter is rejected"), CompileResult.bSuccess);
    TestEqual(TEXT("unknown collection parameter uses INVALID_PARAMS"),
        CompileResult.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(TEXT("error names the missing parameter"),
        CompileResult.ErrorMessage.Contains(TEXT("MissingScalar")));
    TestTrue(TEXT("error names the collection"),
        CompileResult.ErrorMessage.Contains(Collection->GetPathName()));

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    TestNull(TEXT("rejected collection parameter is discarded"),
        FindCollectionParameter(TargetMaterial));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRCollectionParameterRejectsMissingCollectionTest,
    "PinWright.material.mgir.CollectionParameter.RejectsMissingCollection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRCollectionParameterRejectsMissingCollectionTest::RunTest(const FString& Parameters)
{
    using namespace MGIRCollectionParameterTest;

    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRCollectionParameterMissingCollection"));
    const FString InvalidMGIR = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%collection = call `/Script/Engine.MaterialExpressionCollectionParameter`")
        TEXT("(Collection: \"None\", ParameterName: \"MissingScalar\") @(0, 0)\n")
        TEXT("}\n"),
        *TargetScratch.PackagePath);

    FMGIRCompileOptions CompileOptions;
    CompileOptions.bRunLayout = false;
    CompileOptions.bSave = false;
    const FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(InvalidMGIR, CompileOptions);
    TestFalse(TEXT("missing collection is rejected"), CompileResult.bSuccess);
    TestEqual(TEXT("missing collection uses INVALID_PARAMS"),
        CompileResult.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(TEXT("error identifies the missing Collection property"),
        CompileResult.ErrorMessage.Contains(TEXT("valid Collection property")));

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    TestNull(TEXT("missing-collection expression is discarded"),
        FindCollectionParameter(TargetMaterial));
    return true;
}
