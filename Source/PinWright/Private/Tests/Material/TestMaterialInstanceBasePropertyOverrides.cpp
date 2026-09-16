// Copyright (c) 2026 Alexander Penkin. MIT License.

// Covers the material-instance BasePropertyOverrides write path and the wrong-class error
// vocabulary that sits in front of it:
//   - a UMaterial-only verb pointed at a material instance must NOT answer ASSET_NOT_FOUND
//     (the asset loads fine; it is the wrong class), and must name the per-instance route;
//   - set_material_instance_base_property_overrides turns bOverride_* on with a value that
//     reaches the engine's own effective-value accessor, not just the struct;
//   - `clear` turns bOverride_* back off and the effective value returns to the parent's.
//
// Every fixture is built in-test from a bare CreatePackage + factory, so nothing here depends
// on host-project content.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/EngineTypes.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
    // Parent UMaterial + child UMaterialInstanceConstant, both in freshly created (never saved)
    // packages so LoadObject in the handler resolves them by path. Distinctly named — a
    // same-named sibling helper in another test file would ODR-collide under a unity build.
    bool CreateBasePropertyOverrideFixture(
        FAutomationTestBase& Test,
        FString& OutParentPath,
        FString& OutInstancePath,
        UMaterialInstanceConstant*& OutInstance)
    {
        OutInstance = nullptr;
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        OutParentPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/MatBPOParent_%s"), *Suffix);
        OutInstancePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/MatBPOInst_%s"), *Suffix);

        UPackage* ParentPkg = CreatePackage(*OutParentPath);
        if (!Test.TestNotNull(TEXT("Parent package created"), ParentPkg))
            return false;

        UMaterial* ParentMat = NewObject<UMaterial>(
            ParentPkg,
            FName(*FPackageName::GetLongPackageAssetName(OutParentPath)),
            RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("Parent material created"), ParentMat))
        {
            CleanupTestAsset(OutParentPath);
            return false;
        }

        // Pin the parent's own values so "returned to the parent" is a real assertion rather
        // than a coincidence with the override value.
        ParentMat->BlendMode = BLEND_Opaque;
        ParentMat->TwoSided = 0;
        ParentMat->PostEditChange();
        FAssetRegistryModule::AssetCreated(ParentMat);

        UPackage* InstPkg = CreatePackage(*OutInstancePath);
        if (!Test.TestNotNull(TEXT("Instance package created"), InstPkg))
        {
            CleanupTestAsset(OutParentPath);
            return false;
        }

        UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
        Factory->InitialParent = ParentMat;
        UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(
            Factory->FactoryCreateNew(UMaterialInstanceConstant::StaticClass(),
                InstPkg, FName(*FPackageName::GetLongPackageAssetName(OutInstancePath)),
                RF_Public | RF_Standalone, nullptr, GWarn));
        if (!Test.TestNotNull(TEXT("Instance created"), Instance))
        {
            CleanupTestAsset(OutParentPath);
            CleanupTestAsset(OutInstancePath);
            return false;
        }
        Instance->SetParentEditorOnly(ParentMat);
        Instance->PostEditChange();
        FAssetRegistryModule::AssetCreated(Instance);

        OutInstance = Instance;
        return true;
    }

    // Membership walk over a string array field of a handler response.
    bool ResponseArrayHasString(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, const TCHAR* Value)
    {
        return JsonStringArrayContains(Result, FString(Field), FString(Value));
    }
}


// The reported defect: set_blend_mode against a UMaterialInstanceConstant answered
// ASSET_NOT_FOUND, sending the caller to re-check a path that was correct. It must report the
// class it actually found and route to the per-instance verb instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSetBlendModeOnInstanceReportsWrongClassTest,
    "PinWright.material.authoring.set_blend_mode.MaterialInstanceReportsWrongClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSetBlendModeOnInstanceReportsWrongClassTest::RunTest(const FString& Parameters)
{
    FString ParentPath;
    FString InstancePath;
    UMaterialInstanceConstant* Instance = nullptr;
    if (!CreateBasePropertyOverrideFixture(*this, ParentPath, InstancePath, Instance))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), InstancePath);
    Payload->SetStringField(TEXT("blendMode"), TEXT("Translucent"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("set_blend_mode handler found"),
        InvokeHandlerWithCapture(TEXT("material.authoring.set_blend_mode"), Payload, Capture));
    TestFalse(TEXT("set_blend_mode on an instance is an error, not a fake success"), Capture.bSuccess);
    TestNotEqual(TEXT("error code is NOT ASSET_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    TestEqual(TEXT("error code is UNSUPPORTED_ASSET_CLASS"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_CLASS")));
    TestTrue(TEXT("message names the class actually found"),
        Capture.Message.Contains(TEXT("MaterialInstanceConstant")));
    TestTrue(TEXT("message routes to the per-instance override verb"),
        Capture.Message.Contains(TEXT("material.authoring.set_material_instance_base_property_overrides")));

    // The failure must be a pure rejection: nothing may have been written to the instance.
    TestFalse(TEXT("no blend-mode override was written by the rejected call"),
        Instance->BasePropertyOverrides.bOverride_BlendMode != 0);

    CleanupTestAsset(InstancePath);
    CleanupTestAsset(ParentPath);
    return true;
}


