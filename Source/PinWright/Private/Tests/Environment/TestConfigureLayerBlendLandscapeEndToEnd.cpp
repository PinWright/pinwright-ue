// Copyright (c) 2026 Alexander Penkin. MIT License.

// End-to-end regression test for B-configure-layer-blend-wrong-nodes.
//
// WHY A SECOND TEST. The sibling unit test
// (Tests/Material/TestConfigureLayerBlendCreatesLayerBlendNode.cpp) proves the verb builds a
// UMaterialExpressionLandscapeLayerBlend carrying the requested entries. That is the defect
// itself, but it is not the ticket's failure mode: the ticket is that a caller "gets a clean
// success at every step and a landscape material with zero target layers". Each step in the
// chain — create the material, configure the blend, assign it, paint — reported success
// individually while the chain as a whole produced nothing paintable. Only a test that walks
// the whole chain can fail on that, so this one does:
//
//   material.authoring.create_landscape_material
//     -> material.authoring.configure_layer_blend
//       -> the landscape's own target-layer harvest
//         -> landscape.create_procedural_terrain (the layer-paint verb)
//
// The two landscape-side assertions are the ones the pre-fix code could not satisfy:
//   * ALandscapeProxy::RetrieveTargetLayerNamesFromMaterials() contains the requested names.
//     Pre-fix this list was EMPTY, because a ScalarParameter does not implement
//     GetLandscapeLayerNames and so declares no target layer.
//   * The paint reports texelsWithWeight > 0. Pre-fix the paint could not even be attempted:
//     it correctly refused with LANDSCAPE_MATERIAL_NO_LAYERS — which is how the diagnosis
//     landed on the paint verb (B-create-procedural-terrain-paints-nothing, already closed)
//     instead of on the verb that was supposed to create the layers.
//
// Helpers here are file-scope statics with distinctive names rather than an anonymous
// namespace: Tests/ TUs merge under Unity and an anonymous-namespace helper collides with the
// same-named helper in Tests/Environment/TestLandscapePaintLayerHonesty.cpp (docs/lessons.md).
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"

// Reads a string array field off a response object. Returns an empty array when absent.
static TArray<FString> ReadStringArrayForLayerBlendE2E(
    const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
    TArray<FString> Out;
    const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
    if (Object.IsValid() && Object->TryGetArrayField(FieldName, Array) && Array)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Array)
        {
            if (Value.IsValid())
            {
                Out.Add(Value->AsString());
            }
        }
    }
    return Out;
}

// Creates the minimal landscape landscape.create can make (1x1 component of 7-quad
// subsections -> 8x8 vertices) so the paint and its weightmap readback stay cheap.
static ALandscape* CreateTinyLandscapeForLayerBlendE2E(
    FAutomationTestBase& Test, UWorld* World, const FString& Label, const FString& MaterialPath)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Label);
    Payload->SetNumberField(TEXT("componentsX"), 1);
    Payload->SetNumberField(TEXT("componentsY"), 1);
    Payload->SetNumberField(TEXT("quadsPerComponent"), 7);
    Payload->SetNumberField(TEXT("sectionsPerComponent"), 1);
    Payload->SetStringField(TEXT("materialPath"), MaterialPath);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    if (!InvokeHandlerWithSharedCapture(TEXT("landscape.create"), Payload, Capture))
    {
        Test.AddError(TEXT("landscape.create handler is not registered"));
        return nullptr;
    }
    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);
    Test.TestTrue(TEXT("landscape.create fixture succeeded"), Capture->bSuccess);
    if (!Capture->bSuccess)
    {
        return nullptr;
    }

    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        if (It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
        {
            return *It;
        }
    }
    Test.AddError(TEXT("landscape.create reported success but no actor with that label exists"));
    return nullptr;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringConfigureLayerBlendLandscapeEndToEndTest,
    "PinWright.material.authoring.configure_layer_blend.ConfiguredLandscapeHasTargetLayersAndPaints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringConfigureLayerBlendLandscapeEndToEndTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping configure_layer_blend end-to-end test"));
        return true;
    }

    if (!TestTrue(TEXT("material.authoring.configure_layer_blend handler registered"),
            IsHandlerRegistered(TEXT("material.authoring.configure_layer_blend"))))
    {
        return false;
    }

    // Two distinct names so a de-duplicating or off-by-one implementation cannot pass by
    // accident. PW_ prefixed so they cannot collide with a host project's real layers.
    const FString PaintedLayer = TEXT("PW_E2EGrass");
    const FString OtherLayer = TEXT("PW_E2ERock");

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/E2ELayerBlend_%s"), *Suffix);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    // --- Step 1: a landscape material, built the way a caller would. ---
    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("sandbox package created"), Package))
    {
        return false;
    }
    UMaterial* Material = NewObject<UMaterial>(
        Package, FName(*FPackageName::GetLongPackageAssetName(AssetPath)), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("sandbox material created"), Material))
    {
        return false;
    }
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    // --- Step 2: configure_layer_blend. ---
    TSharedPtr<FJsonObject> ConfigurePayload = MakeShared<FJsonObject>();
    ConfigurePayload->SetStringField(TEXT("assetPath"), AssetPath);
    TArray<TSharedPtr<FJsonValue>> LayersJson;
    for (const FString& LayerName : { PaintedLayer, OtherLayer })
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), LayerName);
        Entry->SetStringField(TEXT("blendType"), TEXT("LB_WeightBlend"));
        LayersJson.Add(MakeShared<FJsonValueObject>(Entry));
    }
    ConfigurePayload->SetArrayField(TEXT("layers"), LayersJson);
    ConfigurePayload->SetBoolField(TEXT("save"), false);

    TSharedRef<FTestResponseCapture> ConfigureCapture = MakeShared<FTestResponseCapture>();
    if (!TestTrue(TEXT("configure_layer_blend invoked"),
            InvokeHandlerWithSharedCapture(
                TEXT("material.authoring.configure_layer_blend"), ConfigurePayload, ConfigureCapture)))
    {
        return false;
    }
    PumpUntilCaptured(*ConfigureCapture, /*TimeoutSeconds=*/30.0);
    if (!TestTrue(TEXT("configure_layer_blend reported success"), ConfigureCapture->bSuccess))
    {
        return false;
    }

    // The verb's own report reads back through the engine's target-layer accessor rather than
    // echoing the request, so this is already a real measurement — but the landscape-side
    // assertions below are what the ticket is actually about.
    const TArray<FString> ReportedTargetLayers =
        ReadStringArrayForLayerBlendE2E(ConfigureCapture->Result, TEXT("targetLayers"));
    TestTrue(TEXT("configure_layer_blend reports the painted layer in targetLayers[]"),
        ReportedTargetLayers.Contains(PaintedLayer));
    TestTrue(TEXT("configure_layer_blend reports the second layer in targetLayers[]"),
        ReportedTargetLayers.Contains(OtherLayer));

    // Declaring layers makes the paint POSSIBLE; wiring the node makes it VISIBLE. The fixture
    // material's BaseColor is empty, so the default connectTo must have taken it — an
    // unconnected blend node compiles to no output and nothing would sample the weights.
    FString ConnectionState;
    if (ConfigureCapture->Result.IsValid())
    {
        ConfigureCapture->Result->TryGetStringField(TEXT("connectionState"), ConnectionState);
    }
    TestEqual(TEXT("blend node was wired into the empty BaseColor input"),
        ConnectionState, FString(TEXT("connected")));

    // --- Step 3: a landscape using that material. ---
    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_E2ELayerBlend_%s"), *Suffix);

    ALandscape* Landscape = CreateTinyLandscapeForLayerBlendE2E(*this, World, Label, AssetPath);
    if (!Landscape)
    {
        return false;
    }
    if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
    {
        Info->UpdateLayerInfoMap();
    }

    // Core end-to-end assertion 1: the LANDSCAPE — not the material, and not the verb's own
    // response — reports the requested names as its target layers. Pre-fix this list was
    // empty for a material configure_layer_blend had just reported success on.
    // RetrieveTargetLayerNamesFromMaterials arrived in UE 5.5. GetLayersFromMaterial is the
    // same list on 5.4 (it is the accessor 5.5 renamed and kept deprecated alongside it), so
    // the assertion below is the same assertion on both engines.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    const TArray<FName> LandscapeTargetLayers = Landscape->RetrieveTargetLayerNamesFromMaterials();
