// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-create-procedural-terrain-paints-nothing.
//
// landscape.create_procedural_terrain (a misnomer-named alias for "paint a
// weight-blended layer") answered EVERY call with
// {"success":true,"message":"Layer painted successfully"} — including calls whose
// named layer did not exist on the landscape material at all. The reported repro:
// layerName "Grass" on a landscape whose material had no LandscapeLayerBlend, so its
// target_layers held only __LANDSCAPE_VISIBILITY__. The handler's old "auto-create the
// layer if it is missing" branch manufactured a ULandscapeLayerInfoObject for a name no
// shader samples, wrote a weightmap nothing reads, and reported success — the same
// silent-elision class already closed for actor.spawn_batch and foliage.add_instances.
//
// The fix resolves the requested name against the layers the MATERIAL declares
// (ALandscapeProxy::RetrieveTargetLayerNamesFromMaterials / GetLayersFromMaterial) before
// painting, and rejects a name that is not there with a typed error that LISTS the names
// that are — the difference between a three-hour and a three-second diagnosis. Every
// other way this verb could report a paint it did not perform is now typed too: no
// material assigned (LANDSCAPE_NO_MATERIAL), a material with no paintable target layers
// (LANDSCAPE_MATERIAL_NO_LAYERS), a hollow landscape (LANDSCAPE_NO_COMPONENTS), and an
// empty paint region after clamping (INVALID_ARGUMENT).
//
// Fixtures are built IN CODE and exercise production handlers:
//   * the landscape comes from the registered landscape.create handler (a 1x1 grid of
//     7-quad subsections — 8x8 vertices — so the paint + weightmap readback are cheap),
//   * the layer-bearing material is a transient UMaterial carrying one
//     LandscapeLayerWeight expression per layer name. Those expressions declare target
//     layers through UMaterialExpression::GetLandscapeLayerNames, which
//     FMaterialCachedExpressionData::UpdateForExpressions harvests from EVERY expression
//     in the collection (connected or not), so PostEditChange() is enough to publish the
//     names — no shader compile and no on-disk asset. The expression class is MinimalAPI,
//     so it is resolved by reflection per the plugin's cross-module convention.
//
// Counterfactuals: revert the material-layer gate and the LAYER_NOT_FOUND /
// LANDSCAPE_MATERIAL_NO_LAYERS tests get success:true instead of the asserted code;
// revert the FScopedSetLandscapeEditingLayer scoping around SetAlphaData and the
// paint-lands test reads back an all-zero weightmap, because on an edit-layer landscape
// (which landscape.create's CreateDefaultLayer path produces) an unscoped write lands in
// no persistent edit layer and the next regeneration composites it away.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Compat/EngineVersionCompat.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
// ULandscapeSettings — the blend-method test skips rather than lies on a host that
// configures DefaultLayerInfoObject, because the branch it exercises is the constructed one.
#include "LandscapeSettings.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/UnrealType.h"

namespace
{
    // Name of the layer a "does not exist" call asks for. Deliberately never added to any
    // fixture material.
    const TCHAR* const kMissingLayer = TEXT("PW_LayerThatDoesNotExist");

    // Builds a transient UMaterial that declares one landscape target layer per name.
    //
    // UMaterialExpressionLandscapeLayerWeight is UCLASS(MinimalAPI), so it carries no
    // *_API export and cannot be referenced cross-module via StaticClass()/NewObject<T>;
    // resolve it by reflection and set its ParameterName through FProperty, per the
    // plugin's UE version-compat convention. Returns nullptr when the class or property
    // cannot be resolved so callers can skip rather than fail on an engine layout change.
    UMaterial* MakeLandscapeMaterialDeclaringLayers(const TArray<FName>& LayerNames)
    {
        UClass* LayerWeightClass = FindObject<UClass>(
            nullptr, TEXT("/Script/Landscape.MaterialExpressionLandscapeLayerWeight"));
        if (!LayerWeightClass)
        {
            return nullptr;
        }
        FNameProperty* ParameterNameProp =
            FindFProperty<FNameProperty>(LayerWeightClass, TEXT("ParameterName"));
        if (!ParameterNameProp)
        {
            return nullptr;
        }

        UMaterial* Material = NewObject<UMaterial>(
            GetTransientPackage(),
            FName(*FString::Printf(TEXT("PW_TestLandscapeMat_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits))),
            RF_Transient);
        if (!Material)
        {
            return nullptr;
        }

        int32 NodeY = 0;
        for (const FName& LayerName : LayerNames)
        {
            UMaterialExpression* Expression = NewObject<UMaterialExpression>(
                Material, LayerWeightClass, NAME_None, RF_Transactional);
            if (!Expression)
            {
                continue;
            }
            ParameterNameProp->SetPropertyValue_InContainer(Expression, LayerName);
            Expression->MaterialExpressionEditorX = 0;
            Expression->MaterialExpressionEditorY = NodeY;
            NodeY += 150;
            // Same collection accessor material.authoring.configure_layer_blend uses, so
            // this stays on a shape the plugin already compiles on 5.3-5.8.
            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Expression);
        }

        // Publishes LandscapeLayerNames into the material's cached expression data. The
        // expressions are intentionally left unconnected: nothing compiles them into a
        // shader, but the cached-data harvest walks the whole collection regardless.
        Material->PostEditChange();
        return Material;
    }

    // Creates a minimal landscape through the production landscape.create handler and
    // returns it, or nullptr if the create did not produce a usable actor. The component
    // grid defaults to 1x1; the region-bounding test asks for a wider one so that "outside
    // the painted region" includes a component the write never touches.
    ALandscape* CreateTinyLandscape(FAutomationTestBase& Test, UWorld* World, const FString& Label,
        int32 ComponentsX = 1, int32 ComponentsY = 1)
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), Label);
        CreatePayload->SetNumberField(TEXT("componentsX"), ComponentsX);
        CreatePayload->SetNumberField(TEXT("componentsY"), ComponentsY);
        CreatePayload->SetNumberField(TEXT("quadsPerComponent"), 7);
        CreatePayload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        if (!InvokeHandlerWithSharedCapture(TEXT("landscape.create"), CreatePayload, Capture))
        {
            Test.AddError(TEXT("landscape.create handler is not registered"));
            return nullptr;
        }
        PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);
        // The landscape is the fixture: a create that did not succeed is a failure, not a skip.
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

    // Assigns Material as the landscape material so its target layers become the
    // landscape's. Mirrors what landscape.set_material does for an on-disk asset.
    void AssignLandscapeMaterial(ALandscape* Landscape, UMaterial* Material)
    {
        Landscape->LandscapeMaterial = Material;
        Landscape->PostEditChange();
        if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
        {
            Info->UpdateLayerInfoMap();
        }
    }

    // Runs landscape.create_procedural_terrain and pumps the async response.
    void PaintLayer(FAutomationTestBase& Test, const FString& LandscapeLabel, const FString& LayerName,
        const TSharedRef<FTestResponseCapture>& Capture,
        const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& CustomizePayload)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("landscapeName"), LandscapeLabel);
        Payload->SetStringField(TEXT("layerName"), LayerName);
        CustomizePayload(Payload);

        Test.TestTrue(TEXT("landscape.create_procedural_terrain handler is registered"),
            InvokeHandlerWithSharedCapture(TEXT("landscape.create_procedural_terrain"), Payload, Capture));
        PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/60.0);
        Test.TestTrue(TEXT("landscape.create_procedural_terrain responded"), Capture->bWasCalled);
    }

    void PaintLayer(FAutomationTestBase& Test, const FString& LandscapeLabel, const FString& LayerName,
        const TSharedRef<FTestResponseCapture>& Capture)
    {
        PaintLayer(Test, LandscapeLabel, LayerName, Capture, [](const TSharedPtr<FJsonObject>&) {});
    }

    // Returns the ULandscapeLayerInfoObject bound to LayerName, or nullptr.
    ULandscapeLayerInfoObject* FindLayerInfo(ULandscapeInfo* Info, const FName& LayerName)
    {
        if (!Info)
        {
            return nullptr;
        }
        for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
        {
            if (Layer.LayerName == LayerName)
            {
                return Layer.LayerInfoObj;
            }
        }
        return nullptr;
    }

    // Reads the layer's weightmap over the landscape's full extent and returns how many
    // texels carry any weight. -1 when the extent or layer info is unavailable.
    int32 CountTexelsWithWeight(ALandscape* Landscape, const FName& LayerName)
    {
        ULandscapeInfo* Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
        ULandscapeLayerInfoObject* LayerInfo = FindLayerInfo(Info, LayerName);
        if (!Info || !LayerInfo)
        {
            return -1;
        }
        int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
        {
            return -1;
        }
        const int32 Texels = (MaxX - MinX + 1) * (MaxY - MinY + 1);
        TArray<uint8> Weights;
        Weights.SetNumZeroed(Texels);
        FLandscapeEditDataInterface Read(Info, /*bUploadTextureChangesToGPU=*/false);
        Read.GetWeightDataFast(LayerInfo, MinX, MinY, MaxX, MaxY, Weights.GetData(), /*Stride=*/0);

        int32 NonZero = 0;
        for (uint8 W : Weights)
        {
            if (W != 0)
            {
                ++NonZero;
            }
        }
        return NonZero;
    }

    // Splits the same full-extent weightmap read as CountTexelsWithWeight by whether each
    // weighted texel falls inside [RegionMinX,RegionMinY]..[RegionMaxX,RegionMaxY]. The
    // OUTSIDE half is what a region-bounded paint must leave alone on every other layer.
    // Returns false when the extent or layer info is unavailable.
    bool CountTexelsWithWeightSplitByRegion(ALandscape* Landscape, const FName& LayerName,
        int32 RegionMinX, int32 RegionMinY, int32 RegionMaxX, int32 RegionMaxY,
        int32& OutInside, int32& OutOutside)
    {
        OutInside = 0;
        OutOutside = 0;
        ULandscapeInfo* Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
        ULandscapeLayerInfoObject* LayerInfo = FindLayerInfo(Info, LayerName);
        if (!Info || !LayerInfo)
        {
            return false;
        }
        int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
        {
            return false;
        }
        const int32 SizeX = MaxX - MinX + 1;
        const int32 SizeY = MaxY - MinY + 1;
        TArray<uint8> Weights;
        Weights.SetNumZeroed(SizeX * SizeY);
        FLandscapeEditDataInterface Read(Info, /*bUploadTextureChangesToGPU=*/false);
        Read.GetWeightDataFast(LayerInfo, MinX, MinY, MaxX, MaxY, Weights.GetData(), /*Stride=*/0);

        for (int32 Y = 0; Y < SizeY; ++Y)
        {
            for (int32 X = 0; X < SizeX; ++X)
            {
                if (Weights[X + Y * SizeX] == 0)
                {
                    continue;
                }
                const int32 WorldX = MinX + X;
                const int32 WorldY = MinY + Y;
                const bool bInside = WorldX >= RegionMinX && WorldX <= RegionMaxX &&
                                     WorldY >= RegionMinY && WorldY <= RegionMaxY;
                if (bInside)
                {
                    ++OutInside;
                }
                else
                {
                    ++OutOutside;
                }
            }
        }
        return true;
    }

    // Collects the error data's availableLayers[] as strings.
    TArray<FString> ReadAvailableLayers(const TSharedPtr<FJsonObject>& Data)
    {
        TArray<FString> Out;
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Data.IsValid() && Data->TryGetArrayField(TEXT("availableLayers"), Array) && Array)
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
}

