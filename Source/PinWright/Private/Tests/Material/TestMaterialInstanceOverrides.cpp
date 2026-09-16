// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
// FMaterialParameterInfo et al. moved from the top-level MaterialTypes.h into
// Materials/MaterialParameters.h in UE 5.7; on 5.6 and earlier they live in
// MaterialTypes.h, which has no MaterialParameters.h (and is deprecated on 5.8).
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#include "Materials/MaterialExpression.h"

namespace
{
    // Creates a parent UMaterial (with one parameter expression supplied by
    // AddParam) plus a child UMaterialInstanceConstant pointing at it, mirroring
    // the create-parent-then-instance recipe both round-trip tests need. On
    // success fills OutParentPath/OutInstancePath/OutInstance and returns true;
    // on any failure it records the TestNotNull guard, cleans up whatever was
    // created, and returns false so the caller can early-return from RunTest.
    bool CreateTestMaterialInstance(
        FAutomationTestBase& Test,
        const FString& NamePrefix,
        TFunctionRef<UMaterialExpression*(UMaterial*)> AddParam,
        FString& OutParentPath,
        FString& OutInstancePath,
        UMaterialInstanceConstant*& OutInstance)
    {
        OutInstance = nullptr;
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        OutParentPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/MatParent%s_%s"), *NamePrefix, *Suffix);
        OutInstancePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/MatInst%s_%s"), *NamePrefix, *Suffix);

        // ---- Create parent UMaterial with the caller-supplied parameter ----
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

        UMaterialExpression* Param = AddParam(ParentMat);
        if (!Test.TestNotNull(TEXT("Parameter expression created"), Param))
        {
            CleanupTestAsset(OutParentPath);
            return false;
        }
        if (!Param->MaterialExpressionGuid.IsValid())
        {
            Param->MaterialExpressionGuid = FGuid::NewGuid();
        }
        ParentMat->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Param);
        ParentMat->PostEditChange();
        ParentMat->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(ParentMat);

