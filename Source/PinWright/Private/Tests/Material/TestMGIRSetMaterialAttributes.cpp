// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "MaterialExpressionIO.h"
#include "Misc/EngineVersionComparison.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "UObject/Package.h"

namespace
{
// 5.6+: UMaterialExpressionSetMaterialAttributes exposes CreateOrGetInputAttribute /
// ConnectInputAttribute helpers. They do not exist on 5.4/5.5, so we replicate their exact
// behavior there using the public Inputs / AttributeSetTypes members. Pin index 0 is the
// MaterialAttributes input; dynamic attribute pins live at AttributeSetTypes index + 1.
int32 SetMA_CreateOrGetInputAttribute(
    UMaterialExpressionSetMaterialAttributes* SetAttributes,
    EMaterialProperty Attribute)
{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    if (!SetAttributes)
    {
        return INDEX_NONE;
    }
    // MP_MaterialAttributes maps to the fixed input slot 0.
    if (Attribute == MP_MaterialAttributes)
    {
        return 0;
    }
    const FGuid AttributeId = FMaterialAttributeDefinitionMap::GetID(Attribute);
    int32 InputsIndex = INDEX_NONE;
    if (SetAttributes->AttributeSetTypes.Find(AttributeId, InputsIndex))
    {
        // +1 compensates for AttributeSetTypes not containing the MP_MaterialAttributes slot.
        return InputsIndex + 1;
    }
    const int32 SetTypesIndex = SetAttributes->AttributeSetTypes.Add(AttributeId);
    if (SetTypesIndex == INDEX_NONE)
    {
        return INDEX_NONE;
    }
    SetAttributes->PreEditChange(nullptr);
    InputsIndex = SetAttributes->Inputs.Add(FExpressionInput());
    if (SetAttributes->Inputs.IsValidIndex(InputsIndex))
    {
        SetAttributes->Inputs[InputsIndex].InputName = FName(*FMaterialAttributeDefinitionMap::GetDisplayNameForMaterial(
            SetAttributes->AttributeSetTypes[SetTypesIndex], SetAttributes->Material).ToString());
    }
    return InputsIndex;
#else
    return SetAttributes ? SetAttributes->CreateOrGetInputAttribute(Attribute) : INDEX_NONE;
#endif
}

bool SetMA_ConnectInputAttribute(
    UMaterialExpressionSetMaterialAttributes* SetAttributes,
    EMaterialProperty Attribute,
    UMaterialExpression* Expression,
    int32 OutputIndex = 0)
{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    if (!SetAttributes)
    {
        return false;
    }
    const int32 Index = SetMA_CreateOrGetInputAttribute(SetAttributes, Attribute);
    if (Expression && OutputIndex != INDEX_NONE && SetAttributes->Inputs.IsValidIndex(Index))
    {
        SetAttributes->Inputs[Index].Connect(OutputIndex, Expression);
        return SetAttributes->Inputs[Index].IsConnected();
    }
    return false;
#else
    return SetAttributes ? SetAttributes->ConnectInputAttribute(Attribute, Expression, OutputIndex) : false;
#endif
}
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

template <typename TExpression>
TExpression* AddExpression(UMaterial* Material)
{
    if (!Material || !Material->GetEditorOnlyData())
    {
        return nullptr;
    }

    TExpression* Expression = NewObject<TExpression>(Material, NAME_None, RF_Transactional);
    if (!Expression)
    {
        return nullptr;
    }

    Expression->MaterialExpressionGuid = FGuid::NewGuid();
    Material->GetEditorOnlyData()->ExpressionCollection.AddExpression(Expression);
    return Expression;
}

UMaterialExpressionConstant3Vector* AddVectorConstant(UMaterial* Material, const FLinearColor& Value)
{
    UMaterialExpressionConstant3Vector* Constant = AddExpression<UMaterialExpressionConstant3Vector>(Material);
    if (Constant)
    {
        Constant->Constant = Value;
    }
    return Constant;
}

UMaterialExpressionConstant* AddScalarConstant(UMaterial* Material, float Value)
{
    UMaterialExpressionConstant* Constant = AddExpression<UMaterialExpressionConstant>(Material);
    if (Constant)
    {
        Constant->R = Value;
    }
    return Constant;
}

UMaterialExpressionSetMaterialAttributes* FindSetMaterialAttributes(UMaterial* Material)
{
    if (!Material)
    {
        return nullptr;
    }

    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (UMaterialExpressionSetMaterialAttributes* SetAttributes =
            Cast<UMaterialExpressionSetMaterialAttributes>(Expression))
        {
            return SetAttributes;
        }
    }
    return nullptr;
}

