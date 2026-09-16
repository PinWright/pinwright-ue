// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the MGIR round trip losing every material-level property.
//
// Measured before the fix: a translucent, two-sided master with
// TLM_SurfacePerPixelLighting was decompiled to MGIR and compiled to a scratch path.
// The compile returned blocksCompiled: 1 and produced blendMode "Opaque",
// twoSided false - an Opacity pin wired into an opaque material, which the renderer
// discards. A shipped glass master silently rendered as a solid, with every field of
// the compile response reporting success. The decompiler emitted no material-level
// property at all and the grammar had no syntax to express one.
//
// Counterfactual: revert the `property` opcode, MGIRMaterialProperties::Emit, or the
// apply pass in CompileMaterialBlock, and RoundTripPreservesSurfaceConfiguration fails
// on the first assertion after the compile - the target keeps the opaque, one-sided
// defaults it was created with while the compile still reports success.
#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "MGIR/MGIRMaterialProperties.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
UMaterial* CreateMaterialPropertyScratchMaterial(const IrTest::FScratchAsset& Scratch)
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

// Reads the stored ShadingModel UPROPERTY, which UMaterial keeps private and which is NOT
// what GetShadingModels() returns (that one is derived per domain). Deliberately does not
// route through MGIRMaterialProperties, so a bug that reads or writes the wrong field
// cannot make the comparison pass by being wrong on both sides.
uint8 ReadStoredShadingModel(const UMaterial* Material)
{
    const FByteProperty* Property = CastField<FByteProperty>(
        UMaterial::StaticClass()->FindPropertyByName(TEXT("ShadingModel")));
    return Property ? Property->GetPropertyValue_InContainer(Material) : 0;
}

// Adds one constant so the block is a real graph rather than the empty-body special case.
UMaterialExpressionConstant* AddConstantWiredToOpacity(UMaterial* Material, float Value)
{
    if (!Material || !Material->GetEditorOnlyData())
    {
        return nullptr;
    }

    UMaterialExpressionConstant* Constant =
        NewObject<UMaterialExpressionConstant>(Material, NAME_None, RF_Transactional);
    if (!Constant)
    {
        return nullptr;
    }

    Constant->R = Value;
    Material->GetEditorOnlyData()->ExpressionCollection.AddExpression(Constant);
    Material->GetEditorOnlyData()->Opacity.Expression = Constant;
    return Constant;
}

