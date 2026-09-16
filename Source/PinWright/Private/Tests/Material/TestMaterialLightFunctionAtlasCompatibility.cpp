// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-light-function-atlas-silently-drops-material.
//
// A MD_LightFunction material whose graph manipulates texture coordinates is excluded from the
// light function atlas by construction (HLSLMaterialTranslator sets bPotentiallyManipulateTexCoords
// from UMaterialExpressionTextureCoordinate::Compile unconditionally, and the atlas-compat bit is
// the AND of four such negations OR'd with the per-material override). Such a material still
// modulates opaque surfaces through the classic deferred light-function pass, so the asset, the
// light component and every r.*LightFunctionAtlas cvar all read healthy while volumetric fog gets
// nothing - and BEFORE this fix no PinWright read reported the difference.
//
// The fixture is exactly that shape: TextureCoordinate -> AppendVector -> EmissiveColor on a
// LightFunction-domain material.
//
// Counterfactual. Reverting the fix removes the `lightFunctionAtlas` block from
// get_material_info's response and deletes the set_light_function_atlas_compatible handler
// entirely, so "get_material_info reports a lightFunctionAtlas block" fails in the first test and
// "handler registered" fails in the second. Reverting only the measured half - echoing the
// override back as `compatible` instead of reading the compiled shader map - passes the first
// test's forceCompatible assertion but fails "a texcoord-manipulating light function measures as
// NOT atlas compatible", because the echo would report true as soon as the override is set and
// false while it is not, independent of what the renderer will actually do.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Material/MaterialLightFunctionAtlas.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
    UMaterial* LFAtlas_CreateMaterial(FAutomationTestBase& Test, const TCHAR* NameStem,
        EMaterialDomain Domain, bool bManipulateTexCoords, FString& OutAssetPath)
    {
        OutAssetPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            NameStem, *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*OutAssetPath);
        if (!Test.TestNotNull(TEXT("Package created"), Pkg))
        {
            return nullptr;
        }

        UMaterial* Material = NewObject<UMaterial>(
            Pkg,
            FName(*FPackageName::GetLongPackageAssetName(OutAssetPath)),
            RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("Material created"), Material))
        {
            CleanupTestAsset(OutAssetPath);
            return nullptr;
        }

        Material->MaterialDomain = Domain;

        if (bManipulateTexCoords)
        {
            // TextureCoordinate is the smallest node that forces the exclusion: its Compile()
            // calls FMaterialCompiler::SetPotentiallyManipulateTexCoords() unconditionally, whatever
            // the tiling. AppendVector promotes its float2 to the float3 EmissiveColor expects, so
            // the graph type-checks and the exclusion is the only thing under test.
            UMaterialExpressionTextureCoordinate* TexCoord =
                NewObject<UMaterialExpressionTextureCoordinate>(Material);
            TexCoord->MaterialExpressionGuid = FGuid::NewGuid();

            UMaterialExpressionConstant* Zero = NewObject<UMaterialExpressionConstant>(Material);
            Zero->R = 0.0f;
            Zero->MaterialExpressionGuid = FGuid::NewGuid();

            UMaterialExpressionAppendVector* Append =
                NewObject<UMaterialExpressionAppendVector>(Material);
            Append->MaterialExpressionGuid = FGuid::NewGuid();
            Append->A.Expression = TexCoord;
            Append->A.OutputIndex = 0;
            Append->B.Expression = Zero;
            Append->B.OutputIndex = 0;

            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(TexCoord);
            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Zero);
            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Append);

            FExpressionInput& Emissive = Material->GetEditorOnlyData()->EmissiveColor;
            Emissive.Expression = Append;
            Emissive.OutputIndex = 0;
        }

        Material->PostEditChange();
        Material->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }

    // Drive the production compile verb so the measured bit has a shader map to be read off.
    // PostEditChange alone only translates; without a completed CacheShaders the game-thread
    // shader map can still be absent and the block would honestly report "nothing measured".
    void LFAtlas_Compile(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("material.authoring.compile_material"), Payload, Capture);
    }

    // Reads the lightFunctionAtlas block out of a captured response, or null when absent.
    const TSharedPtr<FJsonObject>* LFAtlas_Block(const FTestResponseCapture& Capture)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (Capture.Result.IsValid() &&
            Capture.Result->TryGetObjectField(TEXT("lightFunctionAtlas"), Block))
        {
            return Block;
        }
        return nullptr;
    }
}


