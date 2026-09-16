// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-light-function-not-reported-from-actor-describe.
//
// The measured "will this light function reach volumetric fog?" verdict shipped on the MATERIAL
// side only (material.authoring.get_material_info). The read a level author actually makes when a
// light function does not reach the fog is actor.describe on the LIGHT, and that reported
// LightFunctionMaterial as a bare path with nothing beside it - so the answer was reachable only by
// someone who already suspected the atlas. ActorDescribeBuilder now routes a light component's
// light function material through PinWright::LightFunctionAtlas::AddReportIfLightFunction.
//
// The fixture is the same shape the material-side test uses (Tests/Material/
// TestMaterialLightFunctionAtlasCompatibility.cpp): a LightFunction-domain material whose graph
// runs TextureCoordinate -> AppendVector -> EmissiveColor. TextureCoordinate::Compile calls
// SetPotentiallyManipulateTexCoords unconditionally, which is what excludes the material from the
// atlas by construction, so the verdict under test is a KNOWN "not compatible" rather than
// whatever the host happens to produce.
//
// Counterfactual. Reverting the hook removes the `lightFunctionAtlas` block from the light
// component's describe entry, so "the light component's describe entry carries a lightFunctionAtlas
// block" fails. The no-noise half is pinned by the same actor before the material is assigned:
// emitting the block for every component, or for a light with no light function, fails "a light
// with no light function material carries no atlas block".

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Handlers/Material/MaterialLightFunctionAtlas.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/LightComponent.h"
#include "Editor.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
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
    // Uniquely prefixed so a Unity merge cannot collide these with the sibling test files'
    // helpers (the material-side fixture uses LFAtlas_* for the same shapes).
    UMaterial* LFAtlasDescribe_CreateTexCoordLightFunction(FAutomationTestBase& Test,
        FString& OutAssetPath)
    {
        OutAssetPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/LFAtlasDescribe_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

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

        Material->MaterialDomain = MD_LightFunction;

        UMaterialExpressionTextureCoordinate* TexCoord =
            NewObject<UMaterialExpressionTextureCoordinate>(Material);
        TexCoord->MaterialExpressionGuid = FGuid::NewGuid();

        UMaterialExpressionConstant* Zero = NewObject<UMaterialExpressionConstant>(Material);
        Zero->R = 0.0f;
        Zero->MaterialExpressionGuid = FGuid::NewGuid();

        // AppendVector promotes the float2 texcoord to the float3 EmissiveColor expects, so the
        // graph type-checks and the atlas exclusion is the only thing under test.
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

        Material->PostEditChange();
        Material->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }

    // Drive the production compile verb so the measured bit has a game-thread shader map to be
    // read off. PostEditChange alone only translates.
    void LFAtlasDescribe_Compile(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("material.authoring.compile_material"), Payload, Capture);
    }

    // Handler-path probes must NOT be RF_Transient: actor.* verbs resolve through
    // UEditorActorSubsystem::GetAllLevelActors, which skips transient actors outright, so a
    // transient probe is invisible to the verb under test. The caller's
    // FScopedEditorWorldActorGuard destroys this actor and restores the level's dirty flag.
    APointLight* LFAtlasDescribe_SpawnPointLight(UWorld* World, const FVector& Location)
    {
        return World
            ? World->SpawnActor<APointLight>(APointLight::StaticClass(), Location, FRotator::ZeroRotator)
            : nullptr;
    }

    // Returns the describe entry for one component of one actor, or null.
    TSharedPtr<FJsonObject> LFAtlasDescribe_ComponentEntry(FAutomationTestBase& Test,
        const FString& ActorPath, const FString& ComponentPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), ActorPath);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.describe handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.describe"), Payload, Capture));
        Test.TestTrue(TEXT("actor.describe succeeded"), Capture.bSuccess);

        return JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("components"), TEXT("path"), ComponentPath);
    }
}