// ============================================================================
// A layer the material does not declare is rejected LAYER_NOT_FOUND, and the
// rejection LISTS the layers that ARE available.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintMissingLayerListsAvailableTest,
    "PinWright.landscape.create_procedural_terrain.MissingLayerListsAvailable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintMissingLayerListsAvailableTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape paint layer-listing test"));
        return true;
    }

    UMaterial* Material = MakeLandscapeMaterialDeclaringLayers(
        { FName(TEXT("PW_Rock")), FName(TEXT("PW_Sand")) });
    if (!Material)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UMaterialExpressionLandscapeLayerWeight could not be resolved by reflection — skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintMissingLayer_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = CreateTinyLandscape(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }
    AssignLandscapeMaterial(Landscape, Material);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, kMissingLayer, Capture);

    // Core regression assertion: a layer nobody's material declares is an ERROR. Before
    // the fix this branch auto-created the layer and returned success:true.
    TestFalse(TEXT("painting an undeclared layer is not reported as success"), Capture->bSuccess);
    TestEqual(TEXT("painting an undeclared layer returns LAYER_NOT_FOUND"),
        Capture->ErrorCode, FString(TEXT("LAYER_NOT_FOUND")));

    // The listing is the point of the ticket: the message must name what IS paintable so
    // the caller can fix the call without opening the editor.
    TestTrue(TEXT("message lists available layer PW_Rock"), Capture->Message.Contains(TEXT("PW_Rock")));
    TestTrue(TEXT("message lists available layer PW_Sand"), Capture->Message.Contains(TEXT("PW_Sand")));
    TestTrue(TEXT("message names the layer that was rejected"), Capture->Message.Contains(kMissingLayer));

    const TArray<FString> Available = ReadAvailableLayers(Capture->Result);
    TestTrue(TEXT("error data availableLayers[] carries PW_Rock"), Available.Contains(TEXT("PW_Rock")));
    TestTrue(TEXT("error data availableLayers[] carries PW_Sand"), Available.Contains(TEXT("PW_Sand")));
    TestEqual(TEXT("error data availableLayers[] carries exactly the declared layers"),
        Available.Num(), 2);
    if (Capture->Result.IsValid())
    {
        double AvailableCount = -1.0;
        TestTrue(TEXT("error data carries availableLayerCount"),
            Capture->Result->TryGetNumberField(TEXT("availableLayerCount"), AvailableCount));
        TestEqual(TEXT("availableLayerCount matches the declared layer count"), AvailableCount, 2.0);
    }

    return true;
}

// ============================================================================
// The reported repro: a material with no LandscapeLayerBlend declares no paintable
// target layers, so NO layer name can be painted — LANDSCAPE_MATERIAL_NO_LAYERS.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintLayerlessMaterialErrorsTest,
    "PinWright.landscape.create_procedural_terrain.LayerlessMaterialErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintLayerlessMaterialErrorsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping layerless-material paint test"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintLayerless_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // landscape.create's default material is /Engine/EngineMaterials/WorldGridMaterial,
    // which has no LandscapeLayerBlend — the exact shape of the reported repro.
    ALandscape* Landscape = CreateTinyLandscape(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }
    TestTrue(TEXT("fixture landscape has a material assigned (so this is not the no-material case)"),
        Landscape->LandscapeMaterial != nullptr);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, TEXT("Grass"), Capture);

    TestFalse(TEXT("painting into a layerless material is not reported as success"), Capture->bSuccess);
    TestEqual(TEXT("painting into a layerless material returns LANDSCAPE_MATERIAL_NO_LAYERS"),
        Capture->ErrorCode, FString(TEXT("LANDSCAPE_MATERIAL_NO_LAYERS")));
    // The message must state the prerequisite, not just the symptom.
    TestTrue(TEXT("message names LandscapeLayerBlend as the prerequisite"),
        Capture->Message.Contains(TEXT("LandscapeLayerBlend")));

    return true;
}

// ============================================================================
// A layer the material DOES declare paints, and the weightmap actually changes.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintValidLayerChangesWeightmapTest,
    "PinWright.landscape.create_procedural_terrain.ValidLayerChangesWeightmap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintValidLayerChangesWeightmapTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape paint round-trip test"));
        return true;
    }

    const FName PaintedLayer(TEXT("PW_Grass"));
    UMaterial* Material = MakeLandscapeMaterialDeclaringLayers({ PaintedLayer });
    if (!Material)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UMaterialExpressionLandscapeLayerWeight could not be resolved by reflection — skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintValidLayer_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = CreateTinyLandscape(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }
    AssignLandscapeMaterial(Landscape, Material);

    // Baseline: a freshly created landscape carries no weight for the layer. Asserted so
    // the post-paint count below is attributable to the paint and not to pre-existing data.
    const int32 BaselineWeighted = CountTexelsWithWeight(Landscape, PaintedLayer);
    TestTrue(FString::Printf(TEXT("baseline weightmap carries no weight for '%s' (got %d)"),
            *PaintedLayer.ToString(), BaselineWeighted),
        BaselineWeighted <= 0);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, PaintedLayer.ToString(), Capture);

    TestTrue(TEXT("painting a declared layer succeeds"), Capture->bSuccess);
    if (!Capture->bSuccess || !Capture->Result.IsValid())
    {
        return false;
    }

    // The handler's own honesty metric.
    double SampledTexels = 0.0;
    double TexelsWithWeight = 0.0;
    TestTrue(TEXT("response carries sampledTexels"),
        Capture->Result->TryGetNumberField(TEXT("sampledTexels"), SampledTexels));
    TestTrue(TEXT("response carries texelsWithWeight"),
        Capture->Result->TryGetNumberField(TEXT("texelsWithWeight"), TexelsWithWeight));
    TestTrue(FString::Printf(TEXT("handler reports painted texels carrying weight (got %f of %f)"),
            TexelsWithWeight, SampledTexels),
        TexelsWithWeight > 0.0);

    // Independent verification: read the weightmap back through the engine directly rather
    // than trusting the handler's self-report. This is the assertion that fails if the
    // edit-layer scoping around SetAlphaData is reverted — an unscoped write on an
    // edit-layer landscape is composited away and reads back all zero.
    const int32 PaintedWeighted = CountTexelsWithWeight(Landscape, PaintedLayer);
    TestTrue(FString::Printf(TEXT("weightmap readback finds painted texels (got %d)"), PaintedWeighted),
        PaintedWeighted > 0);
    TestTrue(FString::Printf(TEXT("weightmap changed from baseline (%d -> %d)"),
            BaselineWeighted, PaintedWeighted),
        PaintedWeighted > BaselineWeighted);

    return true;
}

