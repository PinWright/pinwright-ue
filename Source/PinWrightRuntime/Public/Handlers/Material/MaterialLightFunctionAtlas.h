// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// MaterialLightFunctionAtlas - the measured "will this light function reach volumetric fog?"
// read-back, shared by material.authoring.get_material_info and
// material.authoring.set_light_function_atlas_compatible.
//
// WHY THIS EXISTS. A UMaterial with MaterialDomain == MD_LightFunction feeds two independent
// renderer paths, and only one of them takes every material:
//   * the classic deferred light-function pass, which modulates OPAQUE SURFACES and accepts
//     any light function material; and
//   * the light function atlas (r.LightFunctionAtlas), which is the only source volumetric fog
//     (r.VolumetricFog.UsesLightFunctionAtlas), translucency (r.Translucent.UsesLightFunctionAtlas)
//     and single-layer water (r.SingleLayerWater.UsesLightFunctionAtlas) sample a light function
//     through - and which silently drops materials the first path renders happily.
//
// The atlas rule is decided at TRANSLATION time, not when the material is attached to a light
// (Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp, 5.8 :1769):
//
//     MaterialCompilationOutput.bIsLightFunctionAtlasCompatible =
//         (!bUsesVertexPosition && !bUsesSceneDepth && !MaterialCompilationOutput.bNeedsSceneTextures
//          && !bPotentiallyManipulateTexCoords)
//         || Material->GetForceCompatibleWithLightFunctionAtlas();
//
// An atlas tile is rendered with no world position and no depth, and scaled texcoords stop
// aligning with the tile edges, so any panning/scaling UV chain or any world-position-projected
// pattern - that is, most ANIMATED light functions - is excluded by construction. The atlas
// builder then files the material under NonCompatibleLightFunctionMaterials
// (Runtime/Renderer/Private/LightFunctionAtlas.cpp, 5.8 :481-487) and renders nothing for it.
//
// Nothing game-side records that. The light still paints surfaces, so the material asset, the
// light component's LightFunctionMaterial / LightFunctionScale, and every r.*LightFunctionAtlas
// cvar all read healthy while the fog contribution the light function was attached FOR is simply
// absent. The one engine diagnostic is a show flag (ShowFlag.VisualizeLightFunctionAtlas), whose
// legend is drawn at a fixed pixel offset and runs off the edge of a square capture.
//
// The escape hatch is the per-material checkbox UMaterial::bForceCompatibleWithLightFunctionAtlas
// ("Compatible With Light Function Atlas", category LightFunctionMaterial;
// Runtime/Engine/Public/Materials/Material.h, 5.8 :927), which ORs into the rule above.
//
// MEASURED, NOT REQUESTED - the house pattern lighting.setup_volumetric_fog uses for the
// r.VolumetricFog veto. `forceCompatible` echoes the checkbox, i.e. what the author asked for.
// `compatible` is read off the COMPILED shader map via
// FMaterial::MaterialIsLightFunctionAtlasCompatible_GameThread() - the same bit
// UMaterialInterface::GetRelevance publishes as FMaterialRelevance::bIsLightFunctionAtlasCompatible
// (Runtime/Engine/Private/Materials/MaterialInterface.cpp, 5.8 :765) and the same bit the atlas
// builder tests - so it reports what the renderer will do rather than what the graph looks like.
// When there is no game-thread shader map to read the bit off, `compatible` is OMITTED rather than
// reported false: an unmeasured value must not be readable as a measured one.
//
// TWO GATES, REPORTED SEPARATELY. Material compatibility is necessary but not sufficient: the
// atlas also has to be generated at all (`r.LightFunctionAtlas`), and reporting only the first
// would let `compatible: true` be read as "this reaches the fog" on a host where the atlas is off.
// So the block also carries the measured `atlasGeneration` cvar and its own `atlasWarning`, kept
// as a separate key because the two verdicts are independent and fixing one must not hide the
// other. The per-consumer sampling switches (r.VolumetricFog.UsesLightFunctionAtlas and its
// translucency / single-layer-water siblings) are NOT measured here — they belong to the consumer,
// not to the material, and lighting.setup_volumetric_fog owns the fog side.
//
// ENGINE RANGE. Both the override flag and the game-thread accessor are 5.5+. On 5.3/5.4 the block
// is not emitted at all - 5.4 ships the atlas but neither the override nor a game-thread accessor
// for the bit, so there is nothing honest to report there.

#include "CoreMinimal.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "RHIFeatureLevel.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
#include "RHIShaderPlatform.h"
#endif

