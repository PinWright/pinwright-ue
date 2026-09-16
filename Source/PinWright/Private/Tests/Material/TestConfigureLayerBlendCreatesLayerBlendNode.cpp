// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for "material.authoring.configure_layer_blend configures no layers".
//
// THE DEFECT. The verb's entire reason to exist is to put a landscape's paintable target
// layers on a master material. It did not create a LandscapeLayerBlend node at all: for
// each requested layer it created a UMaterialExpressionScalarParameter named after the
// layer (MaterialAuthoringHandler.cpp, the `NewObject<UMaterialExpressionScalarParameter>`
// loop) and added it to the expression collection.
//
// A ScalarParameter is not a target-layer declaration. Landscape target layers are
// harvested by FMaterialCachedExpressionData::UpdateForExpressions calling
// UMaterialExpression::GetLandscapeLayerNames on every expression in the collection; the
// base implementation is an empty body (MaterialExpression.h:591) and ScalarParameter does
// not override it, while UMaterialExpressionLandscapeLayerBlend returns one name per
// FLayerBlendInput (MaterialExpressionLandscapeLayerBlend.cpp:374-380). So the pre-fix verb
// produced a material declaring ZERO target layers, and every downstream paint verb then
// correctly refused with LAYER_NOT_FOUND / LANDSCAPE_MATERIAL_NO_LAYERS — pointing the
// diagnosis at the paint verb rather than at the verb that was supposed to create the
// layers.
//
// WHY SUCCESS ASSERTIONS ARE WORTHLESS HERE. Every step reported clean: the handler found
// the material, created N expressions, pushed the master edit through
// ApplyMasterMaterialEdit, and answered success with `layerCount: N` and N `nodeIds`. The
// sibling coverage in Tests/Assets/TestMaterialHandlers.cpp
// (configure_layer_blend.ValidParamsNoCrash) asserts exactly that shape and passes against
// the defect. This test therefore asserts on the RESULTING MATERIAL, never on the response.
//
// DIFFERENTIAL PROPERTY. On a material created empty:
//   * pre-fix  0 LandscapeLayerBlend nodes, 3 ScalarParameters named after the layers,
//              0 declared target layer names                                    -> FAILS
//   * post-fix 1 LandscapeLayerBlend node, 0 ScalarParameters, 3 declared names  -> PASSES
// The node CLASS is asserted (not merely "an expression exists"), because the defect did
// create expressions — it created the wrong ones. The declared-name set is read through
// GetLandscapeLayerNames, the same accessor ALandscapeProxy::RetrieveTargetLayerNamesFromMaterials
// uses, so a node that exists but declares nothing cannot pass.
//
// UMaterialExpressionLandscapeLayerBlend is UCLASS(MinimalAPI) in the Landscape module, so
// it is resolved by reflection and its `Layers` array read through FArrayProperty, per the
// plugin's cross-module convention (see Tests/Environment/TestLandscapePaintLayerHonesty.cpp).
// No helper is factored out of this file: anonymous-namespace helpers in Tests/ merge across
// TUs under Unity and collide with sibling material tests (docs/lessons.md).
#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringConfigureLayerBlendCreatesLayerBlendNodeTest,
    "PinWright.material.authoring.configure_layer_blend.CreatesLandscapeLayerBlendNodeWithRequestedLayers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringConfigureLayerBlendCreatesLayerBlendNodeTest::RunTest(const FString& Parameters)
{
    if (!TestTrue(TEXT("material.authoring.configure_layer_blend handler registered"),
            IsHandlerRegistered(TEXT("material.authoring.configure_layer_blend"))))
    {
        return false;
    }

    // The node class the verb must produce. Resolved by reflection because the class is
    // MinimalAPI in the Landscape module; /Script/Landscape is always loaded here (the
    // plugin links Landscape), so a null result is a real failure, not a host difference.
    UClass* LayerBlendClass = FindObject<UClass>(
        nullptr, TEXT("/Script/Landscape.MaterialExpressionLandscapeLayerBlend"));
    if (!TestNotNull(TEXT("UMaterialExpressionLandscapeLayerBlend class resolved"), LayerBlendClass))
    {
        return false;
    }

    // Three distinct names, so an off-by-one or a de-duplicating implementation cannot
    // pass by accident and the requested set is unambiguous.
    const TArray<FString> RequestedLayers = {
        TEXT("PW_LayerGrass"), TEXT("PW_LayerRock"), TEXT("PW_LayerSnow")
    };

    // Throwaway sandbox package — nothing is written to the host project's Content tree
    // (the call below passes save:false, and the package is deleted on scope exit).
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/ConfigureLayerBlend_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("sandbox package created"), Package))
    {
        return false;
    }

    UMaterial* Material = NewObject<UMaterial>(
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("sandbox material created"), Material))
    {
        return false;
    }
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    // The fixture starts with an empty expression collection, so every expression counted
    // below was created by the verb under test.
    if (!TestEqual(TEXT("fixture material starts with no expressions"),
            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Num(), 0))
    {
        return false;
    }

    // --- Drive the verb. ---
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    TArray<TSharedPtr<FJsonValue>> LayersJson;
    for (const FString& LayerName : RequestedLayers)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), LayerName);
        Entry->SetStringField(TEXT("blendType"), TEXT("LB_WeightBlend"));
        LayersJson.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Payload->SetArrayField(TEXT("layers"), LayersJson);
    // Keep the sandbox asset off disk; the node creation under test runs either way.
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("configure_layer_blend invoked"),
            InvokeHandlerWithCapture(
                TEXT("material.authoring.configure_layer_blend"), Payload, Capture)))
    {
        return false;
    }
    TestTrue(TEXT("configure_layer_blend responded"), Capture.bWasCalled);
    // Success both pre- and post-fix — that clean success is the CONTEXT for this test,
    // not anything it distinguishes. The assertions that follow read the material.
    TestTrue(TEXT("configure_layer_blend reported success"), Capture.bSuccess);
    if (!Capture.bWasCalled)
    {
        return false;
    }

    // --- Assertion 1: the created expression is a LandscapeLayerBlend, and no
    //     ScalarParameter stood in for it. ---
    int32 LayerBlendNodeCount = 0;
    UMaterialExpression* BlendNode = nullptr;
    TArray<FString> StrayScalarParameterNames;
    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr :
         Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
    {
        UMaterialExpression* Expression = ExpressionPtr.Get();
        if (!Expression)
        {
            continue;
        }
        // Class identity, not IsA: a subclass would be a different node type than the one
        // a landscape's target-layer harvest expects.
        if (Expression->GetClass() == LayerBlendClass)
        {
            ++LayerBlendNodeCount;
            BlendNode = Expression;
        }
        if (UMaterialExpressionScalarParameter* Scalar =
                Cast<UMaterialExpressionScalarParameter>(Expression))
        {
            StrayScalarParameterNames.Add(Scalar->ParameterName.ToString());
        }
    }

    TestEqual(
        TEXT("configure_layer_blend created exactly one UMaterialExpressionLandscapeLayerBlend "
             "(pre-fix it created none)"),
        LayerBlendNodeCount, 1);
    TestEqual(
        *FString::Printf(
            TEXT("no ScalarParameter was created in the blend node's place (found: [%s])"),
            *FString::Join(StrayScalarParameterNames, TEXT(", "))),
        StrayScalarParameterNames.Num(), 0);

    if (!BlendNode)
    {
        // Nothing further can be asserted; the failures above already name the defect.
        return false;
    }

    // --- Assertion 2a: the node's target-layer entry count equals the request. ---
    // Read through FArrayProperty rather than GetLandscapeLayerNames so a node carrying
    // duplicate or padding entries fails here (GetLandscapeLayerNames AddUnique's).
    FArrayProperty* LayersProp = FindFProperty<FArrayProperty>(LayerBlendClass, TEXT("Layers"));
    if (TestNotNull(TEXT("LandscapeLayerBlend.Layers property resolved"), LayersProp))
    {
        FScriptArrayHelper LayersHelper(
            LayersProp, LayersProp->ContainerPtrToValuePtr<void>(BlendNode));
        TestEqual(TEXT("blend node carries one FLayerBlendInput per requested layer"),
            LayersHelper.Num(), RequestedLayers.Num());
    }

    // --- Assertion 2b: the declared layer NAMES match the request, order-insensitively. ---
    // GetLandscapeLayerNames is the accessor the engine itself harvests target layers
    // through, so this asserts the property a landscape actually consumes.
    TArray<FName> DeclaredLayerNames;
    BlendNode->GetLandscapeLayerNames(DeclaredLayerNames);
    const TSet<FName> DeclaredSet(DeclaredLayerNames);

    TestEqual(TEXT("blend node declares exactly the requested number of target layers"),
        DeclaredSet.Num(), RequestedLayers.Num());
    for (const FString& RequestedLayer : RequestedLayers)
    {
        TestTrue(
            *FString::Printf(TEXT("requested target layer '%s' is declared by the blend node"),
                *RequestedLayer),
            DeclaredSet.Contains(FName(*RequestedLayer)));
    }

    return true;
}