// ============================================================================
// A paint region that is empty after clamping is rejected, not silently no-opped.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintEmptyRegionErrorsTest,
    "PinWright.landscape.create_procedural_terrain.EmptyRegionErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintEmptyRegionErrorsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping empty-region paint test"));
        return true;
    }

    const FName PaintedLayer(TEXT("PW_Grass"));
    UMaterial* Material = MakeLandscapeMaterialDeclaringLayers({ PaintedLayer });
    if (!Material)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UMaterialExpressionLandscapeLayerWeight could not be resolved by reflection — skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintEmptyRegion_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = CreateTinyLandscape(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }
    AssignLandscapeMaterial(Landscape, Material);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    // Inverted region: minX > maxX survives the clamp into the extent and resolves empty.
    PaintLayer(*this, Label, PaintedLayer.ToString(), Capture,
        [](const TSharedPtr<FJsonObject>& Payload)
        {
            TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
            Region->SetNumberField(TEXT("minX"), 6);
            Region->SetNumberField(TEXT("minY"), 0);
            Region->SetNumberField(TEXT("maxX"), 1);
            Region->SetNumberField(TEXT("maxY"), 7);
            Payload->SetObjectField(TEXT("region"), Region);
        });

    TestFalse(TEXT("an empty paint region is not reported as a successful paint"), Capture->bSuccess);
    TestEqual(TEXT("an empty paint region returns INVALID_ARGUMENT"),
        Capture->ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the empty-region message says nothing would be painted"),
        Capture->Message.Contains(TEXT("Empty paint region")));

    return true;
}

// ============================================================================
// A hollow landscape (no registered ULandscapeComponents) is rejected with the
// LANDSCAPE_NO_COMPONENTS diagnostic its sibling verbs already emit.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintHollowLandscapeErrorsTest,
    "PinWright.landscape.create_procedural_terrain.HollowLandscapeErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintHollowLandscapeErrorsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping hollow-landscape paint test"));
        return true;
    }

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // Same 5.3 fixture limitation TestEnvironmentHandlers documents for the landscape.edit
    // hollow diagnostic: registering a component-less ALandscape into a world that already
    // holds a real landscape crashes the engine before the handler is reached.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping hollow-landscape paint test on UE 5.3: the hollow fixture is not constructible there."));
    return true;
#else
    const FName PaintedLayer(TEXT("PW_Grass"));
    UMaterial* Material = MakeLandscapeMaterialDeclaringLayers({ PaintedLayer });
    if (!Material)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UMaterialExpressionLandscapeLayerWeight could not be resolved by reflection — skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintHollow_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Hollow = World->SpawnActor<ALandscape>(
        ALandscape::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
    TestNotNull(TEXT("spawned bare ALandscape"), Hollow);
    if (!Hollow)
    {
        return false;
    }
    Hollow->SetActorLabel(Label);
    Hollow->SetLandscapeGuid(FGuid::NewGuid());
    Hollow->CreateLandscapeInfo();
    // Assign the layer-bearing material WITHOUT PostEditChange (a hollow actor has no
    // components to update) so the paint passes the material/layer gates and reaches the
    // extent check this test is about.
    Hollow->LandscapeMaterial = Material;

    ULandscapeInfo* Info = Hollow->GetLandscapeInfo();
    TestNotNull(TEXT("hollow landscape has a ULandscapeInfo (so the extent check is reached)"), Info);
    if (Info)
    {
        TestEqual(TEXT("hollow landscape has an empty XYtoComponentMap"),
            Info->XYtoComponentMap.Num(), 0);
    }

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, PaintedLayer.ToString(), Capture);

    TestFalse(TEXT("painting a hollow landscape is not reported as success"), Capture->bSuccess);
    TestEqual(TEXT("painting a hollow landscape returns LANDSCAPE_NO_COMPONENTS"),
        Capture->ErrorCode, FString(TEXT("LANDSCAPE_NO_COMPONENTS")));

    return true;
#endif // UE_VERSION_OLDER_THAN(5, 4, 0)
}