        // ---- Create child UMaterialInstanceConstant pointing at the parent ----
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
        Instance->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Instance);

        OutInstance = Instance;
        return true;
    }
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceClearAndReadbackTest,
    "PinWright.material.instance.ClearOverrideAndReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInstanceClearAndReadbackTest::RunTest(const FString& Parameters)
{
    FString ParentPath;
    FString InstancePath;
    UMaterialInstanceConstant* Instance = nullptr;
    // Parent gets a Roughness scalar parameter (default 0.5, sort priority 7).
    if (!CreateTestMaterialInstance(*this, TEXT(""),
            [](UMaterial* Mat) -> UMaterialExpression*
            {
                UMaterialExpressionScalarParameter* ScalarParam =
                    NewObject<UMaterialExpressionScalarParameter>(Mat);
                if (ScalarParam)
                {
                    ScalarParam->ParameterName = TEXT("Roughness");
                    ScalarParam->DefaultValue = 0.5f;
                    ScalarParam->SortPriority = 7;
                }
                return ScalarParam;
            },
            ParentPath, InstancePath, Instance))
    {
        return true;
    }

    // ---- Step 1: set_scalar_parameter_value Roughness=0.9 ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);
        Payload->SetStringField(TEXT("parameterName"), TEXT("Roughness"));
        Payload->SetNumberField(TEXT("value"), 0.9);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.set_scalar_parameter_value"), Payload, Capture);
        TestTrue(TEXT("set_scalar_parameter_value handler found"), bFound);
        TestTrue(TEXT("set_scalar_parameter_value succeeded"), Capture.bSuccess);
    }

    // ---- Step 2: get_material_instance_info shows override + inherited ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.get_material_instance_info"), Payload, Capture);
        TestTrue(TEXT("get_material_instance_info handler found"), bFound);
        TestTrue(TEXT("get_material_instance_info succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* OverridesObj = nullptr;
            TestTrue(TEXT("overrides field present"),
                Capture.Result->TryGetObjectField(TEXT("overrides"), OverridesObj));
            if (OverridesObj && OverridesObj->IsValid())
            {
                const TSharedPtr<FJsonObject>* ScalarObj = nullptr;
                TestTrue(TEXT("overrides.scalar present"),
                    (*OverridesObj)->TryGetObjectField(TEXT("scalar"), ScalarObj));
                if (ScalarObj && ScalarObj->IsValid())
                {
                    double Val = 0.0;
                    const bool bHas = (*ScalarObj)->TryGetNumberField(TEXT("Roughness"), Val);
                    TestTrue(TEXT("overrides.scalar.Roughness present"), bHas);
                    TestEqual(TEXT("overrides.scalar.Roughness == 0.9"), Val, 0.9, 0.0001);
                }
            }

            const TSharedPtr<FJsonObject>* InheritedObj = nullptr;
            TestTrue(TEXT("inherited field present"),
                Capture.Result->TryGetObjectField(TEXT("inherited"), InheritedObj));
            if (InheritedObj && InheritedObj->IsValid())
            {
                const TSharedPtr<FJsonObject>* ScalarObj = nullptr;
                TestTrue(TEXT("inherited.scalar present"),
                    (*InheritedObj)->TryGetObjectField(TEXT("scalar"), ScalarObj));
                if (ScalarObj && ScalarObj->IsValid())
                {
                    double Val = 0.0;
                    const bool bHas = (*ScalarObj)->TryGetNumberField(TEXT("Roughness"), Val);
                    TestTrue(TEXT("inherited.scalar.Roughness present"), bHas);
                    TestEqual(TEXT("inherited.scalar.Roughness == 0.5"), Val, 0.5, 0.0001);
                }
            }

            const TArray<TSharedPtr<FJsonValue>>* ParametersArray = nullptr;
            TestTrue(TEXT("parameters field present"),
                Capture.Result->TryGetArrayField(TEXT("parameters"), ParametersArray));
            bool bFoundRoughnessParameter = false;
            if (ParametersArray)
            {
                for (const TSharedPtr<FJsonValue>& ParameterValue : *ParametersArray)
                {
                    if (!ParameterValue.IsValid())
                    {
                        continue;
                    }

                    const TSharedPtr<FJsonObject> ParameterObj = ParameterValue->AsObject();
                    if (!ParameterObj.IsValid())
                    {
                        continue;
                    }

                    FString Name;
                    if (!ParameterObj->TryGetStringField(TEXT("name"), Name) || Name != TEXT("Roughness"))
                    {
                        continue;
                    }

                    bFoundRoughnessParameter = true;
                    FString Type;
                    TestTrue(TEXT("parameters.Roughness.type present"),
                        ParameterObj->TryGetStringField(TEXT("type"), Type));
                    TestEqual(TEXT("parameters.Roughness.type == scalar"), Type, FString(TEXT("scalar")));

                    double SortPriority = 0.0;
                    const bool bHasSortPriority = ParameterObj->TryGetNumberField(TEXT("sortPriority"), SortPriority);
                    TestTrue(TEXT("parameters.Roughness.sortPriority present"), bHasSortPriority);
                    if (bHasSortPriority)
                    {
                        TestEqual(TEXT("parameters.Roughness.sortPriority == 7"), SortPriority, 7.0, 0.0001);
                    }
                    break;
                }
            }
            TestTrue(TEXT("parameters contains Roughness"), bFoundRoughnessParameter);
        }
    }

    // ---- Step 3: clear_parameter_override removes the entry ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);
        Payload->SetStringField(TEXT("parameterName"), TEXT("Roughness"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("scalar"));
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.clear_parameter_override"), Payload, Capture);
        TestTrue(TEXT("clear_parameter_override handler found"), bFound);
        TestTrue(TEXT("clear_parameter_override succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bCleared = false;
            const bool bHasCleared = Capture.Result->TryGetBoolField(TEXT("cleared"), bCleared);
            TestTrue(TEXT("cleared field present"), bHasCleared);
            TestTrue(TEXT("cleared == true"), bCleared);
        }
    }

    // ---- Step 4: get_material_instance_info no longer lists Roughness override ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.get_material_instance_info"), Payload, Capture);
        TestTrue(TEXT("get_material_instance_info handler found (post-clear)"), bFound);
        TestTrue(TEXT("get_material_instance_info succeeded (post-clear)"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* OverridesObj = nullptr;
            if (Capture.Result->TryGetObjectField(TEXT("overrides"), OverridesObj)
                && OverridesObj && OverridesObj->IsValid())
            {
                const TSharedPtr<FJsonObject>* ScalarObj = nullptr;
                if ((*OverridesObj)->TryGetObjectField(TEXT("scalar"), ScalarObj)
                    && ScalarObj && ScalarObj->IsValid())
                {
                    TestFalse(TEXT("overrides.scalar must no longer contain Roughness after clear"),
                        (*ScalarObj)->HasField(TEXT("Roughness")));
                }
            }
        }
    }

    // ---- Step 5: live read on the instance returns the inherited 0.5 ----
    {
        float Live = 0.0f;
        const bool bGot = Instance->GetScalarParameterValue(FMaterialParameterInfo(FName(TEXT("Roughness"))), Live);
        TestTrue(TEXT("Instance->GetScalarParameterValue returned a value"), bGot);
        TestEqual(TEXT("Instance returns inherited 0.5 after clear"), (double)Live, 0.5, 0.0001);
    }

    CleanupTestAsset(InstancePath);
    CleanupTestAsset(ParentPath);
    return true;
}