FMGIRCompileResult CompileScratchText(const FString& Text)
{
    FMGIRCompileOptions Options;
    Options.bRunLayout = false;
    Options.bSave = false;
    return FMGIRCompiler::Compile(Text, Options);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRMaterialPropertyRoundTripTest,
    "PinWright.material.mgir.MaterialProperties.RoundTripPreservesSurfaceConfiguration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRMaterialPropertyRoundTripTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset SourceScratch(TEXT("M_MGIRPropertySource"));
    UMaterial* SourceMaterial = CreateMaterialPropertyScratchMaterial(SourceScratch);
    TestNotNull(TEXT("source material created"), SourceMaterial);
    if (!SourceMaterial || !SourceMaterial->GetEditorOnlyData())
    {
        return false;
    }

    SourceMaterial->MaterialDomain = MD_Surface;
    SourceMaterial->BlendMode = BLEND_Translucent;
    SourceMaterial->SetShadingModel(MSM_ThinTranslucent);
    SourceMaterial->TwoSided = 1;
    SourceMaterial->bIsThinSurface = 1;
    SourceMaterial->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
    SourceMaterial->OpacityMaskClipValue = 0.75f;
    SourceMaterial->NumCustomizedUVs = 3;
    TestNotNull(TEXT("source constant created"), AddConstantWiredToOpacity(SourceMaterial, 0.5f));

    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(SourceMaterial);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        return false;
    }

    // The text itself must carry them. Without this the next two blocks could pass on a
    // target that merely happened to share the source's values.
    TestTrue(FString::Printf(TEXT("decompile emits the blend mode (text='%s')"), *DecompileResult.MGIRText),
        DecompileResult.MGIRText.Contains(TEXT("property BlendMode: BLEND_Translucent")));
    TestTrue(TEXT("decompile emits the two-sided flag"),
        DecompileResult.MGIRText.Contains(TEXT("property TwoSided: true")));
    TestTrue(TEXT("decompile emits the translucency lighting mode"),
        DecompileResult.MGIRText.Contains(TEXT("property TranslucencyLightingMode: TLM_SurfacePerPixelLighting")));
    TestTrue(TEXT("decompile emits the shading model"),
        DecompileResult.MGIRText.Contains(TEXT("property ShadingModel: MSM_ThinTranslucent")));
    TestTrue(TEXT("decompile emits the material domain"),
        DecompileResult.MGIRText.Contains(TEXT("property MaterialDomain: MD_Surface")));

    // Pre-create the target at UMaterial's defaults so the assertions below are a measured
    // before/after, not a comparison against an asset the compiler happened to make.
    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRPropertyTarget"));
    UMaterial* PreexistingTarget = CreateMaterialPropertyScratchMaterial(TargetScratch);
    TestNotNull(TEXT("target material pre-created"), PreexistingTarget);
    if (!PreexistingTarget)
    {
        return false;
    }
    TestEqual(TEXT("target starts opaque"), static_cast<int32>(PreexistingTarget->BlendMode.GetValue()), static_cast<int32>(BLEND_Opaque));
    TestEqual(TEXT("target starts one-sided"), static_cast<int32>(PreexistingTarget->TwoSided), 0);

    const FString TargetMGIR = IrTest::ReplaceScratchAssetName(
        DecompileResult.MGIRText,
        SourceScratch,
        TargetScratch);

    const FMGIRCompileResult CompileResult = CompileScratchText(TargetMGIR);
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode,
        *CompileResult.ErrorMessage),
        CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    TestNotNull(TEXT("compiled target material exists"), TargetMaterial);
    if (!TargetMaterial)
    {
        return false;
    }

    TestEqual(TEXT("blend mode round-trips"),
        static_cast<int32>(TargetMaterial->BlendMode.GetValue()), static_cast<int32>(BLEND_Translucent));
    TestEqual(TEXT("two-sided round-trips"), static_cast<int32>(TargetMaterial->TwoSided), 1);
    TestEqual(TEXT("thin-surface round-trips"), static_cast<int32>(TargetMaterial->bIsThinSurface), 1);
    TestEqual(TEXT("translucency lighting mode round-trips"),
        static_cast<int32>(TargetMaterial->TranslucencyLightingMode.GetValue()),
        static_cast<int32>(TLM_SurfacePerPixelLighting));
    TestEqual(TEXT("material domain round-trips"),
        static_cast<int32>(TargetMaterial->MaterialDomain.GetValue()), static_cast<int32>(MD_Surface));
    TestEqual(TEXT("opacity mask clip value round-trips"), TargetMaterial->OpacityMaskClipValue, 0.75f);
    TestEqual(TEXT("customized UV count round-trips"), TargetMaterial->NumCustomizedUVs, 3);
    TestEqual(TEXT("stored shading model round-trips"),
        static_cast<int32>(ReadStoredShadingModel(TargetMaterial)), static_cast<int32>(MSM_ThinTranslucent));

    // The bitfield the renderer reads is derived from ShadingModel, not written with it, so
    // a reflection write that skipped the rebuild would leave this holding DefaultLit.
    TestTrue(TEXT("derived shading model field was rebuilt from the applied shading model"),
        TargetMaterial->GetShadingModels().HasOnlyShadingModel(MSM_ThinTranslucent));

    // The pin the original defect left stranded in an opaque material.
    TestTrue(TEXT("the opacity wire survived alongside the blend mode that makes it count"),
        TargetMaterial->GetEditorOnlyData() != nullptr
            && TargetMaterial->GetEditorOnlyData()->Opacity.Expression != nullptr);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRMaterialPropertyUseMaterialAttributesTest,
    "PinWright.material.mgir.MaterialProperties.UseMaterialAttributesRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRMaterialPropertyUseMaterialAttributesTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset SourceScratch(TEXT("M_MGIRAttrsSource"));
    UMaterial* SourceMaterial = CreateMaterialPropertyScratchMaterial(SourceScratch);
    TestNotNull(TEXT("source material created"), SourceMaterial);
    if (!SourceMaterial)
    {
        return false;
    }

    SourceMaterial->bUseMaterialAttributes = 1;
    TestNotNull(TEXT("source constant created"), AddConstantWiredToOpacity(SourceMaterial, 1.0f));

    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(SourceMaterial);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        return false;
    }
    TestTrue(TEXT("decompile emits bUseMaterialAttributes"),
        DecompileResult.MGIRText.Contains(TEXT("property bUseMaterialAttributes: true")));

    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRAttrsTarget"));
    const FMGIRCompileResult CompileResult = CompileScratchText(
        IrTest::ReplaceScratchAssetName(DecompileResult.MGIRText, SourceScratch, TargetScratch));
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    TestNotNull(TEXT("compiled target material exists"), TargetMaterial);
    if (!TargetMaterial)
    {
        return false;
    }

    // Without this flag the renderer ignores the MaterialAttributes root entirely and reads
    // the individual pins, which a graph authored through SetMaterialAttributes leaves
    // unconnected - a fully black master that every response field calls a success.
    TestEqual(TEXT("bUseMaterialAttributes round-trips"),
        static_cast<int32>(TargetMaterial->bUseMaterialAttributes), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRMaterialPropertyEmitCoversCarriedSetTest,
    "PinWright.material.mgir.MaterialProperties.EmitCoversEveryCarriedProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRMaterialPropertyEmitCoversCarriedSetTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
    TestNotNull(TEXT("transient material created"), Material);
    if (!Material)
    {
        return false;
    }

    const TArray<FString> CarriedNames = MGIRMaterialProperties::GetCarriedPropertyNames();
    TestTrue(TEXT("the carried set is not empty"), CarriedNames.Num() > 0);

    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(Material);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        return false;
    }

    // Every carried property must appear as a live `property` line, not as the
    // `# property <name>: <unresolved ...>` comment Emit falls back to when the UPROPERTY it
    // names has gone. That comment is the engine-drift signal: it says out loud that a
    // property left the round trip, which is exactly what used to happen in silence.
    for (const FString& Name : CarriedNames)
    {
        TestTrue(
            FString::Printf(TEXT("decompile emits a live 'property %s:' line"), *Name),
            DecompileResult.MGIRText.Contains(FString::Printf(TEXT("property %s: "), *Name)));
        TestFalse(
            FString::Printf(TEXT("'%s' resolved against UMaterial reflection"), *Name),
            DecompileResult.MGIRText.Contains(FString::Printf(TEXT("# property %s:"), *Name)));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRMaterialPropertyShortEnumFormTest,
    "PinWright.material.mgir.MaterialProperties.ShortEnumFormIsAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRMaterialPropertyShortEnumFormTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRShortEnum"));
    const FString Text = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    property BlendMode: Translucent\n")
        TEXT("    property TwoSided: yes\n")
        TEXT("    %%c = constant Float1(0.5)\n")
        TEXT("    output Opacity: %%c\n")
        TEXT("}\n"),
        *TargetScratch.PackagePath);

    const FMGIRCompileResult CompileResult = CompileScratchText(Text);
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    TestNotNull(TEXT("compiled target material exists"), TargetMaterial);
    if (!TargetMaterial)
    {
        return false;
    }

    TestEqual(TEXT("prefix-less enum member resolves"),
        static_cast<int32>(TargetMaterial->BlendMode.GetValue()), static_cast<int32>(BLEND_Translucent));
    TestEqual(TEXT("yes resolves as true"), static_cast<int32>(TargetMaterial->TwoSided), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRMaterialPropertyRejectionsTest,
    "PinWright.material.mgir.MaterialProperties.BadPropertiesAreRefusedBeforeTheGraphIsTouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRMaterialPropertyRejectionsTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRPropertyReject"));
    UMaterial* TargetMaterial = CreateMaterialPropertyScratchMaterial(TargetScratch);
    TestNotNull(TEXT("target material created"), TargetMaterial);
    if (!TargetMaterial)
    {
        return false;
    }
    TestNotNull(TEXT("target constant created"), AddConstantWiredToOpacity(TargetMaterial, 0.25f));

    const FString UnknownPropertyText = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    property NotAMaterialProperty: 1\n")
        TEXT("}\n"),
        *TargetScratch.PackagePath);
    const FMGIRCompileResult UnknownResult = CompileScratchText(UnknownPropertyText);
    TestFalse(TEXT("an unknown property fails the compile"), UnknownResult.bSuccess);
    TestEqual(TEXT("unknown property reports MGIR_UNKNOWN_PROPERTY"),
        UnknownResult.ErrorCode, FString(TEXT("MGIR_UNKNOWN_PROPERTY")));

    // Append mode empties the expression collection first, so a property rejected only at
    // apply time would leave the target wiped by a call that reported failure. The
    // validation pass runs before the asset is loaded, so the graph is still there.
    TestEqual(TEXT("the rejected document left the target graph intact"),
        TargetMaterial->GetExpressions().Num(), 1);

    const FString BadValueText = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    property BlendMode: NotABlendMode\n")
        TEXT("}\n"),
        *TargetScratch.PackagePath);
    const FMGIRCompileResult BadValueResult = CompileScratchText(BadValueText);
    TestFalse(TEXT("an unresolvable enum member fails the compile"), BadValueResult.bSuccess);
    TestEqual(TEXT("bad value reports MGIR_BAD_PROPERTY_VALUE"),
        BadValueResult.ErrorCode, FString(TEXT("MGIR_BAD_PROPERTY_VALUE")));
    TestTrue(TEXT("bad value error lists the accepted members"),
        BadValueResult.ErrorMessage.Contains(TEXT("BLEND_Translucent")));

    // The _MAX sentinel is a reflected name but not a usable value.
    const FString SentinelText = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    property BlendMode: BLEND_MAX\n")
        TEXT("}\n"),
        *TargetScratch.PackagePath);
    TestFalse(TEXT("an enum sentinel is refused"), CompileScratchText(SentinelText).bSuccess);

    IrTest::FScratchAsset FunctionScratch(TEXT("MF_MGIRPropertyReject"));
    const FString FunctionText = FString::Printf(
        TEXT("entry function `%s` {\n")
        TEXT("    property BlendMode: BLEND_Translucent\n")
        TEXT("}\n"),
        *FunctionScratch.PackagePath);
    const FMGIRCompileResult FunctionResult = CompileScratchText(FunctionText);
    TestFalse(TEXT("a property in a function block fails the compile"), FunctionResult.bSuccess);
    TestTrue(TEXT("the function-block rejection says why"),
        FunctionResult.ErrorMessage.Contains(TEXT("entry material")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRClearCoatAndCustomizedUVOutputsTest,
    "PinWright.material.mgir.MaterialProperties.ClearCoatAndCustomizedUVOutputsRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRClearCoatAndCustomizedUVOutputsTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset SourceScratch(TEXT("M_MGIRRootOutputsSource"));
    UMaterial* SourceMaterial = CreateMaterialPropertyScratchMaterial(SourceScratch);
    TestNotNull(TEXT("source material created"), SourceMaterial);
    if (!SourceMaterial || !SourceMaterial->GetEditorOnlyData())
    {
        return false;
    }

    UMaterialExpressionConstant* Constant = AddConstantWiredToOpacity(SourceMaterial, 0.25f);
    TestNotNull(TEXT("source constant created"), Constant);
    if (!Constant)
    {
        return false;
    }

    UMaterialEditorOnlyData* SourceEditorOnly = SourceMaterial->GetEditorOnlyData();
    SourceEditorOnly->ClearCoat.Expression = Constant;
    SourceEditorOnly->ClearCoatRoughness.Expression = Constant;
    SourceEditorOnly->CustomizedUVs[0].Expression = Constant;
    SourceMaterial->NumCustomizedUVs = 1;

    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(SourceMaterial);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        return false;
    }
    TestTrue(TEXT("decompile emits the clear-coat pins"),
        DecompileResult.MGIRText.Contains(TEXT("output ClearCoat:")));
    TestTrue(TEXT("decompile emits the customized UV pin"),
        DecompileResult.MGIRText.Contains(TEXT("output CustomizedUV0:")));

    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRRootOutputsTarget"));
    const FMGIRCompileResult CompileResult = CompileScratchText(
        IrTest::ReplaceScratchAssetName(DecompileResult.MGIRText, SourceScratch, TargetScratch));

    // These three pin names were emitted by the decompiler and unknown to the compiler's pin
    // resolver, so a material using clear coat or customized UVs could not survive its own
    // decompile: the recompile failed with MGIR_INPUT_NOT_FOUND.
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    TestNotNull(TEXT("compiled target material exists"), TargetMaterial);
    if (!TargetMaterial || !TargetMaterial->GetEditorOnlyData())
    {
        return false;
    }

    UMaterialEditorOnlyData* TargetEditorOnly = TargetMaterial->GetEditorOnlyData();
    TestNotNull(TEXT("ClearCoat wire round-trips"), TargetEditorOnly->ClearCoat.Expression);
    TestNotNull(TEXT("ClearCoatRoughness wire round-trips"), TargetEditorOnly->ClearCoatRoughness.Expression);
    TestNotNull(TEXT("CustomizedUV0 wire round-trips"), TargetEditorOnly->CustomizedUVs[0].Expression);
    TestEqual(TEXT("customized UV count round-trips with the wire"), TargetMaterial->NumCustomizedUVs, 1);
    return true;
}