// The write path. A bare BasePropertyOverrides assignment reads back correctly in memory while
// the compiled permutation keeps the parent's values, so every assertion below that matters is
// on the ENGINE ACCESSOR (GetBlendMode / IsTwoSided), which reads the cached base properties
// UpdateStaticPermutation recomputes — not on the struct the handler wrote.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceBasePropertyOverrideSetTest,
    "PinWright.material.instance.BasePropertyOverrides.SetReachesEffectiveValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInstanceBasePropertyOverrideSetTest::RunTest(const FString& Parameters)
{
    FString ParentPath;
    FString InstancePath;
    UMaterialInstanceConstant* Instance = nullptr;
    if (!CreateBasePropertyOverrideFixture(*this, ParentPath, InstancePath, Instance))
    {
        return true;
    }

    // Baseline: the instance inherits the parent's opaque, single-sided values.
    TestEqual(TEXT("inherited blend mode is Opaque before the write"),
        static_cast<int32>(Instance->GetBlendMode()), static_cast<int32>(BLEND_Opaque));
    TestFalse(TEXT("inherited two-sided is false before the write"), Instance->IsTwoSided());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), InstancePath);
    Payload->SetStringField(TEXT("blendMode"), TEXT("Translucent"));
    Payload->SetBoolField(TEXT("twoSided"), true);
    Payload->SetNumberField(TEXT("opacityMaskClipValue"), 0.25);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("set_material_instance_base_property_overrides handler found"),
        InvokeHandlerWithCapture(
            TEXT("material.authoring.set_material_instance_base_property_overrides"), Payload, Capture));
    TestTrue(TEXT("set_material_instance_base_property_overrides succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        TestTrue(TEXT("applied lists blendMode"),
            ResponseArrayHasString(Capture.Result, TEXT("applied"), TEXT("blendMode")));
        TestTrue(TEXT("applied lists twoSided"),
            ResponseArrayHasString(Capture.Result, TEXT("applied"), TEXT("twoSided")));
        TestTrue(TEXT("overridden lists blendMode"),
            ResponseArrayHasString(Capture.Result, TEXT("overridden"), TEXT("blendMode")));
        TestTrue(TEXT("overridden lists twoSided"),
            ResponseArrayHasString(Capture.Result, TEXT("overridden"), TEXT("twoSided")));
        TestTrue(TEXT("overridden lists opacityMaskClipValue"),
            ResponseArrayHasString(Capture.Result, TEXT("overridden"), TEXT("opacityMaskClipValue")));

        const TSharedPtr<FJsonObject>* EffectiveObj = nullptr;
        TestTrue(TEXT("effective field present"),
            Capture.Result->TryGetObjectField(TEXT("effective"), EffectiveObj));
        if (EffectiveObj && (*EffectiveObj).IsValid())
        {
            FString EffectiveBlend;
            TestTrue(TEXT("effective.blendMode present"),
                (*EffectiveObj)->TryGetStringField(TEXT("blendMode"), EffectiveBlend));
            TestEqual(TEXT("effective.blendMode == Translucent"), EffectiveBlend, FString(TEXT("Translucent")));

            bool bEffectiveTwoSided = false;
            TestTrue(TEXT("effective.twoSided present"),
                (*EffectiveObj)->TryGetBoolField(TEXT("twoSided"), bEffectiveTwoSided));
            TestTrue(TEXT("effective.twoSided == true"), bEffectiveTwoSided);
        }

        // Shared read-back shape: the same basePropertyOverrides block get_material_instance_info
        // and asset.dump emit, so a caller does not need a second spelling of the struct.
        const TSharedPtr<FJsonObject>* BpoObj = nullptr;
        TestTrue(TEXT("basePropertyOverrides echo present"),
            Capture.Result->TryGetObjectField(TEXT("basePropertyOverrides"), BpoObj));
        if (BpoObj && (*BpoObj).IsValid())
        {
            bool bOverrideBlendMode = false;
            TestTrue(TEXT("basePropertyOverrides.bOverride_BlendMode present"),
                (*BpoObj)->TryGetBoolField(TEXT("bOverride_BlendMode"), bOverrideBlendMode));
            TestTrue(TEXT("basePropertyOverrides.bOverride_BlendMode == true"), bOverrideBlendMode);
        }
    }

    // The struct flags + values landed.
    TestTrue(TEXT("bOverride_BlendMode is set"), Instance->BasePropertyOverrides.bOverride_BlendMode != 0);
    TestEqual(TEXT("BasePropertyOverrides.BlendMode == BLEND_Translucent"),
        static_cast<int32>(Instance->BasePropertyOverrides.BlendMode.GetValue()),
        static_cast<int32>(BLEND_Translucent));
    TestTrue(TEXT("bOverride_TwoSided is set"), Instance->BasePropertyOverrides.bOverride_TwoSided != 0);
    TestTrue(TEXT("bOverride_OpacityMaskClipValue is set"),
        Instance->BasePropertyOverrides.bOverride_OpacityMaskClipValue != 0);

    // The part a bare field assignment would fail: the cached/effective values the renderer
    // consults were recomputed by UpdateStaticPermutation.
    TestEqual(TEXT("effective GetBlendMode() == BLEND_Translucent"),
        static_cast<int32>(Instance->GetBlendMode()), static_cast<int32>(BLEND_Translucent));
    TestTrue(TEXT("effective IsTwoSided() == true"), Instance->IsTwoSided());
    TestEqual(TEXT("effective GetOpacityMaskClipValue() == 0.25"),
        Instance->GetOpacityMaskClipValue(), 0.25f, 0.0001f);

    CleanupTestAsset(InstancePath);
    CleanupTestAsset(ParentPath);
    return true;
}


