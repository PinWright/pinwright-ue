// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for material.authoring.create_material_layer and
// material.authoring.create_material_layer_blend.
//
// These tests guard the bug the original create_material_function could not
// satisfy: a plain UMaterialFunction is a *sibling* subclass of
// UMaterialFunctionMaterialLayer / UMaterialFunctionMaterialLayerBlend, so
// Cast<UMaterialFunctionMaterialLayer>(PlainFunction) returns nullptr and
// the asset cannot be assigned to a layer/blend slot of
// UMaterialExpressionMaterialAttributeLayers.
//
// Counterfactual: if the fix is reverted (handler uses
// UMaterialFunctionFactoryNew), the cast to UMaterialFunctionMaterialLayer
// returns nullptr because plain UMaterialFunction is a sibling subclass —
// Cast<UMaterialFunctionMaterialLayer> discriminates on specific subclass
// identity.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "UObject/Package.h"
#include "EditorAssetLibrary.h"

namespace
{
// Test content paths kept under a discoverable sandbox so a stale asset from
// a prior run is overwritten rather than colliding with a real project asset.
const TCHAR* const kLayerTestPath = TEXT("/Game/__PW_GatewayTests/Layers");
const TCHAR* const kBlendTestPath = TEXT("/Game/__PW_GatewayTests/LayerBlends");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCreateMaterialLayerYieldsSubclassTest,
    "PinWright.material.authoring.create_material_layer.ReturnsCorrectSubclass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAuthoringCreateMaterialLayerYieldsSubclassTest::RunTest(const FString& Parameters)
{
    const FString AssetName = TEXT("PW_LayerSubclassTest");
    const FString FullPath = FString(kLayerTestPath) / AssetName + TEXT(".") + AssetName;

    // Clean up any prior asset from a previous run. CleanupTestAsset, not
    // UEditorAssetLibrary::DeleteAsset: the discard path frees the package path and removes
    // the .uasset without ObjectTools::ForceDeleteObjects, so the handler below can create at
    // the same path and this pre-clean costs no full-object-graph reference sweep.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        CleanupTestAsset(FString(kLayerTestPath) / AssetName);
    }

    // The handler creates with save:false, leaving a dirty in-memory package that a
    // later save-all would flush to disk. Remove it on every exit path.
    ON_SCOPE_EXIT { CleanupTestAsset(FString(kLayerTestPath) / AssetName); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("path"), kLayerTestPath);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("material.authoring.create_material_layer"), Payload, Capture);
    TestTrue(TEXT("handler registered"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("response reports success"), Capture.bSuccess))
    {
        return true;
    }

    UObject* Loaded = LoadObject<UObject>(nullptr, *FullPath);
    TestNotNull(TEXT("asset loaded"), Loaded);
    if (!Loaded) return true;

    // The load-bearing assertion: must be the layer subclass, NOT a plain
    // UMaterialFunction. If the handler regresses to UMaterialFunctionFactoryNew,
    // this cast returns nullptr.
    TestNotNull(TEXT("loaded is UMaterialFunctionMaterialLayer"),
        Cast<UMaterialFunctionMaterialLayer>(Loaded));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringCreateMaterialLayerBlendYieldsSubclassTest,
    "PinWright.material.authoring.create_material_layer_blend.ReturnsCorrectSubclass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAuthoringCreateMaterialLayerBlendYieldsSubclassTest::RunTest(const FString& Parameters)
{
    const FString AssetName = TEXT("PW_LayerBlendSubclassTest");
    const FString FullPath = FString(kBlendTestPath) / AssetName + TEXT(".") + AssetName;

    // Same discard-based pre-clean as the layer test above.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        CleanupTestAsset(FString(kBlendTestPath) / AssetName);
    }

    // The handler creates with save:false, leaving a dirty in-memory package that a
    // later save-all would flush to disk. Remove it on every exit path.
    ON_SCOPE_EXIT { CleanupTestAsset(FString(kBlendTestPath) / AssetName); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("path"), kBlendTestPath);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("material.authoring.create_material_layer_blend"), Payload, Capture);
    TestTrue(TEXT("handler registered"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("response reports success"), Capture.bSuccess))
    {
        return true;
    }

    UObject* Loaded = LoadObject<UObject>(nullptr, *FullPath);
    TestNotNull(TEXT("asset loaded"), Loaded);
    if (!Loaded) return true;

    TestNotNull(TEXT("loaded is UMaterialFunctionMaterialLayerBlend"),
        Cast<UMaterialFunctionMaterialLayerBlend>(Loaded));

    return true;
}
