// Copyright (c) 2026 Alexander Penkin. MIT License.

// material.audit (Handlers/Material/MaterialAuditHandler.cpp). Each fixture is a scratch material
// under /Game/PinWrightTests carrying exactly one known defect, and every test asserts the check id
// AND the nodeId of the finding, so a check that fires on the wrong node fails as surely as one that
// does not fire. The root-coverage tests build graphs whose only connection to an output goes
// through a custom output, a named reroute, a landscape layer blend or a material-attribute layer
// stack: a missed reachability root turns those into false islands.
//
// The fixtures are never PostEditChange'd: the audit reads the editor-only graph directly, and a
// shader compile of a deliberately broken material would only add log noise.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionLandscapeGrassOutput.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

// Named, prefixed namespace: test TUs merge under Unity, so anonymous-namespace helpers collide.
namespace MaterialAuditTest
{
    UMaterial* CreateMaterial(FAutomationTestBase& Test, FString& OutAssetPath, const TCHAR* Folder = TEXT("__PW_MaterialAudit"))
    {
        OutAssetPath = FString::Printf(TEXT("/Game/PinWrightTests/%s/M_Audit_%s"),
            Folder, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UPackage* Package = CreatePackage(*OutAssetPath);
        if (!Test.TestNotNull(TEXT("package created"), Package))
        {
            return nullptr;
        }
        return NewObject<UMaterial>(Package, FName(*FPackageName::GetLongPackageAssetName(OutAssetPath)),
            RF_Public | RF_Standalone);
    }

    template <typename T>
    T* AddExpr(UMaterial* Material)
    {
        T* Expr = NewObject<T>(Material);
        Expr->MaterialExpressionGuid = FGuid::NewGuid();
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Expr);
        return Expr;
    }

    TSharedPtr<FJsonObject> RunAudit(FAutomationTestBase& Test, const FString& AssetPath,
        const TCHAR* FailOn = TEXT("error"), const TArray<FString>& Checks = {})
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("assets"), { MakeShared<FJsonValueString>(AssetPath) });
        Payload->SetStringField(TEXT("failOn"), FailOn);
        if (Checks.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> CheckValues;
            for (const FString& Check : Checks)
            {
                CheckValues.Add(MakeShared<FJsonValueString>(Check));
            }
            Payload->SetArrayField(TEXT("checks"), CheckValues);
        }
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("material.audit is registered"), InvokeHandlerWithCapture(TEXT("material.audit"), Payload, Capture));
        Test.TestTrue(TEXT("material.audit succeeded"), Capture.bSuccess);
        return Capture.bSuccess ? Capture.Result : nullptr;
    }

    bool Pass(const TSharedPtr<FJsonObject>& Result)
    {
        return Result.IsValid() && Result->GetBoolField(TEXT("pass"));
    }

    // Every finding of `Check`, optionally narrowed to one node.
    TArray<TSharedPtr<FJsonObject>> Findings(const TSharedPtr<FJsonObject>& Result, const TCHAR* Check,
        const UMaterialExpression* Node = nullptr)
    {
        TArray<TSharedPtr<FJsonObject>> Out;
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("findings"), Rows))
        {
            return Out;
        }
        for (const TSharedPtr<FJsonValue>& Row : *Rows)
        {
            const TSharedPtr<FJsonObject> Finding = Row->AsObject();
            FString FindingCheck, NodeId;
            Finding->TryGetStringField(TEXT("check"), FindingCheck);
            Finding->TryGetStringField(TEXT("nodeId"), NodeId);
            if (FindingCheck == Check && (!Node || NodeId == Node->MaterialExpressionGuid.ToString()))
            {
                Out.Add(Finding);
            }
        }
        return Out;
    }

    FString Severity(const TSharedPtr<FJsonObject>& Finding)
    {
        return Finding.IsValid() ? Finding->GetStringField(TEXT("severity")) : FString();
    }
}