// ============================================================================
// Regression test for B-paint-layer-destroys-other-layer-weights.
//
// Painting one weight-blended layer over a BOUNDED region must not change any
// other layer OUTSIDE that region. Weight-blended layers normalize as a group, so
// the sibling losing its weight INSIDE the region is the engine's behaviour and is
// deliberately not asserted against; outside it, the weight belongs to work the
// caller never put at risk.
//
// The landscape is deliberately 2x1 components so "outside the region" includes a
// whole component the write never touches — the axis on which the reported damage
// escaped. The region is derived from the live extent rather than hardcoded, so the
// test does not depend on how landscape.create computes it.
//
// Counterfactual — this is the assertion that CAUGHT the real mechanism, so the
// counterfactual is the fix itself: drop the handler's
// ULandscapeInfo::CreateTargetLayerSettingsFor(NewLayerInfo) call and the
// auto-created ULandscapeLayerInfoObject is bound only into ULandscapeInfo::Layers,
// which from 5.5 on is rebuilt PURELY from ALandscape::TargetLayers
// (Landscape.cpp:4582-4595). The handler's own UpdateLayerInfoMap() at the top of the
// NEXT paint then unbinds the sibling; it drops out of GetTargetLayerNames()
// (LandscapeEdit.cpp:8323, filters LayerInfoObj != nullptr), so the merge does not
// request it (LandscapeEditLayers.cpp:5883) and ReallocateLayersWeightmaps deletes
// its allocation from every component (:5744-5755) — the sibling's weight erased over
// the WHOLE landscape, region-independent. In that state this test fails on
// "sibling layer weight is readable after the region paint" (FindLayerInfo returns
// null) and on "census covers both target layers" (the census enumerates
// LandscapeInfo->Layers and skips entries with no LayerInfoObj, so it sees 1).
//
// Second counterfactual: delete the all-layers census and layersAffected[] /
// otherLayerTexelsLost disappear, which is the state in which
// texelsWithWeight == paintedTexels reported clean over the loss.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintRegionLeavesOtherLayersOutsideRegionTest,
    "PinWright.landscape.create_procedural_terrain.RegionPaintLeavesOtherLayersOutsideRegion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintRegionLeavesOtherLayersOutsideRegionTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape multi-layer region paint test"));
        return true;
    }

    const FName SiblingLayer(TEXT("PW_RegionSibling"));
    const FName PaintedLayer(TEXT("PW_RegionPainted"));
    UMaterial* Material = MakeLandscapeMaterialDeclaringLayers({ SiblingLayer, PaintedLayer });
    if (!Material)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UMaterialExpressionLandscapeLayerWeight could not be resolved by reflection — skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintRegionBound_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = CreateTinyLandscape(*this, World, Label, /*ComponentsX=*/2, /*ComponentsY=*/1);
    if (!Landscape)
    {
        return false;
    }
    AssignLandscapeMaterial(Landscape, Material);

    ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
    int32 FullMinX = 0, FullMinY = 0, FullMaxX = 0, FullMaxY = 0;
    if (!Info || !Info->GetLandscapeExtent(FullMinX, FullMinY, FullMaxX, FullMaxY))
    {
        AddError(TEXT("landscape fixture has no usable extent"));
        return false;
    }

    // A region strictly inside the extent on both axes, so "outside" is non-empty.
    const int32 RegionMinX = FullMinX;
    const int32 RegionMinY = FullMinY;
    const int32 RegionMaxX = FullMinX + FMath::Max(1, (FullMaxX - FullMinX) / 4);
    const int32 RegionMaxY = FullMinY + FMath::Max(1, (FullMaxY - FullMinY) / 2);
    if (!TestTrue(FString::Printf(
            TEXT("fixture region [%d,%d]..[%d,%d] is strictly inside extent [%d,%d]..[%d,%d]"),
            RegionMinX, RegionMinY, RegionMaxX, RegionMaxY, FullMinX, FullMinY, FullMaxX, FullMaxY),
        RegionMaxX < FullMaxX && RegionMaxY < FullMaxY))
    {
        return false;
    }

    // Establish the sibling's weight across the WHOLE landscape first: without weight to
    // lose, "the sibling kept its weight outside the region" is vacuously true.
    TSharedRef<FTestResponseCapture> SiblingCapture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, SiblingLayer.ToString(), SiblingCapture);
    TestTrue(TEXT("painting the sibling layer over the full extent succeeds"), SiblingCapture->bSuccess);
    if (!SiblingCapture->bSuccess)
    {
        return false;
    }

    int32 SiblingInsideBefore = 0;
    int32 SiblingOutsideBefore = 0;
    if (!TestTrue(TEXT("sibling layer weight is readable before the region paint"),
        CountTexelsWithWeightSplitByRegion(Landscape, SiblingLayer,
            RegionMinX, RegionMinY, RegionMaxX, RegionMaxY,
            SiblingInsideBefore, SiblingOutsideBefore)))
    {
        return false;
    }
    if (!TestTrue(FString::Printf(
            TEXT("sibling layer carries weight OUTSIDE the region before the second paint (got %d)"),
            SiblingOutsideBefore),
        SiblingOutsideBefore > 0))
    {
        return false;
    }

    // The call under test: paint the second layer, bounded to the region.
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, PaintedLayer.ToString(), Capture,
        [RegionMinX, RegionMinY, RegionMaxX, RegionMaxY](const TSharedPtr<FJsonObject>& Payload)
        {
            TSharedPtr<FJsonObject> RegionJson = MakeShared<FJsonObject>();
            RegionJson->SetNumberField(TEXT("minX"), RegionMinX);
            RegionJson->SetNumberField(TEXT("minY"), RegionMinY);
            RegionJson->SetNumberField(TEXT("maxX"), RegionMaxX);
            RegionJson->SetNumberField(TEXT("maxY"), RegionMaxY);
            Payload->SetObjectField(TEXT("region"), RegionJson);
            Payload->SetNumberField(TEXT("strength"), 1.0);
        });
    TestTrue(TEXT("region-bounded paint of the second layer succeeds"), Capture->bSuccess);
    if (!Capture->bSuccess || !Capture->Result.IsValid())
    {
        return false;
    }

    // The painted layer changed INSIDE the region.
    int32 PaintedInside = 0;
    int32 PaintedOutside = 0;
    if (TestTrue(TEXT("painted layer weight is readable after the region paint"),
        CountTexelsWithWeightSplitByRegion(Landscape, PaintedLayer,
            RegionMinX, RegionMinY, RegionMaxX, RegionMaxY, PaintedInside, PaintedOutside)))
    {
        TestTrue(FString::Printf(
                TEXT("painted layer carries weight INSIDE the region (got %d)"), PaintedInside),
            PaintedInside > 0);
    }

    // The assertion this ticket exists for: the sibling kept its weight OUTSIDE the region.
    int32 SiblingInsideAfter = 0;
    int32 SiblingOutsideAfter = 0;
    if (TestTrue(TEXT("sibling layer weight is readable after the region paint"),
        CountTexelsWithWeightSplitByRegion(Landscape, SiblingLayer,
            RegionMinX, RegionMinY, RegionMaxX, RegionMaxY,
            SiblingInsideAfter, SiblingOutsideAfter)))
    {
        TestEqual(FString::Printf(
                TEXT("sibling layer keeps every weighted texel OUTSIDE the painted region (%d -> %d)"),
                SiblingOutsideBefore, SiblingOutsideAfter),
            SiblingOutsideAfter, SiblingOutsideBefore);
    }

    // The response must be able to SEE that, not merely be right by luck: the verb's own
    // census has to reach the same conclusion the direct readback above reached.
    const TArray<TSharedPtr<FJsonValue>>* LayersAffected = nullptr;
    if (TestTrue(TEXT("paint response carries the all-layers census layersAffected[]"),
        Capture->Result->TryGetArrayField(TEXT("layersAffected"), LayersAffected) && LayersAffected))
    {
        TestTrue(FString::Printf(
                TEXT("census covers both target layers (got %d)"), LayersAffected->Num()),
            LayersAffected->Num() >= 2);
    }

    double OtherLayerTexelsLost = -1.0;
    if (TestTrue(TEXT("paint response carries otherLayerTexelsLost"),
        Capture->Result->TryGetNumberField(TEXT("otherLayerTexelsLost"), OtherLayerTexelsLost)))
    {
        TestEqual(FString::Printf(
                TEXT("the verb reports no other-layer weight lost outside the region (got %f)"),
                OtherLayerTexelsLost),
            OtherLayerTexelsLost, 0.0);
    }

    return true;
}

// ============================================================================
// Regression tests for B-paint-erases-orphaned-layer.
//
// The residual case its parent Critical (B-paint-layer-destroys-other-layer-weights)
// deliberately left unfixed. That ticket fixed the PRODUCER: the handler now registers
// its auto-created ULandscapeLayerInfoObject through CreateTargetLayerSettingsFor, so a
// paint no longer unbinds the layers earlier paints created. It did nothing for a
// landscape that is ALREADY in the orphaned state — weight allocated on the components,
// no registration pointing at it — and that state has live, non-migration producers: the
// Target Layers panel's RENAME (RemoveTargetLayer iterates no components,
// Landscape.cpp:7725-7746), a delete with World Partition proxies unloaded, clearing a
// layer's asset slot, undo/redo, and any third-party tool that paints without registering
// (FLandscapeEditDataInterface::SetAlphaData never checks).
//
// TWO THINGS ARE ASSERTED, and they fail for different reasons before the fix.
//
//   1. The verb REFUSES. Before the fix it painted, and the edit-layer merge then deleted
//      the unregistered allocation from every component of every loaded proxy —
//      landscape-wide, independent of `region` and of `strength`, with editor.undo unable
//      to reverse it because the merge runs in the settle AFTER the paint's transaction
//      has closed. Its only trace was a VeryVerbose log line.
//   2. The census could not SEE it. layersAffected[] / otherLayerTexelsLost enumerate
//      ULandscapeInfo::Layers — the REGISTRATION side — so an orphaned allocation is
//      absent from the census right up until it is destroyed, and otherLayerTexelsLost
//      reports 0 over a landscape-wide erase. The response now carries orphanedLayers[]
//      built from what the weightmaps actually CONTAIN.
//
// The fixture therefore reads the allocation side DIRECTLY through
// ULandscapeComponent::GetWeightmapLayerAllocations. The file's existing FindLayerInfo
// helper cannot be used for the victim: it walks LandscapeInfo->Layers and is blind to an
// orphan by construction — which is the defect, not a shortcut.
//
// 5.5+ only, and honestly so: on 5.3/5.4 ULandscapeInfo::UpdateLayerInfoMapInternal still
// rebuilt Layers from the component allocations, so an orphan self-heals there and neither
// the erase nor the census blindness exists. From 5.5 the rebuild reads
// ALandscape::GetTargetLayers() and nothing else (Landscape.cpp:4582-4597).
// ALandscapeProxy::RemoveTargetLayer, which is how the fixture mints the orphan, does not
// exist before 5.5 either.
// ============================================================================
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)