// Regression: get_material_info must report MEASURED atlas compatibility for a LightFunction
// material. Without it, a light function that reaches only opaque surfaces is indistinguishable
// from one that also reaches volumetric fog.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialLightFunctionAtlasReadbackTest,
    "PinWright.material.authoring.get_material_info.LightFunctionAtlasCompatibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialLightFunctionAtlasReadbackTest::RunTest(const FString& Parameters)
{
    if constexpr (!PinWright::LightFunctionAtlas::bSupportedOnThisEngine)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-has-no-atlas-override"),
            TEXT("UMaterial::bForceCompatibleWithLightFunctionAtlas and ")
            TEXT("FMaterial::MaterialIsLightFunctionAtlasCompatible_GameThread are both 5.5+."));
        return true;
    }

    FString AssetPath;
    UMaterial* Material = LFAtlas_CreateMaterial(
        *this, TEXT("LFAtlasRead"), MD_LightFunction, /*bManipulateTexCoords=*/true, AssetPath);
    if (!Material)
    {
        return true;
    }

    LFAtlas_Compile(AssetPath);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("material.authoring.get_material_info"), Payload, Capture));
    TestTrue(TEXT("get_material_info succeeded"), Capture.bSuccess);

    const TSharedPtr<FJsonObject>* Block = LFAtlas_Block(Capture);
    if (TestTrue(TEXT("get_material_info reports a lightFunctionAtlas block"), Block != nullptr))
    {
        bool bForceCompatible = true;
        TestTrue(TEXT("forceCompatible is reported"),
            (*Block)->TryGetBoolField(TEXT("forceCompatible"), bForceCompatible));
        TestFalse(TEXT("forceCompatible is false on a material that never set the override"),
            bForceCompatible);

        bool bShaderMapReady = false;
        TestTrue(TEXT("shaderMapReady is reported"),
            (*Block)->TryGetBoolField(TEXT("shaderMapReady"), bShaderMapReady));

        // The second gate is reported beside the first, so a caller cannot read
        // compatible:true as "this reaches the fog" on a host with the atlas turned off.
        const TSharedPtr<FJsonObject>* AtlasGeneration = nullptr;
        if (TestTrue(TEXT("the block measures the atlas-generation cvar"),
                (*Block)->TryGetObjectField(TEXT("atlasGeneration"), AtlasGeneration) &&
                    AtlasGeneration != nullptr))
        {
            FString CVarName;
            (*AtlasGeneration)->TryGetStringField(TEXT("cvar"), CVarName);
            TestEqual(TEXT("the measured cvar is r.LightFunctionAtlas"), CVarName,
                FString(TEXT("r.LightFunctionAtlas")));
            TestTrue(TEXT("the cvar lookup reports whether it was found"),
                (*AtlasGeneration)->HasField(TEXT("found")));
        }

        if (bShaderMapReady)
        {
            bool bCompatible = true;
            TestTrue(TEXT("compatible is reported once there is a shader map to measure"),
                (*Block)->TryGetBoolField(TEXT("compatible"), bCompatible));
            TestFalse(
                TEXT("a texcoord-manipulating light function measures as NOT atlas compatible"),
                bCompatible);

            FString Warning;
            TestTrue(TEXT("an incompatible light function carries a warning"),
                (*Block)->TryGetStringField(TEXT("warning"), Warning));
            TestTrue(TEXT("the warning names the remedy verb"),
                Warning.Contains(PinWright::LightFunctionAtlas::RemedyVerb()));
        }
        else
        {
            // Honesty half of the contract: an unmeasured value must not be readable as a
            // measured one, so `compatible` is absent rather than false.
            TestFalse(TEXT("compatible is omitted when nothing was measured"),
                (*Block)->HasField(TEXT("compatible")));
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-material-shader-map"),
                TEXT("compile_material produced no game-thread shader map on this host, so the ")
                TEXT("measured atlas bit could not be asserted."));
        }
    }

    CleanupTestAsset(AssetPath);
    return true;
}


// Regression: set_light_function_atlas_compatible must actually write the override (verified on
// the UObject, not from its own echo) and the measured bit must flip after a recompile. Also
// covers the two no-noise / no-silent-no-op edges: a non-LightFunction material gets no block from
// get_material_info, and gets a domainWarning from the setter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialLightFunctionAtlasOverrideTest,
    "PinWright.material.authoring.set_light_function_atlas_compatible.OverrideFlipsTheMeasuredBit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialLightFunctionAtlasOverrideTest::RunTest(const FString& Parameters)
{
    if constexpr (!PinWright::LightFunctionAtlas::bSupportedOnThisEngine)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-has-no-atlas-override"),
            TEXT("UMaterial::bForceCompatibleWithLightFunctionAtlas and ")
            TEXT("FMaterial::MaterialIsLightFunctionAtlasCompatible_GameThread are both 5.5+."));
        return true;
    }

    FString AssetPath;
    UMaterial* Material = LFAtlas_CreateMaterial(
        *this, TEXT("LFAtlasSet"), MD_LightFunction, /*bManipulateTexCoords=*/true, AssetPath);
    if (!Material)
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetBoolField(TEXT("forceCompatible"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(
            TEXT("material.authoring.set_light_function_atlas_compatible"), Payload, Capture));
    TestTrue(TEXT("set_light_function_atlas_compatible succeeded"), Capture.bSuccess);

    // The write, measured on the object rather than trusted from the response.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    TestTrue(TEXT("bForceCompatibleWithLightFunctionAtlas is set on the asset"),
        Material->bForceCompatibleWithLightFunctionAtlas != 0);