using namespace MaterialAuditTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditIslandTest, "PinWright.material.audit.IslandIsFlaggedOnItsNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditIslandTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    UMaterialExpressionConstant3Vector* Wired = AddExpr<UMaterialExpressionConstant3Vector>(Material);
    Material->GetEditorOnlyData()->BaseColor.Expression = Wired;
    UMaterialExpressionConstant* Island = AddExpr<UMaterialExpressionConstant>(Material);

    const TSharedPtr<FJsonObject> Any = RunAudit(*this, Path, TEXT("any"));
    TestFalse(TEXT("an island fails failOn:any"), Pass(Any));
    const TArray<TSharedPtr<FJsonObject>> OnIsland = Findings(Any, TEXT("island"), Island);
    TestEqual(TEXT("one island finding on the orphan node"), OnIsland.Num(), 1);
    TestEqual(TEXT("an island is a warning"), Severity(OnIsland.IsEmpty() ? nullptr : OnIsland[0]), FString(TEXT("warning")));
    TestEqual(TEXT("the wired node is not an island"), Findings(Any, TEXT("island"), Wired).Num(), 0);

    // Islands are warnings: the default failOn:error bar lets this material pass.
    TestTrue(TEXT("default failOn passes a warning-only material"), Pass(RunAudit(*this, Path)));

    // Failure direction: wire the orphan in and the finding goes away.
    Material->GetEditorOnlyData()->Roughness.Expression = Island;
    TestEqual(TEXT("wired node is no longer an island"), Findings(RunAudit(*this, Path, TEXT("any")), TEXT("island")).Num(), 0);
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditNullTextureTest, "PinWright.material.audit.NullTextureIsAnErrorOnItsNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditNullTextureTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    UMaterialExpressionTextureSample* Sample = AddExpr<UMaterialExpressionTextureSample>(Material);
    Material->GetEditorOnlyData()->BaseColor.Expression = Sample;

    const TSharedPtr<FJsonObject> Result = RunAudit(*this, Path);
    TestFalse(TEXT("a reachable null texture fails the default bar"), Pass(Result));
    const TArray<TSharedPtr<FJsonObject>> Rows = Findings(Result, TEXT("null_texture"), Sample);
    TestEqual(TEXT("one null_texture finding on the sample"), Rows.Num(), 1);
    TestEqual(TEXT("reachable null texture is an error"), Severity(Rows.IsEmpty() ? nullptr : Rows[0]), FString(TEXT("error")));

    Sample->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
    if (TestNotNull(TEXT("engine default texture loads"), Sample->Texture.Get()))
    {
        TestEqual(TEXT("assigned texture clears the finding"), Findings(RunAudit(*this, Path), TEXT("null_texture")).Num(), 0);
    }
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditNullFunctionTest, "PinWright.material.audit.NullFunctionIsAnErrorOnItsNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditNullFunctionTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    UMaterialExpressionMaterialFunctionCall* Call = AddExpr<UMaterialExpressionMaterialFunctionCall>(Material);
    Material->GetEditorOnlyData()->BaseColor.Expression = Call;

    const TSharedPtr<FJsonObject> Result = RunAudit(*this, Path);
    TestFalse(TEXT("a reachable null function fails"), Pass(Result));
    const TArray<TSharedPtr<FJsonObject>> Rows = Findings(Result, TEXT("null_function"), Call);
    TestEqual(TEXT("one null_function finding on the call"), Rows.Num(), 1);
    TestEqual(TEXT("reachable null function is an error"), Severity(Rows.IsEmpty() ? nullptr : Rows[0]), FString(TEXT("error")));
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditMaskedTest, "PinWright.material.audit.MaskedWithoutOpacityMaskFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditMaskedTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    Material->BlendMode = BLEND_Masked;
    Material->GetEditorOnlyData()->BaseColor.Expression = AddExpr<UMaterialExpressionConstant3Vector>(Material);

    const TSharedPtr<FJsonObject> Result = RunAudit(*this, Path);
    TestFalse(TEXT("Masked without OpacityMask fails"), Pass(Result));
    const TArray<TSharedPtr<FJsonObject>> Rows = Findings(Result, TEXT("blend_output_mismatch"));
    TestEqual(TEXT("one blend_output_mismatch finding"), Rows.Num(), 1);
    if (Rows.Num() == 1)
    {
        TestEqual(TEXT("it names OpacityMask"), Rows[0]->GetStringField(TEXT("property")), FString(TEXT("MP_OpacityMask")));
        TestEqual(TEXT("it is an error"), Severity(Rows[0]), FString(TEXT("error")));
    }

    Material->GetEditorOnlyData()->OpacityMask.Expression = AddExpr<UMaterialExpressionConstant>(Material);
    TestTrue(TEXT("wiring OpacityMask passes"), Pass(RunAudit(*this, Path, TEXT("any"))));
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditIgnoredPinTest, "PinWright.material.audit.OpaqueWithOpacityPinIsAWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditIgnoredPinTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    Material->BlendMode = BLEND_Opaque;
    Material->GetEditorOnlyData()->BaseColor.Expression = AddExpr<UMaterialExpressionConstant3Vector>(Material);
    Material->GetEditorOnlyData()->Opacity.Expression = AddExpr<UMaterialExpressionConstant>(Material);

    const TArray<TSharedPtr<FJsonObject>> Rows = Findings(RunAudit(*this, Path, TEXT("any")), TEXT("blend_output_mismatch"));
    TestEqual(TEXT("one ignored-pin finding"), Rows.Num(), 1);
    if (Rows.Num() == 1)
    {
        TestEqual(TEXT("it names Opacity"), Rows[0]->GetStringField(TEXT("property")), FString(TEXT("MP_Opacity")));
        TestEqual(TEXT("an ignored pin is a warning"), Severity(Rows[0]), FString(TEXT("warning")));
    }
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditParamsTest, "PinWright.material.audit.UnusedAndConflictingParametersAreFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditParamsTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    // "Shared": two identical scalar copies - one parameter, deliberate, must NOT be flagged.
    UMaterialExpressionScalarParameter* SharedA = AddExpr<UMaterialExpressionScalarParameter>(Material);
    UMaterialExpressionScalarParameter* SharedB = AddExpr<UMaterialExpressionScalarParameter>(Material);
    SharedA->ParameterName = SharedB->ParameterName = TEXT("Shared");
    Material->GetEditorOnlyData()->Roughness.Expression = SharedA;
    Material->GetEditorOnlyData()->Metallic.Expression = SharedB;
    // "Clash": a scalar and a vector under one name.
    UMaterialExpressionScalarParameter* ClashScalar = AddExpr<UMaterialExpressionScalarParameter>(Material);
    UMaterialExpressionVectorParameter* ClashVector = AddExpr<UMaterialExpressionVectorParameter>(Material);
    ClashScalar->ParameterName = ClashVector->ParameterName = TEXT("Clash");
    Material->GetEditorOnlyData()->Specular.Expression = ClashScalar;
    Material->GetEditorOnlyData()->BaseColor.Expression = ClashVector;
    // "Orphan": reached by nothing.
    UMaterialExpressionScalarParameter* Orphan = AddExpr<UMaterialExpressionScalarParameter>(Material);
    Orphan->ParameterName = TEXT("Orphan");

    const TSharedPtr<FJsonObject> Result = RunAudit(*this, Path);
    TestFalse(TEXT("a type clash fails the default bar"), Pass(Result));

    const TArray<TSharedPtr<FJsonObject>> Dupes = Findings(Result, TEXT("duplicate_param"));
    TestEqual(TEXT("only the clash is a duplicate_param finding"), Dupes.Num(), 1);
    if (Dupes.Num() == 1)
    {
        TestEqual(TEXT("it names Clash"), Dupes[0]->GetStringField(TEXT("parameterName")), FString(TEXT("Clash")));
        TestEqual(TEXT("a type clash is an error"), Severity(Dupes[0]), FString(TEXT("error")));
        TestEqual(TEXT("nodeId is the second Clash node"), Dupes[0]->GetStringField(TEXT("nodeId")),
            ClashVector->MaterialExpressionGuid.ToString());
    }

    const TArray<TSharedPtr<FJsonObject>> Unused = Findings(Result, TEXT("unused_param"));
    TestEqual(TEXT("only the orphan is unused"), Unused.Num(), 1);
    TestEqual(TEXT("unused_param names the orphan node"), Findings(Result, TEXT("unused_param"), Orphan).Num(), 1);
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditUvWidthTest, "PinWright.material.audit.WideCoordinatesIntoA2DTextureAreFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditUvWidthTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    UMaterialExpressionTextureSample* Sample = AddExpr<UMaterialExpressionTextureSample>(Material);
    Sample->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
    if (!TestNotNull(TEXT("engine default texture loads"), Sample->Texture.Get())) { CleanupTestAsset(Path); return true; }
    Sample->Coordinates.Expression = AddExpr<UMaterialExpressionConstant3Vector>(Material);
    Material->GetEditorOnlyData()->BaseColor.Expression = Sample;

    const TArray<TSharedPtr<FJsonObject>> Rows = Findings(RunAudit(*this, Path, TEXT("any")), TEXT("uv_width"), Sample);
    TestEqual(TEXT("float3 into a 2D texture is flagged on the sample"), Rows.Num(), 1);
    if (Rows.Num() == 1)
    {
        TestEqual(TEXT("coordinateWidth"), static_cast<int32>(Rows[0]->GetNumberField(TEXT("coordinateWidth"))), 3);
        TestEqual(TEXT("expectedWidth"), static_cast<int32>(Rows[0]->GetNumberField(TEXT("expectedWidth"))), 2);
    }
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditBudgetTest, "PinWright.material.audit.ExpressionBudgetIsFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditBudgetTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    for (int32 Index = 0; Index < 201; ++Index)
    {
        AddExpr<UMaterialExpressionConstant>(Material);
    }
    const TSharedPtr<FJsonObject> Result = RunAudit(*this, Path, TEXT("any"), { TEXT("expression_budget") });
    TestFalse(TEXT("201 expressions fail failOn:any"), Pass(Result));
    TestEqual(TEXT("one expression_budget finding"), Findings(Result, TEXT("expression_budget")).Num(), 1);
    TestEqual(TEXT("unselected island check reported nothing"), Findings(Result, TEXT("island")).Num(), 0);
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditCleanTest, "PinWright.material.audit.CleanMaterialPasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditCleanTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path, TEXT("__PW_MaterialAuditClean"));
    if (!Material) { return true; }
    Material->GetEditorOnlyData()->BaseColor.Expression = AddExpr<UMaterialExpressionConstant3Vector>(Material);
    UMaterialExpressionScalarParameter* Rough = AddExpr<UMaterialExpressionScalarParameter>(Material);
    Rough->ParameterName = TEXT("Roughness");
    Material->GetEditorOnlyData()->Roughness.Expression = Rough;
    FAssetRegistryModule::AssetCreated(Material);

    const TSharedPtr<FJsonObject> Result = RunAudit(*this, Path, TEXT("any"));
    TestTrue(TEXT("clean material passes failOn:any"), Pass(Result));
    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    TestTrue(TEXT("findings present"), Result.IsValid() && Result->TryGetArrayField(TEXT("findings"), Rows));
    TestEqual(TEXT("zero findings"), Rows ? Rows->Num() : -1, 0);

    // Folder mode reaches the same material through the asset registry.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("folder"), TEXT("/Game/PinWrightTests/__PW_MaterialAuditClean"));
    Payload->SetStringField(TEXT("failOn"), TEXT("any"));
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("material.audit"), Payload, Capture);
    TestTrue(TEXT("folder audit succeeded"), Capture.bSuccess);
    if (Capture.bSuccess)
    {
        const TSharedPtr<FJsonObject> Summary = Capture.Result->GetObjectField(TEXT("summary"));
        TestTrue(TEXT("folder audit saw the material"), Summary->GetNumberField(TEXT("subjects")) >= 1);
        TestEqual(TEXT("buckets sum to subjects"),
            Summary->GetNumberField(TEXT("clean")) + Summary->GetNumberField(TEXT("flagged"))
                + Summary->GetNumberField(TEXT("unrunnable")) + Summary->GetNumberField(TEXT("notApplicable")),
            Summary->GetNumberField(TEXT("subjects")));
    }
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditRootsTest, "PinWright.material.audit.CustomOutputAndRerouteRootsAreNotIslands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditRootsTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    // Landscape grass output: a CustomOutput reached by no material property.
    UMaterialExpressionLandscapeGrassOutput* Grass = AddExpr<UMaterialExpressionLandscapeGrassOutput>(Material);
    FGrassInput& GrassInput = Grass->GrassTypes.AddDefaulted_GetRef();
    GrassInput.Name = TEXT("Grass");
    GrassInput.Input.Expression = AddExpr<UMaterialExpressionConstant>(Material);
    // Named reroute: declaration reached only through the usage's Declaration pointer.
    UMaterialExpressionNamedRerouteDeclaration* Declaration = AddExpr<UMaterialExpressionNamedRerouteDeclaration>(Material);
    Declaration->Input.Expression = AddExpr<UMaterialExpressionConstant>(Material);
    UMaterialExpressionNamedRerouteUsage* Usage = AddExpr<UMaterialExpressionNamedRerouteUsage>(Material);
    Usage->Declaration = Declaration;
    Material->GetEditorOnlyData()->Roughness.Expression = Usage;
    // Landscape layer blend: layer inputs live in the node's own array.
    UMaterialExpressionLandscapeLayerBlend* Blend = AddExpr<UMaterialExpressionLandscapeLayerBlend>(Material);
    FLayerBlendInput& Layer = Blend->Layers.AddDefaulted_GetRef();
    Layer.LayerName = TEXT("Rock");
    Layer.LayerInput.Expression = AddExpr<UMaterialExpressionConstant3Vector>(Material);
    Material->GetEditorOnlyData()->BaseColor.Expression = Blend;

    const TSharedPtr<FJsonObject> Result = RunAudit(*this, Path, TEXT("any"), { TEXT("island") });
    TestEqual(TEXT("no false island through custom output, reroute or layer blend"), Findings(Result, TEXT("island")).Num(), 0);
    TestTrue(TEXT("island-only audit passes"), Pass(Result));

    // Failure direction: the same graph with a genuine orphan still reports it.
    UMaterialExpressionConstant* Orphan = AddExpr<UMaterialExpressionConstant>(Material);
    TestEqual(TEXT("a real orphan is still found"), Findings(RunAudit(*this, Path, TEXT("any"), { TEXT("island") }), TEXT("island"), Orphan).Num(), 1);
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditLayerStackTest, "PinWright.material.audit.LayerStackMaterialHasNoFalseIslands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditLayerStackTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }
    Material->bUseMaterialAttributes = true;
    Material->GetEditorOnlyData()->MaterialAttributes.Expression = AddExpr<UMaterialExpressionMaterialAttributeLayers>(Material);

    const TSharedPtr<FJsonObject> Result = RunAudit(*this, Path, TEXT("any"));
    TestEqual(TEXT("no island in a layer-stack material"), Findings(Result, TEXT("island")).Num(), 0);
    // The blend check cannot read per-property pins in attributes mode: unrunnable, never clean.
    const TArray<TSharedPtr<FJsonObject>> Blend = Findings(Result, TEXT("blend_output_mismatch"));
    TestEqual(TEXT("blend check reports unrunnable"), Blend.Num(), 1);
    if (Blend.Num() == 1)
    {
        TestEqual(TEXT("status unrunnable"), Blend[0]->GetStringField(TEXT("status")), FString(TEXT("unrunnable")));
    }
    TestFalse(TEXT("an unrunnable check fails even failOn:none"), Pass(RunAudit(*this, Path, TEXT("none"))));
    CleanupTestAsset(Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuditArgumentsTest, "PinWright.material.audit.BadArgumentsAreErrorsAndMissingAssetsUnrunnable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAuditArgumentsTest::RunTest(const FString&)
{
    FString Path;
    UMaterial* Material = CreateMaterial(*this, Path);
    if (!Material) { return true; }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("assets"), { MakeShared<FJsonValueString>(Path) });
    Payload->SetArrayField(TEXT("checks"), { MakeShared<FJsonValueString>(TEXT("isand")) });
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("material.audit"), Payload, Capture);
    TestFalse(TEXT("a typo in checks is an error"), Capture.bSuccess);
    TestEqual(TEXT("AUDIT_UNKNOWN_CHECK"), Capture.ErrorCode, FString(TEXT("AUDIT_UNKNOWN_CHECK")));

    const TSharedPtr<FJsonObject> Missing = RunAudit(*this, Path + TEXT("_DoesNotExist"));
    TestFalse(TEXT("a path that loads nothing fails pass"), Pass(Missing));
    if (Missing.IsValid())
    {
        TestEqual(TEXT("it is an unrunnable subject"),
            static_cast<int32>(Missing->GetObjectField(TEXT("summary"))->GetNumberField(TEXT("unrunnable"))), 1);
    }
    CleanupTestAsset(Path);
    return true;
}