namespace
{
    // How many components carry a LIVE weightmap allocation bound to this exact
    // ULandscapeLayerInfoObject. Walks ForEachLandscapeProxy rather than
    // ULandscapeInfo::XYtoComponentMap: the latter holds only fully-registered components
    // and is keyed by section base, and it is exactly the enumeration whose gaps make an
    // unloaded-proxy delete an orphan source in the first place. Returns -1 when the
    // landscape or layer info is unusable, so a fixture failure cannot read as a zero.
    int32 CountLandscapeComponentsAllocatingLayerInfo(ALandscape* Landscape,
        const ULandscapeLayerInfoObject* LayerInfo)
    {
        ULandscapeInfo* Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
        if (!Info || !LayerInfo)
        {
            return -1;
        }
        int32 Count = 0;
        Info->ForEachLandscapeProxy([&Count, LayerInfo](ALandscapeProxy* Proxy) -> bool
        {
            if (!Proxy)
            {
                return true;
            }
            for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
            {
                if (!Component)
                {
                    continue;
                }
                const TArray<FWeightmapLayerAllocationInfo>& Allocations =
                    Component->GetWeightmapLayerAllocations(/*InReturnEditingWeightmap=*/false);
                for (const FWeightmapLayerAllocationInfo& Alloc : Allocations)
                {
                    const ULandscapeLayerInfoObject* AllocLayerInfo = Alloc.LayerInfo;
                    if (Alloc.IsAllocated() && AllocLayerInfo == LayerInfo)
                    {
                        ++Count;
                        break;
                    }
                }
            }
            return true;
        });
        return Count;
    }

    // layerName of each orphanedLayers[] entry that reported one. A dead orphan (null
    // LayerInfo) has no name to report and deliberately omits the field.
    TArray<FString> ReadOrphanedLayerNames(const TSharedPtr<FJsonObject>& Data)
    {
        TArray<FString> Out;
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Data.IsValid() && Data->TryGetArrayField(TEXT("orphanedLayers"), Array) && Array)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Array)
            {
                const TSharedPtr<FJsonObject>* Entry = nullptr;
                FString Name;
                if (Value.IsValid() && Value->TryGetObject(Entry) && Entry &&
                    (*Entry)->TryGetStringField(TEXT("layerName"), Name))
                {
                    Out.Add(Name);
                }
            }
        }
        return Out;
    }

    // Shared fixture for both orphan tests: a 2x1-component landscape whose material
    // declares both layers, with VictimLayer painted over the full extent and then
    // DEREGISTERED the way the Target Layers panel's rename deregisters — TargetLayers
    // entry gone, components untouched, so the allocation survives with a live LayerInfo.
    //
    // bPostEditChange=false on the removal is deliberate. The panel passes the default
    // true; skipping it changes nothing about the state under test (the entry is removed
    // either way and no component is visited either way) and keeps the fixture from
    // kicking a material/MIC refresh whose side effects would be a second variable in a
    // test about weight allocations. UpdateLayerInfoMap() is then called explicitly, which
    // is what the paint handler itself does at the top of every call.
    //
    // Returns the victim's ULandscapeLayerInfoObject (now unregistered) and the component
    // count carrying it, or nullptr when a fixture step failed — the failure is already
    // recorded on Test in that case.
    ULandscapeLayerInfoObject* BuildOrphanedLayerFixture(FAutomationTestBase& Test, UWorld* World,
        const FString& Label, const FName& VictimLayer, const FName& PaintedLayer,
        ALandscape*& OutLandscape, int32& OutOrphanComponents)
    {
        OutLandscape = nullptr;
        OutOrphanComponents = 0;

        UMaterial* Material = MakeLandscapeMaterialDeclaringLayers({ VictimLayer, PaintedLayer });
        if (!Material)
        {
            return nullptr;
        }

        ALandscape* Landscape = CreateTinyLandscape(Test, World, Label, /*ComponentsX=*/2, /*ComponentsY=*/1);
        if (!Landscape)
        {
            return nullptr;
        }
        AssignLandscapeMaterial(Landscape, Material);
        OutLandscape = Landscape;

        // Give the victim real weight and a real registration first: without weight to
        // lose, "the orphaned layer's weight survived" is vacuously true.
        TSharedRef<FTestResponseCapture> VictimCapture = MakeShared<FTestResponseCapture>();
        PaintLayer(Test, Label, VictimLayer.ToString(), VictimCapture);
        Test.TestTrue(TEXT("painting the victim layer over the full extent succeeds"),
            VictimCapture->bSuccess);
        if (!VictimCapture->bSuccess)
        {
            return nullptr;
        }

        ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
        ULandscapeLayerInfoObject* VictimInfo = FindLayerInfo(Info, VictimLayer);
        if (!Test.TestNotNull(TEXT("victim layer is registered after its own paint"), VictimInfo))
        {
            return nullptr;
        }

        const int32 AllocatedBefore = CountLandscapeComponentsAllocatingLayerInfo(Landscape, VictimInfo);
        if (!Test.TestTrue(FString::Printf(
                TEXT("victim layer holds weightmap allocations on at least one component (got %d)"),
                AllocatedBefore),
            AllocatedBefore > 0))
        {
            return nullptr;
        }

        // The orphaning step itself.
        Test.TestTrue(TEXT("RemoveTargetLayer dropped the victim's registration"),
            Landscape->RemoveTargetLayer(VictimLayer, /*bPostEditChange=*/false));
        Info->UpdateLayerInfoMap();

        // Both halves of the orphaned state, asserted rather than assumed: the
        // registration is gone AND the weight is still there. If FixupWeightmaps or
        // anything else had eaten the allocation, the second assert catches it here
        // instead of letting the test blame the verb.
        Test.TestNull(TEXT("registration-side lookup can no longer see the victim layer"),
            FindLayerInfo(Info, VictimLayer));
        const int32 AllocatedAfter = CountLandscapeComponentsAllocatingLayerInfo(Landscape, VictimInfo);
        if (!Test.TestEqual(FString::Printf(
                TEXT("the orphaned layer's weightmap allocations survive deregistration (%d -> %d)"),
                AllocatedBefore, AllocatedAfter),
            AllocatedAfter, AllocatedBefore))
        {
            return nullptr;
        }

        OutOrphanComponents = AllocatedAfter;
        return VictimInfo;
    }
}

// ============================================================================
// Painting a second layer over an ALREADY-orphaned allocation is REFUSED, and the
// orphaned weight is still there afterwards.
//
// Counterfactual: remove the ScanLandscapeOrphanedWeightAllocations call (or its
// refusal) from LandscapeHandler.cpp and the paint succeeds, the settle inside its
// verify pass runs the edit-layer merge, ReallocateLayersWeightmaps drops every
// allocation the merge did not request, and both the LANDSCAPE_ORPHANED_LAYER_WEIGHT
// assertion and the "orphaned layer keeps its weightmap allocations" assertion fail —
// while otherLayerTexelsLost still reports 0, because the census never saw the layer.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintOrphanedAllocationRefusedTest,
    "PinWright.landscape.create_procedural_terrain.OrphanedAllocationRefusesPaint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintOrphanedAllocationRefusedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping orphaned-allocation refusal test"));
        return true;
    }

    const FName VictimLayer(TEXT("PW_OrphanVictim"));
    const FName PaintedLayer(TEXT("PW_OrphanPainter"));

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintOrphanRefuse_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = nullptr;
    int32 OrphanComponents = 0;
    ULandscapeLayerInfoObject* VictimInfo = BuildOrphanedLayerFixture(*this, World, Label,
        VictimLayer, PaintedLayer, Landscape, OrphanComponents);
    if (!VictimInfo)
    {
        if (!Landscape)
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
                TEXT("Orphaned-allocation fixture could not be built — skipping"));
            return true;
        }
        return false;
    }

    // The call under test.
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, PaintedLayer.ToString(), Capture);

    TestFalse(TEXT("a paint that would erase an orphaned allocation is not reported as success"),
        Capture->bSuccess);
    TestEqual(TEXT("the refusal is typed LANDSCAPE_ORPHANED_LAYER_WEIGHT"),
        Capture->ErrorCode, FString(TEXT("LANDSCAPE_ORPHANED_LAYER_WEIGHT")));

    // The message has to name the layer at risk and the opt-in, or the caller cannot act
    // on it without opening the editor.
    TestTrue(TEXT("the refusal names the orphaned layer"),
        Capture->Message.Contains(VictimLayer.ToString()));
    TestTrue(TEXT("the refusal names the allowOrphanedLayerLoss opt-in"),
        Capture->Message.Contains(TEXT("allowOrphanedLayerLoss")));

    const TArray<FString> Orphaned = ReadOrphanedLayerNames(Capture->Result);
    TestTrue(TEXT("error data orphanedLayers[] carries the orphaned layer"),
        Orphaned.Contains(VictimLayer.ToString()));
    if (Capture->Result.IsValid())
    {
        // The scan's own scope, so a zero it did not earn is impossible to read as clean.
        bool bLoadedOnly = false;
        TestTrue(TEXT("error data records that only loaded proxies were scanned"),
            Capture->Result->TryGetBoolField(TEXT("orphanScanLoadedProxiesOnly"), bLoadedOnly));
        TestTrue(TEXT("orphanScanLoadedProxiesOnly is true"), bLoadedOnly);

        double ScannedComponents = -1.0;
        if (TestTrue(TEXT("error data carries orphanScanComponents"),
            Capture->Result->TryGetNumberField(TEXT("orphanScanComponents"), ScannedComponents)))
        {
            TestTrue(FString::Printf(
                    TEXT("orphanScanComponents covers the whole landscape (got %f)"), ScannedComponents),
                ScannedComponents >= (double)OrphanComponents);
        }
    }

    // The assertion this ticket exists for: the refusal actually saved the data.
    const int32 AllocatedAfterRefusal =
        CountLandscapeComponentsAllocatingLayerInfo(Landscape, VictimInfo);
    TestEqual(FString::Printf(
            TEXT("the orphaned layer keeps its weightmap allocations after the refused paint (%d -> %d)"),
            OrphanComponents, AllocatedAfterRefusal),
        AllocatedAfterRefusal, OrphanComponents);

    return true;
}