namespace PinWright::LightFunctionAtlas
{
    // Whether this engine carries both halves of the read: the override flag
    // UMaterial::bForceCompatibleWithLightFunctionAtlas and the game-thread accessor
    // FMaterial::MaterialIsLightFunctionAtlasCompatible_GameThread. Both arrive in 5.5.
    inline constexpr bool bSupportedOnThisEngine =
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        true;
#else
        false;
#endif

    // The verb that flips bForceCompatibleWithLightFunctionAtlas. The incompatible warning below
    // spells it inline (it is one compile-time-concatenated literal and cannot interpolate), so
    // this is the single value the regression test asserts the warning still contains — the drift
    // guard for a rename, not a formatting helper.
    inline const TCHAR* RemedyVerb()
    {
        return TEXT("material.authoring.set_light_function_atlas_compatible");
    }

    struct FCompatibility
    {
        // Whether there was a compiled game-thread shader map to read the measured bit off.
        // False makes bCompatible meaningless - callers must omit it, not report it as false.
        bool bShaderMapReady = false;
        // The measured bit: what the atlas builder will test on the render thread.
        bool bCompatible = false;
        // The authored override, UMaterial::bForceCompatibleWithLightFunctionAtlas.
        bool bForceCompatible = false;
    };

    inline FCompatibility Measure(const UMaterial* Material)
    {
        FCompatibility Out;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        if (!Material)
        {
            return Out;
        }

        Out.bForceCompatible = Material->bForceCompatibleWithLightFunctionAtlas != 0;

#if UE_VERSION_OLDER_THAN(5, 7, 0)
        const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel);
#else
        // UE 5.7 changed GetMaterialResource's selector from feature level to shader platform;
        // GMaxRHIShaderPlatform is the platform paired with GMaxRHIFeatureLevel. Same split as
        // MaterialCompileErrorCollector.h.
        const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
#endif
        // No shader map means the material has not been translated for this platform yet (freshly
        // created, or mid-recompile after an edit). The compatibility bit lives in that map's
        // MaterialCompilationOutput, so there is nothing to measure - say so rather than guess.
        if (!Resource || !Resource->GetGameThreadShaderMap())
        {
            return Out;
        }