// Regression for E-create-material-instance-duplicate-divergent-shape: the
// preferred create verb (material.authoring.create_material_instance) used to
// have NO inline override slot, so an agent that wanted create+tint in one call
// was pushed onto the legacy asset.create_material_instance creator. The fix
// gives the preferred verb the same type-keyed `parameters` object as
// set_material_instance_parameters, so create + override is one round-trip.
// This test creates a parent UMaterial (Roughness scalar default 0.5), drives
// the REAL material.authoring.create_material_instance handler with an inline
// parameters={scalar:{Roughness:0.9}}, and asserts via get_material_instance_info
// that the override landed at creation — it fails (no override, or no `parameters`
// param accepted) if the inline-override fix is reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceCreateWithInlineParamsTest,
    "PinWright.material.instance.CreateWithInlineParameters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInstanceCreateWithInlineParamsTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ParentPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/MatParentInline_%s"), *Suffix);
    const FString InstanceDir = TEXT("/Game/__PW_GatewayTests");
    const FString InstanceName = FString::Printf(TEXT("MatInstInline_%s"), *Suffix);
    const FString InstancePath = InstanceDir / InstanceName;

    // ---- Create parent UMaterial with a Roughness scalar parameter (default 0.5) ----
    UPackage* ParentPkg = CreatePackage(*ParentPath);
    if (!TestNotNull(TEXT("Parent package created"), ParentPkg))
        return true;

    UMaterial* ParentMat = NewObject<UMaterial>(
        ParentPkg,
        FName(*FPackageName::GetLongPackageAssetName(ParentPath)),
        RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Parent material created"), ParentMat))
    {
        CleanupTestAsset(ParentPath);
        return true;
    }

    UMaterialExpressionScalarParameter* ScalarParam =
        NewObject<UMaterialExpressionScalarParameter>(ParentMat);
    if (!TestNotNull(TEXT("Roughness scalar parameter created"), ScalarParam))
    {
        CleanupTestAsset(ParentPath);
        return true;
    }
    ScalarParam->ParameterName = TEXT("Roughness");
    ScalarParam->DefaultValue = 0.5f;
    ScalarParam->MaterialExpressionGuid = FGuid::NewGuid();
    ParentMat->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(ScalarParam);
    ParentMat->PostEditChange();
    ParentMat->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(ParentMat);

    // ---- Drive material.authoring.create_material_instance with inline overrides ----
    {
        TSharedPtr<FJsonObject> ScalarMap = MakeShared<FJsonObject>();
        ScalarMap->SetNumberField(TEXT("Roughness"), 0.9);
        TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
        ParamsObj->SetObjectField(TEXT("scalar"), ScalarMap);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), InstanceName);
        Payload->SetStringField(TEXT("parentMaterial"), ParentPath);
        Payload->SetStringField(TEXT("path"), InstanceDir);
        Payload->SetObjectField(TEXT("parameters"), ParamsObj);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.create_material_instance"), Payload, Capture);
        TestTrue(TEXT("create_material_instance handler found"), bFound);
        TestTrue(TEXT("create_material_instance succeeded"), Capture.bSuccess);

        // The inline-override path reports applied entries; assert Roughness applied.
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* AppliedArr = nullptr;
            const bool bHasApplied = Capture.Result->TryGetArrayField(TEXT("applied"), AppliedArr);
            TestTrue(TEXT("create response has applied array (inline params path)"), bHasApplied);
            bool bRoughnessApplied = false;
            if (AppliedArr)
            {
                for (const TSharedPtr<FJsonValue>& V : *AppliedArr)
                {
                    const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
                    FString Type, ParamName;
                    if (Obj.IsValid()
                        && Obj->TryGetStringField(TEXT("type"), Type) && Type == TEXT("scalar")
                        && Obj->TryGetStringField(TEXT("name"), ParamName) && ParamName == TEXT("Roughness"))
                    {
                        bRoughnessApplied = true;
                        break;
                    }
                }
            }
            TestTrue(TEXT("applied contains scalar Roughness"), bRoughnessApplied);
        }
    }

    // ---- get_material_instance_info: the override landed AT CREATION ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.get_material_instance_info"), Payload, Capture);
        TestTrue(TEXT("get_material_instance_info handler found"), bFound);
        TestTrue(TEXT("get_material_instance_info succeeded"), Capture.bSuccess);

        bool bRoughnessOverridePresent = false;
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* OverridesObj = nullptr;
            if (Capture.Result->TryGetObjectField(TEXT("overrides"), OverridesObj)
                && OverridesObj && OverridesObj->IsValid())
            {
                const TSharedPtr<FJsonObject>* ScalarObj = nullptr;
                if ((*OverridesObj)->TryGetObjectField(TEXT("scalar"), ScalarObj)
                    && ScalarObj && ScalarObj->IsValid())
                {
                    double Val = 0.0;
                    bRoughnessOverridePresent = (*ScalarObj)->TryGetNumberField(TEXT("Roughness"), Val);
                    if (bRoughnessOverridePresent)
                    {
                        TestEqual(TEXT("inline override Roughness == 0.9 at creation"), Val, 0.9, 0.0001);
                    }
                }
            }
        }
        TestTrue(TEXT("overrides.scalar.Roughness present after create-with-inline-params"),
            bRoughnessOverridePresent);
    }

    CleanupTestAsset(InstancePath);
    CleanupTestAsset(ParentPath);
    return true;
}