// ============================================================================
// allowOrphanedLayerLoss=true proceeds — and says what it cost.
//
// The opt-in must not buy silence: the loss is landscape-wide and editor.undo does not
// cover it, so the response still has to carry orphanedLayers[] and a warning. This is
// also the test that demonstrates the erasure is real rather than hypothetical, which is
// what makes the refusal above worth having.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintOrphanedAllocationOptInReportsLossTest,
    "PinWright.landscape.create_procedural_terrain.OrphanedAllocationOptInReportsLoss",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintOrphanedAllocationOptInReportsLossTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping orphaned-allocation opt-in test"));
        return true;
    }

    const FName VictimLayer(TEXT("PW_OrphanOptInVictim"));
    const FName PaintedLayer(TEXT("PW_OrphanOptInPainter"));

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintOrphanOptIn_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = nullptr;
    int32 OrphanComponents = 0;
    ULandscapeLayerInfoObject* VictimInfo = BuildOrphanedLayerFixture(*this, World, Label,
        VictimLayer, PaintedLayer, Landscape, OrphanComponents);
    if (!VictimInfo)
    {
        if (!Landscape)
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
                TEXT("Orphaned-allocation fixture could not be built — skipping"));
            return true;
        }
        return false;
    }

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, PaintedLayer.ToString(), Capture,
        [](const TSharedPtr<FJsonObject>& Payload)
        {
            Payload->SetBoolField(TEXT("allowOrphanedLayerLoss"), true);
        });

    TestTrue(TEXT("allowOrphanedLayerLoss=true lets the paint proceed"), Capture->bSuccess);
    if (!Capture->bSuccess || !Capture->Result.IsValid())
    {
        return false;
    }

    const TArray<FString> Orphaned = ReadOrphanedLayerNames(Capture->Result);
    TestTrue(TEXT("the success response still reports orphanedLayers[]"),
        Orphaned.Contains(VictimLayer.ToString()));

    bool bLoadedOnly = false;
    TestTrue(TEXT("the success response records that only loaded proxies were scanned"),
        Capture->Result->TryGetBoolField(TEXT("orphanScanLoadedProxiesOnly"), bLoadedOnly));
    TestTrue(TEXT("orphanScanLoadedProxiesOnly is true"), bLoadedOnly);

    // Per the ticket: silence here is data loss. The opt-in downgrades the refusal to a
    // warning, never to nothing.
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    if (TestTrue(TEXT("the opt-in paint carries warnings[]"),
        Capture->Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings))
    {
        bool bFoundOrphanWarning = false;
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            if (Value.IsValid() && Value->AsString().Contains(TEXT("allowOrphanedLayerLoss")))
            {
                bFoundOrphanWarning = true;
                break;
            }
        }
        TestTrue(TEXT("a warnings[] entry names the accepted orphaned-weight erasure"),
            bFoundOrphanWarning);
    }

    // The erasure the refusal exists to prevent, demonstrated. The verify pass settles the
    // deferred regeneration, so the merge has run by the time the response arrives.
    const int32 AllocatedAfterOptIn =
        CountLandscapeComponentsAllocatingLayerInfo(Landscape, VictimInfo);
    TestEqual(FString::Printf(
            TEXT("the opt-in paint really did erase the orphaned layer's allocations (%d -> %d)"),
            OrphanComponents, AllocatedAfterOptIn),
        AllocatedAfterOptIn, 0);

    // And the proof that the census could never have told the caller: it enumerates the
    // registration side, so the layer it just destroyed is absent from layersAffected[]
    // and contributes nothing to otherLayerTexelsLost.
    double OtherLayerTexelsLost = -1.0;
    if (Capture->Result->TryGetNumberField(TEXT("otherLayerTexelsLost"), OtherLayerTexelsLost))
    {
        TestEqual(FString::Printf(
                TEXT("otherLayerTexelsLost is blind to the orphaned erase, which is why orphanedLayers[] exists (got %f)"),
                OtherLayerTexelsLost),
            OtherLayerTexelsLost, 0.0);
    }

    return true;
}

#endif // UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)

