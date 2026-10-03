// Copyright (c) 2026 Alexander Penkin. MIT License.

// The typed instance setters (set_scalar/vector/texture_parameter_value) used to answer
// ASSET_NOT_FOUND when handed a base UMaterial: the asset loaded, it was just the wrong class.
// They must answer UNSUPPORTED_ASSET_CLASS naming the class found and the instance route, and
// keep ASSET_NOT_FOUND for a path where nothing loads (B-material-param-setters-wrong-class-error).

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialParamSettersBaseMaterialWrongClassTest,
    "PinWright.material.authoring.ParamSetters.BaseMaterialReportsWrongClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialParamSettersBaseMaterialWrongClassTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString MaterialPath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/MatParamSetterBase_%s"), *Suffix);
    const FString MissingPath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/MatParamSetterMissing_%s"), *Suffix);

    UPackage* Pkg = CreatePackage(*MaterialPath);
    if (!TestNotNull(TEXT("Material package created"), Pkg))
        return true;
    UMaterial* Material = NewObject<UMaterial>(
        Pkg, FName(*FPackageName::GetLongPackageAssetName(MaterialPath)), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Base material created"), Material))
    {
        CleanupTestAsset(MaterialPath);
        return true;
    }
    FAssetRegistryModule::AssetCreated(Material);

    // Precondition: the path loads, as a UMaterial — so a wrong answer cannot be blamed on the fixture.
    TestNotNull(TEXT("fixture path loads as a UMaterial"), LoadObject<UMaterial>(nullptr, *MaterialPath));

    const TCHAR* Verbs[] = {
        TEXT("material.authoring.set_scalar_parameter_value"),
        TEXT("material.authoring.set_vector_parameter_value"),
        TEXT("material.authoring.set_texture_parameter_value"),
    };
    for (const TCHAR* Verb : Verbs)
    {
        for (const bool bMissing : { false, true })
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("assetPath"), bMissing ? MissingPath : MaterialPath);
            Payload->SetStringField(TEXT("parameterName"), TEXT("Roughness"));
            if (FCString::Strstr(Verb, TEXT("texture")))
            {
                Payload->SetStringField(TEXT("texturePath"), TEXT("/Engine/EngineResources/DefaultTexture"));
            }
            Payload->SetBoolField(TEXT("save"), false);

            FTestResponseCapture Capture;
            const FString Ctx = FString::Printf(TEXT("%s on %s"), Verb, bMissing ? TEXT("absent path") : TEXT("base UMaterial"));
            TestTrue(FString::Printf(TEXT("%s: handler found"), *Ctx), InvokeHandlerWithCapture(Verb, Payload, Capture));
            TestFalse(FString::Printf(TEXT("%s: is an error"), *Ctx), Capture.bSuccess);
            if (bMissing)
            {
                TestEqual(FString::Printf(TEXT("%s: code"), *Ctx), Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
                continue;
            }
            TestEqual(FString::Printf(TEXT("%s: code"), *Ctx), Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_CLASS")));
            TestTrue(FString::Printf(TEXT("%s: message names the expected class"), *Ctx),
                Capture.Message.Contains(TEXT("UMaterialInstanceConstant")));
            TestTrue(FString::Printf(TEXT("%s: message names the class found"), *Ctx),
                Capture.Message.Contains(TEXT("Received class: Material")));
            TestTrue(FString::Printf(TEXT("%s: message names the instance route"), *Ctx),
                Capture.Message.Contains(TEXT("material.authoring.create_material_instance")));
        }
    }

    // set_material_instance_base_property_overrides on a base Material routes to the base-property
    // verbs, the mirror of set_blend_mode naming this verb for an instance.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialPath);
        Payload->SetStringField(TEXT("blendMode"), TEXT("Translucent"));
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("base_property_overrides: handler found"),
            InvokeHandlerWithCapture(TEXT("material.authoring.set_material_instance_base_property_overrides"), Payload, Capture));
        TestEqual(TEXT("base_property_overrides on base UMaterial: code"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_CLASS")));
        TestTrue(TEXT("base_property_overrides on base UMaterial: message names set_blend_mode"),
            Capture.Message.Contains(TEXT("material.authoring.set_blend_mode")));
    }

    CleanupTestAsset(MaterialPath);
    return true;
}