// The clear half: `clear` must turn bOverride_* back OFF and let the parent's value through
// again. Without it the verb could only ever add overrides, which is how the instance ends up
// permanently diverged from a master it was meant to track.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceBasePropertyOverrideClearTest,
    "PinWright.material.instance.BasePropertyOverrides.ClearReturnsToParentValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInstanceBasePropertyOverrideClearTest::RunTest(const FString& Parameters)
{
    FString ParentPath;
    FString InstancePath;
    UMaterialInstanceConstant* Instance = nullptr;
    if (!CreateBasePropertyOverrideFixture(*this, ParentPath, InstancePath, Instance))
    {
        return true;
    }

    // ---- Step 1: establish two overrides ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);
        Payload->SetStringField(TEXT("blendMode"), TEXT("Translucent"));
        Payload->SetBoolField(TEXT("twoSided"), true);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("set handler found"),
            InvokeHandlerWithCapture(
                TEXT("material.authoring.set_material_instance_base_property_overrides"), Payload, Capture));
        TestTrue(TEXT("set succeeded"), Capture.bSuccess);
    }
    TestTrue(TEXT("bOverride_BlendMode is set before the clear"),
        Instance->BasePropertyOverrides.bOverride_BlendMode != 0);
    TestTrue(TEXT("effective IsTwoSided() is true before the clear"), Instance->IsTwoSided());

    // ---- Step 2: clear blendMode only; twoSided must survive ----
    {
        TArray<TSharedPtr<FJsonValue>> ClearList;
        ClearList.Add(MakeShared<FJsonValueString>(TEXT("blendMode")));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);
        Payload->SetArrayField(TEXT("clear"), ClearList);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("clear handler found"),
            InvokeHandlerWithCapture(
                TEXT("material.authoring.set_material_instance_base_property_overrides"), Payload, Capture));
        TestTrue(TEXT("clear succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            TestTrue(TEXT("cleared lists blendMode"),
                ResponseArrayHasString(Capture.Result, TEXT("cleared"), TEXT("blendMode")));
            TestFalse(TEXT("overridden no longer lists blendMode"),
                ResponseArrayHasString(Capture.Result, TEXT("overridden"), TEXT("blendMode")));
            TestTrue(TEXT("overridden still lists twoSided (an omitted slot is untouched)"),
                ResponseArrayHasString(Capture.Result, TEXT("overridden"), TEXT("twoSided")));
        }
    }

    TestFalse(TEXT("bOverride_BlendMode is off after the clear"),
        Instance->BasePropertyOverrides.bOverride_BlendMode != 0);
    TestEqual(TEXT("effective GetBlendMode() returned to the parent's Opaque"),
        static_cast<int32>(Instance->GetBlendMode()), static_cast<int32>(BLEND_Opaque));
    TestTrue(TEXT("bOverride_TwoSided survived a clear that did not name it"),
        Instance->BasePropertyOverrides.bOverride_TwoSided != 0);
    TestTrue(TEXT("effective IsTwoSided() still true"), Instance->IsTwoSided());

    // ---- Step 3: clear:["all"] drops the rest ----
    {
        TArray<TSharedPtr<FJsonValue>> ClearList;
        ClearList.Add(MakeShared<FJsonValueString>(TEXT("all")));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);
        Payload->SetArrayField(TEXT("clear"), ClearList);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("clear-all handler found"),
            InvokeHandlerWithCapture(
                TEXT("material.authoring.set_material_instance_base_property_overrides"), Payload, Capture));
        TestTrue(TEXT("clear-all succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* OverriddenArr = nullptr;
            TestTrue(TEXT("overridden field present"),
                Capture.Result->TryGetArrayField(TEXT("overridden"), OverriddenArr));
            if (OverriddenArr)
            {
                TestEqual(TEXT("no overrides remain after clear-all"), OverriddenArr->Num(), 0);
            }
        }
    }

    TestFalse(TEXT("bOverride_TwoSided is off after clear-all"),
        Instance->BasePropertyOverrides.bOverride_TwoSided != 0);
    TestFalse(TEXT("effective IsTwoSided() returned to the parent's false"), Instance->IsTwoSided());

    CleanupTestAsset(InstancePath);
    CleanupTestAsset(ParentPath);
    return true;
}