        Out.bShaderMapReady = true;
        Out.bCompatible = Resource->MaterialIsLightFunctionAtlasCompatible_GameThread();
#else
        (void)Material;
#endif
        return Out;
    }

    // Build the `lightFunctionAtlas` response block. Always emits forceCompatible (requested) and
    // shaderMapReady (whether anything was measured); emits compatible (measured) only when there
    // was a shader map; emits warning only in the three states a caller cannot otherwise detect.
    // Returns null on an engine with no atlas override, where there is nothing honest to report.
    inline TSharedPtr<FJsonObject> MakeReport(const UMaterial* Material)
    {
        if constexpr (!bSupportedOnThisEngine)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        const FCompatibility Compat = Measure(Material);

        Block->SetBoolField(TEXT("forceCompatible"), Compat.bForceCompatible);
        Block->SetBoolField(TEXT("shaderMapReady"), Compat.bShaderMapReady);
        if (Compat.bShaderMapReady)
        {
            Block->SetBoolField(TEXT("compatible"), Compat.bCompatible);
        }

        // The SECOND gate, reported beside the first so `compatible: true` cannot be read as
        // "this reaches the fog". r.LightFunctionAtlas is what generates the atlas at all: with it
        // at 0 no light function reaches any atlas consumer no matter how compatible the material
        // is. Same measured-not-requested shape as lighting.setup_volumetric_fog's cvar block, and
        // the value is absent rather than 0 when the registry did not carry the cvar.
        int32 AtlasCVarValue = 0;
        bool bAtlasCVarFound = false;
        if (const IConsoleVariable* AtlasCVar =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightFunctionAtlas")))
        {
            AtlasCVarValue = AtlasCVar->GetInt();
            bAtlasCVarFound = true;
        }
        TSharedPtr<FJsonObject> AtlasCVarInfo = MakeShared<FJsonObject>();
        AtlasCVarInfo->SetStringField(TEXT("cvar"), TEXT("r.LightFunctionAtlas"));
        AtlasCVarInfo->SetBoolField(TEXT("found"), bAtlasCVarFound);
        if (bAtlasCVarFound)
        {
            AtlasCVarInfo->SetNumberField(TEXT("value"), AtlasCVarValue);
        }
        Block->SetObjectField(TEXT("atlasGeneration"), AtlasCVarInfo);

        if (bAtlasCVarFound && AtlasCVarValue == 0)
        {
            // Deliberately its own key, not folded into `warning`: the material verdict and the
            // renderer verdict are independent, and a caller who fixes one must still see the
            // other. This verb does not change the cvar.
            Block->SetStringField(TEXT("atlasWarning"),
                TEXT("r.LightFunctionAtlas is 0, so the light function atlas is not generated at all ")
                TEXT("and NO light function reaches volumetric fog, translucency or single-layer ")
                TEXT("water on this host regardless of this material's compatibility. Turn it on for ")
                TEXT("the session with system.console_command \"r.LightFunctionAtlas 1\", or pin it ")
                TEXT("with r.LightFunctionAtlas=1 under [SystemSettings] in the project's ")
                TEXT("DefaultEngine.ini. The per-consumer sampling cvars are separate switches: ")
                TEXT("r.VolumetricFog.UsesLightFunctionAtlas, r.Translucent.UsesLightFunctionAtlas ")
                TEXT("and r.SingleLayerWater.UsesLightFunctionAtlas."));
        }

        if (!Compat.bShaderMapReady)
        {
            Block->SetStringField(TEXT("warning"),
                TEXT("Light function atlas compatibility was NOT measured: this material has no ")
                TEXT("compiled game-thread shader map on the current shader platform, so ")
                TEXT("`compatible` is omitted rather than reported false. Run ")
                TEXT("material.authoring.compile_material and read this back."));
        }
        else if (Compat.bCompatible)
        {
            // Compatible: nothing to warn about on the MATERIAL side. Whether a given consumer
            // then samples the atlas is its own switch (r.VolumetricFog.UsesLightFunctionAtlas,
            // r.Translucent.UsesLightFunctionAtlas, r.SingleLayerWater.UsesLightFunctionAtlas) and
            // is not measured here; atlasGeneration above covers only whether an atlas exists.
        }
        else if (Compat.bForceCompatible)
        {
            // The override is set but the compiled map still says no, which can only mean the map
            // predates the write. This is its own state because the remedy differs: recompile,
            // do not set the flag again.
            Block->SetStringField(TEXT("warning"),
                TEXT("bForceCompatibleWithLightFunctionAtlas is set, but the compiled shader map ")
                TEXT("still reports this material as NOT light-function-atlas compatible, so the ")
                TEXT("shader map predates the write and the renderer is still dropping this light ")
                TEXT("function from the atlas. Run material.authoring.compile_material and read ")
                TEXT("this back; the override only takes effect at translation time."));
        }
        else
        {
            Block->SetStringField(TEXT("warning"),
                TEXT("This material is a light function, but its compiled shader map reports it is ")
                TEXT("NOT compatible with the light function atlas. It will keep modulating opaque ")
                TEXT("surfaces through the deferred light-function pass - which is why every other ")
                TEXT("read-back looks healthy - while contributing NOTHING to volumetric fog ")
                TEXT("(r.VolumetricFog.UsesLightFunctionAtlas), translucency ")
                TEXT("(r.Translucent.UsesLightFunctionAtlas) or single-layer water ")
                TEXT("(r.SingleLayerWater.UsesLightFunctionAtlas): the atlas is the only path those ")
                TEXT("sample a light function through. The translator excludes a material by ")
                TEXT("construction when the graph manipulates texture coordinates - which means any ")
                TEXT("TextureCoordinate node at all (whatever its tiling), or a texture sample ")
                TEXT("parameter with its Coordinates input wired, so any Panner / Rotator / scaled ")
                TEXT("UV chain reaches it - or when the graph reads world position, scene depth or ")
                TEXT("scene textures. An atlas tile is rendered without world position or depth, and ")
                TEXT("scaled texcoords no longer align with the tile edges, so most ANIMATED light ")
                TEXT("functions land here. ")
                TEXT("Override it with material.authoring.set_light_function_atlas_compatible ")
                TEXT("(UMaterial::bForceCompatibleWithLightFunctionAtlas), which also forces every ")
                TEXT("deferred light using this material onto the batched lighting path with no ")
                TEXT("screen-space shadow mask."));
        }

        return Block;
    }

    // get_material_info's route: emit the block only for a material that is actually a light
    // function. A Surface/UI/PostProcess material never reaches the atlas, so reporting atlas
    // compatibility on one would be noise on every material read in the project.
    inline void AddReportIfLightFunction(const TSharedPtr<FJsonObject>& Out, const UMaterial* Material)
    {
        if (!Out.IsValid() || !Material || Material->MaterialDomain != MD_LightFunction)
        {
            return;
        }
        if (const TSharedPtr<FJsonObject> Block = MakeReport(Material))
        {
            Out->SetObjectField(TEXT("lightFunctionAtlas"), Block);
        }
    }
} // namespace PinWright::LightFunctionAtlas