// Regression for F-material-instance-overrides-incomplete history #7: the
// per-instance static-switch override returned success but never persisted,
// because the setter wrote on the instance while an
// FMaterialInstanceParameterUpdateContext was open and the context dtor
// committed its stale pre-write snapshot, discarding the switch. This test
// drives set_static_switch_parameter_value and the batch
// set_material_instance_parameters static-switch branch through the real
// handlers and asserts the override lands in overrides.staticSwitch — it fails
// (empty staticSwitch) if the fix is reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceStaticSwitchRoundTripTest,
    "PinWright.material.instance.StaticSwitchOverrideRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInstanceStaticSwitchRoundTripTest::RunTest(const FString& Parameters)
{
    FString ParentPath;
    FString InstancePath;
    UMaterialInstanceConstant* Instance = nullptr;
    // Parent gets an EnableRust static switch parameter (default false).
    if (!CreateTestMaterialInstance(*this, TEXT("SS"),
            [](UMaterial* Mat) -> UMaterialExpression*
            {
                UMaterialExpressionStaticSwitchParameter* SwitchParam =
                    NewObject<UMaterialExpressionStaticSwitchParameter>(Mat);
                if (SwitchParam)
                {
                    SwitchParam->ParameterName = TEXT("EnableRust");
                    SwitchParam->DefaultValue = false;
                }
                return SwitchParam;
            },
            ParentPath, InstancePath, Instance))
    {
        return true;
    }

    // Reads overrides.staticSwitch.EnableRust off get_material_instance_info,
    // returning whether the key is present and (out) its value.
    auto ReadStaticSwitchOverride = [&](const TCHAR* Label, bool& bOutPresent, bool& bOutValue)
    {
        bOutPresent = false;
        bOutValue = false;

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.get_material_instance_info"), Payload, Capture);
        TestTrue(FString::Printf(TEXT("%s: get_material_instance_info handler found"), Label), bFound);
        TestTrue(FString::Printf(TEXT("%s: get_material_instance_info succeeded"), Label), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* OverridesObj = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("overrides"), OverridesObj)
            || !OverridesObj || !OverridesObj->IsValid())
        {
            return;
        }
        const TSharedPtr<FJsonObject>* StaticSwitchObj = nullptr;
        if (!(*OverridesObj)->TryGetObjectField(TEXT("staticSwitch"), StaticSwitchObj)
            || !StaticSwitchObj || !StaticSwitchObj->IsValid())
        {
            return;
        }
        bOutPresent = (*StaticSwitchObj)->TryGetBoolField(TEXT("EnableRust"), bOutValue);
    };

    // ---- Step 1: set_static_switch_parameter_value EnableRust=true ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);
        Payload->SetStringField(TEXT("parameterName"), TEXT("EnableRust"));
        Payload->SetBoolField(TEXT("value"), true);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.set_static_switch_parameter_value"), Payload, Capture);
        TestTrue(TEXT("set_static_switch_parameter_value handler found"), bFound);
        TestTrue(TEXT("set_static_switch_parameter_value succeeded"), Capture.bSuccess);
    }

    // ---- Step 2: the override actually persisted (the regression assertion) ----
    {
        bool bPresent = false;
        bool bValue = false;
        ReadStaticSwitchOverride(TEXT("after single set"), bPresent, bValue);
        TestTrue(TEXT("overrides.staticSwitch.EnableRust present after single set"), bPresent);
        TestTrue(TEXT("overrides.staticSwitch.EnableRust == true after single set"), bValue);
    }

    // ---- Step 3: batch set_material_instance_parameters flips it to false ----
    {
        TSharedPtr<FJsonObject> StaticSwitchMap = MakeShared<FJsonObject>();
        StaticSwitchMap->SetBoolField(TEXT("EnableRust"), false);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), InstancePath);
        Payload->SetObjectField(TEXT("staticSwitch"), StaticSwitchMap);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("material.authoring.set_material_instance_parameters"), Payload, Capture);
        TestTrue(TEXT("set_material_instance_parameters handler found"), bFound);
        TestTrue(TEXT("set_material_instance_parameters succeeded"), Capture.bSuccess);
    }

    // ---- Step 4: batch static-switch override persisted (value flipped to false, still overridden) ----
    {
        bool bPresent = false;
        bool bValue = true;
        ReadStaticSwitchOverride(TEXT("after batch set"), bPresent, bValue);
        TestTrue(TEXT("overrides.staticSwitch.EnableRust present after batch set"), bPresent);
        TestFalse(TEXT("overrides.staticSwitch.EnableRust == false after batch set"), bValue);
    }

    CleanupTestAsset(InstancePath);
    CleanupTestAsset(ParentPath);
    return true;
}