#endif

    if (const TSharedPtr<FJsonObject>* SetBlock = LFAtlas_Block(Capture))
    {
        bool bForceCompatible = false;
        TestTrue(TEXT("the setter reports forceCompatible back"),
            (*SetBlock)->TryGetBoolField(TEXT("forceCompatible"), bForceCompatible));
        TestTrue(TEXT("forceCompatible reads true after the write"), bForceCompatible);
    }
    else
    {
        AddError(TEXT("set_light_function_atlas_compatible returned no lightFunctionAtlas block"));
    }

    // The override only takes effect at translation time, so recompile before re-measuring.
    LFAtlas_Compile(AssetPath);

    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("assetPath"), AssetPath);
    FTestResponseCapture InfoCapture;
    InvokeHandlerWithCapture(TEXT("material.authoring.get_material_info"), InfoPayload, InfoCapture);

    if (const TSharedPtr<FJsonObject>* InfoBlock = LFAtlas_Block(InfoCapture))
    {
        bool bShaderMapReady = false;
        (*InfoBlock)->TryGetBoolField(TEXT("shaderMapReady"), bShaderMapReady);
        if (bShaderMapReady)
        {
            bool bCompatible = false;
            TestTrue(TEXT("compatible is reported after the recompile"),
                (*InfoBlock)->TryGetBoolField(TEXT("compatible"), bCompatible));
            TestTrue(TEXT("the override makes the same graph measure as atlas compatible"),
                bCompatible);
            TestFalse(TEXT("a compatible light function carries no warning"),
                (*InfoBlock)->HasField(TEXT("warning")));
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-material-shader-map"),
                TEXT("compile_material produced no game-thread shader map on this host, so the ")
                TEXT("post-override measured atlas bit could not be asserted."));
        }
    }
    else
    {
        AddError(TEXT("get_material_info returned no lightFunctionAtlas block after the override"));
    }

    CleanupTestAsset(AssetPath);

    // A Surface material: the flag is inert there, so the setter must say so rather than report a
    // bare success, and the read must not carry the block at all (no noise on every material).
    FString SurfacePath;
    UMaterial* Surface = LFAtlas_CreateMaterial(
        *this, TEXT("LFAtlasSurface"), MD_Surface, /*bManipulateTexCoords=*/false, SurfacePath);
    if (!Surface)
    {
        return true;
    }

    TSharedPtr<FJsonObject> SurfaceSetPayload = MakeShared<FJsonObject>();
    SurfaceSetPayload->SetStringField(TEXT("assetPath"), SurfacePath);
    SurfaceSetPayload->SetBoolField(TEXT("forceCompatible"), true);
    SurfaceSetPayload->SetBoolField(TEXT("save"), false);
    FTestResponseCapture SurfaceSetCapture;
    InvokeHandlerWithCapture(TEXT("material.authoring.set_light_function_atlas_compatible"),
        SurfaceSetPayload, SurfaceSetCapture);
    TestTrue(TEXT("setting the flag on a non-LightFunction material warns about the domain"),
        SurfaceSetCapture.Result.IsValid() &&
            SurfaceSetCapture.Result->HasField(TEXT("domainWarning")));

    TSharedPtr<FJsonObject> SurfaceInfoPayload = MakeShared<FJsonObject>();
    SurfaceInfoPayload->SetStringField(TEXT("assetPath"), SurfacePath);
    FTestResponseCapture SurfaceInfoCapture;
    InvokeHandlerWithCapture(TEXT("material.authoring.get_material_info"),
        SurfaceInfoPayload, SurfaceInfoCapture);
    TestTrue(TEXT("get_material_info emits no atlas block for a non-LightFunction material"),
        SurfaceInfoCapture.Result.IsValid() &&
            !SurfaceInfoCapture.Result->HasField(TEXT("lightFunctionAtlas")));

    CleanupTestAsset(SurfacePath);
    return true;
}