// ============================================================================
// Regression test for B-paint-blanks-unallocated-layers-component-wide.
//
// A region-scoped paint on a VIRGIN component changes what the whole component renders,
// not just the requested rectangle. A landscape component's material permutation is built
// from THAT component's own weightmap allocation list (GetLayerAllocationKey,
// LandscapeEdit.cpp:549; one FStaticTerrainLayerWeightParameter per allocation, :638-648),
// so once a component holds an allocation, every material-declared layer it holds no
// allocation for compiles to Compiler->Constant(0.f) across all of it
// (HLSLMaterialTranslator.cpp:9030/:9089-9101,
// MaterialExpressionLandscapeLayerSample.cpp:39-51). Measured on a real landscape: a
// 51x51-texel Rock paint removed the grass from 6300-uu components, 6.1x the requested area.
//
// THE POINT OF THIS TEST IS THAT BOTH EXISTING INSTRUMENTS STAY CLEAN AND ARE RIGHT TO.
// No weightmap byte moves, so otherLayerTexelsLost is 0 and that 0 is arithmetically
// correct; the painted layer is a registered target layer, so the orphan scan is silent and
// that silence is correct too. The assertions below therefore pair "the census reports no
// texel lost" with "the response nonetheless names the component-wide reach", which is the
// only shape in which the defect is expressible.
//
// Counterfactual: delete the ScanLandscapeComponentAllocationCoverage /
// DiffLandscapeComponentAllocationCoverage pair from LandscapeHandler.cpp and every
// assertion below on componentsGainingFirstAllocation,
// layersNowSampledZeroOnTouchedComponents[], allocationFootprintUU and the warnings[] line
// fails on a missing field — while otherLayerTexelsLost still reads 0 and success still
// reads true, which is exactly the state the ticket was filed against.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintFirstAllocationBlastRadiusTest,
    "PinWright.landscape.create_procedural_terrain.FirstPaintReportsComponentWideAllocationBlastRadius",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintFirstAllocationBlastRadiusTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape allocation blast-radius test"));
        return true;
    }

    // Declared by the material, never painted, and therefore never allocated anywhere: this
    // is the layer whose sample goes to a constant 0 across every component the paint
    // touches, and it is invisible to a texel census because it never had a texel to lose.
    const FName ZeroSampledLayer(TEXT("PW_BlastSibling"));
    const FName PaintedLayer(TEXT("PW_BlastPainted"));
    UMaterial* Material = MakeLandscapeMaterialDeclaringLayers({ ZeroSampledLayer, PaintedLayer });
    if (!Material)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UMaterialExpressionLandscapeLayerWeight could not be resolved by reflection — skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintBlastRadius_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Two components so the affected footprint can be compared against ONE component's size,
    // and so the extent is wider than the rectangle asked for on the X axis.
    ALandscape* Landscape = CreateTinyLandscape(*this, World, Label, /*ComponentsX=*/2, /*ComponentsY=*/1);
    if (!Landscape)
    {
        return false;
    }
    AssignLandscapeMaterial(Landscape, Material);

    ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
    int32 FullMinX = 0, FullMinY = 0, FullMaxX = 0, FullMaxY = 0;
    if (!Info || !Info->GetLandscapeExtent(FullMinX, FullMinY, FullMaxX, FullMaxY))
    {
        AddError(TEXT("landscape fixture has no usable extent"));
        return false;
    }

    // Deliberately NOT preceded by any paint: every component starts with an empty
    // weightmap allocation array, which is the state every freshly created landscape is in
    // and the state in which the first paint is expensive.
    const int32 RegionMinX = FullMinX;
    const int32 RegionMinY = FullMinY;
    const int32 RegionMaxX = FullMinX + 2;
    const int32 RegionMaxY = FullMinY + 2;
    if (!TestTrue(FString::Printf(
            TEXT("fixture region [%d,%d]..[%d,%d] is a small rectangle inside extent [%d,%d]..[%d,%d]"),
            RegionMinX, RegionMinY, RegionMaxX, RegionMaxY, FullMinX, FullMinY, FullMaxX, FullMaxY),
        RegionMaxX < FullMaxX && RegionMaxY < FullMaxY))
    {
        return false;
    }

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, PaintedLayer.ToString(), Capture,
        [RegionMinX, RegionMinY, RegionMaxX, RegionMaxY](const TSharedPtr<FJsonObject>& Payload)
        {
            TSharedPtr<FJsonObject> RegionJson = MakeShared<FJsonObject>();
            RegionJson->SetNumberField(TEXT("minX"), RegionMinX);
            RegionJson->SetNumberField(TEXT("minY"), RegionMinY);
            RegionJson->SetNumberField(TEXT("maxX"), RegionMaxX);
            RegionJson->SetNumberField(TEXT("maxY"), RegionMaxY);
            Payload->SetObjectField(TEXT("region"), RegionJson);
            Payload->SetNumberField(TEXT("strength"), 1.0);
        });
    TestTrue(TEXT("the first region-bounded paint on a virgin landscape succeeds"), Capture->bSuccess);
    if (!Capture->bSuccess || !Capture->Result.IsValid())
    {
        return false;
    }

    // Half one: the weight census is CLEAN, and correctly so. No layer lost a weightmap
    // byte anywhere, inside the region or out. This half passes both before and after the
    // fix and is asserted so a future change cannot "fix" the blast radius by breaking the
    // census into reporting a phantom loss.
    double OtherLayerTexelsLost = -1.0;
    if (TestTrue(TEXT("paint response carries otherLayerTexelsLost"),
        Capture->Result->TryGetNumberField(TEXT("otherLayerTexelsLost"), OtherLayerTexelsLost)))
    {
        TestEqual(FString::Printf(
                TEXT("no OTHER layer lost a weightmap texel outside the region, and 0 is the true answer (got %f)"),
                OtherLayerTexelsLost),
            OtherLayerTexelsLost, 0.0);
    }

    // Half two: the response nonetheless reports that whole components changed what they
    // sample. Every assertion from here down fails on a missing field before the fix.
    double ComponentsGainingAllocation = -1.0;
    double ComponentsGainingFirstAllocation = -1.0;
    if (TestTrue(TEXT("paint response carries componentsGainingAllocation"),
        Capture->Result->TryGetNumberField(TEXT("componentsGainingAllocation"), ComponentsGainingAllocation)))
    {
        TestTrue(FString::Printf(
                TEXT("at least one component gained a weightmap allocation (got %f)"),
                ComponentsGainingAllocation),
            ComponentsGainingAllocation >= 1.0);
    }
    if (TestTrue(TEXT("paint response carries componentsGainingFirstAllocation"),
        Capture->Result->TryGetNumberField(TEXT("componentsGainingFirstAllocation"),
            ComponentsGainingFirstAllocation)))
    {
        TestTrue(FString::Printf(
                TEXT("at least one component gained its FIRST allocation — the expensive transition (got %f)"),
                ComponentsGainingFirstAllocation),
            ComponentsGainingFirstAllocation >= 1.0);
    }

    // The layer the caller never touched, named as the thing that now samples zero across
    // whole components. This is the number the ticket says was never computed.
    const TArray<TSharedPtr<FJsonValue>>* ZeroSampled = nullptr;
    if (TestTrue(TEXT("paint response carries layersNowSampledZeroOnTouchedComponents[]"),
        Capture->Result->TryGetArrayField(TEXT("layersNowSampledZeroOnTouchedComponents"), ZeroSampled)
            && ZeroSampled))
    {
        bool bFoundSibling = false;
        double SiblingComponentCount = 0.0;
        for (const TSharedPtr<FJsonValue>& Value : *ZeroSampled)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString EntryName;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry &&
                (*Entry)->TryGetStringField(TEXT("layerName"), EntryName) &&
                EntryName == ZeroSampledLayer.ToString())
            {
                bFoundSibling = true;
                (*Entry)->TryGetNumberField(TEXT("componentCount"), SiblingComponentCount);
                break;
            }
        }
        TestTrue(FString::Printf(
                TEXT("the unpainted material-declared layer '%s' is reported as now sampling zero"),
                *ZeroSampledLayer.ToString()),
            bFoundSibling);
        TestTrue(FString::Printf(
                TEXT("it is reported on at least one component (got %f)"), SiblingComponentCount),
            SiblingComponentCount >= 1.0);
    }

    // The blast radius, in the units the caller can compare against `region`: the measured
    // footprint of the affected components against the footprint that was requested.
    double ComponentSizeX = 0.0;
    const TSharedPtr<FJsonObject>* ComponentSizeJson = nullptr;
    if (TestTrue(TEXT("paint response carries componentSizeUU"),
        Capture->Result->TryGetObjectField(TEXT("componentSizeUU"), ComponentSizeJson) && ComponentSizeJson))
    {
        (*ComponentSizeJson)->TryGetNumberField(TEXT("x"), ComponentSizeX);
        TestTrue(FString::Printf(TEXT("componentSizeUU.x is a real span (got %f)"), ComponentSizeX),
            ComponentSizeX > 0.0);
    }

    double RequestedFootprintX = 0.0;
    const TSharedPtr<FJsonObject>* RequestedFootprintJson = nullptr;
    if (TestTrue(TEXT("paint response carries requestedFootprintUU"),
        Capture->Result->TryGetObjectField(TEXT("requestedFootprintUU"), RequestedFootprintJson)
            && RequestedFootprintJson))
    {
        (*RequestedFootprintJson)->TryGetNumberField(TEXT("sizeX"), RequestedFootprintX);
    }

    const TSharedPtr<FJsonObject>* FootprintJson = nullptr;
    if (TestTrue(TEXT("paint response carries allocationFootprintUU"),
        Capture->Result->TryGetObjectField(TEXT("allocationFootprintUU"), FootprintJson) && FootprintJson))
    {
        double FootprintSizeX = 0.0;
        (*FootprintJson)->TryGetNumberField(TEXT("sizeX"), FootprintSizeX);
        TestTrue(FString::Printf(
                TEXT("the MEASURED affected footprint (%f uu) is wider than the REQUESTED one (%f uu)"),
                FootprintSizeX, RequestedFootprintX),
            FootprintSizeX > RequestedFootprintX);
        // The whole finding in one number: the reach is a component, not a rectangle. The
        // 0.9 slack is for bounds padding, not for a component-sized tolerance.
        TestTrue(FString::Printf(
                TEXT("the affected footprint spans at least a whole component (%f uu vs componentSizeUU.x %f uu)"),
                FootprintSizeX, ComponentSizeX),
            ComponentSizeX <= 0.0 || FootprintSizeX >= ComponentSizeX * 0.9);
    }

    // And it is said out loud at the call site, not left in a field a caller has to know to
    // read — the ticket's minimum ask.
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    if (TestTrue(TEXT("the paint carries warnings[]"),
        Capture->Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings))
    {
        bool bFoundBlastWarning = false;
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            if (Value.IsValid() && Value->AsString().Contains(ZeroSampledLayer.ToString()) &&
                Value->AsString().Contains(TEXT("WHOLE component")))
            {
                bFoundBlastWarning = true;
                break;
            }
        }
        TestTrue(TEXT("a warnings[] entry names the sibling layer and the component-wide reach"),
            bFoundBlastWarning);
    }

    return true;
}