bool IsAttributeInputConnected(
    UMaterialExpressionSetMaterialAttributes* SetAttributes,
    EMaterialProperty Property)
{
    if (!SetAttributes)
    {
        return false;
    }

    const FGuid AttributeID = FMaterialAttributeDefinitionMap::GetID(Property);
    const int32 AttributeIndex = SetAttributes->AttributeSetTypes.Find(AttributeID);
    if (AttributeIndex == INDEX_NONE)
    {
        return false;
    }

    const int32 InputIndex = AttributeIndex + 1;
    return SetAttributes->Inputs.IsValidIndex(InputIndex)
        && SetAttributes->Inputs[InputIndex].Expression != nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRSetMaterialAttributes_DecompileEmitsNamedAttributeListTest,
    "PinWright.material.mgir.SetMaterialAttributes.DecompileEmitsNamedAttributeList",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRSetMaterialAttributes_DecompileEmitsNamedAttributeListTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset SourceScratch(TEXT("M_MGIRSetMaterialAttributesSource"));
    UMaterial* SourceMaterial = CreateScratchMaterial(SourceScratch);
    TestNotNull(TEXT("source material created"), SourceMaterial);
    if (!SourceMaterial)
    {
        return false;
    }

    UMaterialExpressionSetMaterialAttributes* SetAttributes =
        AddExpression<UMaterialExpressionSetMaterialAttributes>(SourceMaterial);
    UMaterialExpressionConstant3Vector* BaseColor = AddVectorConstant(SourceMaterial, FLinearColor(0.1f, 0.2f, 0.3f));
    UMaterialExpressionConstant3Vector* Normal = AddVectorConstant(SourceMaterial, FLinearColor(0.0f, 0.0f, 1.0f));
    UMaterialExpressionConstant* PixelDepthOffset = AddScalarConstant(SourceMaterial, 4.0f);
    TestNotNull(TEXT("SetMaterialAttributes expression created"), SetAttributes);
    TestNotNull(TEXT("BaseColor source created"), BaseColor);
    TestNotNull(TEXT("Normal source created"), Normal);
    TestNotNull(TEXT("PixelDepthOffset source created"), PixelDepthOffset);
    if (!SetAttributes || !BaseColor || !Normal || !PixelDepthOffset)
    {
        return false;
    }

    SetMA_ConnectInputAttribute(SetAttributes, MP_BaseColor, BaseColor);
    SetMA_ConnectInputAttribute(SetAttributes, MP_Normal, Normal);
    SetMA_CreateOrGetInputAttribute(SetAttributes, MP_EmissiveColor);
    SetMA_ConnectInputAttribute(SetAttributes, MP_PixelDepthOffset, PixelDepthOffset);

    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(SourceMaterial);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        return false;
    }

    TestTrue(TEXT("decompile emits authored attribute list"),
        DecompileResult.MGIRText.Contains(TEXT("Attributes:")));
    TestTrue(TEXT("attribute list contains BaseColor"),
        DecompileResult.MGIRText.Contains(TEXT("BaseColor")));
    TestTrue(TEXT("attribute list contains Normal"),
        DecompileResult.MGIRText.Contains(TEXT("Normal")));
    TestTrue(TEXT("attribute list contains EmissiveColor"),
        DecompileResult.MGIRText.Contains(TEXT("EmissiveColor")));
    TestTrue(TEXT("attribute list contains PixelDepthOffset"),
        DecompileResult.MGIRText.Contains(TEXT("PixelDepthOffset")));
    TestFalse(TEXT("decompile suppresses raw AttributeSetTypes"),
        DecompileResult.MGIRText.Contains(TEXT("AttributeSetTypes:")));

    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRSetMaterialAttributesTarget"));
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
    TestNotNull(TEXT("compiled material loads"), TargetMaterial);
    UMaterialExpressionSetMaterialAttributes* CompiledSetAttributes = FindSetMaterialAttributes(TargetMaterial);
    TestNotNull(TEXT("compiled SetMaterialAttributes expression exists"), CompiledSetAttributes);
    if (!CompiledSetAttributes)
    {
        return false;
    }

    TestEqual(TEXT("compiled attribute pin count"),
        CompiledSetAttributes->AttributeSetTypes.Num(),
        4);
    TestTrue(TEXT("compiled BaseColor dynamic pin exists"),
        CompiledSetAttributes->AttributeSetTypes.Contains(FMaterialAttributeDefinitionMap::GetID(MP_BaseColor)));
    TestTrue(TEXT("compiled Normal dynamic pin exists"),
        CompiledSetAttributes->AttributeSetTypes.Contains(FMaterialAttributeDefinitionMap::GetID(MP_Normal)));
    TestTrue(TEXT("compiled EmissiveColor dynamic pin exists"),
        CompiledSetAttributes->AttributeSetTypes.Contains(FMaterialAttributeDefinitionMap::GetID(MP_EmissiveColor)));
    TestTrue(TEXT("compiled PixelDepthOffset dynamic pin exists"),
        CompiledSetAttributes->AttributeSetTypes.Contains(FMaterialAttributeDefinitionMap::GetID(MP_PixelDepthOffset)));
    TestTrue(TEXT("compiled PixelDepthOffset input remains connected"),
        IsAttributeInputConnected(CompiledSetAttributes, MP_PixelDepthOffset));

    return true;
}