// Regression: actor.describe must carry the MEASURED atlas verdict beside a light's light
// function material. Without it the verdict exists only on the material read, which nobody makes
// unless they already suspect the atlas.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDescribeLightFunctionAtlasTest,
    "PinWright.actor.describe.LightFunctionAtlasVerdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDescribeLightFunctionAtlasTest::RunTest(const FString& Parameters)
{
    if constexpr (!PinWright::LightFunctionAtlas::bSupportedOnThisEngine)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-has-no-atlas-override"),
            TEXT("UMaterial::bForceCompatibleWithLightFunctionAtlas and ")
            TEXT("FMaterial::MaterialIsLightFunctionAtlasCompatible_GameThread are both 5.5+."));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available to place the light probe in."));
        return true;
    }

    FString AssetPath;
    UMaterial* Material = LFAtlasDescribe_CreateTexCoordLightFunction(*this, AssetPath);
    if (!Material)
    {
        return true;
    }

    LFAtlasDescribe_Compile(AssetPath);

    {
        FScopedEditorWorldActorGuard Guard;

        APointLight* Light = LFAtlasDescribe_SpawnPointLight(World, FVector(0.0f, 0.0f, 300.0f));
        ULightComponent* LightComponent = Light ? Light->GetLightComponent() : nullptr;
        if (!LightComponent)
        {
            AddError(TEXT("Failed to spawn the point light probe."));
        }
        else
        {
            const FString ActorPath = Light->GetPathName();
            const FString ComponentPath = LightComponent->GetPathName();

            // No-noise half: a light with no light function must not carry the block, or every
            // light in every describe would grow a section answering a question nobody asked.
            const TSharedPtr<FJsonObject> Before =
                LFAtlasDescribe_ComponentEntry(*this, ActorPath, ComponentPath);
            if (TestTrue(TEXT("describe reports the light component"), Before.IsValid()))
            {
                TestFalse(TEXT("a light with no light function material carries no atlas block"),
                    Before->HasField(TEXT("lightFunctionAtlas")));
            }

            LightComponent->LightFunctionMaterial = Material;

            const TSharedPtr<FJsonObject> After =
                LFAtlasDescribe_ComponentEntry(*this, ActorPath, ComponentPath);
            const TSharedPtr<FJsonObject>* Block = nullptr;
            if (TestTrue(TEXT("describe still reports the light component"), After.IsValid()) &&
                TestTrue(TEXT("the light component's describe entry carries a lightFunctionAtlas block"),
                    After->TryGetObjectField(TEXT("lightFunctionAtlas"), Block) && Block != nullptr))
            {
                bool bForceCompatible = true;
                TestTrue(TEXT("forceCompatible is reported"),
                    (*Block)->TryGetBoolField(TEXT("forceCompatible"), bForceCompatible));
                TestFalse(TEXT("forceCompatible is false on a material that never set the override"),
                    bForceCompatible);

                bool bShaderMapReady = false;
                TestTrue(TEXT("shaderMapReady is reported"),
                    (*Block)->TryGetBoolField(TEXT("shaderMapReady"), bShaderMapReady));

                // The second gate travels with the first here too, so a reader cannot take
                // compatible:true for "this reaches the fog" on a host with the atlas turned off.
                TestTrue(TEXT("the block measures the atlas-generation cvar"),
                    (*Block)->HasField(TEXT("atlasGeneration")));

                if (bShaderMapReady)
                {
                    bool bCompatible = true;
                    TestTrue(TEXT("compatible is reported once there is a shader map to measure"),
                        (*Block)->TryGetBoolField(TEXT("compatible"), bCompatible));
                    TestFalse(
                        TEXT("a texcoord-manipulating light function measures as NOT atlas compatible"),
                        bCompatible);

                    FString Warning;
                    TestTrue(TEXT("the describe-side block carries the incompatibility warning"),
                        (*Block)->TryGetStringField(TEXT("warning"), Warning));
                    TestTrue(TEXT("the warning names the remedy verb"),
                        Warning.Contains(PinWright::LightFunctionAtlas::RemedyVerb()));
                }
                else
                {
                    // Honesty half: an unmeasured value must not be readable as a measured one.
                    TestFalse(TEXT("compatible is omitted when nothing was measured"),
                        (*Block)->HasField(TEXT("compatible")));
                    PinWrightTestSkip::SkipAssertions(*this, TEXT("no-material-shader-map"),
                        TEXT("compile_material produced no game-thread shader map on this host, so ")
                        TEXT("the measured atlas verdict could not be asserted from describe."));
                }
            }

            // Drop the reference before the guard destroys the actor and the asset is deleted.
            LightComponent->LightFunctionMaterial = nullptr;
        }
    }

    CleanupTestAsset(AssetPath);
    return true;
}