#else
    const TArray<FName> LandscapeTargetLayers = Landscape->GetLayersFromMaterial();
#endif
    TestTrue(
        *FString::Printf(TEXT("landscape declares target layer '%s' (declares: %d)"),
            *PaintedLayer, LandscapeTargetLayers.Num()),
        LandscapeTargetLayers.Contains(FName(*PaintedLayer)));
    TestTrue(
        *FString::Printf(TEXT("landscape declares target layer '%s' (declares: %d)"),
            *OtherLayer, LandscapeTargetLayers.Num()),
        LandscapeTargetLayers.Contains(FName(*OtherLayer)));

    // --- Step 4: the layer actually paints. ---
    TSharedPtr<FJsonObject> PaintPayload = MakeShared<FJsonObject>();
    PaintPayload->SetStringField(TEXT("landscapeName"), Label);
    PaintPayload->SetStringField(TEXT("layerName"), PaintedLayer);
    PaintPayload->SetNumberField(TEXT("strength"), 1.0);

    TSharedRef<FTestResponseCapture> PaintCapture = MakeShared<FTestResponseCapture>();
    TestTrue(TEXT("landscape.create_procedural_terrain handler is registered"),
        InvokeHandlerWithSharedCapture(
            TEXT("landscape.create_procedural_terrain"), PaintPayload, PaintCapture));
    PumpUntilCaptured(*PaintCapture, /*TimeoutSeconds=*/60.0);

    // Core end-to-end assertion 2: the paint is accepted. Pre-fix it was correctly refused
    // with LANDSCAPE_MATERIAL_NO_LAYERS, because the material declared nothing paintable.
    TestTrue(
        *FString::Printf(TEXT("painting the configured layer succeeds (error was '%s': %s)"),
            *PaintCapture->ErrorCode, *PaintCapture->Message),
        PaintCapture->bSuccess);

    if (PaintCapture->bSuccess && PaintCapture->Result.IsValid())
    {
        // texelsWithWeight is the paint verb's own weightmap readback, so this asserts the
        // weights landed rather than that the call returned.
        double TexelsWithWeight = 0.0;
        if (TestTrue(TEXT("paint response carries texelsWithWeight"),
                PaintCapture->Result->TryGetNumberField(TEXT("texelsWithWeight"), TexelsWithWeight)))
        {
            TestTrue(
                *FString::Printf(TEXT("paint wrote a non-zero weight somewhere (texelsWithWeight=%.0f)"),
                    TexelsWithWeight),
                TexelsWithWeight > 0.0);
        }
    }

    return true;
}