// ============================================================================
// Regression test for B-paint-auto-created-layer-never-weight-blended.
//
// The verb auto-creates a ULandscapeLayerInfoObject for any target layer the material
// declares but no asset backs — the default path for the first paint of every layer. That
// object used to come out of a bare NewObject, so its BlendMethod was whatever the
// constructor read from ULandscapeSettings::GetTargetLayerDefaultBlendMethod()
// (LandscapeLayerInfoObject.cpp:27), whose compiled default is
// ELandscapeTargetLayerBlendMethod::None (LandscapeSettings.h:167) — display name
// "No Weight Blending". The merge's final weight-blending pass admits a layer only under
// GetBlendMethod() == FinalWeightBlending (LandscapeEditLayers.cpp:3594), so the group
// normalisation the verb's own registered summary promises in capitals could not occur for
// any layer this verb had built, and no response field named the method.
//
// The engine's blessed factory UE::Landscape::CreateTargetLayerInfo is deliberately NOT
// adopted wholesale — its first act is CreatePackage() and it always mints a /Game asset,
// while this verb creates a private per-landscape LayerInfo on purpose. What is adopted is
// its DefaultLayerInfoObject duplication step, which is the project-level control the ticket
// found disconnected. That half is not asserted here: it needs a configured project setting,
// and this host sets none (grepping Config/, Saved/Config/ and the engine config for
// TargetLayerDefaultBlendMethod and [/Script/Landscape.LandscapeSettings] finds no blend
// entry), so the constructed branch is the one under test and the one that reproduces.
//
// Counterfactual: drop the SetBlendMethod call from the auto-create branch and both the
// direct GetBlendMethod() assertion and the layerBlendMethod field assertion below fail with
// "None", while the paint still answers success:true — which is the state the ticket was
// filed against. Drop the weightBlended parameter's handling and the opt-out half fails the
// other way.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePaintAutoCreatedLayerIsWeightBlendedTest,
    "PinWright.landscape.create_procedural_terrain.AutoCreatedLayerInfoIsWeightBlended",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapePaintAutoCreatedLayerIsWeightBlendedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape layer blend-method test"));
        return true;
    }

    // The branch under test is the CONSTRUCTED one. A host that configures
    // ULandscapeSettings::DefaultLayerInfoObject gets the duplication branch instead, whose
    // blend method is the template's by design — asserting FinalWeightBlending there would be
    // asserting against that project's own configuration, which is the host-dependent-fixture
    // failure mode this suite refuses to reproduce.
    const ULandscapeSettings* LandscapeSettings = GetDefault<ULandscapeSettings>();
    if (LandscapeSettings && !LandscapeSettings->GetDefaultLayerInfoObject().IsNull())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("landscape-default-layerinfo-template-configured"),
            TEXT("This host configures ULandscapeSettings::DefaultLayerInfoObject, so the auto-create duplicates that template rather than constructing — skipping the constructed-branch blend-method assertions"));
        return true;
    }

    const FName BlendedLayer(TEXT("PW_BlendedDefault"));
    const FName UnblendedLayer(TEXT("PW_BlendedOptOut"));
    UMaterial* Material = MakeLandscapeMaterialDeclaringLayers({ BlendedLayer, UnblendedLayer });
    if (!Material)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("UMaterialExpressionLandscapeLayerWeight could not be resolved by reflection — skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_PaintBlendMethod_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = CreateTinyLandscape(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }
    AssignLandscapeMaterial(Landscape, Material);

    // ---- The default path: no weightBlended in the payload at all. ----
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, BlendedLayer.ToString(), Capture);
    TestTrue(TEXT("painting a declared layer with no LayerInfo succeeds"), Capture->bSuccess);
    if (!Capture->bSuccess || !Capture->Result.IsValid())
    {
        return false;
    }

    bool bAutoCreated = false;
    TestTrue(TEXT("the response reports layerInfoAutoCreated"),
        Capture->Result->TryGetBoolField(TEXT("layerInfoAutoCreated"), bAutoCreated));
    if (!TestTrue(TEXT("the fixture really exercised the auto-create branch"), bAutoCreated))
    {
        return false;
    }

    // Read the engine object directly rather than trusting the handler's self-report: this is
    // the assertion that says the layer can actually participate in normalisation.
    ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
    ULandscapeLayerInfoObject* CreatedInfo = FindLayerInfo(Info, BlendedLayer);
    if (TestNotNull(TEXT("the auto-created ULandscapeLayerInfoObject is reachable"), CreatedInfo))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        TestTrue(FString::Printf(
                TEXT("the auto-created layer is FinalWeightBlending, so LandscapeEditLayers.cpp:3594 admits it to the final weight-blending pass (got %d)"),
                (int32)CreatedInfo->GetBlendMethod()),
            CreatedInfo->GetBlendMethod() == ELandscapeTargetLayerBlendMethod::FinalWeightBlending);
#else
        TestFalse(TEXT("the auto-created layer is weight-blended (bNoWeightBlend is false)"),
            CreatedInfo->bNoWeightBlend);
#endif
    }

    // And the caller can find that out without opening the landscape actor's package, which
    // was the other half of the defect: nothing in the response named a blend method.
    FString ReportedBlendMethod;
    if (TestTrue(TEXT("the response carries layerBlendMethod"),
        Capture->Result->TryGetStringField(TEXT("layerBlendMethod"), ReportedBlendMethod)))
    {
        TestEqual(TEXT("layerBlendMethod reports the measured FinalWeightBlending"),
            ReportedBlendMethod, FString(TEXT("FinalWeightBlending")));
    }

    // No project template is configured on this host, so the field must be ABSENT rather than
    // present-and-empty — the omit-do-not-zero rule this verb follows everywhere else.
    FString UnusedTemplatePath;
    TestFalse(TEXT("layerInfoTemplatePath is omitted when no DefaultLayerInfoObject is configured"),
        Capture->Result->TryGetStringField(TEXT("layerInfoTemplatePath"), UnusedTemplatePath));

    // ---- The opt-out: a caller who genuinely wants a non-blended layer can say so, and is
    // told what that costs. ----
    TSharedRef<FTestResponseCapture> OptOutCapture = MakeShared<FTestResponseCapture>();
    PaintLayer(*this, Label, UnblendedLayer.ToString(), OptOutCapture,
        [](const TSharedPtr<FJsonObject>& Payload)
        {
            Payload->SetBoolField(TEXT("weightBlended"), false);
        });
    TestTrue(TEXT("weightBlended=false still paints"), OptOutCapture->bSuccess);
    if (!OptOutCapture->bSuccess || !OptOutCapture->Result.IsValid())
    {
        return false;
    }

    ULandscapeLayerInfoObject* OptOutInfo = FindLayerInfo(Landscape->GetLandscapeInfo(), UnblendedLayer);
    if (TestNotNull(TEXT("the opt-out ULandscapeLayerInfoObject is reachable"), OptOutInfo))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        TestTrue(FString::Printf(TEXT("weightBlended=false yields blend method None (got %d)"),
                (int32)OptOutInfo->GetBlendMethod()),
            OptOutInfo->GetBlendMethod() == ELandscapeTargetLayerBlendMethod::None);
#else
        TestTrue(TEXT("weightBlended=false yields bNoWeightBlend"), OptOutInfo->bNoWeightBlend);
#endif
    }

    FString OptOutBlendMethod;
    if (OptOutCapture->Result->TryGetStringField(TEXT("layerBlendMethod"), OptOutBlendMethod))
    {
        TestEqual(TEXT("layerBlendMethod reports the measured None for the opt-out layer"),
            OptOutBlendMethod, FString(TEXT("None")));
    }

    // Opting out is legitimate, but it must never be silent: a non-blended layer does not
    // displace its siblings, which is exactly the expectation the verb's summary sets.
    const TArray<TSharedPtr<FJsonValue>>* OptOutWarnings = nullptr;
    if (TestTrue(TEXT("the opt-out paint carries warnings[]"),
        OptOutCapture->Result->TryGetArrayField(TEXT("warnings"), OptOutWarnings) && OptOutWarnings))
    {
        bool bFoundBlendWarning = false;
        for (const TSharedPtr<FJsonValue>& Value : *OptOutWarnings)
        {
            if (Value.IsValid() && Value->AsString().Contains(TEXT("no part in group normalisation")))
            {
                bFoundBlendWarning = true;
                break;
            }
        }
        TestTrue(TEXT("a warnings[] entry says the non-blended layer will not reduce sibling weights"),
            bFoundBlendWarning);
    }

    return true;
}
