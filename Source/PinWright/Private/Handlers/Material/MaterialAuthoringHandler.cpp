// Copyright (c) 2026 Alexander Penkin. MIT License.

// MaterialAuthoringHandler.cpp - Migrated from PinWright_MaterialAuthoringHandlers.cpp
// Advanced material creation and shader authoring capabilities.
// Implements: create_material, add expressions, connect nodes, material instances,
// material functions, specialized materials (landscape, decal, post-process).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Compat/EngineVersionCompat.h"
#include "Compat/JsonKeyCompat.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/JsonBuilders.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialFunctionMaterialLayerFactory.h"
#include "Factories/MaterialFunctionMaterialLayerBlendFactory.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "Material/MaterialExpressionFactory.h"
#include "Materials/Material.h"
// MaterialDomain.h was introduced in UE 5.1 - in UE 5.0 EMaterialDomain is in MaterialShared.h
#include "MaterialDomain.h"
#include "Materials/MaterialExpression.h"
// FMaterialParameterMetadata / FMaterialParameterValue / EMaterialParameterType — the
// uniform typed-default + group/sortPriority struct GetParameterValue() returns.
// These moved from the top-level MaterialTypes.h into Materials/MaterialParameters.h in
// UE 5.7; on 5.6 and earlier they live in MaterialTypes.h, which has no MaterialParameters.h.
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionIf.h"
// UMaterialExpressionLandscapeLayerBlend + FLayerBlendInput + ELandscapeLayerBlendType.
// UCLASS(MinimalAPI) in the Landscape module, but MinimalAPI still exports
// Z_Construct_UClass_UMaterialExpressionLandscapeLayerBlend and the constructor as
// LANDSCAPE_API, so StaticClass()/Cast<> link directly; Landscape is a public dependency of
// this module (PinWright.Build.cs:24). No reflection detour is needed here.
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
// UE::Landscape::RetrieveTargetLayerNamesFromMaterial — the accessor
// ALandscapeProxy::RetrieveTargetLayerNamesFromMaterials harvests target layers through.
#include "LandscapeUtils.h"
// ALandscapeProxy::GetLayersFromMaterial — the pre-5.8 spelling of the same read.
#include "LandscapeProxy.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionReflectionVectorWS.h"
// MaterialExpressionRotator is not available in UE 5.0
#include "Materials/MaterialExpressionRotator.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionCrossProduct.h"
#include "Materials/MaterialExpressionDesaturation.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
// Default-template seeding for create_material_layer / create_material_layer_blend: the
// MaterialLayerOutput node + SetMaterialAttributes (layer) / BlendMaterialAttributes (blend) that
// UE's material-function editor seeds on-open, wired via MaterialEditingLibrary — the engine factory
// itself seeds nothing.
#include "Materials/MaterialExpressionMaterialLayerOutput.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionBlendMaterialAttributes.h"
#include "MaterialEditingLibrary.h"
#include "Handlers/Asset/MaterialInstanceDumpBuilder.h"
#include "Handlers/Material/MaterialLayerStackHelpers.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "StaticParameterSet.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/SavePackage.h"
#include "EditorAssetLibrary.h"

// Landscape layer info (for add_landscape_layer)
#include "LandscapeLayerInfoObject.h"

// ============================================================================
// Helper functions (file-local)
// ============================================================================

#include "Handlers/Material/MaterialFinders.h"
#include "Handlers/Material/MaterialCompileErrorCollector.h"
// Persistent FunctionInput/FunctionOutput identity: the save-path guard, and the read that
// reports a call node whose cached pin GUID will not re-link on the consumer's next load.
#include "Handlers/Material/MaterialFunctionIdentity.h"
// AddMaterialVerification: the asset-verification block every write verb here ends on, plus the
// shader-compile verdict for the material it wrote (E-material-verbs-have-no-shader-compile-signal).
#include "Handlers/Material/MaterialShaderState.h"
// The EMaterialUsage set: the one observable that separates a valid material assignment from one
// the renderer substitutes with the Default Material, and which no reader here used to emit.
#include "Handlers/Material/MaterialUsageFlags.h"
#include "Handlers/Material/MaterialLandscapeConsumers.h"
#include "Handlers/Material/MaterialHandlerUtils.h"
#include "Handlers/Material/MaterialCreatePathParamUtils.h"
// add_landscape_layer composes its package path by hand rather than through
// MaterialCreatePathParamUtils, so it needs the shared composer directly.
#include "Handlers/PackagePathCompose.h"
#include "Handlers/Material/MaterialInstanceOverrides.h"
#include "Handlers/Material/MainInputBindings.h"
// Measured light-function-atlas compatibility: the read get_material_info emits for a
// MD_LightFunction material and the read-back set_light_function_atlas_compatible returns.
#include "Handlers/Material/MaterialLightFunctionAtlas.h"
#include "MGIR/MGIRExpressionUtils.h"
#include "MGIR/MGIRLayoutEngine.h"
// FMaterialUpdateContext — compile_material scopes the master's PostEditChange inside one
// so dependent material instances recache. MaterialCompileErrorCollector.h already pulls
// MaterialShared.h in, but the -StrictIncludes -DisableUnity packaging build does not
// inherit includes from sibling headers by accident; name it here.
#include "MaterialShared.h"
#include "Utils/DerivedStateReport.h"

static bool TryGetMaterialParameterName(UMaterialExpression* Expr, FString& OutParameterName)
{
    if (!Expr)
    {
        return false;
    }

    if (UMaterialExpressionParameter* ParamExpr = Cast<UMaterialExpressionParameter>(Expr))
    {
        OutParameterName = ParamExpr->ParameterName.ToString();
        return !OutParameterName.IsEmpty();
    }

    if (UMaterialExpressionTextureSampleParameter2D* TexParamExpr = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
    {
        OutParameterName = TexParamExpr->ParameterName.ToString();
        return !OutParameterName.IsEmpty();
    }

    return false;
}

// Augments a get_material_info parameter entry with the metadata + default value the
// caller wrote at create time (add_scalar_parameter sets DefaultValue/Group/SortPriority,
// etc.) so the author-then-verify round-trip can read back what it set. Drives both the
// group/sortPriority and the typed defaultValue off the engine's single uniform virtual,
// UMaterialExpression::GetParameterValue(FMaterialParameterMetadata&): the returned struct
// carries Group, SortPriority and a typed FMaterialParameterValue for EVERY parameter
// expression type in one call, including UMaterialExpressionTextureSampleParameter (which
// does NOT derive from UMaterialExpressionParameter but supplies its own Group/SortPriority).
// Dispatching on Value.Type yields scalar->number, vector/doubleVector->{r,g,b,a},
// staticSwitch->bool, and the texture/asset families->asset path, mirroring the typed default
// semantics of the instance sibling get_material_instance_info and the create side. Group is
// emitted only when explicitly set (matching the instance read-back); sortPriority always.
static void AddMaterialParameterDetails(const TSharedPtr<FJsonObject>& ParamObj, UMaterialExpression* Expr)
{
    if (!ParamObj.IsValid() || !Expr)
    {
        return;
    }

#if WITH_EDITOR
    FMaterialParameterMetadata Meta;
    if (!Expr->GetParameterValue(Meta))
    {
        return;
    }

    if (!Meta.Group.IsNone())
    {
        ParamObj->SetStringField(TEXT("group"), Meta.Group.ToString());
    }
    ParamObj->SetNumberField(TEXT("sortPriority"), Meta.SortPriority);

    switch (Meta.Value.Type)
    {
    case EMaterialParameterType::Scalar:
        ParamObj->SetNumberField(TEXT("defaultValue"), Meta.Value.AsScalar());
        break;
    case EMaterialParameterType::Vector:
        ParamObj->SetObjectField(TEXT("defaultValue"),
            JsonBuilders::BuildLinearColorJson(Meta.Value.AsLinearColor()));
        break;
    case EMaterialParameterType::DoubleVector:
    {
        const FVector4d V = Meta.Value.AsVector4d();
        ParamObj->SetObjectField(TEXT("defaultValue"),
            JsonBuilders::BuildLinearColorJson(FLinearColor(V.X, V.Y, V.Z, V.W)));
        break;
    }
    case EMaterialParameterType::StaticSwitch:
        ParamObj->SetBoolField(TEXT("defaultValue"), Meta.Value.AsStaticSwitch());
        break;
    // Asset-valued parameters: serialize the referenced asset's path. AsTextureObject()
    // (the same helper the engine uses) resolves Texture/RuntimeVirtualTexture/
    // SparseVolumeTexture to a UObject* without needing those types complete here.
    case EMaterialParameterType::Texture:
    case EMaterialParameterType::RuntimeVirtualTexture:
    case EMaterialParameterType::SparseVolumeTexture:
        ParamObj->SetStringField(TEXT("defaultValue"),
            JsonBuilders::GetObjectPathSafe(Meta.Value.AsTextureObject()));
        break;
    default:
        // Font, TextureCollection, ParameterCollection, StaticComponentMask, and any future
        // type: name/type/nodeId plus group/sortPriority are still emitted above; the typed
        // defaultValue is omitted (no single asset-path/scalar form that mirrors the create
        // side, and the collection asset types are only forward-declared in this TU).
        break;
    }
#endif // WITH_EDITOR
}

static bool TryParseCustomExpressionInputNames(const TSharedPtr<FJsonObject>& Payload, TArray<FName>& OutInputNames, FHandlerContext& Ctx)
{
    if (!Payload.IsValid())
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
    if (!Payload->TryGetArrayField(TEXT("inputs"), InputsArray) || !InputsArray)
    {
        return true;
    }

    for (int32 Index = 0; Index < InputsArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& InputValue = (*InputsArray)[Index];
        FString InputName;

        if (!InputValue.IsValid())
        {
            Ctx.SendError(TEXT("INVALID_INPUTS"),
                FString::Printf(TEXT("Custom expression input at index %d is null."), Index));
            return false;
        }

        if (InputValue->Type == EJson::String)
        {
            InputName = InputValue->AsString();
        }
        else if (InputValue->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> InputObject = InputValue->AsObject();
            if (!InputObject.IsValid()
                || (!InputObject->TryGetStringField(TEXT("name"), InputName)
                    && !InputObject->TryGetStringField(TEXT("inputName"), InputName)))
            {
                Ctx.SendError(TEXT("INVALID_INPUTS"),
                    FString::Printf(TEXT("Custom expression input at index %d must define 'name' or 'inputName'."), Index));
                return false;
            }
        }
        else
        {
            Ctx.SendError(TEXT("INVALID_INPUTS"),
                FString::Printf(TEXT("Custom expression input at index %d must be a string or object."), Index));
            return false;
        }

        InputName = InputName.TrimStartAndEnd();
        if (InputName.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_INPUTS"),
                FString::Printf(TEXT("Custom expression input at index %d has an empty name."), Index));
            return false;
        }

        const FName InputFName(*InputName);
        if (OutInputNames.Contains(InputFName))
        {
            Ctx.SendError(TEXT("INVALID_INPUTS"),
                FString::Printf(TEXT("Custom expression input '%s' is duplicated."), *InputName));
            return false;
        }

        OutInputNames.Add(InputFName);
    }

    return true;
}

// Save a material asset to disk for real and report whether the .uasset landed on
// disk. The former MarkPackageDirty()-only body (B-material-authoring-save-no-disk-write)
// reported saved/existsAfter:true while writing nothing, so a save:true create/compile
// lived only in memory + the asset registry and vanished on a cold editor restart /
// git reset. Materials / MaterialFunctions / MaterialInstanceConstants are
// non-Blueprint/non-SCS, so the bulkdata-corruption vector that forced the deferred
// mark-dirty on Blueprint edits (B-bp-saved-state-corruption-mcp-edits) does not apply.
// Route through the shared forced-save + on-disk IFileManager::FileSize probe (gated by
// ShouldTreatAssetSaveAsSuccess) — the same path the audio / niagara / metasound create
// fixes adopted. The old "Do NOT call SaveAsset - triggers modal dialogs that crash
// D3D12RHI" warning was about the interactive SaveAsset modal; the headless
// SaveLoadedAsset path used here raises no dialog. Returns true only when the .uasset
// is actually on disk.
//
// The single real body lives in SaveMaterialAssetToDisk(UObject*); the three typed
// wrappers below are thin forwards kept only for call-site type-safety, so the save
// path (bForce + disk-presence gating) is defined once instead of copied per asset
// class. Null-safe: a null asset reports false (SaveAssetToDiskReportingPresence also
// null-checks its arg), so the wrappers need no separate guard.
static bool SaveMaterialAssetToDisk(UObject* Asset)
{
    return Asset && SaveAssetToDiskReportingPresence(Asset, /*bForce=*/true);
}

static bool SaveMaterialAsset(UMaterial* Material) { return SaveMaterialAssetToDisk(Material); }
// Never write a function graph whose FunctionInput / FunctionOutput pins carry no persistent Id:
// callers link to those pins by GUID only, and an unset GUID is re-minted by PostLoad on every
// load, so every already-saved caller of the function silently loses its wires. Repair here
// rather than at each authoring site so a graph built by any route is safe at the one point where
// it becomes durable. See Handlers/Material/MaterialFunctionIdentity.h.
static bool SaveMaterialFunctionAsset(UMaterialFunction* Function)
{
    PinWright::MaterialFunctionIdentity::EnsurePersistentIds(Function);
    return SaveMaterialAssetToDisk(Function);
}
static bool SaveMaterialInstanceAsset(UMaterialInstanceConstant* Instance) { return SaveMaterialAssetToDisk(Instance); }

// ApplyMaterialInstanceParameterOverrides — the shared override-apply path for all
// material-instance creator/setter verbs — now lives in the header
// Handlers/Material/MaterialInstanceOverrides.h (included at the top of this file) so
// both verbs share the same implementation.

// Dirty the host asset (UMaterial or UMaterialFunction) and respond with the new node's GUID.
// Asset-level (PostEditChange/MarkPackageDirty) so it serves both container types.
static bool FinishCreatedExpression(UObject* Asset, UMaterialExpression* Expression, FHandlerContext& Ctx)
{
    Asset->PostEditChange();
    Asset->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), Expression->MaterialExpressionGuid.ToString());
    Ctx.SendSuccess(Result);
    return true;
}

static bool CreateExpressionWithFactoryAndRespond(
    UMaterial* Material,
    UClass* ExpressionClass,
    const TSharedPtr<FJsonObject>& Properties,
    float ExprX,
    float ExprY,
    FHandlerContext& Ctx)
{
    FCreateResult CreateResult = FMaterialExpressionFactory::Create(
        Material, ExpressionClass, Properties, FVector2D(ExprX, ExprY));
    if (!CreateResult.IsSuccess())
    {
        Ctx.SendError(CreateResult.ErrorCode, CreateResult.ErrorMessage);
        return true;
    }

    return FinishCreatedExpression(Material, CreateResult.Expression, Ctx);
}

// Mutation-target overload: dispatches FMaterialExpressionFactory::Create to the UMaterial or
// UMaterialFunction container so the typed node-add helpers can author either asset's graph.
static bool CreateExpressionWithFactoryAndRespond(
    const PinWright::Material::FMaterialMutationTarget& Target,
    UClass* ExpressionClass,
    const TSharedPtr<FJsonObject>& Properties,
    float ExprX,
    float ExprY,
    FHandlerContext& Ctx)
{
    FCreateResult CreateResult = Target.CreateExpression(ExpressionClass, Properties, FVector2D(ExprX, ExprY));
    if (!CreateResult.IsSuccess())
    {
        Ctx.SendError(CreateResult.ErrorCode, CreateResult.ErrorMessage);
        return true;
    }

    return FinishCreatedExpression(Target.AssetObject(), CreateResult.Expression, Ctx);
}

static EMaterialSamplerType SamplerTypeFromName(const FString& SamplerType)
{
    if (SamplerType == TEXT("LinearColor"))
        return SAMPLERTYPE_LinearColor;
    if (SamplerType == TEXT("Normal"))
        return SAMPLERTYPE_Normal;
    if (SamplerType == TEXT("Masks"))
        return SAMPLERTYPE_Masks;
    if (SamplerType == TEXT("Alpha"))
        return SAMPLERTYPE_Alpha;
    return SAMPLERTYPE_Color;
}

// ---------------------------------------------------------------------------
// Blend-mode / shading-model wire vocabulary.
//
// One table each, so the UMaterial setters (set_blend_mode / set_shading_model), the readback
// (get_material_info) and the per-instance override verb
// (set_material_instance_base_property_overrides) cannot grow three spellings of the same enum.
// Both enums are read off UE 5.8's Engine/Classes/Engine/EngineTypes.h:
//   EBlendMode           (line 245): Opaque, Masked, Translucent, Additive, Modulate,
//                        AlphaComposite, AlphaHoldout, TranslucentColoredTransmittance.
//                        BLEND_TranslucentGreyTransmittance and BLEND_ColoredTransmittanceOnly
//                        are ALIASES (= BLEND_Translucent / = BLEND_Modulate), so they parse but
//                        can never be a distinct switch case or a distinct name.
//   EMaterialShadingModel (line 707): adds SingleLayerWater over the set the old if-chain took;
//                        MSM_Strata/MSM_FromMaterialExpression stay unexposed (hidden / graph-driven).
// ---------------------------------------------------------------------------
struct FMaterialEnumName
{
    const TCHAR* Name;
    int32 Value;
};

// Canonical names first; trailing entries may be aliases onto an earlier value.
static const FMaterialEnumName* GetBlendModeNames(int32& OutNum)
{
    static const FMaterialEnumName Table[] = {
        { TEXT("Opaque"),                          BLEND_Opaque },
        { TEXT("Masked"),                          BLEND_Masked },
        { TEXT("Translucent"),                     BLEND_Translucent },
        { TEXT("Additive"),                        BLEND_Additive },
        { TEXT("Modulate"),                        BLEND_Modulate },
        { TEXT("AlphaComposite"),                  BLEND_AlphaComposite },
        { TEXT("AlphaHoldout"),                    BLEND_AlphaHoldout },
        { TEXT("TranslucentColoredTransmittance"), BLEND_TranslucentColoredTransmittance },
        // Substrate spellings of two legacy modes; same underlying values.
        { TEXT("TranslucentGreyTransmittance"),    BLEND_TranslucentGreyTransmittance },
        { TEXT("ColoredTransmittanceOnly"),        BLEND_ColoredTransmittanceOnly },
    };
    OutNum = UE_ARRAY_COUNT(Table);
    return Table;
}

static const FMaterialEnumName* GetShadingModelNames(int32& OutNum)
{
    static const FMaterialEnumName Table[] = {
        { TEXT("Unlit"),             MSM_Unlit },
        { TEXT("DefaultLit"),        MSM_DefaultLit },
        { TEXT("Subsurface"),        MSM_Subsurface },
        { TEXT("PreintegratedSkin"), MSM_PreintegratedSkin },
        { TEXT("ClearCoat"),         MSM_ClearCoat },
        { TEXT("SubsurfaceProfile"), MSM_SubsurfaceProfile },
        { TEXT("TwoSidedFoliage"),   MSM_TwoSidedFoliage },
        { TEXT("Hair"),              MSM_Hair },
        { TEXT("Cloth"),             MSM_Cloth },
        { TEXT("Eye"),               MSM_Eye },
        { TEXT("SingleLayerWater"),  MSM_SingleLayerWater },
        { TEXT("ThinTranslucent"),   MSM_ThinTranslucent },
    };
    OutNum = UE_ARRAY_COUNT(Table);
    return Table;
}

static bool TryParseMaterialEnum(const FMaterialEnumName* Table, int32 Num, const FString& Needle, int32& OutValue)
{
    const FString Trimmed = Needle.TrimStartAndEnd();
    for (int32 i = 0; i < Num; ++i)
    {
        if (Trimmed.Equals(Table[i].Name, ESearchCase::IgnoreCase))
        {
            OutValue = Table[i].Value;
            return true;
        }
    }
    return false;
}

// First name matching Value — canonical, because aliases are listed after the names they shadow.
static FString MaterialEnumToString(const FMaterialEnumName* Table, int32 Num, int32 Value)
{
    for (int32 i = 0; i < Num; ++i)
    {
        if (Table[i].Value == Value)
        {
            return Table[i].Name;
        }
    }
    return TEXT("Unknown");
}

static FString JoinMaterialEnumNames(const FMaterialEnumName* Table, int32 Num)
{
    TArray<FString> Names;
    Names.Reserve(Num);
    for (int32 i = 0; i < Num; ++i)
    {
        Names.Add(Table[i].Name);
    }
    return FString::Join(Names, TEXT(", "));
}

static bool TryParseBlendMode(const FString& BlendMode, EBlendMode& OutMode)
{
    int32 Num = 0;
    const FMaterialEnumName* Table = GetBlendModeNames(Num);
    int32 Value = 0;
    if (!TryParseMaterialEnum(Table, Num, BlendMode, Value)) return false;
    OutMode = static_cast<EBlendMode>(Value);
    return true;
}

static FString MaterialBlendModeToString(EBlendMode Mode)
{
    int32 Num = 0;
    const FMaterialEnumName* Table = GetBlendModeNames(Num);
    return MaterialEnumToString(Table, Num, static_cast<int32>(Mode));
}

static FString GetBlendModeNameList()
{
    int32 Num = 0;
    const FMaterialEnumName* Table = GetBlendModeNames(Num);
    return JoinMaterialEnumNames(Table, Num);
}

static bool TryParseShadingModel(const FString& ShadingModel, EMaterialShadingModel& OutModel)
{
    int32 Num = 0;
    const FMaterialEnumName* Table = GetShadingModelNames(Num);
    int32 Value = 0;
    if (!TryParseMaterialEnum(Table, Num, ShadingModel, Value)) return false;
    OutModel = static_cast<EMaterialShadingModel>(Value);
    return true;
}

static FString GetShadingModelNameList()
{
    int32 Num = 0;
    const FMaterialEnumName* Table = GetShadingModelNames(Num);
    return JoinMaterialEnumNames(Table, Num);
}

// Shared helper: parse blend mode string and apply to a UMaterial. Returns false on an
// unrecognised name WITHOUT touching the material — callers must report that, not ignore it.
static bool ApplyBlendMode(UMaterial* Material, const FString& BlendMode)
{
    EBlendMode Parsed = BLEND_Opaque;
    if (!TryParseBlendMode(BlendMode, Parsed)) return false;
    Material->BlendMode = Parsed;
    return true;
}

// Shared helper: parse shading model string and apply
static bool ApplyShadingModel(UMaterial* Material, const FString& ShadingModel)
{
    EMaterialShadingModel Parsed = MSM_DefaultLit;
    if (!TryParseShadingModel(ShadingModel, Parsed)) return false;
    Material->SetShadingModel(Parsed);
    return true;
}

// Shared helper: parse material domain string and apply
static bool ApplyMaterialDomain(UMaterial* Material, const FString& Domain)
{
    if (Domain == TEXT("Surface"))
        Material->MaterialDomain = EMaterialDomain::MD_Surface;
    else if (Domain == TEXT("DeferredDecal"))
        Material->MaterialDomain = EMaterialDomain::MD_DeferredDecal;
    else if (Domain == TEXT("LightFunction"))
        Material->MaterialDomain = EMaterialDomain::MD_LightFunction;
    else if (Domain == TEXT("Volume"))
        Material->MaterialDomain = EMaterialDomain::MD_Volume;
    else if (Domain == TEXT("PostProcess"))
        Material->MaterialDomain = EMaterialDomain::MD_PostProcess;
    else if (Domain == TEXT("UI"))
        Material->MaterialDomain = EMaterialDomain::MD_UI;
    else
        return false;
    return true;
}

// Binds common local names around the shared guarded material mutation loader.
#define LOAD_MATERIAL_OR_RETURN()                                                  \
    FString AssetPath;                                                             \
    UMaterial* Material = PinWright::Material::LoadMaterialForMutationOrReportError( \
        Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath);             \
    if (!Material) return true

// Extract the mandatory x/y node-position pair into two named float locals.
// Pairs with LOAD_MATERIAL_OR_RETURN at every node-creating handler so the
// 5-line RequireNumber + cast block isn't duplicated at every site.
#define REQUIRE_NODE_POSITION_OR_RETURN(VarX, VarY)                                \
    double VarX##D = 0.0, VarY##D = 0.0;                                           \
    if (!Ctx.RequireNumber(TEXT("x"), VarX##D)) return true;                       \
    if (!Ctx.RequireNumber(TEXT("y"), VarY##D)) return true;                       \
    float VarX = static_cast<float>(VarX##D);                                      \
    float VarY = static_cast<float>(VarY##D)

// After creating an expression, finalize it: set the owning-material back-pointer and position,
// add to material, post-edit, send response. The back-pointer is what lets a LATER edit of the
// node reach the material — see FMaterialExpressionFactory::Create.
#define FINALIZE_EXPR_AND_RESPOND(Expr, Message)                                   \
    (Expr)->Material = Material;                                                   \
    (Expr)->MaterialExpressionEditorX = (int32)ExprX;                              \
    (Expr)->MaterialExpressionEditorY = (int32)ExprY;                              \
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Expr);                                       \
    Material->PostEditChange();                                                    \
    Material->MarkPackageDirty();                                                  \
    {                                                                              \
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();                     \
        R->SetStringField(TEXT("nodeId"), (Expr)->MaterialExpressionGuid.ToString()); \
        Ctx.SendSuccess(R);                                                        \
    }                                                                              \
    return true


// ============================================================================
// 8.1 Material Creation Actions
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.create_material", "material.authoring",
    "Create a new material asset with optional domain, blend mode, and shading model. Idempotent: an existing UMaterial at the path is UPDATED IN PLACE (only the properties you pass are applied; the expression graph is left alone) and returned with existing:true, mode:\"updated_in_place\", so its material instances and mesh slots keep resolving. Pass overwrite:true for the old wipe-and-recreate behaviour, which errors ASSET_IN_USE when other packages reference it; a non-UMaterial asset at the path errors ASSET_ALREADY_EXISTS.",
    RPC_PARAMS(
        MaterialCreatePathParamUtils::MaterialCreateNameParamReq(TEXT("Material name (leaf). Or pass one combined 'assetPath' to set name + folder together.")),
        MaterialCreatePathParamUtils::MaterialCreateFolderParamOpt(TEXT("Destination folder (default /Game/Materials)")),
        RPC_PARAM_OPT("materialDomain", "string", "Surface|DeferredDecal|LightFunction|Volume|PostProcess|UI"),
        RPC_PARAM_OPT("blendMode", "string", "Opaque|Masked|Translucent|Additive|Modulate|AlphaComposite|AlphaHoldout"),
        RPC_PARAM_OPT("shadingModel", "string", "Unlit|DefaultLit|Subsurface|SubsurfaceProfile|PreintegratedSkin|ClearCoat|Hair|Cloth|Eye|TwoSidedFoliage|ThinTranslucent"),
        RPC_PARAM_OPT("twoSided", "boolean", "Enable two-sided rendering"),
        RPC_PARAM_OPT("save", "boolean", "Save asset after creation (default true)"),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing material (discarding its expression graph) instead of updating it in place. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
    FString Name;
    FString Path;
    if (!MaterialCreatePathParamUtils::ResolveCreateNameAndFolder(Ctx, TEXT("/Game/Materials"), Name, Path)) return true;

    // Validate and sanitize the asset name
    FString OriginalName = Name;
    FString SanitizedName = SanitizeAssetName(Name);

    FString NormalizedOriginal = OriginalName.Replace(TEXT("_"), TEXT(""));
    FString NormalizedSanitized = SanitizedName.Replace(TEXT("_"), TEXT(""));
    if (NormalizedSanitized != NormalizedOriginal)
    {
        Ctx.SendError(TEXT("INVALID_NAME"),
            FString::Printf(TEXT("Invalid material name '%s': contains characters that cannot be used in asset names. Valid name would be: '%s'"),
                *OriginalName, *SanitizedName));
        return true;
    }
    Name = SanitizedName;

    // Validate path
    FString ValidatedPath;
    FString PathError;
    if (!ValidateAssetCreationPath(Path, Name, ValidatedPath, PathError))
    {
        Ctx.SendError(TEXT("INVALID_PATH"), PathError);
        return true;
    }

    if (ValidatedPath.Contains(TEXT(":")))
    {
        Ctx.SendError(TEXT("INVALID_PATH"),
            FString::Printf(TEXT("Invalid path '%s': absolute Windows paths are not allowed"), *ValidatedPath));
        return true;
    }

    FText MountReason;
    if (!FPackageName::IsValidLongPackageName(ValidatedPath, true, &MountReason))
    {
        Ctx.SendError(TEXT("INVALID_PATH"),
            FString::Printf(TEXT("Invalid package path '%s': %s"), *ValidatedPath, *MountReason.ToString()));
        return true;
    }

    // CreatePackage + FactoryCreateNew raises no prompt, but it SILENTLY replaces the
    // UMaterial already living in that package — and every material.graph.* node a
    // previous run authored with it. Resolve first so a re-run updates in place, a
    // foreign asset class is refused, and overwrite:true is referencer-checked without
    // a dialog.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        ValidatedPath, Name, UMaterial::StaticClass(), Ctx.GetBool(TEXT("overwrite"), false));
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }

    UMaterial* NewMaterial = nullptr;
    if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace)
    {
        // Reuse the existing material and apply only what the caller supplied. The
        // expression graph is deliberately left alone: authoring scripts rebuild it
        // themselves through material.graph.*, and every material instance / mesh slot
        // pointing at this asset keeps resolving.
        NewMaterial = CastChecked<UMaterial>(Resolution.Existing);
    }
    else
    {
        UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
        UPackage* Package = CreatePackage(*ValidatedPath);
        if (!Package)
        {
            Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package."));
            return true;
        }

        NewMaterial = Cast<UMaterial>(
            Factory->FactoryCreateNew(UMaterial::StaticClass(), Package,
                FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
        if (!NewMaterial)
        {
            Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create material."));
            return true;
        }
    }

    // Set optional properties. Each is "empty string => skip", which is what makes the
    // update-in-place branch apply exactly the subset the caller passed.
    FString MaterialDomain = Ctx.GetString(TEXT("materialDomain"));
    if (!MaterialDomain.IsEmpty()) ApplyMaterialDomain(NewMaterial, MaterialDomain);

    FString BlendMode = Ctx.GetString(TEXT("blendMode"));
    if (!BlendMode.IsEmpty()) ApplyBlendMode(NewMaterial, BlendMode);

    FString ShadingModel = Ctx.GetString(TEXT("shadingModel"));
    if (!ShadingModel.IsEmpty()) ApplyShadingModel(NewMaterial, ShadingModel);

    const auto& Payload = Ctx.GetRawPayload();
    bool bTwoSided = false;
    if (Payload.IsValid()) Payload->TryGetBoolField(TEXT("twoSided"), bTwoSided);
    if (bTwoSided) NewMaterial->TwoSided = bTwoSided;

    NewMaterial->PostEditChange();
    NewMaterial->MarkPackageDirty();

    if (Resolution.Action != AssetCreatePolicy::EAction::UpdateInPlace)
    {
        FAssetRegistryModule::AssetCreated(NewMaterial);
    }

    bool bSave = Ctx.GetBool(TEXT("save"), true);
    // Honest persistence verdict: only true when save was requested AND the .uasset
    // actually reached disk. AddAssetSaveReport then sets saved/pendingFlush so the
    // unconditional existsAfter:true (a memory/registry fact) is never read as proof
    // of durable state (B-material-authoring-save-no-disk-write).
    const bool bSavedToDisk = bSave && SaveMaterialAsset(NewMaterial);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    AssetCreatePolicy::AddCreateReport(Result, Resolution);
    PinWright::Material::AddMaterialVerification(Result, NewMaterial);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// set_blend_mode
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.set_blend_mode", "material.authoring",
    "Set the blend mode on a material (Opaque, Masked, Translucent, etc.). Triggers a recompile; rendering changes apply after the compile finishes.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("blendMode", "string", "Opaque|Masked|Translucent|Additive|Modulate|AlphaComposite|AlphaHoldout|TranslucentColoredTransmittance (the last is Substrate-only and is remapped to Translucent when Substrate is off)"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString BlendMode;
    if (!Ctx.RequireString(TEXT("blendMode"), BlendMode)) return true;

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        // A UMaterialInstanceConstant loads fine here — it is just not a UMaterial. Answering
        // ASSET_NOT_FOUND sent the caller to re-check a correct path; name the class and the
        // per-instance route instead (blend mode IS overridable per instance).
        PinWright::Material::ReportMaterialLoadFailure(
            Ctx, AssetPath, PinWright::Material::InstanceBasePropertyOverrideVerb());
        return true;
    }

    // ApplyBlendMode's false return used to be discarded, so an unrecognised blendMode changed
    // nothing and still reported success.
    if (!ApplyBlendMode(Material, BlendMode))
    {
        Ctx.SendError(TEXT("INVALID_BLEND_MODE"),
            FString::Printf(TEXT("Unknown blendMode '%s'. Expected one of: %s."),
                *BlendMode, *GetBlendModeNameList()));
        return true;
    }
    Material->PostEditChange();
    Material->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Material);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// set_shading_model
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.set_shading_model", "material.authoring",
    "Set the shading model on a material (Unlit, DefaultLit, Subsurface, ClearCoat, Hair, etc.). Forces a recompile.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("shadingModel", "string", "Unlit|DefaultLit|Subsurface|SubsurfaceProfile|PreintegratedSkin|ClearCoat|Hair|Cloth|Eye|TwoSidedFoliage|SingleLayerWater|ThinTranslucent"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString ShadingModel;
    if (!Ctx.RequireString(TEXT("shadingModel"), ShadingModel)) return true;

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        // Shading model is overridable per instance, so a material instance at this path gets
        // routed rather than told (falsely) that nothing is there.
        PinWright::Material::ReportMaterialLoadFailure(
            Ctx, AssetPath, PinWright::Material::InstanceBasePropertyOverrideVerb());
        return true;
    }

    if (!ApplyShadingModel(Material, ShadingModel))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("Unknown shadingModel '%s'. Expected one of: %s."),
                *ShadingModel, *GetShadingModelNameList()));
        return true;
    }
    Material->PostEditChange();
    Material->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Material);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// set_material_domain
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.set_material_domain", "material.authoring",
    "Set the material domain (Surface, DeferredDecal, LightFunction, Volume, PostProcess, UI). Changes which inputs the material exposes; forces a recompile.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("materialDomain", "string", "Surface|DeferredDecal|LightFunction|Volume|PostProcess|UI"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString Domain;
    if (!Ctx.RequireString(TEXT("materialDomain"), Domain)) return true;

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        // No per-instance route named here: MaterialDomain is NOT in
        // FMaterialInstanceBasePropertyOverrides, so an instance cannot override it — the
        // caller has to edit the master (or reparent).
        PinWright::Material::ReportMaterialLoadFailure(Ctx, AssetPath);
        return true;
    }

    if (!ApplyMaterialDomain(Material, Domain))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("Unknown materialDomain '%s'. Expected one of: Surface, DeferredDecal, LightFunction, Volume, PostProcess, UI."),
                *Domain));
        return true;
    }
    Material->PostEditChange();
    Material->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Material);
    // A material switched INTO the LightFunction domain gains a second, silent gate: whether the
    // light function atlas will take it. Report the measured state here so the caller learns it at
    // the write rather than only if they later think to read get_material_info.
    PinWright::LightFunctionAtlas::AddReportIfLightFunction(Result, Material);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// set_light_function_atlas_compatible
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.set_light_function_atlas_compatible", "material.authoring",
    "Set UMaterial::bForceCompatibleWithLightFunctionAtlas (\"Compatible With Light Function Atlas\") and report the MEASURED result. "
    "This is the only override for a light function the atlas rejects - and a rejected light function still modulates opaque surfaces, so nothing else distinguishes it from a working one. "
    "The atlas is what volumetric fog, translucency and single-layer water sample a light function through; the translator excludes any material that manipulates texcoords or reads world position / scene depth / scene textures, which is most animated light functions. "
    "Forces a recompile. The response's `lightFunctionAtlas.compatible` is read back off the compiled shader map, not echoed from the write, and is omitted while the recompile has not produced one yet.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("forceCompatible", "boolean", "Value for bForceCompatibleWithLightFunctionAtlas (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    if constexpr (!PinWright::LightFunctionAtlas::bSupportedOnThisEngine)
    {
        Ctx.SendUnsupportedEngineVersion(TEXT("5.5"),
            TEXT("UMaterial::bForceCompatibleWithLightFunctionAtlas"));
        return true;
    }

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        // No per-instance route: bForceCompatibleWithLightFunctionAtlas is not in
        // FMaterialInstanceBasePropertyOverrides, so an instance cannot override it — same shape
        // as set_material_domain.
        PinWright::Material::ReportMaterialLoadFailure(Ctx, AssetPath);
        return true;
    }

    const bool bForceCompatible = Ctx.GetBool(TEXT("forceCompatible"), true);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    Material->bForceCompatibleWithLightFunctionAtlas = bForceCompatible ? 1 : 0;
#endif
    // The flag is consumed at TRANSLATION time (HLSLMaterialTranslator ORs it into
    // bIsLightFunctionAtlasCompatible), so without a recompile the write is durable but the
    // renderer keeps using the old shader map — the exact stale state MakeReport warns about.
    Material->PostEditChange();
    Material->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Material);
    if (const TSharedPtr<FJsonObject> Block = PinWright::LightFunctionAtlas::MakeReport(Material))
    {
        Result->SetObjectField(TEXT("lightFunctionAtlas"), Block);
    }
    if (Material->MaterialDomain != EMaterialDomain::MD_LightFunction)
    {
        // The flag is inert outside the LightFunction domain: only a light's LightFunctionMaterial
        // ever reaches the atlas. Written, saved, and doing nothing is precisely the silent-success
        // shape this verb exists to expose, so say it rather than report a bare success.
        Result->SetStringField(TEXT("domainWarning"),
            TEXT("bForceCompatibleWithLightFunctionAtlas was written, but this material's domain is ")
            TEXT("not LightFunction, so it can never be assigned as a light's LightFunctionMaterial ")
            TEXT("and the flag has no effect. Set the domain with material.authoring.set_material_domain ")
            TEXT("{materialDomain:\"LightFunction\"} first."));
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// 8.2 Material Expressions
// ============================================================================

// --------------------------------------------------------------------------
// add_texture_sample
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_texture_sample", "material.authoring",
    "Add a texture sample or texture sample parameter to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("texturePath", "path", "Texture asset to assign"),
        RPC_PARAM_OPT("parameterName", "string", "If set, creates a parameter node instead of a plain sample"),
        RPC_PARAM_OPT("samplerType", "string", "Color|LinearColor|Normal|Masks|Alpha"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    FString TexturePath = Ctx.GetString(TEXT("texturePath"));
    FString ParameterName = Ctx.GetString(TEXT("parameterName"));
    FString SamplerType = Ctx.GetString(TEXT("samplerType"));

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    if (!TexturePath.IsEmpty() && LoadObject<UTexture>(nullptr, *TexturePath))
    {
        Properties->SetStringField(TEXT("Texture"), TexturePath);
    }
    Properties->SetNumberField(TEXT("SamplerType"), static_cast<int32>(SamplerTypeFromName(SamplerType)));

    UClass* ExpressionClass = UMaterialExpressionTextureSample::StaticClass();
    if (!ParameterName.IsEmpty())
    {
        ExpressionClass = UMaterialExpressionTextureSampleParameter2D::StaticClass();
        Properties->SetStringField(TEXT("ParameterName"), ParameterName);
    }

    return CreateExpressionWithFactoryAndRespond(
        Material, ExpressionClass, Properties, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// set_texture_sample_texture
// --------------------------------------------------------------------------
// add_texture_sample only reads texturePath at node-creation time, so reassigning
// or first-assigning a texture on an existing TextureSample node previously forced
// a raw property.set on the engine `Texture` UPROPERTY. This typed verb writes the
// node's Texture through the open-editor-guarded loader and, by default, auto-derives
// SamplerType from the texture (UMaterialExpressionTextureBase::GetSamplerTypeForTexture
// via AutoSetSampleType) so the sampler/texture pair is consistent by construction and
// the caller never has to hunt for a texture whose derived sampler class matches a
// hand-picked samplerType.
REGISTER_RPC_HANDLER("material.authoring.set_texture_sample_texture", "material.authoring",
    "Assign or change the texture on an existing TextureSample / TextureSampleParameter2D node. Auto-derives SamplerType from the texture unless samplerType is given.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("nodeId", "string", "TextureSample node ID (or name/parameter name) to assign the texture to. A parameter name shared by several nodes is refused with AMBIGUOUS_NODE listing each candidate."),
        RPC_PARAM_REQ("texturePath", "path", "Texture asset to assign"),
        RPC_PARAM_OPT("samplerType", "string", "Color|LinearColor|Normal|Masks|Alpha. Omit to auto-derive from the texture (recommended).")
    ))
{
    LOAD_MATERIAL_OR_RETURN();

    FString NodeId;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;
    FString TexturePath;
    if (!Ctx.RequireString(TEXT("texturePath"), TexturePath)) return true;

    UMaterialExpression* Expr = PinWright::Material::ResolveExpressionOrSendError(
        Ctx, ResolveExpressionByIdOrName(Material, NodeId), NodeId,
        TEXT("NOT_FOUND"), TEXT("Node not found."));
    if (!Expr) return true;

    UMaterialExpressionTextureBase* TexExpr = Cast<UMaterialExpressionTextureBase>(Expr);
    if (!TexExpr)
    {
        Ctx.SendError(TEXT("WRONG_NODE_TYPE"),
            TEXT("Node is not a TextureSample (UMaterialExpressionTextureBase). Use the matching typed setter for this node class."));
        return true;
    }

    UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
    if (!Texture)
    {
        Ctx.SendError(TEXT("TEXTURE_NOT_FOUND"),
            FString::Printf(TEXT("Could not load texture '%s'."), *TexturePath));
        return true;
    }

    TexExpr->Texture = Texture;

    const FString SamplerType = Ctx.GetString(TEXT("samplerType"));
    if (SamplerType.IsEmpty())
    {
        // Derive the sampler class the engine expects for this texture (matching what the
        // compiler's VerifySamplerType check enforces) so the pair is consistent by construction.
        TexExpr->AutoSetSampleType();
    }
    else
    {
        TexExpr->SamplerType = SamplerTypeFromName(SamplerType);
    }

    Material->PostEditChange();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), Expr->MaterialExpressionGuid.ToString());
    Result->SetStringField(TEXT("texturePath"), Texture->GetPathName());
    Result->SetStringField(TEXT("samplerType"), UEnum::GetValueAsString(TexExpr->SamplerType));
    // Always 1 on the success path (a wider match is refused above), stated so a caller reading
    // only the payload can see the write covered every node the nodeId named.
    Result->SetNumberField(TEXT("nodesChanged"), 1);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// add_texture_coordinate
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_texture_coordinate", "material.authoring",
    "Add a texture coordinate node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("coordinateIndex", "integer", "UV channel index"),
        RPC_PARAM_OPT("uTiling", "number", "U tiling (default 1.0)"),
        RPC_PARAM_OPT("vTiling", "number", "V tiling (default 1.0)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    int32 CoordIndex = Ctx.GetInt(TEXT("coordinateIndex"), 0);
    double UTiling = Ctx.GetNumber(TEXT("uTiling"), 1.0);
    double VTiling = Ctx.GetNumber(TEXT("vTiling"), 1.0);

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetNumberField(TEXT("CoordinateIndex"), CoordIndex);
    Properties->SetNumberField(TEXT("UTiling"), UTiling);
    Properties->SetNumberField(TEXT("VTiling"), VTiling);

    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionTextureCoordinate::StaticClass(), Properties, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_scalar_parameter
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_scalar_parameter", "material.authoring",
    "Add a scalar parameter expression to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("parameterName", "string", "Parameter name"),
        RPC_PARAM_OPT("defaultValue", "number", "Default scalar value"),
        RPC_PARAM_OPT("group", "string", "Parameter group"),
        RPC_PARAM_OPT("sortPriority", "number", "Sort priority within group (default 32)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;

    double DefaultValue = Ctx.GetNumber(TEXT("defaultValue"), 0.0);
    FString Group = Ctx.GetString(TEXT("group"));

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("ParameterName"), ParamName);
    Properties->SetNumberField(TEXT("DefaultValue"), DefaultValue);
    if (!Group.IsEmpty()) Properties->SetStringField(TEXT("Group"), Group);
    if (Ctx.GetRawPayload().IsValid() && Ctx.GetRawPayload()->HasField(TEXT("sortPriority")))
    {
        Properties->SetNumberField(TEXT("SortPriority"), Ctx.GetNumber(TEXT("sortPriority"), 32));
    }

    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionScalarParameter::StaticClass(), Properties, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_vector_parameter
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_vector_parameter", "material.authoring",
    "Add a vector parameter expression to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("parameterName", "string", "Parameter name"),
        RPC_PARAM_OPT("defaultValue", "object", "Default color {r,g,b,a}"),
        RPC_PARAM_OPT("group", "string", "Parameter group"),
        RPC_PARAM_OPT("sortPriority", "number", "Sort priority within group (default 32)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;
    FString Group = Ctx.GetString(TEXT("group"));

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("ParameterName"), ParamName);
    if (!Group.IsEmpty()) Properties->SetStringField(TEXT("Group"), Group);
    if (Ctx.GetRawPayload().IsValid() && Ctx.GetRawPayload()->HasField(TEXT("sortPriority")))
    {
        Properties->SetNumberField(TEXT("SortPriority"), Ctx.GetNumber(TEXT("sortPriority"), 32));
    }

    TSharedPtr<FJsonObject> DefaultObj = Ctx.GetObject(TEXT("defaultValue"));
    if (DefaultObj.IsValid())
    {
        double R = 1.0, G = 1.0, B = 1.0, A = 1.0;
        DefaultObj->TryGetNumberField(TEXT("r"), R);
        DefaultObj->TryGetNumberField(TEXT("g"), G);
        DefaultObj->TryGetNumberField(TEXT("b"), B);
        DefaultObj->TryGetNumberField(TEXT("a"), A);
        TSharedPtr<FJsonObject> DefaultValue = MakeShared<FJsonObject>();
        DefaultValue->SetNumberField(TEXT("R"), R);
        DefaultValue->SetNumberField(TEXT("G"), G);
        DefaultValue->SetNumberField(TEXT("B"), B);
        DefaultValue->SetNumberField(TEXT("A"), A);
        Properties->SetObjectField(TEXT("DefaultValue"), DefaultValue);
    }

    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionVectorParameter::StaticClass(), Properties, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_static_switch_parameter
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_static_switch_parameter", "material.authoring",
    "Add a static switch parameter to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("parameterName", "string", "Parameter name"),
        RPC_PARAM_OPT("defaultValue", "boolean", "Default switch value"),
        RPC_PARAM_OPT("group", "string", "Parameter group"),
        RPC_PARAM_OPT("sortPriority", "number", "Sort priority within group (default 32)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;

    bool DefaultValue = Ctx.GetBool(TEXT("defaultValue"), false);
    FString Group = Ctx.GetString(TEXT("group"));

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("ParameterName"), ParamName);
    Properties->SetBoolField(TEXT("DefaultValue"), DefaultValue);
    if (!Group.IsEmpty()) Properties->SetStringField(TEXT("Group"), Group);
    if (Ctx.GetRawPayload().IsValid() && Ctx.GetRawPayload()->HasField(TEXT("sortPriority")))
    {
        Properties->SetNumberField(TEXT("SortPriority"), Ctx.GetNumber(TEXT("sortPriority"), 32));
    }

    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionStaticSwitchParameter::StaticClass(), Properties, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_math_node
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_math_node", "material.authoring",
    "Add a math operation node (Add, Subtract, Multiply, Divide, Lerp, Clamp, Power, Frac, OneMinus, Append) to a UMaterial or UMaterialFunction graph",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material or material-function asset path")),
        RPC_PARAM_REQ("operation", "string", "Add|Subtract|Multiply|Divide|Lerp|Clamp|Power|Frac|OneMinus|Append"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    FString AssetPath;
    PinWright::Material::FMaterialMutationTarget Target =
        PinWright::Material::LoadMaterialOrFunctionForMutationOrReportError(
            Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath);
    if (!Target.IsValid()) return true;
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    FString Operation;
    if (!Ctx.RequireString(TEXT("operation"), Operation)) return true;

    UClass* ExpressionClass = nullptr;
    if (Operation == TEXT("Add"))
        ExpressionClass = UMaterialExpressionAdd::StaticClass();
    else if (Operation == TEXT("Subtract"))
        ExpressionClass = UMaterialExpressionSubtract::StaticClass();
    else if (Operation == TEXT("Multiply"))
        ExpressionClass = UMaterialExpressionMultiply::StaticClass();
    else if (Operation == TEXT("Divide"))
        ExpressionClass = UMaterialExpressionDivide::StaticClass();
    else if (Operation == TEXT("Lerp"))
        ExpressionClass = UMaterialExpressionLinearInterpolate::StaticClass();
    else if (Operation == TEXT("Clamp"))
        ExpressionClass = UMaterialExpressionClamp::StaticClass();
    else if (Operation == TEXT("Power"))
        ExpressionClass = UMaterialExpressionPower::StaticClass();
    else if (Operation == TEXT("Frac"))
        ExpressionClass = UMaterialExpressionFrac::StaticClass();
    else if (Operation == TEXT("OneMinus"))
        ExpressionClass = UMaterialExpressionOneMinus::StaticClass();
    else if (Operation == TEXT("Append"))
        ExpressionClass = UMaterialExpressionAppendVector::StaticClass();
    else
    {
        Ctx.SendError(TEXT("UNKNOWN_OPERATION"), FString::Printf(TEXT("Unknown operation: %s"), *Operation));
        return true;
    }

    return CreateExpressionWithFactoryAndRespond(
        Target, ExpressionClass, nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_world_position
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_world_position", "material.authoring",
    "Add a WorldPosition node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionWorldPosition::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_vertex_normal
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_vertex_normal", "material.authoring",
    "Add a VertexNormalWS node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionVertexNormalWS::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_pixel_depth
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_pixel_depth", "material.authoring",
    "Add a PixelDepth node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionPixelDepth::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_fresnel
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_fresnel", "material.authoring",
    "Add a Fresnel node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionFresnel::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_reflection_vector
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_reflection_vector", "material.authoring",
    "Add a ReflectionVectorWS node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionReflectionVectorWS::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_panner
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_panner", "material.authoring",
    "Add a Panner node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionPanner::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_rotator
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_rotator", "material.authoring",
    "Add a Rotator node to a material (UE 5.1+)",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    UClass* RotatorClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpressionRotator"));
    if (!RotatorClass)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Rotator expression."));
        return true;
    }

    return CreateExpressionWithFactoryAndRespond(
        Material, RotatorClass, nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_noise
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_noise", "material.authoring",
    "Add a Noise node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionNoise::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_voronoi
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_voronoi", "material.authoring",
    "Add a Voronoi noise node to a material (Noise with VoronoiALU function)",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetNumberField(TEXT("NoiseFunction"), static_cast<double>(ENoiseFunction::NOISEFUNCTION_VoronoiALU));
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionNoise::StaticClass(), Properties, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_if
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_if", "material.authoring",
    "Add an If conditional node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionIf::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_switch
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_switch", "material.authoring",
    "Add an If comparator node (alias of add_if; not a bool true/false switch)",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionIf::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_component_mask
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_component_mask", "material.authoring",
    "Add a ComponentMask node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("r", "boolean", "Include R channel (default true)"),
        RPC_PARAM_OPT("g", "boolean", "Include G channel (default true)"),
        RPC_PARAM_OPT("b", "boolean", "Include B channel (default true)"),
        RPC_PARAM_OPT("a", "boolean", "Include A channel (default false)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    bool bR = Ctx.GetBool(TEXT("r"), true);
    bool bG = Ctx.GetBool(TEXT("g"), true);
    bool bB = Ctx.GetBool(TEXT("b"), true);
    bool bA = Ctx.GetBool(TEXT("a"), false);

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetBoolField(TEXT("R"), bR);
    Properties->SetBoolField(TEXT("G"), bG);
    Properties->SetBoolField(TEXT("B"), bB);
    Properties->SetBoolField(TEXT("A"), bA);

    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionComponentMask::StaticClass(), Properties, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_dot_product
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_dot_product", "material.authoring",
    "Add a DotProduct node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionDotProduct::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_cross_product
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_cross_product", "material.authoring",
    "Add a CrossProduct node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionCrossProduct::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_desaturation
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_desaturation", "material.authoring",
    "Add a Desaturation node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("luminanceFactors", "object", "{r,g,b} custom luminance factors"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    TSharedPtr<FJsonObject> LumObj = Ctx.GetObject(TEXT("luminanceFactors"));
    TSharedPtr<FJsonObject> Properties = nullptr;
    if (LumObj.IsValid())
    {
        double R = 0.3, G = 0.59, B = 0.11;
        LumObj->TryGetNumberField(TEXT("r"), R);
        LumObj->TryGetNumberField(TEXT("g"), G);
        LumObj->TryGetNumberField(TEXT("b"), B);
        TSharedPtr<FJsonObject> LuminanceFactors = MakeShared<FJsonObject>();
        LuminanceFactors->SetNumberField(TEXT("R"), R);
        LuminanceFactors->SetNumberField(TEXT("G"), G);
        LuminanceFactors->SetNumberField(TEXT("B"), B);
        LuminanceFactors->SetNumberField(TEXT("A"), 1.0);
        Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("LuminanceFactors"), LuminanceFactors);
    }

    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionDesaturation::StaticClass(), Properties, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_append
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_append", "material.authoring",
    "Add an AppendVector node to a material",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);
    return CreateExpressionWithFactoryAndRespond(
        Material, UMaterialExpressionAppendVector::StaticClass(), nullptr, ExprX, ExprY, Ctx);
}

// --------------------------------------------------------------------------
// add_custom_expression
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_custom_expression", "material.authoring",
    "Add a UMaterialExpressionCustom node containing inline HLSL code. Use sparingly: custom HLSL nodes break shader permutation deduplication and slow material compilation.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("code", "string", "HLSL code"),
        RPC_PARAM_OPT("outputType", "string", "Float1|Float2|Float3|Float4|MaterialAttributes"),
        RPC_PARAM_OPT("description", "string", "Node description"),
        RPC_PARAM_REQ("inputs", "array", "Array of custom input pin names or objects with {name}; pass [] for no inputs"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    FString Code;
    if (!Ctx.RequireString(TEXT("code"), Code)) return true;
    const TArray<TSharedPtr<FJsonValue>>* RequestedInputs = nullptr;
    if (!Ctx.RequireArray(TEXT("inputs"), RequestedInputs)) return true;
    if (!RequestedInputs)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Missing required array field: inputs"));
        return true;
    }
    FString OutputType = Ctx.GetString(TEXT("outputType"));
    FString Description = Ctx.GetString(TEXT("description"));
    TArray<FName> InputNames;
    if (!TryParseCustomExpressionInputNames(Ctx.GetRawPayload(), InputNames, Ctx)) return true;

    UMaterialExpressionCustom* CustomExpr =
        NewObject<UMaterialExpressionCustom>(
            Material, UMaterialExpressionCustom::StaticClass(), NAME_None, RF_Transactional);
    // Owning-material back-pointer — see FMaterialExpressionFactory::Create. Without it a later
    // edit of this node's Code (the usual way an HLSL node is iterated on) never reaches the
    // material and the shader is never retranslated.
    CustomExpr->Material = Material;
    CustomExpr->Code = Code;
    CustomExpr->Inputs.Empty();

    if (OutputType == TEXT("Float1") || OutputType == TEXT("CMOT_Float1"))
        CustomExpr->OutputType = CMOT_Float1;
    else if (OutputType == TEXT("Float2") || OutputType == TEXT("CMOT_Float2"))
        CustomExpr->OutputType = CMOT_Float2;
    else if (OutputType == TEXT("Float3") || OutputType == TEXT("CMOT_Float3"))
        CustomExpr->OutputType = CMOT_Float3;
    else if (OutputType == TEXT("Float4") || OutputType == TEXT("CMOT_Float4"))
        CustomExpr->OutputType = CMOT_Float4;
    else if (OutputType == TEXT("MaterialAttributes"))
        CustomExpr->OutputType = CMOT_MaterialAttributes;
    else
        CustomExpr->OutputType = CMOT_Float1;

    if (!Description.IsEmpty()) CustomExpr->Description = Description;

    for (const FName& InputName : InputNames)
    {
        FCustomInput& CustomInput = CustomExpr->Inputs.AddDefaulted_GetRef();
        CustomInput.InputName = InputName;
    }

    CustomExpr->MaterialExpressionEditorX = (int32)ExprX;
    CustomExpr->MaterialExpressionEditorY = (int32)ExprY;
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(CustomExpr);
    Material->PostEditChange();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), CustomExpr->MaterialExpressionGuid.ToString());
    Result->SetNumberField(TEXT("inputCount"), CustomExpr->Inputs.Num());

    TArray<TSharedPtr<FJsonValue>> ConfiguredInputs;
    for (const FCustomInput& CustomInput : CustomExpr->Inputs)
    {
        ConfiguredInputs.Add(MakeShared<FJsonValueString>(CustomInput.InputName.ToString()));
    }
    Result->SetArrayField(TEXT("inputs"), ConfiguredInputs);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// 8.2 Node Connections
// ============================================================================

// --------------------------------------------------------------------------
// connect_nodes
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.connect_nodes", "material.authoring",
    "Connect a source expression output to a target expression input or main material node. A wire is a GRAPH write, not a shader compile: read shaderCompile.status in the response for the material's measured shader state, and call material.authoring.compile_material for the final verdict when it reads notCompiled.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("sourceNodeId", "string", "Source node ID/name"),
        RPC_PARAM_OPT("targetNodeId", "string", "Target node ID/name (empty or 'Main' for main material node)"),
        RPC_PARAM_OPT("inputName", "string", "Input pin name on target node"),
        RPC_PARAM_OPT("sourcePin", "string", "Output pin name on source node"),
        RPC_PARAM_OPT("sourceOutputIndex", "integer", "Output index on source node (fallback when sourcePin name not matched)"),
        RPC_PARAM_OPT("x", "number", "Node X position (unused)"),
        RPC_PARAM_OPT("y", "number", "Node Y position (unused)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();

    FString SourceNodeId = Ctx.GetString(TEXT("sourceNodeId"));
    FString TargetNodeId = Ctx.GetString(TEXT("targetNodeId"));
    FString InputName = Ctx.GetString(TEXT("inputName"));
    FString SourcePin = Ctx.GetString(TEXT("sourcePin"), TEXT(""));
    int32 SourceOutputIndex = Ctx.GetInt(TEXT("sourceOutputIndex"), -1);

    UMaterialExpression* SourceExpr = PinWright::Material::ResolveExpressionOrSendError(
        Ctx, ResolveExpressionByIdOrName(Material, SourceNodeId), SourceNodeId,
        TEXT("NODE_NOT_FOUND"), TEXT("Source node not found."));
    if (!SourceExpr) return true;

    // Target is main material node?
    if (TargetNodeId.IsEmpty() || TargetNodeId == TEXT("Main"))
    {
        FExpressionInput* MainInput =
            PinWright::Material::ResolveMainInput(Material->GetEditorOnlyData(), InputName);
        if (MainInput)
        {
            PinWright::Material::ApplyConnection(*MainInput, SourceExpr, SourcePin, SourceOutputIndex);
            Material->PostEditChange();
            Material->MarkPackageDirty();

            if (MainInput->Expression != SourceExpr)
            {
                Ctx.SendError(TEXT("CONNECTION_FAILED"),
                    FString::Printf(TEXT("Main input '%s' did not retain the requested source node."), *InputName));
                return true;
            }

            Ctx.SendSuccess(TEXT("Connected to main material node."));
        }
        else
        {
            Ctx.SendError(TEXT("INVALID_PIN"),
                FString::Printf(TEXT("Unknown input on main node: %s. Valid: %s"),
                    *InputName, *PinWright::Material::GetValidMainInputNames()));
        }
        return true;
    }

    // Connect to another expression
    UMaterialExpression* TargetExpr = PinWright::Material::ResolveExpressionOrSendError(
        Ctx, ResolveExpressionByIdOrName(Material, TargetNodeId), TargetNodeId,
        TEXT("NODE_NOT_FOUND"), TEXT("Target node not found."));
    if (!TargetExpr) return true;

    TArray<FString> CandidateNames;
    FExpressionInput* InputPtr = FMaterialExpressionFactory::FindExpressionInputByName(
        TargetExpr, InputName, &CandidateNames);
    if (InputPtr)
    {
        PinWright::Material::ApplyConnection(*InputPtr, SourceExpr, SourcePin, SourceOutputIndex);
        Material->PostEditChange();
        Material->MarkPackageDirty();

        // Re-resolve before reading back: on a MaterialFunctionCall target the pin above lives in
        // UMaterialExpressionMaterialFunctionCall::FunctionInputs, and UMaterial::PostEditChange
        // rebuilds that array (UpdateCachedExpressionData -> AnalyzeMaterial -> UpdateForExpressions
        // -> UpdateFromFunctionResource, MaterialCachedData.cpp 5.8) — the old buffer is moved into a
        // local and freed, so verifying through the pre-change pointer read released memory and could
        // refuse a wire the engine had in fact carried over by pin GUID.
        InputPtr = FMaterialExpressionFactory::FindExpressionInputByName(TargetExpr, InputName);
        if (!InputPtr || InputPtr->Expression != SourceExpr)
        {
            Ctx.SendError(TEXT("CONNECTION_FAILED"),
                FString::Printf(TEXT("Input pin '%s' did not retain the requested source node."), *InputName));
            return true;
        }

        Ctx.SendSuccess(TEXT("Nodes connected."));
        return true;
    }

    TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
    ErrorData->SetArrayField(TEXT("candidates"), JsonBuilders::BuildStringArrayJson(CandidateNames, false));
    Ctx.SendError(TEXT("PIN_NOT_FOUND"),
        FString::Printf(TEXT("Input pin '%s' not found. Valid names: %s."),
            *InputName, *FString::Join(CandidateNames, TEXT(", "))),
        ErrorData);
    return true;
}

// ============================================================================
// 8.3 Material Functions
// ============================================================================

// --------------------------------------------------------------------------
// create_material_function
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.create_material_function", "material.authoring",
    "Create a UMaterialFunction asset — a reusable subgraph that can be called from many materials. Add inputs/outputs with add_function_input / add_function_output, then call from materials with use_material_function.",
    RPC_PARAMS(
        MaterialCreatePathParamUtils::MaterialCreateNameParamReq(TEXT("Function name (leaf). Or pass one combined 'assetPath' to set name + folder together.")),
        MaterialCreatePathParamUtils::MaterialCreateFolderParamOpt(TEXT("Destination folder (default /Game/Materials/Functions)")),
        RPC_PARAM_OPT("description", "string", "Function description"),
        RPC_PARAM_OPT("exposeToLibrary", "boolean", "Expose to material function library (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Save after creation (default true)")
    ))
{
    FString Name;
    FString Path;
    FString PackagePath;
    if (!MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath(Ctx, TEXT("/Game/Materials/Functions"), Name, Path, PackagePath)) return true;
    FString Description = Ctx.GetString(TEXT("description"));
    bool bExposeToLibrary = Ctx.GetBool(TEXT("exposeToLibrary"), true);

    UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package."));
        return true;
    }

    UMaterialFunction* NewFunc = Cast<UMaterialFunction>(
        Factory->FactoryCreateNew(UMaterialFunction::StaticClass(), Package,
            FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewFunc)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create material function."));
        return true;
    }

    if (!Description.IsEmpty()) NewFunc->Description = Description;
    NewFunc->bExposeToLibrary = bExposeToLibrary;
    NewFunc->PostEditChange();
    NewFunc->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialFunctionAsset(NewFunc);
    FAssetRegistryModule::AssetCreated(NewFunc);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, NewFunc);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// add_function_input
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_function_input", "material.authoring",
    "Add an input to a material function. By default the input is REQUIRED — a calling material that leaves it unwired fails to compile with \"Missing function input <name>\". Pass optional:true (or a defaultValue) to make it optional-with-preview so callers can omit it.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material function asset path")),
        RPC_PARAM_REQ("inputName", "string", "Input name"),
        RPC_PARAM_OPT("inputType", "string", "Scalar|Vector2|Vector3|Vector4|Texture2D|TextureCube|Bool|MaterialAttributes"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("optional", "boolean", "Mark the input optional (sets bUsePreviewValueAsDefault=true) so callers may leave it unwired. Default false (required)."),
        RPC_PARAM_OPT("defaultValue", "array|number", "Preview/default value as a numeric array [x,y,z,w] (or a single number for a scalar), mapped into PreviewValue. Supplying it implies optional:true. Not applicable to Texture/MaterialAttributes inputs.")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString InputName;
    if (!Ctx.RequireString(TEXT("inputName"), InputName)) return true;
    FString InputType = Ctx.GetString(TEXT("inputType"));

    REQUIRE_NODE_POSITION_OR_RETURN(X, Y);

    UMaterialFunction* Func = LoadObject<UMaterialFunction>(nullptr, *AssetPath);
    if (!Func)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Material Function."));
        return true;
    }

    UMaterialExpressionFunctionInput* Input =
        NewObject<UMaterialExpressionFunctionInput>(
            Func, UMaterialExpressionFunctionInput::StaticClass(), NAME_None, RF_Transactional);
    // Owning-function back-pointer — see FMaterialExpressionFactory::Create (function overload).
    Input->Function = Func;
    Input->InputName = FName(*InputName);

    if (InputType == TEXT("Float1") || InputType == TEXT("Scalar"))
        Input->InputType = EFunctionInputType::FunctionInput_Scalar;
    else if (InputType == TEXT("Float2") || InputType == TEXT("Vector2"))
        Input->InputType = EFunctionInputType::FunctionInput_Vector2;
    else if (InputType == TEXT("Float3") || InputType == TEXT("Vector3"))
        Input->InputType = EFunctionInputType::FunctionInput_Vector3;
    else if (InputType == TEXT("Float4") || InputType == TEXT("Vector4"))
        Input->InputType = EFunctionInputType::FunctionInput_Vector4;
    else if (InputType == TEXT("Texture2D"))
        Input->InputType = EFunctionInputType::FunctionInput_Texture2D;
    else if (InputType == TEXT("TextureCube"))
        Input->InputType = EFunctionInputType::FunctionInput_TextureCube;
    else if (InputType == TEXT("Bool"))
        Input->InputType = EFunctionInputType::FunctionInput_StaticBool;
    else if (InputType == TEXT("MaterialAttributes"))
        Input->InputType = EFunctionInputType::FunctionInput_MaterialAttributes;
    else
        Input->InputType = EFunctionInputType::FunctionInput_Vector3;

    Input->MaterialExpressionEditorX = (int32)X;
    Input->MaterialExpressionEditorY = (int32)Y;
    // Persistent identity, exactly as add_function_output does for the other side of the pin
    // table. A caller stores FFunctionExpressionInput::ExpressionInputId and re-links purely by
    // that GUID; leaving Id unset means PostLoad mints a new one on every load, so every caller
    // saved against this input loses its wire silently on load. See MaterialFunctionIdentity.h.
    Input->ConditionallyGenerateId(false);

    // Optionality: a function input is REQUIRED by default (bUsePreviewValueAsDefault=false),
    // which makes a calling material that leaves it unwired fail to compile with
    // "Missing function input <name>". `optional:true` — or supplying a `defaultValue`
    // (which implies optional) — sets bUsePreviewValueAsDefault so the input falls back to
    // PreviewValue when no wire is connected at the call site.
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasDefaultValue = Payload.IsValid() && Payload->HasField(TEXT("defaultValue"));
    if (bHasDefaultValue)
    {
        // Map up to four numeric components into PreviewValue (X,Y,Z,W). Accept either a
        // numeric array [x,y,z,w] or a bare number (scalar → X). Unspecified components keep 0.
        double Scalar = 0.0;
        if (Payload->TryGetNumberField(TEXT("defaultValue"), Scalar))
        {
            Input->PreviewValue = FVector4f((float)Scalar, 0.0f, 0.0f, 0.0f);
        }
        else if (const TArray<TSharedPtr<FJsonValue>>* Components = Ctx.GetArray(TEXT("defaultValue")))
        {
            FVector4f Preview(0.0f, 0.0f, 0.0f, 0.0f);
            const int32 Count = FMath::Min(Components->Num(), 4);
            for (int32 Index = 0; Index < Count; ++Index)
            {
                double Component = 0.0;
                if ((*Components)[Index].IsValid() && (*Components)[Index]->TryGetNumber(Component))
                {
                    Preview[Index] = (float)Component;
                }
            }
            Input->PreviewValue = Preview;
        }
    }
    const bool bOptional = Ctx.GetBool(TEXT("optional")) || bHasDefaultValue;
    Input->bUsePreviewValueAsDefault = bOptional ? 1 : 0; // bitfield is uint32:1, so coerce

        Func->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Input);
    Func->PostEditChange();
    Func->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), Input->MaterialExpressionGuid.ToString());
    Result->SetBoolField(TEXT("optional"), bOptional);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// add_function_output
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_function_output", "material.authoring",
    "Add an output to a material function",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material function asset path")),
        RPC_PARAM_REQ("inputName", "string", "Output name"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString InputName;
    if (!Ctx.RequireString(TEXT("inputName"), InputName)) return true;

    REQUIRE_NODE_POSITION_OR_RETURN(X, Y);

    UMaterialFunction* Func = LoadObject<UMaterialFunction>(nullptr, *AssetPath);
    if (!Func)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Material Function."));
        return true;
    }

    UMaterialExpressionFunctionOutput* Output =
        NewObject<UMaterialExpressionFunctionOutput>(
            Func, UMaterialExpressionFunctionOutput::StaticClass(), NAME_None, RF_Transactional);
    // Owning-function back-pointer — see FMaterialExpressionFactory::Create (function overload).
    Output->Function = Func;
    Output->OutputName = FName(*InputName);
    Output->MaterialExpressionEditorX = (int32)X;
    Output->MaterialExpressionEditorY = (int32)Y;
    Output->ConditionallyGenerateId(false);

        Func->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Output);
    Func->PostEditChange();
    Func->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), Output->MaterialExpressionGuid.ToString());
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// use_material_function
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.use_material_function", "material.authoring",
    "Insert a material function call node into a material's graph, exposing the function's inputs/outputs as wireable pins. "
    "ALSO WRITES THE FUNCTION at functionPath, always: the call node links to the function's pins by GUID and by nothing else, and a function whose .uasset carries no pin GUID gets a fresh one minted on every load (PostLoad), which disconnects each of its callers on every load forever. That state is invisible in memory, so the function is re-saved unconditionally to put the GUIDs this node is about to cache on disk. The `functionIdentity` block reports that write (`assetPath`, `saved`, `saveState`, `saveDetail`); a write that did not land also raises a `warnings[]` entry, because the node's wires are then not durable. The MATERIAL is not saved by this verb.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("functionPath", "path", "Material function asset path"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();
    REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    FString FunctionPath;
    if (!Ctx.RequireString(TEXT("functionPath"), FunctionPath)) return true;

    UMaterialFunction* Func = LoadObject<UMaterialFunction>(nullptr, *FunctionPath);
    if (!Func)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Material Function."));
        return true;
    }

    // SetMaterialFunction copies each pin's Id into the call node's cached pin table, and that
    // cached GUID is the ONLY thing the wire survives on. So the function's pin GUIDs must be on
    // DISK before this material caches them, and that write is UNCONDITIONAL because the state
    // needing it cannot be seen from here: UMaterialExpressionFunctionInput::PostLoad and
    // UMaterialExpressionFunctionOutput::PostLoad both call ConditionallyGenerateId(false)
    // (Runtime/Engine/Private/Materials/MaterialExpressions.cpp, 5.8), so a function whose .uasset
    // holds an all-zero pin GUID still presents a perfectly valid one in memory. An in-memory
    // check therefore reports "nothing to repair" for exactly the asset that needs repairing, and
    // outside a cook the next load mints a DIFFERENT GUID again (CookDeterminism::NewGuid is plain
    // FGuid::NewGuid), so such a function disconnects each of its callers on every single load,
    // forever. Writing the function back replaces the anonymous pin on disk with the GUID this
    // call node is about to cache, and one bind ends the loop. The package is dirtied first
    // because PostLoad's repair does not dirty it and a clean package is not written.
    PinWright::MaterialFunctionIdentity::EnsurePersistentIds(Func);
    Func->GetOutermost()->SetDirtyFlag(true);
    EAssetSaveState FunctionSaveState = EAssetSaveState::NotRequested;
    const bool bFunctionSaved = SaveAssetToDiskReportingPresence(
        Func, /*bForce=*/true, nullptr, nullptr, &FunctionSaveState);

    UMaterialExpressionMaterialFunctionCall* FuncCall =
        NewObject<UMaterialExpressionMaterialFunctionCall>(
            Material, UMaterialExpressionMaterialFunctionCall::StaticClass(),
            NAME_None, RF_Transactional);
    FuncCall->SetMaterialFunction(Func);

    // Hand-rolled rather than FINALIZE_EXPR_AND_RESPOND: that macro's response carries only
    // nodeId, and a bind whose function write did not land produces a call node whose pins are
    // already doomed. The caller has to be able to see that.
    FuncCall->Material = Material;
    FuncCall->MaterialExpressionEditorX = (int32)ExprX;
    FuncCall->MaterialExpressionEditorY = (int32)ExprY;
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(FuncCall);
    Material->PostEditChange();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), FuncCall->MaterialExpressionGuid.ToString());

    TSharedPtr<FJsonObject> Identity = MakeShared<FJsonObject>();
    Identity->SetStringField(TEXT("assetPath"), Func->GetPathName());
    AddAssetSaveReport(Identity, /*bSaveRequested=*/true, bFunctionSaved, FunctionSaveState);
    Result->SetObjectField(TEXT("functionIdentity"), Identity);

    if (!bFunctionSaved)
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("The material function '%s' could not be written to disk (%s), so its pin GUIDs ")
            TEXT("are only in memory. This call node's wires will not survive a reload if the ")
            TEXT("function's .uasset carries no pin identity. Re-issue this call once the function ")
            TEXT("can be saved, and re-wire and re-save this material afterwards."),
            *Func->GetPathName(), AssetSaveStateToWire(FunctionSaveState))));
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// 8.4 Material Instances
// ============================================================================

// --------------------------------------------------------------------------
// create_material_instance
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.create_material_instance", "material.authoring",
    "Create a UMaterialInstanceConstant asset that inherits from a parent UMaterial. Pass the optional `parameters` object to override values in the same call (same type-keyed shape as set_material_instance_parameters); otherwise use the set_*_parameter_value methods afterward.",
    RPC_PARAMS(
        MaterialCreatePathParamUtils::MaterialCreateNameParamReq(TEXT("Instance name (leaf). Or pass one combined 'assetPath' to set name + folder together.")),
        RPC_PARAM_REQ("parentMaterial", "path", "Parent material asset path"),
        MaterialCreatePathParamUtils::MaterialCreateFolderParamOpt(TEXT("Destination folder (default /Game/Materials)")),
        RPC_PARAM_OPT_NESTED("parameters", "object", "Inline overrides applied at creation: {scalar:{Name:num}, vector:{Name:{r,g,b,a}}, texture:{Name:assetPath}, staticSwitch:{Name:bool}} — same shape as set_material_instance_parameters. Those four bucket names are the whole first level and any other key is refused with UNKNOWN_NESTED_PARAMS: a payload one level too shallow ({\"r\":1,\"g\":0}) or a misspelt bucket ('vectors', 'static_switch') used to return success with empty applied[] and failed[] arrays. The parameter names INSIDE each bucket are yours and are not checked here.",
            TEXT("scalar"), TEXT("vector"), TEXT("texture"), TEXT("staticSwitch")),
        RPC_PARAM_OPT("save", "boolean", "Save after creation (default true)")
    ))
{
    // Resolved AND validated before parentMaterial is touched, and that ordering is load-bearing
    // for PinWright.material.authoring.create_material_instance.NameCarryingAPathIsRefused: the
    // test pairs a path-shaped `name` with a well-formed parentMaterial naming no asset, so a
    // build without this guard is refused ASSET_NOT_FOUND below - above the concatenation - and
    // goes red, instead of composing "//" into CreatePackage and killing the suite host.
    FString Name;
    FString Path;
    FString PackagePath;
    if (!MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath(Ctx, TEXT("/Game/Materials"), Name, Path, PackagePath)) return true;
    FString ParentMaterial;
    if (!Ctx.RequireString(TEXT("parentMaterial"), ParentMaterial)) return true;

    UMaterial* Parent = LoadObject<UMaterial>(nullptr, *ParentMaterial);
    if (!Parent)
    {
        // Same class-vs-missing split as the rest of the family. Note the remaining gap this
        // makes visible rather than hides: UMaterialInstance::Parent is a UMaterialInterface, so
        // the engine allows an instance-of-an-instance, but this verb only accepts a UMaterial
        // master — the error now says so instead of claiming the path is empty.
        if (UObject* RawParent = LoadObject<UObject>(nullptr, *ParentMaterial))
        {
            Ctx.SendError(TEXT("UNSUPPORTED_ASSET_CLASS"),
                FString::Printf(
                    TEXT("parentMaterial is not a Material. Received class: %s. This verb parents to a UMaterial master only."),
                    *RawParent->GetClass()->GetName()));
            return true;
        }
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load parent material."));
        return true;
    }

    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = Parent;

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package."));
        return true;
    }

    UMaterialInstanceConstant* NewInstance = Cast<UMaterialInstanceConstant>(
        Factory->FactoryCreateNew(UMaterialInstanceConstant::StaticClass(),
            Package, FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewInstance)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create material instance."));
        return true;
    }

    NewInstance->PostEditChange();
    NewInstance->MarkPackageDirty();

    // Inline overrides (optional): pass `parameters` to create + override in one
    // round-trip instead of a follow-up set_material_instance_parameters call.
    // Same type-keyed shape, shared apply path.
    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Failed;
    TSharedPtr<FJsonObject> ParamsObj = Ctx.GetObject(TEXT("parameters"));
    if (ParamsObj.IsValid())
    {
        // Already MarkPackageDirty()'d above; the override apply mutates only
        // editor-only parameter values and never clears the dirty flag, so a
        // second MarkPackageDirty() here would be a no-op.
        ApplyMaterialInstanceParameterOverrides(NewInstance, ParamsObj, Applied, Failed);
    }

    // Register before saving so SaveLoadedAsset flushes a never-before-seen package,
    // mirroring the audio.authoring create order. The honest persistence verdict gates
    // saved/pendingFlush on disk presence rather than the unconditional existsAfter:true
    // (B-material-authoring-save-no-disk-write).
    FAssetRegistryModule::AssetCreated(NewInstance);
    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    const bool bSavedToDisk = bSave && SaveMaterialInstanceAsset(NewInstance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    PinWright::Material::AddMaterialVerification(Result, NewInstance);
    if (ParamsObj.IsValid())
    {
        Result->SetArrayField(TEXT("applied"), Applied);
        Result->SetArrayField(TEXT("failed"), Failed);
    }
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_scalar_parameter_value
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.set_scalar_parameter_value", "material.authoring",
    "Override a named scalar parameter on a UMaterialInstanceConstant. Adds the parameter to the instance's overrides if it wasn't previously overridden.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material instance asset path")),
        RPC_PARAM_REQ("parameterName", "string", "Parameter name"),
        RPC_PARAM_OPT("value", "number", "Scalar value"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;
    double Value = Ctx.GetNumber(TEXT("value"), 0.0);

    UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
    if (!Instance)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load material instance."));
        return true;
    }

    Instance->SetScalarParameterValueEditorOnly(FName(*ParamName), Value);
    Instance->PostEditChange();
    Instance->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialInstanceAsset(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Instance);
    Result->SetStringField(TEXT("parameterName"), ParamName);
    Result->SetNumberField(TEXT("value"), Value);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_vector_parameter_value
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.set_vector_parameter_value", "material.authoring",
    "Override a named FLinearColor / vector parameter on a UMaterialInstanceConstant. Adds the parameter to the instance's overrides if it wasn't previously overridden.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material instance asset path")),
        RPC_PARAM_REQ("parameterName", "string", "Parameter name"),
        RPC_PARAM_OPT("value", "object", "Color value {r,g,b,a}"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;

    UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
    if (!Instance)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load material instance."));
        return true;
    }

    FLinearColor Color(1.0f, 1.0f, 1.0f, 1.0f);
    TSharedPtr<FJsonObject> ValueObj = Ctx.GetObject(TEXT("value"));
    if (ValueObj.IsValid())
    {
        double R = 1.0, G = 1.0, B = 1.0, A = 1.0;
        ValueObj->TryGetNumberField(TEXT("r"), R);
        ValueObj->TryGetNumberField(TEXT("g"), G);
        ValueObj->TryGetNumberField(TEXT("b"), B);
        ValueObj->TryGetNumberField(TEXT("a"), A);
        Color = FLinearColor(R, G, B, A);
    }

    Instance->SetVectorParameterValueEditorOnly(FName(*ParamName), Color);
    Instance->PostEditChange();
    Instance->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialInstanceAsset(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Instance);
    Result->SetStringField(TEXT("parameterName"), ParamName);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_texture_parameter_value
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.set_texture_parameter_value", "material.authoring",
    "Override a named texture parameter on a UMaterialInstanceConstant by pointing it at a UTexture asset path.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material instance asset path")),
        RPC_PARAM_REQ("parameterName", "string", "Parameter name"),
        RPC_PARAM_REQ("texturePath", "path", "Texture asset path"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;
    FString TexturePath;
    if (!Ctx.RequireAssetPath(TEXT("texturePath"), TexturePath)) return true;

    UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
    if (!Instance)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load material instance."));
        return true;
    }

    UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
    if (!Texture)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load texture."));
        return true;
    }

    Instance->SetTextureParameterValueEditorOnly(FName(*ParamName), Texture);
    Instance->PostEditChange();
    Instance->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialInstanceAsset(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Instance);
    Result->SetStringField(TEXT("parameterName"), ParamName);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// Helpers (file-local) for the material-instance read/clear/batch handlers below.
// --------------------------------------------------------------------------
namespace
{
    // Loads a UMaterialInstanceConstant for the given asset path; emits the
    // mirror of get_material_info's UNSUPPORTED_ASSET_CLASS branch when the
    // asset exists but isn't an instance.
    UMaterialInstanceConstant* LoadMaterialInstanceOrError(FHandlerContext& Ctx, const FString& AssetPath)
    {
        if (UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath))
        {
            return Instance;
        }
        if (UObject* RawAsset = LoadObject<UObject>(nullptr, *AssetPath))
        {
            Ctx.SendError(
                TEXT("UNSUPPORTED_ASSET_CLASS"),
                FString::Printf(
                    TEXT("Asset is not a UMaterialInstanceConstant. Received class: %s"),
                    *RawAsset->GetClass()->GetName()));
            return nullptr;
        }
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load material instance."));
        return nullptr;
    }
}

// --------------------------------------------------------------------------
// clear_parameter_override
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.clear_parameter_override", "material.authoring",
    "Clear a single parameter override on a UMaterialInstanceConstant, returning it to the parent value. UE 5.6 has no per-parameter Clear*Editor API; this RemoveAll's the matching entry from the override array (or static switch list) directly.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Path to UMaterialInstanceConstant")),
        RPC_PARAM_REQ("parameterName", "string", "Parameter name"),
        RPC_PARAM_REQ("parameterType", "string", "One of scalar|vector|texture|staticSwitch"),
        RPC_PARAM_OPT("save", "boolean", "Save the asset (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;
    FString ParamType;
    if (!Ctx.RequireString(TEXT("parameterType"), ParamType)) return true;

    UMaterialInstanceConstant* Instance = LoadMaterialInstanceOrError(Ctx, AssetPath);
    if (!Instance) return true;

    const FName ParamFName(*ParamName);
    int32 RemovedCount = 0;

    if (ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
    {
        RemovedCount = Instance->ScalarParameterValues.RemoveAll(
            [&](const FScalarParameterValue& V) { return V.ParameterInfo.Name == ParamFName; });
    }
    else if (ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
    {
        RemovedCount = Instance->VectorParameterValues.RemoveAll(
            [&](const FVectorParameterValue& V) { return V.ParameterInfo.Name == ParamFName; });
    }
    else if (ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
    {
        RemovedCount = Instance->TextureParameterValues.RemoveAll(
            [&](const FTextureParameterValue& V) { return V.ParameterInfo.Name == ParamFName; });
    }
    else if (ParamType.Equals(TEXT("staticSwitch"), ESearchCase::IgnoreCase))
    {
        // Static switches live in the private StaticParametersRuntime array.
        // Mutate via a copy returned from GetStaticParameters() + UpdateStaticPermutation.
        FStaticParameterSet Params = Instance->GetStaticParameters();
        RemovedCount = Params.StaticSwitchParameters.RemoveAll(
            [&](const FStaticSwitchParameter& P) { return P.ParameterInfo.Name == ParamFName; });
        if (RemovedCount > 0)
        {
            Instance->UpdateStaticPermutation(Params);
        }
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER_TYPE"),
            FString::Printf(TEXT("parameterType must be one of scalar|vector|texture|staticSwitch (got '%s')."), *ParamType));
        return true;
    }

    Instance->PostEditChange();
    Instance->MarkPackageDirty();
    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialInstanceAsset(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Instance);
    Result->SetBoolField(TEXT("cleared"), RemovedCount > 0);
    Result->SetStringField(TEXT("parameterName"), ParamName);
    Result->SetStringField(TEXT("parameterType"), ParamType);

    UMaterialInterface* Parent = Instance->Parent;
    if (Parent)
    {
        const FHashedMaterialParameterInfo HashedInfo(ParamFName);
        if (ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
        {
            float Val = 0.0f;
            if (Parent->GetScalarParameterDefaultValue(HashedInfo, Val))
            {
                Result->SetNumberField(TEXT("inheritedValue"), Val);
            }
        }
        else if (ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
        {
            FLinearColor Val(ForceInit);
            if (Parent->GetVectorParameterDefaultValue(HashedInfo, Val))
            {
                Result->SetObjectField(TEXT("inheritedValue"), JsonBuilders::BuildLinearColorJson(Val));
            }
        }
        else if (ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
        {
            UTexture* Val = nullptr;
            if (Parent->GetTextureParameterDefaultValue(HashedInfo, Val) && Val)
            {
                Result->SetStringField(TEXT("inheritedValue"), Val->GetPathName());
            }
        }
        else if (ParamType.Equals(TEXT("staticSwitch"), ESearchCase::IgnoreCase))
        {
            bool Val = false;
            FGuid ExprGuid;
            if (Parent->GetStaticSwitchParameterDefaultValue(HashedInfo, Val, ExprGuid))
            {
                Result->SetBoolField(TEXT("inheritedValue"), Val);
            }
        }
    }

    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_static_switch_parameter_value
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.set_static_switch_parameter_value", "material.authoring",
    "Override a static switch parameter on a UMaterialInstanceConstant. Triggers a single static permutation rebuild via FMaterialInstanceParameterUpdateContext.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material instance asset path")),
        RPC_PARAM_REQ("parameterName", "string", "Parameter name"),
        RPC_PARAM_REQ("value", "boolean", "Switch value"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;
    bool Value = false;
    if (!Ctx.RequireBool(TEXT("value"), Value)) return true;

    UMaterialInstanceConstant* Instance = LoadMaterialInstanceOrError(Ctx, AssetPath);
    if (!Instance) return true;

    {
        // Route the override through the context: SetParameterValueEditorOnly
        // dispatches a static-switch-typed value into UpdateCtx's own static set,
        // which the dtor commits via UpdateStaticPermutation. Writing on the
        // Instance directly is clobbered by the dtor's stale snapshot.
        FMaterialInstanceParameterUpdateContext UpdateCtx(Instance, EMaterialInstanceClearParameterFlag::None);
        UpdateCtx.SetParameterValueEditorOnly(FMaterialParameterInfo(FName(*ParamName)), FMaterialParameterMetadata(FMaterialParameterValue(Value)));
    }
    Instance->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialInstanceAsset(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Instance);
    Result->SetStringField(TEXT("parameterName"), ParamName);
    Result->SetBoolField(TEXT("value"), Value);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_material_instance_parent
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.set_material_instance_parent", "material.authoring",
    "Reassign the parent material of a UMaterialInstanceConstant. With preserveOverrides=false, also clears all existing overrides.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material instance asset path")),
        RPC_PARAM_REQ("parentMaterial", "path", "New parent material asset path"),
        RPC_PARAM_OPT("preserveOverrides", "boolean", "Keep existing overrides (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString ParentPath;
    if (!Ctx.RequireAssetPath(TEXT("parentMaterial"), ParentPath)) return true;
    const bool bPreserve = Ctx.GetBool(TEXT("preserveOverrides"), true);

    UMaterialInstanceConstant* Instance = LoadMaterialInstanceOrError(Ctx, AssetPath);
    if (!Instance) return true;

    UMaterialInterface* NewParent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
    if (!NewParent)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load parent material interface."));
        return true;
    }

    Instance->SetParentEditorOnly(NewParent, /*RecacheShader=*/ true);
    if (!bPreserve)
    {
        Instance->ClearParameterValuesEditorOnly();
    }
    Instance->PostEditChange();
    Instance->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialInstanceAsset(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Instance);
    Result->SetStringField(TEXT("parent"), NewParent->GetPathName());
    Result->SetBoolField(TEXT("preserveOverrides"), bPreserve);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// get_material_instance_info
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.get_material_instance_info", "material.authoring",
    "Read parent + per-type overrides + inherited (parent default) values + the parent's parameter metadata for a UMaterialInstanceConstant. "
    "`usage` reports the EMaterialUsage set in effect on the instance next to the parent's - `declared`, `parentDeclared`, and `flags[]` rows carrying the UMaterial property behind each flag plus whether the instance overrides it. A consumer whose usage is not declared draws the engine Default Material, and shaderCompile cannot see it: the permutations of an undeclared usage are never compiled into the shader map. "
    "`shaderCompile.measuredSubject` says whose shader map the compile fields describe - `instanceStaticPermutation` when the instance owns one, `parentInherited` when it does not and the parent's resource was read instead.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material instance asset path"))
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    UMaterialInstanceConstant* Instance = LoadMaterialInstanceOrError(Ctx, AssetPath);
    if (!Instance) return true;

    // Build the dump-shared subset (parent, parentChain, overrides, basePropertyOverrides,
    // materialLayers, parentLightmassSettings, nanitePassthrough, phys*, subsurfaceProfile,
    // flags). This keeps live read output byte-aligned with the asset.dump sidecar.
    TSharedPtr<FJsonObject> Result = MaterialInstanceDumpBuilder::BuildMaterialInstanceJson(Instance);
    if (!Result.IsValid())
    {
        Result = MakeShared<FJsonObject>();
    }
    PinWright::Material::AddMaterialVerification(Result, Instance);

    UMaterialInterface* Parent = Instance->Parent;

    // Usage flags, beside the parent's. An instance inherits the parent's set and may override
    // individual bits, so the caller of an instance-only task needs both columns: the flag that
    // decides whether this instance renders on a given consumer lives on the parent unless the
    // instance overrode it, and property.set must then be aimed at the right asset.
    PinWright::MaterialUsage::AddUsageReport(Result, Instance, Parent);

    // ---- inherited (parent default values) + parameters metadata ----
    TSharedPtr<FJsonObject> Inherited = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ParametersArr;
    if (Parent)
    {
        TSharedPtr<FJsonObject> InhScalar = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> InhVector = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> InhTexture = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> InhStaticSwitch = MakeShared<FJsonObject>();

        auto WalkType = [&](EMaterialParameterType Type, const TCHAR* TypeName)
        {
            TArray<FMaterialParameterInfo> Infos;
            TArray<FGuid> Guids;
            Parent->GetAllParameterInfoOfType(Type, Infos, Guids);
            for (const FMaterialParameterInfo& Info : Infos)
            {
                const FHashedMaterialParameterInfo HashedInfo(Info);
                const FString Name = Info.Name.ToString();

                // Inherited (parent default) value
                if (Type == EMaterialParameterType::Scalar)
                {
                    float V = 0.0f;
                    if (Parent->GetScalarParameterDefaultValue(HashedInfo, V))
                    {
                        InhScalar->SetNumberField(Name, V);
                    }
                }
                else if (Type == EMaterialParameterType::Vector)
                {
                    FLinearColor V(ForceInit);
                    if (Parent->GetVectorParameterDefaultValue(HashedInfo, V))
                    {
                        InhVector->SetObjectField(Name, JsonBuilders::BuildLinearColorJson(V));
                    }
                }
                else if (Type == EMaterialParameterType::Texture)
                {
                    UTexture* V = nullptr;
                    if (Parent->GetTextureParameterDefaultValue(HashedInfo, V) && V)
                    {
                        InhTexture->SetStringField(Name, V->GetPathName());
                    }
                }
                else if (Type == EMaterialParameterType::StaticSwitch)
                {
                    bool V = false;
                    FGuid ExprGuid;
                    if (Parent->GetStaticSwitchParameterDefaultValue(HashedInfo, V, ExprGuid))
                    {
                        InhStaticSwitch->SetBoolField(Name, V);
                    }
                }

                TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
                ParamObj->SetStringField(TEXT("name"), Name);
                ParamObj->SetStringField(TEXT("type"), TypeName);
                FName GroupName;
                if (Parent->GetGroupName(HashedInfo, GroupName) && !GroupName.IsNone())
                {
                    ParamObj->SetStringField(TEXT("group"), GroupName.ToString());
                }
                int32 SortPriority = 32;
                if (!Parent->GetParameterSortPriority(HashedInfo, SortPriority))
                {
                    SortPriority = 32;
                }
                ParamObj->SetNumberField(TEXT("sortPriority"), SortPriority);
                ParametersArr.Add(MakeShared<FJsonValueObject>(ParamObj));
            }
        };

        WalkType(EMaterialParameterType::Scalar, TEXT("scalar"));
        WalkType(EMaterialParameterType::Vector, TEXT("vector"));
        WalkType(EMaterialParameterType::Texture, TEXT("texture"));
        WalkType(EMaterialParameterType::StaticSwitch, TEXT("staticSwitch"));

        Inherited->SetObjectField(TEXT("scalar"), InhScalar);
        Inherited->SetObjectField(TEXT("vector"), InhVector);
        Inherited->SetObjectField(TEXT("texture"), InhTexture);
        Inherited->SetObjectField(TEXT("staticSwitch"), InhStaticSwitch);
    }
    else
    {
        Inherited->SetObjectField(TEXT("scalar"), MakeShared<FJsonObject>());
        Inherited->SetObjectField(TEXT("vector"), MakeShared<FJsonObject>());
        Inherited->SetObjectField(TEXT("texture"), MakeShared<FJsonObject>());
        Inherited->SetObjectField(TEXT("staticSwitch"), MakeShared<FJsonObject>());
    }
    Result->SetObjectField(TEXT("inherited"), Inherited);
    Result->SetArrayField(TEXT("parameters"), ParametersArr);

    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_material_instance_parameters
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.set_material_instance_parameters", "material.authoring",
    "Batch-apply scalar/vector/texture/staticSwitch overrides on a UMaterialInstanceConstant. Wraps every set in a single FMaterialInstanceParameterUpdateContext so one PostEditChange / shader recompile pass covers them all.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material instance asset path")),
        RPC_PARAM_OPT("scalar", "object", "Map of {ParamName: number}"),
        RPC_PARAM_OPT("vector", "object", "Map of {ParamName: {r,g,b,a}}"),
        RPC_PARAM_OPT("texture", "object", "Map of {ParamName: TextureAssetPath}"),
        RPC_PARAM_OPT("staticSwitch", "object", "Map of {ParamName: bool}"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    UMaterialInstanceConstant* Instance = LoadMaterialInstanceOrError(Ctx, AssetPath);
    if (!Instance) return true;

    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Failed;

    // This verb takes the four type maps at the top level of args; the shared
    // apply helper takes them nested under one object (the same shape the inline
    // `parameters` slot on create_material_instance uses). Re-nest, then delegate
    // so both verbs share one apply path / one update-context recompile.
    TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
    for (const TCHAR* Key : { TEXT("scalar"), TEXT("vector"), TEXT("texture"), TEXT("staticSwitch") })
    {
        TSharedPtr<FJsonObject> Map = Ctx.GetObject(Key);
        if (Map.IsValid())
        {
            ParamsObj->SetObjectField(Key, Map);
        }
    }
    ApplyMaterialInstanceParameterOverrides(Instance, ParamsObj, Applied, Failed);

    Instance->MarkPackageDirty();
    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialInstanceAsset(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Instance);
    Result->SetArrayField(TEXT("applied"), Applied);
    Result->SetArrayField(TEXT("failed"), Failed);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_material_instance_base_property_overrides
// --------------------------------------------------------------------------

// One boolean slot of FMaterialInstanceBasePropertyOverrides: the wire name, the bOverride_*
// gate, the value it gates, and the engine accessor that reports the EFFECTIVE value once the
// write has landed. Function pointers rather than pointer-to-member because every one of these
// is a `uint8 x : 1` bitfield and C++ forbids taking a bitfield's address; captureless lambdas
// decay to function pointers, so the table stays a plain static array.
// Field spellings verified against UE 5.8
// Engine/Source/Runtime/Engine/Public/Materials/MaterialInstanceBasePropertyOverrides.h — note
// the flag names are NOT uniform (bOverride_TwoSided but bOverride_bIsThinSurface), which is
// exactly why they are written out once here instead of derived from the wire name.
struct FInstanceBoolOverrideSlot
{
    const TCHAR* WireName;
    bool (*GetOverride)(const FMaterialInstanceBasePropertyOverrides&);
    void (*SetOverride)(FMaterialInstanceBasePropertyOverrides&, bool);
    bool (*GetValue)(const FMaterialInstanceBasePropertyOverrides&);
    void (*SetValue)(FMaterialInstanceBasePropertyOverrides&, bool);
    bool (*GetEffective)(const UMaterialInstance&);
};

#define PW_INSTANCE_BOOL_OVERRIDE(WireName, FlagField, ValueField, EffectiveExpr)                    \
    FInstanceBoolOverrideSlot{                                                                       \
        TEXT(WireName),                                                                              \
        [](const FMaterialInstanceBasePropertyOverrides& O) -> bool { return O.FlagField != 0; },    \
        [](FMaterialInstanceBasePropertyOverrides& O, bool b) { O.FlagField = b ? 1 : 0; },          \
        [](const FMaterialInstanceBasePropertyOverrides& O) -> bool { return O.ValueField != 0; },   \
        [](FMaterialInstanceBasePropertyOverrides& O, bool b) { O.ValueField = b ? 1 : 0; },         \
        [](const UMaterialInstance& MI) -> bool { return EffectiveExpr; } }

static const FInstanceBoolOverrideSlot* GetInstanceBoolOverrideSlots(int32& OutNum)
{
    static const FInstanceBoolOverrideSlot Table[] = {
        PW_INSTANCE_BOOL_OVERRIDE("twoSided",                 bOverride_TwoSided,                       TwoSided,                        MI.IsTwoSided()),
        PW_INSTANCE_BOOL_OVERRIDE("isThinSurface",            bOverride_bIsThinSurface,                 bIsThinSurface,                  MI.IsThinSurface()),
        PW_INSTANCE_BOOL_OVERRIDE("ditheredLODTransition",    bOverride_DitheredLODTransition,          DitheredLODTransition,           MI.IsDitheredLODTransition()),
        PW_INSTANCE_BOOL_OVERRIDE("castDynamicShadowAsMasked", bOverride_CastDynamicShadowAsMasked,     bCastDynamicShadowAsMasked,      MI.GetCastDynamicShadowAsMasked()),
        PW_INSTANCE_BOOL_OVERRIDE("outputTranslucentVelocity", bOverride_OutputTranslucentVelocity,     bOutputTranslucentVelocity,      MI.IsTranslucencyWritingVelocity()),
        // bHasPixelAnimation / bEnableTessellation, their bOverride_* gates and the
        // HasPixelAnimation() / IsTessellationEnabled() accessors all arrived in UE 5.4. Same
        // treatment as the two slots below: off the table on 5.3, refused by name in the handler
        // body so a caller learns why rather than reading `applied` back without them.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        PW_INSTANCE_BOOL_OVERRIDE("hasPixelAnimation",        bOverride_bHasPixelAnimation,             bHasPixelAnimation,              MI.HasPixelAnimation()),
        PW_INSTANCE_BOOL_OVERRIDE("enableTessellation",       bOverride_bEnableTessellation,            bEnableTessellation,             MI.IsTessellationEnabled()),
#endif
        // bEnableDisplacementFade / bOverride_bEnableDisplacementFade /
        // IsDisplacementFadeEnabled() all arrived in UE 5.5, one release after tessellation.
        // Same treatment as the Lumen slot below: off the table here, refused by name in the
        // handler body.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        PW_INSTANCE_BOOL_OVERRIDE("enableDisplacementFade",   bOverride_bEnableDisplacementFade,        bEnableDisplacementFade,         MI.IsDisplacementFadeEnabled()),
#endif
        // bCompatibleWithLumenCardSharing / bOverride_CompatibleWithLumenCardSharing /
        // IsCompatibleWithLumenCardSharing() all arrived in UE 5.6. Off the table on older
        // engines, and the handler body refuses the wire name there rather than dropping it.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        PW_INSTANCE_BOOL_OVERRIDE("compatibleWithLumenCardSharing", bOverride_CompatibleWithLumenCardSharing, bCompatibleWithLumenCardSharing, MI.IsCompatibleWithLumenCardSharing()),
#endif
    };
    OutNum = UE_ARRAY_COUNT(Table);
    return Table;
}

#undef PW_INSTANCE_BOOL_OVERRIDE

// The two non-bool, non-enum slots, kept beside the bool table so `clear` and the "which names
// exist" validation see one list.
static const TCHAR* const GInstanceFloatOverrideNames[] = {
    TEXT("opacityMaskClipValue"),
    TEXT("maxWorldPositionOffsetDisplacement"),
};
static const TCHAR* const GInstanceEnumOverrideNames[] = {
    TEXT("blendMode"),
    TEXT("shadingModel"),
};

REGISTER_RPC_HANDLER("material.authoring.set_material_instance_base_property_overrides", "material.authoring",
    "Override base material properties (blend mode, shading model, two-sided, opacity mask clip value, ...) on a UMaterialInstanceConstant, so a translucent/two-sided variant of a master needs an instance rather than a second master material. Every override param is optional and independent: omitting one leaves that override exactly as it was. To turn an override back OFF pass its name in the `clear` array (or `clear:[\"all\"]`); `clear` is applied before the value params, so naming a slot in both ends up set. Writes go through FMaterialInstanceParameterUpdateContext, which rebuilds the static permutation — a bare field write would read back correctly and render nothing. The response returns the overrides now active plus the resulting effective values, so no second call is needed to verify.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material instance asset path")),
        RPC_PARAM_OPT("blendMode", "string", "Override the blend mode: Opaque|Masked|Translucent|Additive|Modulate|AlphaComposite|AlphaHoldout|TranslucentColoredTransmittance"),
        RPC_PARAM_OPT("shadingModel", "string", "Override the shading model: Unlit|DefaultLit|Subsurface|SubsurfaceProfile|PreintegratedSkin|ClearCoat|Hair|Cloth|Eye|TwoSidedFoliage|SingleLayerWater|ThinTranslucent"),
        RPC_PARAM_OPT("twoSided", "boolean", "Override two-sided rendering"),
        RPC_PARAM_OPT("isThinSurface", "boolean", "Override the thin-surface flag"),
        RPC_PARAM_OPT("opacityMaskClipValue", "number", "Override the masked-blend clip threshold"),
        RPC_PARAM_OPT("ditheredLODTransition", "boolean", "Override dithered LOD transition (foliage)"),
        RPC_PARAM_OPT("castDynamicShadowAsMasked", "boolean", "Override casting translucent shadows as masked"),
        RPC_PARAM_OPT("outputTranslucentVelocity", "boolean", "Override velocity output for translucent surfaces"),
        RPC_PARAM_OPT("hasPixelAnimation", "boolean", "Override the has-pixel-animation hint"),
        RPC_PARAM_OPT("enableTessellation", "boolean", "Override tessellation (required for displacement)"),
        RPC_PARAM_OPT("enableDisplacementFade", "boolean", "Override displacement fade"),
        RPC_PARAM_OPT("maxWorldPositionOffsetDisplacement", "number", "Override the max WPO distance (0 = no maximum)"),
        RPC_PARAM_OPT("compatibleWithLumenCardSharing", "boolean", "Override Lumen card sharing compatibility"),
        RPC_PARAM_OPT("clear", "array", "Override names to turn back OFF (inherit from the parent again), e.g. [\"blendMode\",\"twoSided\"]. Pass [\"all\"] to clear every override this verb writes. Names that are not override slots are echoed back in unknownClear rather than failing the call."),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    // Same wrong-class vocabulary the UMaterial-only verbs now use, mirrored: a UMaterial here
    // gets UNSUPPORTED_ASSET_CLASS naming the class, not ASSET_NOT_FOUND.
    UMaterialInstanceConstant* Instance = LoadMaterialInstanceOrError(Ctx, AssetPath);
    if (!Instance) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    int32 NumBoolSlots = 0;
    const FInstanceBoolOverrideSlot* BoolSlots = GetInstanceBoolOverrideSlots(NumBoolSlots);

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // The one slot this engine has no field for. Refused by name so a caller who sets it learns
    // that, rather than reading `applied` back without it and having to notice the absence.
    if (Payload.IsValid() && Payload->HasField(TEXT("compatibleWithLumenCardSharing")))
    {
        Ctx.SendError(TEXT("UNSUPPORTED_ENGINE_VERSION"), TEXT(
            "compatibleWithLumenCardSharing needs FMaterialInstanceBasePropertyOverrides::"
            "bOverride_CompatibleWithLumenCardSharing, added in UE 5.6. Every other override on "
            "this verb works on this engine."));
        return true;
    }
#endif

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // Same treatment for the two slots UE 5.4 introduced.
    if (Payload.IsValid() && Payload->HasField(TEXT("hasPixelAnimation")))
    {
        Ctx.SendError(TEXT("UNSUPPORTED_ENGINE_VERSION"), TEXT(
            "hasPixelAnimation needs FMaterialInstanceBasePropertyOverrides::"
            "bOverride_bHasPixelAnimation, added in UE 5.4. Every other override on "
            "this verb works on this engine."));
        return true;
    }
    if (Payload.IsValid() && Payload->HasField(TEXT("enableTessellation")))
    {
        Ctx.SendError(TEXT("UNSUPPORTED_ENGINE_VERSION"), TEXT(
            "enableTessellation needs FMaterialInstanceBasePropertyOverrides::"
            "bOverride_bEnableTessellation, added in UE 5.4. Every other override on "
            "this verb works on this engine."));
        return true;
    }
#endif

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    // Same treatment for the displacement-fade slot, which this engine has no field for.
    if (Payload.IsValid() && Payload->HasField(TEXT("enableDisplacementFade")))
    {
        Ctx.SendError(TEXT("UNSUPPORTED_ENGINE_VERSION"), TEXT(
            "enableDisplacementFade needs FMaterialInstanceBasePropertyOverrides::"
            "bOverride_bEnableDisplacementFade, added in UE 5.5. Every other override on "
            "this verb works on this engine."));
        return true;
    }
#endif

    // Parse `clear` first. Names are matched case-insensitively by lowercasing both sides; an
    // unrecognised name is reported rather than silently dropped, because a typo'd clear that
    // reports success is indistinguishable from a clear that did nothing.
    TSet<FString> ClearRequested;
    bool bClearAll = false;
    TArray<TSharedPtr<FJsonValue>> UnknownClear;
    if (const TArray<TSharedPtr<FJsonValue>>* ClearArray = Ctx.GetArray(TEXT("clear")))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *ClearArray)
        {
            FString Name;
            if (!Entry.IsValid() || !Entry->TryGetString(Name)) continue;
            Name = Name.TrimStartAndEnd();
            if (Name.Equals(TEXT("all"), ESearchCase::IgnoreCase)) { bClearAll = true; continue; }
            ClearRequested.Add(Name.ToLower());
        }

        // Anything left over after removing every known slot name is a typo.
        TSet<FString> Known;
        for (int32 i = 0; i < NumBoolSlots; ++i) Known.Add(FString(BoolSlots[i].WireName).ToLower());
        for (const TCHAR* N : GInstanceFloatOverrideNames) Known.Add(FString(N).ToLower());
        for (const TCHAR* N : GInstanceEnumOverrideNames)  Known.Add(FString(N).ToLower());
        for (const FString& Requested : ClearRequested)
        {
            if (!Known.Contains(Requested))
            {
                UnknownClear.Add(MakeShared<FJsonValueString>(Requested));
            }
        }
    }

    auto WantsClear = [&](const TCHAR* WireName)
    {
        return bClearAll || ClearRequested.Contains(FString(WireName).ToLower());
    };

    // Mutate a COPY: FMaterialInstanceParameterUpdateContext snapshots the instance's current
    // overrides in its constructor, so a struct edited in place before the context opens would
    // be read back by that snapshot and the staged value would be a no-op diff.
    FMaterialInstanceBasePropertyOverrides NewOverrides = Instance->BasePropertyOverrides;

    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Cleared;
    auto NoteApplied = [&Applied](const TCHAR* WireName) { Applied.Add(MakeShared<FJsonValueString>(WireName)); };
    auto NoteCleared = [&Cleared](const TCHAR* WireName) { Cleared.Add(MakeShared<FJsonValueString>(WireName)); };

    for (int32 i = 0; i < NumBoolSlots; ++i)
    {
        const FInstanceBoolOverrideSlot& Slot = BoolSlots[i];
        if (WantsClear(Slot.WireName))
        {
            Slot.SetOverride(NewOverrides, false);
            NoteCleared(Slot.WireName);
        }
        bool bValue = false;
        if (Payload.IsValid() && Payload->TryGetBoolField(Slot.WireName, bValue))
        {
            Slot.SetOverride(NewOverrides, true);
            Slot.SetValue(NewOverrides, bValue);
            NoteApplied(Slot.WireName);
        }
    }

    if (WantsClear(TEXT("blendMode")))
    {
        NewOverrides.bOverride_BlendMode = 0;
        NoteCleared(TEXT("blendMode"));
    }
    FString BlendModeStr;
    if (Payload.IsValid() && Payload->TryGetStringField(TEXT("blendMode"), BlendModeStr))
    {
        EBlendMode ParsedBlend = BLEND_Opaque;
        if (!TryParseBlendMode(BlendModeStr, ParsedBlend))
        {
            Ctx.SendError(TEXT("INVALID_BLEND_MODE"),
                FString::Printf(TEXT("Unknown blendMode '%s'. Expected one of: %s."),
                    *BlendModeStr, *GetBlendModeNameList()));
            return true;
        }
        NewOverrides.bOverride_BlendMode = 1;
        NewOverrides.BlendMode = ParsedBlend;
        NoteApplied(TEXT("blendMode"));
    }

    if (WantsClear(TEXT("shadingModel")))
    {
        NewOverrides.bOverride_ShadingModel = 0;
        NoteCleared(TEXT("shadingModel"));
    }
    FString ShadingModelStr;
    if (Payload.IsValid() && Payload->TryGetStringField(TEXT("shadingModel"), ShadingModelStr))
    {
        EMaterialShadingModel ParsedModel = MSM_DefaultLit;
        if (!TryParseShadingModel(ShadingModelStr, ParsedModel))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("Unknown shadingModel '%s'. Expected one of: %s."),
                    *ShadingModelStr, *GetShadingModelNameList()));
            return true;
        }
        NewOverrides.bOverride_ShadingModel = 1;
        NewOverrides.ShadingModel = ParsedModel;
        NoteApplied(TEXT("shadingModel"));
    }

    if (WantsClear(TEXT("opacityMaskClipValue")))
    {
        NewOverrides.bOverride_OpacityMaskClipValue = 0;
        NoteCleared(TEXT("opacityMaskClipValue"));
    }
    double ClipValue = 0.0;
    if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("opacityMaskClipValue"), ClipValue))
    {
        NewOverrides.bOverride_OpacityMaskClipValue = 1;
        NewOverrides.OpacityMaskClipValue = static_cast<float>(ClipValue);
        NoteApplied(TEXT("opacityMaskClipValue"));
    }

    if (WantsClear(TEXT("maxWorldPositionOffsetDisplacement")))
    {
        NewOverrides.bOverride_MaxWorldPositionOffsetDisplacement = 0;
        NoteCleared(TEXT("maxWorldPositionOffsetDisplacement"));
    }
    double MaxWpo = 0.0;
    if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("maxWorldPositionOffsetDisplacement"), MaxWpo))
    {
        NewOverrides.bOverride_MaxWorldPositionOffsetDisplacement = 1;
        NewOverrides.MaxWorldPositionOffsetDisplacement = static_cast<float>(MaxWpo);
        NoteApplied(TEXT("maxWorldPositionOffsetDisplacement"));
    }

    // The write path. BasePropertyOverrides feed the STATIC PERMUTATION, so assigning
    // Instance->BasePropertyOverrides directly produces a change that reads back correctly in
    // memory and renders nothing. Route through FMaterialInstanceParameterUpdateContext exactly
    // as the engine's own editor does in UMaterialEditingLibrary::SetMaterialUsageOverride
    // (Engine/Source/Editor/MaterialEditor/Private/MaterialEditingLibrary.cpp:853-902): the ctor
    // snapshots the current static parameter set, SetBasePropertyOverrides stages the new
    // struct, and the dtor commits both through UMaterialInstance::UpdateStaticPermutation —
    // which assigns the struct, re-runs UpdateOverridableBaseProperties() so the cached base
    // properties match, then UpdateCachedData + CacheResourceShadersForRendering +
    // RecacheUniformExpressions when the permutation actually changed
    // (MaterialInstance.cpp:4591-4645). Scoped so the dtor has run before anything is read back.
    {
        FMaterialInstanceParameterUpdateContext UpdateCtx(Instance);
        UpdateCtx.SetBasePropertyOverrides(NewOverrides);
    }

    Instance->MarkPackageDirty();
    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    const bool bSavedToDisk = bSave && SaveMaterialInstanceAsset(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    PinWright::Material::AddMaterialVerification(Result, Instance);
    Result->SetArrayField(TEXT("applied"), Applied);
    Result->SetArrayField(TEXT("cleared"), Cleared);
    if (UnknownClear.Num() > 0)
    {
        Result->SetArrayField(TEXT("unknownClear"), UnknownClear);
    }

    // Everything below is read off the INSTANCE after the update context committed, never off
    // the local NewOverrides copy — that is the difference between reporting what was asked for
    // and reporting what the asset now holds.
    const FMaterialInstanceBasePropertyOverrides& Live = Instance->BasePropertyOverrides;

    TArray<TSharedPtr<FJsonValue>> ActiveOverrides;
    for (int32 i = 0; i < NumBoolSlots; ++i)
    {
        if (BoolSlots[i].GetOverride(Live))
        {
            ActiveOverrides.Add(MakeShared<FJsonValueString>(BoolSlots[i].WireName));
        }
    }
    if (Live.bOverride_BlendMode)    ActiveOverrides.Add(MakeShared<FJsonValueString>(TEXT("blendMode")));
    if (Live.bOverride_ShadingModel) ActiveOverrides.Add(MakeShared<FJsonValueString>(TEXT("shadingModel")));
    if (Live.bOverride_OpacityMaskClipValue)
        ActiveOverrides.Add(MakeShared<FJsonValueString>(TEXT("opacityMaskClipValue")));
    if (Live.bOverride_MaxWorldPositionOffsetDisplacement)
        ActiveOverrides.Add(MakeShared<FJsonValueString>(TEXT("maxWorldPositionOffsetDisplacement")));
    Result->SetArrayField(TEXT("overridden"), ActiveOverrides);

    // Effective values through the engine's own accessors, which resolve override-or-inherit for
    // us — so a cleared slot reports the parent's value here without a second call.
    TSharedPtr<FJsonObject> Effective = MakeShared<FJsonObject>();
    Effective->SetStringField(TEXT("blendMode"), MaterialBlendModeToString(Instance->GetBlendMode()));
    {
        const FMaterialShadingModelField Models = Instance->GetShadingModels();
        int32 NumShadingNames = 0;
        const FMaterialEnumName* ShadingTable = GetShadingModelNames(NumShadingNames);
        // GetFirstShadingModel() check()s IsValid(), so a MaterialAttributes-driven parent with
        // no shading model must not reach it.
        Effective->SetStringField(TEXT("shadingModel"),
            Models.IsValid()
                ? MaterialEnumToString(ShadingTable, NumShadingNames,
                    static_cast<int32>(Models.GetFirstShadingModel()))
                : FString(TEXT("None")));
    }
    Effective->SetNumberField(TEXT("opacityMaskClipValue"), Instance->GetOpacityMaskClipValue());
    Effective->SetNumberField(TEXT("maxWorldPositionOffsetDisplacement"),
        Instance->GetMaxWorldPositionOffsetDisplacement());
    for (int32 i = 0; i < NumBoolSlots; ++i)
    {
        Effective->SetBoolField(BoolSlots[i].WireName, BoolSlots[i].GetEffective(*Instance));
    }
    Result->SetObjectField(TEXT("effective"), Effective);

    // Raw struct echo through the SAME builder get_material_instance_info / asset.dump use, so
    // this write verb's read-back is field-for-field the read verb's shape instead of a second
    // spelling of one struct.
    if (TSharedPtr<FJsonObject> Dump = MaterialInstanceDumpBuilder::BuildMaterialInstanceJson(Instance))
    {
        const TSharedPtr<FJsonObject>* BasePropertyOverridesJson = nullptr;
        if (Dump->TryGetObjectField(TEXT("basePropertyOverrides"), BasePropertyOverridesJson)
            && BasePropertyOverridesJson && (*BasePropertyOverridesJson).IsValid())
        {
            Result->SetObjectField(TEXT("basePropertyOverrides"), *BasePropertyOverridesJson);
        }
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// 8.5 Specialized Materials
// ============================================================================

// --------------------------------------------------------------------------
// create_landscape_material
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.create_landscape_material", "material.authoring",
    "Create a UMaterial pre-configured for landscape use (Surface domain, Opaque blend). The graph is EMPTY: it declares no target layers, so a landscape using it as-is has nothing paintable. Follow with material.authoring.configure_layer_blend to declare the layers and wire them in - that, not add_landscape_layer, is what makes a landscape paintable.",
    RPC_PARAMS(
        MaterialCreatePathParamUtils::MaterialCreateNameParamReq(TEXT("Material name (leaf). Or pass one combined 'assetPath' to set name + folder together.")),
        MaterialCreatePathParamUtils::MaterialCreateFolderParamOpt(TEXT("Destination folder (default /Game/Materials)")),
        RPC_PARAM_OPT("save", "boolean", "Save after creation (default true)")
    ))
{
    FString Name;
    FString Path;
    FString PackagePath;
    if (!MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath(Ctx, TEXT("/Game/Materials"), Name, Path, PackagePath)) return true;

    UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package) { Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package.")); return true; }

    UMaterial* NewMaterial = Cast<UMaterial>(
        Factory->FactoryCreateNew(UMaterial::StaticClass(), Package,
            FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewMaterial) { Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create material.")); return true; }

    NewMaterial->MaterialDomain = EMaterialDomain::MD_Surface;
    NewMaterial->BlendMode = EBlendMode::BLEND_Opaque;
    NewMaterial->PostEditChange();
    NewMaterial->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(NewMaterial);
    FAssetRegistryModule::AssetCreated(NewMaterial);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, NewMaterial);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// create_decal_material
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.create_decal_material", "material.authoring",
    "Create a UMaterial pre-configured for deferred decal use (DeferredDecal domain, Translucent blend). Project decals via ADecalActor or DecalComponent referencing the resulting asset.",
    RPC_PARAMS(
        MaterialCreatePathParamUtils::MaterialCreateNameParamReq(TEXT("Material name (leaf). Or pass one combined 'assetPath' to set name + folder together.")),
        MaterialCreatePathParamUtils::MaterialCreateFolderParamOpt(TEXT("Destination folder (default /Game/Materials)")),
        RPC_PARAM_OPT("save", "boolean", "Save after creation (default true)")
    ))
{
    FString Name;
    FString Path;
    FString PackagePath;
    if (!MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath(Ctx, TEXT("/Game/Materials"), Name, Path, PackagePath)) return true;

    UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package) { Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package.")); return true; }

    UMaterial* NewMaterial = Cast<UMaterial>(
        Factory->FactoryCreateNew(UMaterial::StaticClass(), Package,
            FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewMaterial) { Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create material.")); return true; }

    NewMaterial->MaterialDomain = EMaterialDomain::MD_DeferredDecal;
    NewMaterial->BlendMode = EBlendMode::BLEND_Translucent;
    NewMaterial->PostEditChange();
    NewMaterial->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(NewMaterial);
    FAssetRegistryModule::AssetCreated(NewMaterial);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, NewMaterial);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// create_post_process_material
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.create_post_process_material", "material.authoring",
    "Create a UMaterial pre-configured for post-process effects (PostProcess domain, Opaque blend). Reference from a post-process volume to apply screen-space effects.",
    RPC_PARAMS(
        MaterialCreatePathParamUtils::MaterialCreateNameParamReq(TEXT("Material name (leaf). Or pass one combined 'assetPath' to set name + folder together.")),
        MaterialCreatePathParamUtils::MaterialCreateFolderParamOpt(TEXT("Destination folder (default /Game/Materials)")),
        RPC_PARAM_OPT("save", "boolean", "Save after creation (default true)")
    ))
{
    FString Name;
    FString Path;
    FString PackagePath;
    if (!MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath(Ctx, TEXT("/Game/Materials"), Name, Path, PackagePath)) return true;

    UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package) { Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package.")); return true; }

    UMaterial* NewMaterial = Cast<UMaterial>(
        Factory->FactoryCreateNew(UMaterial::StaticClass(), Package,
            FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewMaterial) { Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create material.")); return true; }

    NewMaterial->MaterialDomain = EMaterialDomain::MD_PostProcess;
    NewMaterial->BlendMode = EBlendMode::BLEND_Opaque;
    NewMaterial->PostEditChange();
    NewMaterial->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(NewMaterial);
    FAssetRegistryModule::AssetCreated(NewMaterial);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, NewMaterial);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// add_landscape_layer
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.add_landscape_layer", "material.authoring",
    "Create a shared ULandscapeLayerInfoObject asset for one target layer. This does NOT make a layer paintable - what declares a target layer is a LandscapeLayerBlend node on the material, which is material.authoring.configure_layer_blend's job. The LayerInfo is bound per LANDSCAPE, not by the material, and the paint verb auto-creates a private one inside the landscape actor's package if none exists; use this verb when the layer is shared between landscapes and needs a real /Game asset instead.",
    RPC_PARAMS(
        RPC_PARAM_REQ("layerName", "string", "Layer name"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Landscape/Layers)"),
        RPC_PARAM_OPT("hardness", "number", "Layer hardness (default 0.5)"),
        RPC_PARAM_OPT("physicalMaterialPath", "path", "Physical material asset path"),
        RPC_PARAM_OPT("noWeightBlend", "boolean", "Disable weight blending"),
        RPC_PARAM_OPT("save", "boolean", "Save after creation (default true)")
    ))
{
    FString LayerName;
    if (!Ctx.RequireString(TEXT("layerName"), LayerName)) return true;

    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game/Landscape/Layers"));

    // The only unguarded create in this file that does not go through
    // MaterialCreatePathParamUtils: both halves arrive raw off the wire, so validate them here
    // with the same shared composer. Without it `layerName: "a//b"` composes "//" and
    // CreatePackage logs Fatal, ending the editor process rather than failing the call
    // (board B-createpackage-unvalidated-paths-plugin-wide). `path` goes in untrimmed: the
    // composer joins with FString::operator/, which absorbs one trailing separator, so
    // `path: "/Game/X/"` is still accepted.
    FString PackageName;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, LayerName, PackageName, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare layer name in 'layerName' and choose the folder "
                                 "with 'path' (default /Game/Landscape/Layers)."), *PathError));
        return true;
    }

    UPackage* Package = CreatePackage(*PackageName);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package."));
        return true;
    }

    ULandscapeLayerInfoObject* LayerInfo = NewObject<ULandscapeLayerInfoObject>(
        Package, FName(*LayerName), RF_Public | RF_Standalone);
    if (!LayerInfo)
    {
        Ctx.SendError(TEXT("CREATION_ERROR"), TEXT("Failed to create layer info."));
        return true;
    }

PRAGMA_DISABLE_DEPRECATION_WARNINGS
    LayerInfo->LayerName = FName(*LayerName);
PRAGMA_ENABLE_DEPRECATION_WARNINGS

    const auto& Payload = Ctx.GetRawPayload();
    double Hardness = 0.5;
    if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("hardness"), Hardness))
    {
PRAGMA_DISABLE_DEPRECATION_WARNINGS
        LayerInfo->Hardness = static_cast<float>(Hardness);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
    }

    FString PhysMaterialPath = Ctx.GetString(TEXT("physicalMaterialPath"));
    if (!PhysMaterialPath.IsEmpty())
    {
        UPhysicalMaterial* PhysMat = LoadObject<UPhysicalMaterial>(nullptr, *PhysMaterialPath);
        if (PhysMat)
        {
PRAGMA_DISABLE_DEPRECATION_WARNINGS
            LayerInfo->PhysMaterial = PhysMat;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
        }
    }

    bool bNoWeightBlend = false;
    if (Payload.IsValid() && Payload->TryGetBoolField(TEXT("noWeightBlend"), bNoWeightBlend))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        LayerInfo->SetBlendMethod(bNoWeightBlend ? ELandscapeTargetLayerBlendMethod::None : ELandscapeTargetLayerBlendMethod::FinalWeightBlending, false);
#else
        LayerInfo->bNoWeightBlend = bNoWeightBlend;
#endif
    }

    bool bSave = Ctx.GetBool(TEXT("save"), true);
    if (bSave)
    {
        LayerInfo->MarkPackageDirty();
    }

    FAssetRegistryModule::AssetCreated(LayerInfo);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    PinWright::Material::AddMaterialVerification(Result, LayerInfo);
    Result->SetStringField(TEXT("layerName"), LayerName);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// configure_layer_blend
// --------------------------------------------------------------------------

// Maps the wire's blendType string onto ELandscapeLayerBlendType. Accepts the engine's own
// enum spelling ("LB_WeightBlend") and the bare form ("WeightBlend" / "Weight"). Returns
// false for anything else, so an unrecognised blend type is rejected instead of silently
// becoming a weight blend. Distinctively named (not anonymous-namespace) because unity
// merges this TU with sibling material handlers (docs/lessons.md).
static bool ParseLandscapeLayerBlendTypeForConfigureVerb(
    const FString& InBlendType,
    TEnumAsByte<ELandscapeLayerBlendType>& OutBlendType)
{
    const FString Normalized = InBlendType.TrimStartAndEnd();
    if (Normalized.IsEmpty()
        || Normalized.Equals(TEXT("LB_WeightBlend"), ESearchCase::IgnoreCase)
        || Normalized.Equals(TEXT("WeightBlend"), ESearchCase::IgnoreCase)
        || Normalized.Equals(TEXT("Weight"), ESearchCase::IgnoreCase))
    {
        OutBlendType = LB_WeightBlend;
        return true;
    }
    if (Normalized.Equals(TEXT("LB_AlphaBlend"), ESearchCase::IgnoreCase)
        || Normalized.Equals(TEXT("AlphaBlend"), ESearchCase::IgnoreCase)
        || Normalized.Equals(TEXT("Alpha"), ESearchCase::IgnoreCase))
    {
        OutBlendType = LB_AlphaBlend;
        return true;
    }
    if (Normalized.Equals(TEXT("LB_HeightBlend"), ESearchCase::IgnoreCase)
        || Normalized.Equals(TEXT("HeightBlend"), ESearchCase::IgnoreCase)
        || Normalized.Equals(TEXT("Height"), ESearchCase::IgnoreCase))
    {
        OutBlendType = LB_HeightBlend;
        return true;
    }
    return false;
}

REGISTER_RPC_HANDLER("material.authoring.configure_layer_blend", "material.authoring",
    "Declare a landscape material's paintable TARGET LAYERS by writing them into a LandscapeLayerBlend node on the material, and wire that node into a material output so the paint is visible. The node is what makes a layer name exist for painting: the engine harvests target layers by calling GetLandscapeLayerNames on every expression, which only the LandscapeLayerBlend / LayerWeight / LayerSample nodes implement. Declarative and re-runnable - it rewrites the material's first LandscapeLayerBlend node (creating one if there is none) to exactly the layers you pass, carrying over the pin wiring of any layer name that survives the rewrite. Read targetLayers, not the success flag: it is read back out of the material's cached expression data after the edit, so it reports the layers a landscape will actually see rather than the layers that were requested. connectTo defaults to BaseColor and NEVER overwrites an input that is already wired - when it declines, connectionState says so and the layers are paintable but unsampled until you wire the node yourself.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("layers", "array", "Array of {name, blendType?, previewWeight?} objects. name is the target layer name a landscape paints; blendType is LB_WeightBlend (default), LB_AlphaBlend or LB_HeightBlend; previewWeight is the weight used where no landscape weightmap exists (defaults to 1 for the first layer, 0 for the rest)."),
        RPC_PARAM_DEF("connectTo", "string", "Main material input to wire the blend node's output into (BaseColor, Roughness, Normal, ...). Only wired when that input is currently empty; an already-wired input is left alone and reported in connectionState. Pass \"none\" to leave the node unconnected.", "BaseColor"),
        RPC_PARAM_OPT("x", "number", "Editor X position for a newly created blend node"),
        RPC_PARAM_OPT("y", "number", "Editor Y position for a newly created blend node"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        PinWright::Material::ReportMaterialLoadFailure(Ctx, AssetPath);
        return true;
    }
    if (!Material->GetEditorOnlyData())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("Material has no editor-only data, so its expression graph cannot be edited."));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* LayersArray = Ctx.GetArray(TEXT("layers"));
    if (!LayersArray || LayersArray->Num() == 0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing or empty 'layers' array."));
        return true;
    }

    const auto& Payload = Ctx.GetRawPayload();
    int32 BaseX = 0, BaseY = 0;
    if (Payload.IsValid())
    {
        Payload->TryGetNumberField(TEXT("x"), BaseX);
        Payload->TryGetNumberField(TEXT("y"), BaseY);
    }

    // Validate the WHOLE request before touching the material. Every rejection below used to
    // be a silent `continue`, which is what let the verb answer success with a layerCount
    // lower than the caller asked for and no statement of which entries were dropped.
    struct FConfigureLayerBlendRequestEntry
    {
        FName Name;
        TEnumAsByte<ELandscapeLayerBlendType> BlendType = LB_WeightBlend;
        float PreviewWeight = 0.0f;
        bool bPreviewWeightGiven = false;
    };
    TArray<FConfigureLayerBlendRequestEntry> Requested;
    Requested.Reserve(LayersArray->Num());
    TSet<FName> RequestedNames;

    for (int32 i = 0; i < LayersArray->Num(); ++i)
    {
        const TSharedPtr<FJsonObject>* LayerObj = nullptr;
        if (!(*LayersArray)[i]->TryGetObject(LayerObj) || !LayerObj || !LayerObj->IsValid())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("layers[%d] is not an object; each entry must be {name, blendType?, previewWeight?}."), i));
            return true;
        }

        FString LayerName;
        if (!(*LayerObj)->TryGetStringField(TEXT("name"), LayerName) || LayerName.TrimStartAndEnd().IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("layers[%d] has no non-empty 'name'. A blend entry without a name declares no target layer, so the landscape would gain nothing from it."), i));
            return true;
        }

        FConfigureLayerBlendRequestEntry Entry;
        Entry.Name = FName(*LayerName.TrimStartAndEnd());

        bool bAlreadyPresent = false;
        RequestedNames.Add(Entry.Name, &bAlreadyPresent);
        if (bAlreadyPresent)
        {
            // GetLandscapeLayerNames AddUnique's, so a duplicated name yields fewer declared
            // target layers than entries — the response would over-report by exactly the
            // number of duplicates.
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("layers[%d] repeats the layer name '%s'. Target layer names are a set, so a duplicate would declare fewer layers than requested."),
                    i, *Entry.Name.ToString()));
            return true;
        }

        FString BlendTypeName;
        (*LayerObj)->TryGetStringField(TEXT("blendType"), BlendTypeName);
        if (!ParseLandscapeLayerBlendTypeForConfigureVerb(BlendTypeName, Entry.BlendType))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("layers[%d] ('%s') has unknown blendType '%s'. Valid: LB_WeightBlend, LB_AlphaBlend, LB_HeightBlend."),
                    i, *Entry.Name.ToString(), *BlendTypeName));
            return true;
        }

        double PreviewWeight = 0.0;
        if ((*LayerObj)->TryGetNumberField(TEXT("previewWeight"), PreviewWeight))
        {
            Entry.PreviewWeight = static_cast<float>(PreviewWeight);
            Entry.bPreviewWeightGiven = true;
        }

        Requested.Add(Entry);
    }

    // One blend node is configured: the material's first LandscapeLayerBlend if it already has
    // one, else a new one. A real landscape material can carry several (colour, normal,
    // roughness); the extras are left alone and reported, never silently rewritten.
    UMaterialExpressionLandscapeLayerBlend* BlendNode = nullptr;
    int32 ExistingBlendNodeCount = 0;
    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr :
         Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
    {
        if (UMaterialExpressionLandscapeLayerBlend* Existing =
                Cast<UMaterialExpressionLandscapeLayerBlend>(ExpressionPtr.Get()))
        {
            ++ExistingBlendNodeCount;
            if (!BlendNode)
            {
                BlendNode = Existing;
            }
        }
    }

    const bool bCreatedBlendNode = (BlendNode == nullptr);
    if (bCreatedBlendNode)
    {
        const FCreateResult Created = FMaterialExpressionFactory::Create(
            Material,
            UMaterialExpressionLandscapeLayerBlend::StaticClass(),
            nullptr,
            FVector2D(static_cast<double>(BaseX), static_cast<double>(BaseY)));
        if (!Created.IsSuccess())
        {
            Ctx.SendError(*Created.ErrorCode, Created.ErrorMessage);
            return true;
        }
        BlendNode = Cast<UMaterialExpressionLandscapeLayerBlend>(Created.Expression);
        if (!BlendNode)
        {
            Ctx.SendError(TEXT("CREATE_FAILED"),
                TEXT("Created expression is not a UMaterialExpressionLandscapeLayerBlend."));
            return true;
        }
    }

    // Rewrite the entry list, keeping the pin wiring (LayerInput / HeightInput) and the
    // constant fallbacks of any layer name that survives, so re-running the verb to add a
    // layer does not unwire the layers already connected in the graph.
    TMap<FName, FLayerBlendInput> PreviousByName;
    PreviousByName.Reserve(BlendNode->Layers.Num());
    for (const FLayerBlendInput& Previous : BlendNode->Layers)
    {
        PreviousByName.Add(Previous.LayerName, Previous);
    }

    TArray<FLayerBlendInput> NewLayers;
    NewLayers.Reserve(Requested.Num());
    for (int32 i = 0; i < Requested.Num(); ++i)
    {
        const FConfigureLayerBlendRequestEntry& Entry = Requested[i];
        const FLayerBlendInput* Carried = PreviousByName.Find(Entry.Name);

        FLayerBlendInput LayerInput = Carried ? *Carried : FLayerBlendInput();
        LayerInput.LayerName = Entry.Name;
        LayerInput.BlendType = Entry.BlendType;
        if (Entry.bPreviewWeightGiven)
        {
            LayerInput.PreviewWeight = Entry.PreviewWeight;
        }
        else if (!Carried)
        {
            // Compile() treats PreviewWeight <= 0 as "no default weight", so a node whose
            // entries are all 0 contributes nothing wherever there is no landscape weightmap
            // (MaterialExpressionLandscapeLayerBlend.cpp:177). Seed the first layer as the
            // base, matching what the pre-fix verb expressed with its scalar defaults.
            LayerInput.PreviewWeight = (i == 0) ? 1.0f : 0.0f;
        }
        NewLayers.Add(LayerInput);
    }
    BlendNode->Layers = MoveTemp(NewLayers);

    // Wire the blend node into a main material input. Declaring target layers makes a layer
    // PAINTABLE; it does not make the paint VISIBLE — an unconnected blend node compiles into
    // no output, so the weights land in the weightmap and no shader samples them. That is the
    // same "every step succeeded and nothing happened" shape as the node-type defect, one
    // layer down, so the verb finishes the job by default.
    //
    // It never clobbers: an input that is already wired is left alone and reported. Pass
    // connectTo:"none" to skip entirely.
    const FString ConnectTo = Ctx.GetString(TEXT("connectTo"), TEXT("BaseColor"));
    const bool bConnectRequested = !ConnectTo.IsEmpty() && !ConnectTo.Equals(TEXT("none"), ESearchCase::IgnoreCase);
    FString ConnectionState = TEXT("skipped_not_requested");
    FString ConnectedInputName;

    if (bConnectRequested)
    {
        FExpressionInput* MainInput =
            PinWright::Material::ResolveMainInput(Material->GetEditorOnlyData(), ConnectTo);
        if (!MainInput)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("Unknown connectTo input '%s'. Valid: %s. Pass \"none\" to leave the blend node unconnected."),
                    *ConnectTo, *PinWright::Material::GetValidMainInputNames()));
            return true;
        }

        if (MainInput->Expression == nullptr)
        {
            PinWright::Material::ApplyConnection(*MainInput, BlendNode, FString(), 0);
            ConnectionState = TEXT("connected");
            ConnectedInputName = ConnectTo;
        }
        else if (MainInput->Expression == BlendNode)
        {
            ConnectionState = TEXT("already_connected_to_this_node");
            ConnectedInputName = ConnectTo;
        }
        else
        {
            // Someone else's wire. Overwriting it would be an unannounced graph mutation.
            ConnectionState = TEXT("skipped_input_already_wired");
        }
    }

    // Route through the node's own PostEditChangeProperty for Layers: it drops HeightInput
    // wiring on entries that are no longer height-blended and relinks the graph node's pins
    // if a material editor has this material open.
    if (FProperty* LayersProperty = FindFProperty<FProperty>(
            UMaterialExpressionLandscapeLayerBlend::StaticClass(),
            GET_MEMBER_NAME_CHECKED(UMaterialExpressionLandscapeLayerBlend, Layers)))
    {
        FPropertyChangedEvent LayersChanged(LayersProperty);
        BlendNode->PostEditChangeProperty(LayersChanged);
    }

    // Changing the declared target layers IS a change to the landscape's layer allocation,
    // which is the exact key ALandscapeProxy::MaterialInstanceConstantMap caches its
    // combination materials on (LandscapeEdit.cpp:617-618). A bare PostEditChange() leaves
    // every component rendering the pre-layer shader map, so this verb — whose only reason
    // to exist is landscape layer blending — used to configure layers a landscape could
    // not see. Same fix as compile_material (0fe35187).
    PinWright::DerivedState::FConsumerRefreshReport ConsumerRefresh;
    PinWright::MaterialConsumers::ApplyMasterMaterialEdit(Material, ConsumerRefresh);
    PinWright::MaterialConsumers::AddKnownUnrefreshedConsumers(ConsumerRefresh);
    Material->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(Material);

    // Verification the write path cannot fake: the target layers are read back out of the
    // material's cached expression data through the same accessor
    // ALandscapeProxy::RetrieveTargetLayerNamesFromMaterials uses (LandscapeUtils.cpp:577),
    // not from the array just written. A node that exists but declares nothing shows up here
    // as an empty targetLayers, which is precisely what the pre-fix verb produced.
    //
    // UE::Landscape::RetrieveTargetLayerNamesFromMaterial is the 5.8 spelling; before 5.8 the
    // same read is ALandscapeProxy::GetLayersFromMaterial (deprecated in 5.8 in favour of it).
    // With bIncludeVisibilityLayer=true the two are behaviourally identical - both return
    // FMaterialCachedExpressionEditorOnlyData::LandscapeLayerNames minus the None entries.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    TArray<FName> DeclaredTargetLayers =
        UE::Landscape::RetrieveTargetLayerNamesFromMaterial(Material, /*bIncludeVisibilityLayer=*/true);
#else
    TArray<FName> DeclaredTargetLayers = ALandscapeProxy::GetLayersFromMaterial(Material);
#endif
    DeclaredTargetLayers.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("mode"), bCreatedBlendNode ? TEXT("created") : TEXT("updated_in_place"));
    Result->SetStringField(TEXT("blendNodeId"), BlendNode->MaterialExpressionGuid.ToString());
    Result->SetNumberField(TEXT("layerCount"), BlendNode->Layers.Num());

    TArray<TSharedPtr<FJsonValue>> TargetLayerArray;
    for (const FName& LayerName : DeclaredTargetLayers)
    {
        TargetLayerArray.Add(MakeShared<FJsonValueString>(LayerName.ToString()));
    }
    Result->SetArrayField(TEXT("targetLayers"), TargetLayerArray);
    Result->SetNumberField(TEXT("targetLayerCount"), DeclaredTargetLayers.Num());

    // Other LandscapeLayerBlend nodes on this material contribute their own names to
    // targetLayers and were NOT rewritten — say so rather than letting the caller read
    // targetLayers as this verb's output.
    Result->SetNumberField(TEXT("otherLayerBlendNodes"),
        bCreatedBlendNode ? 0 : FMath::Max(0, ExistingBlendNodeCount - 1));

    // Whether the paint will be VISIBLE, reported separately from whether it is possible.
    // "skipped_input_already_wired" is the case a caller must act on: the layers are paintable
    // but this node feeds nothing.
    Result->SetStringField(TEXT("connectionState"), ConnectionState);
    if (ConnectedInputName.IsEmpty())
    {
        Result->SetField(TEXT("connectedTo"), MakeShared<FJsonValueNull>());
    }
    else
    {
        Result->SetStringField(TEXT("connectedTo"), ConnectedInputName);
    }
    if (ConnectionState == TEXT("skipped_input_already_wired"))
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Material input '%s' is already wired to another expression, so the layer blend node was left unconnected. ")
            TEXT("The layers are paintable, but nothing samples them through this node — wire it yourself with ")
            TEXT("material.graph.connect_nodes, or pass a different connectTo."),
            *ConnectTo)));
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    // Retained for callers of the pre-fix response shape. It now carries the one blend node
    // this verb configured, not one scalar parameter per layer.
    TArray<TSharedPtr<FJsonValue>> NodeIdArray;
    NodeIdArray.Add(MakeShared<FJsonValueString>(BlendNode->MaterialExpressionGuid.ToString()));
    Result->SetArrayField(TEXT("nodeIds"), NodeIdArray);
    PinWright::DerivedState::AddConsumerRefreshReport(Result, ConsumerRefresh);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// 8.6 Utilities
// ============================================================================

// --------------------------------------------------------------------------
// compile_material
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.compile_material", "material.authoring",
    "Force a material to recompile its shaders synchronously, push the change into the consumers that cache data derived from it, and report any shader-compile errors. Accepts a UMaterial or a UMaterialInstanceConstant. Use after batched graph edits to make rendering changes visible without a manual save. Blocks until this material's shader compilation finishes, or until a 90s ceiling expires. Read compileStatus for what actually happened - completed, failed, timedOut, outstanding or notCompiled - and compileSucceeded for the boolean verdict; the legacy compiled field is a fixed true and is NOT a measurement. compileSucceeded is true only when a compile LANDED with no errors: a skipped compile, an expired wait, or a material with no shader map all report an empty compileErrors list and are not successes. If any permutation fails, compileStatus is failed, compileSucceeded is false, compiledWithErrors is true, and the HLSL errors are listed in compileErrors (the material would otherwise silently fall back to the Default Material in game). compileWaitedMs is the wall clock spent waiting. shaderCompile.measuredSubject names which resource was compiled: an instance with a static-switch or base-property override owns its own static permutation and is compiled directly, while one without owns no shader at all and its parent's resource is compiled and reported instead - the parent is neither dirtied nor saved. consumerRefresh reports the landscapes whose per-component material instances were rebuilt, measured by MIC identity - without that rebuild a graph edit to a landscape master material renders as a no-op while every verb reports success; it is emitted for a UMaterial only, since an instance is not a master.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("save", "boolean", "Save after compilation (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    // A material INSTANCE is where a static-switch override lives, so it is exactly where a new
    // static permutation is created and most needs compiling — and refusing it left the
    // `notCompiled` hint on every instance response pointing at a verb that rejected the asset.
    UMaterialInterface* MaterialInterface = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
    if (!MaterialInterface)
    {
        if (UObject* RawAsset = LoadObject<UObject>(nullptr, *AssetPath))
        {
            Ctx.SendError(TEXT("UNSUPPORTED_ASSET_CLASS"), FString::Printf(
                TEXT("Asset is not a Material or Material Instance. Received class: %s"),
                *RawAsset->GetClass()->GetName()));
        }
        else
        {
            Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Material."));
        }
        return true;
    }
    UMaterial* Material = Cast<UMaterial>(MaterialInterface);

    // The master edit is finished, so announce it and push it: an FMaterialUpdateContext
    // around PreEditChange/PostEditChange recaches the dependent material instances, and
    // the landscape rebuild resets the private combination-MIC cache no update context can
    // see. Both halves live in one named helper so the next verb that completes a master
    // edit cannot ship the bare PostEditChange() again — the omission that let this defect
    // survive its first fix. See MaterialLandscapeConsumers.h.
    //
    // Master-only, deliberately. An instance is nobody's master, and a caller scoped to one
    // instance must not have the shared parent dirtied, rebuilt or saved underneath it.
    PinWright::DerivedState::FConsumerRefreshReport ConsumerRefresh;
    if (Material)
    {
        PinWright::MaterialConsumers::ApplyMasterMaterialEdit(Material, ConsumerRefresh);
        PinWright::MaterialConsumers::AddKnownUnrefreshedConsumers(ConsumerRefresh);
        Material->MarkPackageDirty();
    }

    // Ordering matters: ApplyMasterMaterialEdit's PostEditChange regenerates the master's
    // StateId and starts its recompile, so the wait has to come after it or it measures the
    // PREVIOUS shader map and reports a verdict on a compile the caller did not ask for.
    TArray<FString> CompileErrors;
    const MaterialCompileErrorCollector::FCompileWaitOutcome CompileOutcome =
        MaterialCompileErrorCollector::WaitAndCollect(MaterialInterface, CompileErrors);
    // The verdict is what the wait OBSERVED, not that the wait ran. A compile that was
    // skipped, that expired against the wait's ceiling, or that never produced a complete
    // shader map also yields an empty error list, and publishing that as compileSucceeded is
    // the defect this measurement closes (B-compile-material-blocks-and-mislabels).
    const bool bCompileLanded = MaterialCompileErrorCollector::DidCompileLand(CompileOutcome);

    bool bSave = Ctx.GetBool(TEXT("save"), true);
    // saved previously echoed the requested flag unconditionally, claiming a disk
    // write that never happened. Gate it on real on-disk presence
    // (B-material-authoring-save-no-disk-write). The asset saved is the one the caller
    // named — never the parent an instance measured against.
    const bool bSavedToDisk = bSave && SaveMaterialAssetToDisk(MaterialInterface);

    TArray<TSharedPtr<FJsonValue>> ErrorArray;
    for (const FString& Error : CompileErrors)
        ErrorArray.Add(MakeShared<FJsonValueString>(Error));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    // `compiled` is a fixed true meaning only "the handler ran a compile step" - kept
    // verbatim for backward compatibility, but it is routinely misread as a pass/fail
    // verdict and it is NOT a measurement. compileSucceeded is the verdict, and it now
    // requires the compile to have actually landed: an empty error list from a compile that
    // never happened is not a clean compile. compiledWithErrors is unchanged, and
    // compileStatus is the single field to branch on.
    Result->SetBoolField(TEXT("compiled"), true);
    Result->SetBoolField(TEXT("compileSucceeded"), bCompileLanded && CompileErrors.Num() == 0);
    Result->SetBoolField(TEXT("compiledWithErrors"), CompileErrors.Num() > 0);
    Result->SetStringField(TEXT("compileStatus"),
        MaterialCompileErrorCollector::DescribeCompileOutcome(CompileOutcome));
    // Wall-clock spent inside the bounded drain, so a caller can tell a compile this verb
    // waited on from one that was already finished when it looked.
    Result->SetNumberField(TEXT("compileWaitedMs"), CompileOutcome.WaitedSeconds * 1000.0);
    Result->SetArrayField(TEXT("compileErrors"), ErrorArray);
    // The same verdict under the namespace-wide field name, so a caller that branches on
    // shaderCompile.status works identically here and on every write verb that only probes.
    // Built from the wait outcome rather than re-probed, so the two blocks cannot disagree.
    const PinWright::MaterialShaderState::FState ShaderState =
        PinWright::MaterialShaderState::FromWaitOutcome(CompileOutcome, CompileErrors,
            MaterialInterface);
    PinWright::MaterialShaderState::AddReport(Result, ShaderState);
    // Measured coverage of the downstream rebuild. A caller editing a landscape master
    // reads consumerRefresh.consumersRefreshed to tell "the edit reached the terrain"
    // from "compiled fine, changed nothing on screen" — the two used to be one response.
    // Omitted entirely on an instance rather than emitted unmeasured: a measured:false block
    // there would read as "we could not check the consumers", which is not what happened.
    if (Material)
    {
        PinWright::DerivedState::AddConsumerRefreshReport(Result, ConsumerRefresh);
    }

    TArray<TSharedPtr<FJsonValue>> Warnings;

    // The instance route measured something other than the asset the caller named. Say so where
    // a caller reads outcomes, not only in the shaderCompile block.
    if (ShaderState.Subject ==
            PinWright::MaterialShaderState::EMeasuredSubject::ParentInherited &&
        !ShaderState.MeasuredMaterialPath.IsEmpty())
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
            "This material instance owns no shader of its own (no static-switch or base-property "
            "override, so bHasStaticPermutationResource is false) and inherits its parent's "
            "resource. The compile above ran on that parent, '%s', and every compile field in "
            "this response describes it; the parent was NOT dirtied and NOT saved. There is no "
            "instance-level compile to obtain — set a static parameter on the instance first if "
            "you need one."),
            *ShaderState.MeasuredMaterialPath)));
    }

    // Said before the consumer warnings because it is the more fundamental one: if no compile
    // landed there is no new shader map for any consumer to pick up, and the empty
    // compileErrors list means "nothing was measured", not "nothing was wrong".
    if (!bCompileLanded)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
            "No shader compile landed for this material (compileStatus=%s), so compileSucceeded "
            "is false and the empty compileErrors list is NOT evidence of a clean compile. "
            "compileStatus names which case this was: timedOut and outstanding mean the compile "
            "is still running and can be re-read by calling this verb again; notCompiled means "
            "the editor submitted no shader jobs at all (shader compilation skipped, or no "
            "material resource for the running platform), which no retry will change."),
            MaterialCompileErrorCollector::DescribeCompileOutcome(CompileOutcome))));
    }

    // A compile whose consumers were NOT refreshed is not a plain success: the shaders
    // are current and the screen is not. Say so in the response rather than leaving the
    // caller to infer it from a render that did not change. Master-only, matching the report
    // block above: on an instance nothing was enumerated because nothing needed to be.
    if (Material && !ConsumerRefresh.bMeasured)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT(
            "Consumers were not enumerated (no editor world available), so this result is "
            "not evidence that the edit reached anything rendering the material. Open the "
            "level and call material.authoring.compile_material again.")));
    }
    else if (Material && !ConsumerRefresh.IsComplete())
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
            "%d of %d landscape consumers did not rebuild their component material "
            "instances, so the edit has not reached them and a render will still show the "
            "previous shader map. Re-apply with landscape.set_material on the affected "
            "landscape."),
            ConsumerRefresh.ConsumersFound - ConsumerRefresh.ConsumersRefreshed
                - ConsumerRefresh.ConsumersWithNothingToRefresh,
            ConsumerRefresh.ConsumersFound)));
    }
    if (Warnings.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// get_material_info
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.get_material_info", "material.authoring",
    "Get information about a material (domain, blend mode, shading model, two-sided, node count, parameters with their default value/group/sortPriority, and the wired main-output inputs). "
    "A LightFunction-domain material also returns a `lightFunctionAtlas` block: `compatible` is MEASURED off the compiled shader map (omitted when there is no shader map to measure), `forceCompatible` echoes bForceCompatibleWithLightFunctionAtlas, `atlasGeneration` measures the r.LightFunctionAtlas cvar, and `warning` / `atlasWarning` name the remedy for each gate. An incompatible material is the state where the light function still paints opaque surfaces but contributes nothing to volumetric fog, translucency or single-layer water. "
    "`shaderCompile` reports the material's measured shader state: a material that can never compile still reads back here with the right domain, blend mode and wired mainInputs, so nothing else in this response separates it from one that renders. "
    "`usage` reports the whole EMaterialUsage set - `declared` lists what the material is allowed on, `flags[]` carries every usage with the UMaterial property behind it, and `autoSetInEditor` says whether the editor silently patches a missing flag at draw time. A consumer whose usage is not declared draws the engine Default Material, and no other field here (shaderCompile included) can see that: the permutations of an undeclared usage are never compiled into the shader map, so it reads complete without them. "
    "`functionCallIdentity` appears ONLY when a MaterialFunctionCall node caches a pin GUID that will not re-link on the next load - {unstable: [{nodeId, functionPath, pinKind, pinName, reason}], warning}, reason one of missing-persistent-id / stale-persistent-id / duplicate-persistent-id. Those wires are dropped during load with no compile error and no other field here can see it, so the block's presence is the only warning a caller gets.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path"))
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        // Same class-vs-missing split this verb has always made, now through the shared helper
        // so every UMaterial-only verb in the family emits one message shape.
        PinWright::Material::ReportMaterialLoadFailure(
            Ctx, AssetPath, TEXT("material.authoring.get_material_instance_info"));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // Domain
    switch (Material->MaterialDomain) {
    case EMaterialDomain::MD_Surface:       Result->SetStringField(TEXT("domain"), TEXT("Surface")); break;
    case EMaterialDomain::MD_DeferredDecal: Result->SetStringField(TEXT("domain"), TEXT("DeferredDecal")); break;
    case EMaterialDomain::MD_LightFunction: Result->SetStringField(TEXT("domain"), TEXT("LightFunction")); break;
    case EMaterialDomain::MD_Volume:        Result->SetStringField(TEXT("domain"), TEXT("Volume")); break;
    case EMaterialDomain::MD_PostProcess:   Result->SetStringField(TEXT("domain"), TEXT("PostProcess")); break;
    case EMaterialDomain::MD_UI:            Result->SetStringField(TEXT("domain"), TEXT("UI")); break;
    default:                                Result->SetStringField(TEXT("domain"), TEXT("Unknown")); break;
    }

    // Blend mode. Through the shared table, which closes a read/write asymmetry: the local
    // switch here covered five modes, so a material set to AlphaComposite or AlphaHoldout by
    // set_blend_mode (both of which it accepts) read back as "Unknown".
    Result->SetStringField(TEXT("blendMode"), MaterialBlendModeToString(Material->BlendMode));

    Result->SetBoolField(TEXT("twoSided"), Material->TwoSided);

    // Shading model: create_material / set_shading_model accept it but the readback never
    // surfaced it (write/read asymmetry). Report it alongside domain/blendMode.
    Result->SetStringField(TEXT("shadingModel"),
        PinWright::Material::GetShadingModelString(Material));

    // Light function atlas compatibility, MEASURED off the compiled shader map, for a
    // MD_LightFunction material only. Without it nothing on any read distinguishes a light
    // function that reaches volumetric fog from one the atlas silently drops - the material,
    // the light component and every r.*LightFunctionAtlas cvar all read healthy either way,
    // and the material keeps painting opaque surfaces so the failure is invisible on geometry.
    // See MaterialLightFunctionAtlas.h for the translator rule that decides it.
    PinWright::LightFunctionAtlas::AddReportIfLightFunction(Result, Material);

    // The EMaterialUsage set. Everything else on this response describes a material that is
    // authored correctly; this is the only field that can say the renderer will substitute the
    // engine Default Material for it on the mesh it is assigned to, and it was the missing one:
    // domain, blend mode, shading model, node count, wired mainInputs, parameters and the shader
    // verdict all read healthy for a skeletal-mesh material with bUsedWithSkeletalMesh unset.
    PinWright::MaterialUsage::AddUsageReport(Result, Material);

    Result->SetNumberField(TEXT("nodeCount"), Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Num());

    // Main material output node wiring: which expressions feed BaseColor/EmissiveColor/... so
    // callers can confirm "is the output wired?" without decompiling the whole graph.
    Result->SetArrayField(TEXT("mainInputs"),
        PinWright::Material::BuildMainNodeInputsJson(Material->GetEditorOnlyData()));

    // List parameters
    TArray<TSharedPtr<FJsonValue>> ParamsArray;
    for (UMaterialExpression* Expr : Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
    {
        FString ParamName;
        if (TryGetMaterialParameterName(Expr, ParamName))
        {
            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
            ParamObj->SetStringField(TEXT("name"), ParamName);
            ParamObj->SetStringField(TEXT("type"), Expr->GetClass()->GetName());
            ParamObj->SetStringField(TEXT("nodeId"), Expr->MaterialExpressionGuid.ToString());
            // Echo back the create-time metadata + typed default so the author-then-verify
            // round-trip can read what it set (E-get-material-info-no-param-defaults).
            AddMaterialParameterDetails(ParamObj, Expr);
            ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
        }
    }
    Result->SetArrayField(TEXT("parameters"), ParamsArray);

    // Function-call pin identity. A wire into a MaterialFunctionCall survives a reload only if the
    // pin GUID the call node cached still names a pin of the function; when it does not, the
    // engine drops the wire during load with no compile error and no diagnostic anywhere, so the
    // read verbs agree with a graph that is about to lose connections. Emitted only when something
    // is actually wrong — see MaterialFunctionIdentity.h for what is and is not detectable.
    PinWright::MaterialFunctionIdentity::AddUnstableFunctionCallReport(Result, Material);

    // Read-back is the other half of the defect: a material that never compiles reads back with
    // the right domain, blend mode and wired mainInputs, which is exactly what an author checks
    // before concluding the asset is correct. The non-blocking probe costs nothing here.
    PinWright::MaterialShaderState::AddReportForAsset(Result, Material);

    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// get_material_node_details
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.get_material_node_details", "material.authoring",
    "Get details about a specific material expression node",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node ID to inspect ('Main' for the main material output node)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;
    FString NodeId;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        PinWright::Material::ReportMaterialLoadFailure(Ctx, AssetPath);
        return true;
    }

    // The main material output node is not a UMaterialExpression, so the expression resolver
    // cannot resolve it. Accept the same documented "Main" sentinel that connect_nodes /
    // break_connections accept so the read side can answer "is BaseColor/EmissiveColor wired?".
    // nodeId is required (validated non-empty above), so only the explicit "Main" token applies here.
    if (NodeId.Equals(TEXT("Main"), ESearchCase::IgnoreCase))
    {
        Ctx.SendSuccess(PinWright::Material::BuildMainNodeDetailsJson(Material));
        return true;
    }

    // Reading back one of several same-named nodes is what made the write-side defect
    // unverifiable: the read verb agreed with the wrong answer.
    UMaterialExpression* Expr = PinWright::Material::ResolveExpressionOrSendError(
        Ctx, ResolveExpressionByIdOrName(Material, NodeId), NodeId,
        TEXT("NOT_FOUND"), TEXT("Node not found."));
    if (!Expr) return true;

    TSharedPtr<FJsonObject> Result = MGIRExpressionUtils::BuildExpressionDetailsJson(Expr);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_two_sided
// --------------------------------------------------------------------------
REGISTER_RPC_HANDLER("material.authoring.set_two_sided", "material.authoring",
    "Toggle the bTwoSided flag on a UMaterial so it renders both faces (foliage / cloth / decals). Forces a recompile.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("twoSided", "boolean", "Enable two-sided (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Save after change (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        // TwoSided IS overridable per instance (FMaterialInstanceBasePropertyOverrides::TwoSided),
        // so a material instance here gets routed to the instance verb rather than a false
        // "nothing at that path".
        PinWright::Material::ReportMaterialLoadFailure(
            Ctx, AssetPath, PinWright::Material::InstanceBasePropertyOverrideVerb());
        return true;
    }

    bool bTwoSided = Ctx.GetBool(TEXT("twoSided"), true);
    Material->TwoSided = bTwoSided ? 1 : 0;
    // PostEditChange invalidates the cached render state so the recompile actually fires
    // (matching set_blend_mode / set_shading_model / set_material_domain); without it the
    // TwoSided write never reaches the live/compiled material.
    Material->PostEditChange();
    Material->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialAsset(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetBoolField(TEXT("twoSided"), bTwoSided);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// 8.x Material Layers & Layer Blends
// ============================================================================

// Seed the validation-passing default template UE's material-function editor creates on-open for an
// otherwise-empty layer/blend function (its "example graph"). The engine content-browser factory
// (UMaterialFunctionMaterialLayer(Blend)Factory::FactoryCreateNew) seeds NOTHING — it only
// NewObject()s + SetMaterialFunctionUsage()s — so a freshly created layer/blend has an empty body and
// fails material-layer validation as-created. This mirrors FMaterialEditor's on-open seeding
// (Editor/MaterialEditor MaterialEditor.cpp): a UMaterialExpressionMaterialLayerOutput plus, for a
// layer, one MaterialAttributes FunctionInput -> SetMaterialAttributes -> output; for a blend, two
// MaterialAttributes FunctionInputs (Top/Bottom) -> BlendMaterialAttributes -> output. Distinctively
// named (not anonymous-namespace) so unity TU merges cannot ODR-collide it with a sibling helper.
static void SeedMaterialLayerFunctionTemplate(UMaterialFunction* Function, bool bIsBlend)
{
    if (!Function) return;

    // Output node: MaterialLayerOutput is a FunctionOutput subclass whose single input is typed
    // MCT_MaterialAttributes and which the user cannot delete — the required sink for a layer/blend.
    FCreateResult OutputResult = FMaterialExpressionFactory::Create(
        Function, UMaterialExpressionMaterialLayerOutput::StaticClass(), nullptr,
        FVector2D(bIsBlend ? 275.0 : 300.0, 269.0));
    UMaterialExpression* Output = OutputResult.Expression;
    if (!Output) return;
    Output->bCollapsed = true;

    if (!bIsBlend)
    {
        // Layer: one MaterialAttributes input -> SetMaterialAttributes -> output.
        FCreateResult InputResult = FMaterialExpressionFactory::Create(
            Function, UMaterialExpressionFunctionInput::StaticClass(), nullptr, FVector2D(-350.0, 300.0));
        UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(InputResult.Expression);
        if (Input)
        {
            Input->InputType = FunctionInput_MaterialAttributes;
            Input->InputName = TEXT("Material Attributes");
            // Without a preview-value default, an unconnected input compiles to "Missing function input".
            Input->bUsePreviewValueAsDefault = true;
            Input->ConditionallyGenerateId(true);
        }

        FCreateResult SetResult = FMaterialExpressionFactory::Create(
            Function, UMaterialExpressionSetMaterialAttributes::StaticClass(), nullptr, FVector2D(40.0, 300.0));
        UMaterialExpression* SetMA = SetResult.Expression;
        if (Input && SetMA)
        {
            // Empty pin names == first output / first input, matching FMaterialEditor's own calls.
            UMaterialEditingLibrary::ConnectMaterialExpressions(Input, FString(), SetMA, FString());
            UMaterialEditingLibrary::ConnectMaterialExpressions(SetMA, FString(), Output, FString());
        }
    }
    else
    {
        // Blend: two MaterialAttributes inputs (Bottom/Top) -> BlendMaterialAttributes(A,B) -> output.
        // Names must be the engine's "Bottom Layer"/"Top Layer" (the Bottom/TopMaterialBlendInputName
        // constants on UE 5.7+, where BlendInputRelevance must also be set) so the layer stack binds
        // them; the validator also requires exactly two FunctionInputs.
        FCreateResult TopResult = FMaterialExpressionFactory::Create(
            Function, UMaterialExpressionFunctionInput::StaticClass(), nullptr, FVector2D(-300.0, 400.0));
        UMaterialExpressionFunctionInput* InputTop = Cast<UMaterialExpressionFunctionInput>(TopResult.Expression);
        if (InputTop)
        {
            InputTop->InputType = FunctionInput_MaterialAttributes;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            InputTop->InputName = TopMaterialBlendInputName;
            InputTop->BlendInputRelevance = EBlendInputRelevance::Top;
#else
            // Pre-5.7 engines have neither the Top/BottomMaterialBlendInputName constants nor the
            // BlendInputRelevance field; their FMaterialEditor seeds the same literal names directly.
            InputTop->InputName = TEXT("Top Layer");
#endif
            InputTop->bUsePreviewValueAsDefault = true;
            InputTop->ConditionallyGenerateId(true);
        }

        FCreateResult BottomResult = FMaterialExpressionFactory::Create(
            Function, UMaterialExpressionFunctionInput::StaticClass(), nullptr, FVector2D(-300.0, 200.0));
        UMaterialExpressionFunctionInput* InputBottom = Cast<UMaterialExpressionFunctionInput>(BottomResult.Expression);
        if (InputBottom)
        {
            InputBottom->InputType = FunctionInput_MaterialAttributes;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            InputBottom->InputName = BottomMaterialBlendInputName;
            InputBottom->BlendInputRelevance = EBlendInputRelevance::Bottom;
#else
            InputBottom->InputName = TEXT("Bottom Layer");
#endif
            InputBottom->bUsePreviewValueAsDefault = true;
            InputBottom->ConditionallyGenerateId(true);
        }

        FCreateResult BlendResult = FMaterialExpressionFactory::Create(
            Function, UMaterialExpressionBlendMaterialAttributes::StaticClass(), nullptr, FVector2D(40.0, 300.0));
        UMaterialExpression* BlendMA = BlendResult.Expression;
        if (InputTop && InputBottom && BlendMA)
        {
            UMaterialEditingLibrary::ConnectMaterialExpressions(InputBottom, FString(), BlendMA, FString(TEXT("A")));
            UMaterialEditingLibrary::ConnectMaterialExpressions(InputTop, FString(), BlendMA, FString(TEXT("B")));
            UMaterialEditingLibrary::ConnectMaterialExpressions(BlendMA, FString(), Output, FString());
        }
    }
}

// --------------------------------------------------------------------------
// create_material_layer
// --------------------------------------------------------------------------
// Distinct from create_material_function: returns the specific
// UMaterialFunctionMaterialLayer subclass required to satisfy the
// Cast<UMaterialFunctionMaterialLayer> check that
// UMaterialExpressionMaterialAttributeLayers performs when assigning to
// DefaultLayers.Layers[]. A plain UMaterialFunction is a sibling subclass
// and will not satisfy that cast.
REGISTER_RPC_HANDLER("material.authoring.create_material_layer", "material.authoring",
    "Create a UMaterialFunctionMaterialLayer asset — a reusable single-stack of material attributes referenced by UMaterialExpressionMaterialAttributeLayers.DefaultLayers.Layers. Use this (not create_material_function) when authoring a material-layer stack.",
    RPC_PARAMS(
        MaterialCreatePathParamUtils::MaterialCreateNameParamReq(TEXT("Layer asset name (leaf). Or pass one combined 'assetPath' to set name + folder together.")),
        MaterialCreatePathParamUtils::MaterialCreateFolderParamOpt(TEXT("Destination folder (default /Game/Materials/Layers)")),
        RPC_PARAM_OPT("description", "string", "Layer description"),
        RPC_PARAM_OPT("exposeToLibrary", "boolean", "Expose to material function library (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Save after creation (default true)"),
        RPC_PARAM_DEF("seedTemplate", "boolean", "Seed the validation-passing default template UE's material-function editor produces on-open (a MaterialAttributes FunctionInput -> SetMaterialAttributes -> MaterialLayerOutput) so the layer drops into set_material_layer_stack and compiles as-created. Pass false to get an empty body for hand-authoring a custom layer.", "true")
    ))
{
    FString Name;
    FString Path;
    FString PackagePath;
    if (!MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath(Ctx, TEXT("/Game/Materials/Layers"), Name, Path, PackagePath)) return true;
    FString Description = Ctx.GetString(TEXT("description"));
    bool bExposeToLibrary = Ctx.GetBool(TEXT("exposeToLibrary"), true);

    UMaterialFunctionMaterialLayerFactory* Factory = NewObject<UMaterialFunctionMaterialLayerFactory>();
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package."));
        return true;
    }

    UMaterialFunctionMaterialLayer* NewLayer = Cast<UMaterialFunctionMaterialLayer>(
        Factory->FactoryCreateNew(UMaterialFunctionMaterialLayer::StaticClass(), Package,
            FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewLayer)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create material layer."));
        return true;
    }

    if (!Description.IsEmpty()) NewLayer->Description = Description;
    NewLayer->bExposeToLibrary = bExposeToLibrary;
    if (Ctx.GetBool(TEXT("seedTemplate"), true))
    {
        SeedMaterialLayerFunctionTemplate(NewLayer, /*bIsBlend=*/false);
    }
    NewLayer->PostEditChange();
    NewLayer->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialFunctionAsset(NewLayer);
    FAssetRegistryModule::AssetCreated(NewLayer);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, NewLayer);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// create_material_layer_blend
// --------------------------------------------------------------------------
// Returns UMaterialFunctionMaterialLayerBlend — the only subclass accepted by
// DefaultLayers.Blends[]. See create_material_layer for the cast rationale.
REGISTER_RPC_HANDLER("material.authoring.create_material_layer_blend", "material.authoring",
    "Create a UMaterialFunctionMaterialLayerBlend asset — combines two adjacent material layers into one. Required at index N-1 of every material attribute layer stack (blends.length == layers.length - 1).",
    RPC_PARAMS(
        MaterialCreatePathParamUtils::MaterialCreateNameParamReq(TEXT("Layer-blend asset name (leaf). Or pass one combined 'assetPath' to set name + folder together.")),
        MaterialCreatePathParamUtils::MaterialCreateFolderParamOpt(TEXT("Destination folder (default /Game/Materials/LayerBlends)")),
        RPC_PARAM_OPT("description", "string", "Blend description"),
        RPC_PARAM_OPT("exposeToLibrary", "boolean", "Expose to material function library (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Save after creation (default true)"),
        RPC_PARAM_DEF("seedTemplate", "boolean", "Seed the validation-passing default template UE's material-function editor produces on-open (two MaterialAttributes FunctionInputs 'Top Layer'/'Bottom Layer' -> BlendMaterialAttributes -> MaterialLayerOutput) so the blend drops into set_material_layer_stack and compiles as-created. Pass false to get an empty body for hand-authoring a custom blend.", "true")
    ))
{
    FString Name;
    FString Path;
    FString PackagePath;
    if (!MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath(Ctx, TEXT("/Game/Materials/LayerBlends"), Name, Path, PackagePath)) return true;
    FString Description = Ctx.GetString(TEXT("description"));
    bool bExposeToLibrary = Ctx.GetBool(TEXT("exposeToLibrary"), true);

    UMaterialFunctionMaterialLayerBlendFactory* Factory = NewObject<UMaterialFunctionMaterialLayerBlendFactory>();
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package."));
        return true;
    }

    UMaterialFunctionMaterialLayerBlend* NewBlend = Cast<UMaterialFunctionMaterialLayerBlend>(
        Factory->FactoryCreateNew(UMaterialFunctionMaterialLayerBlend::StaticClass(), Package,
            FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewBlend)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create material layer blend."));
        return true;
    }

    if (!Description.IsEmpty()) NewBlend->Description = Description;
    NewBlend->bExposeToLibrary = bExposeToLibrary;
    if (Ctx.GetBool(TEXT("seedTemplate"), true))
    {
        SeedMaterialLayerFunctionTemplate(NewBlend, /*bIsBlend=*/true);
    }
    NewBlend->PostEditChange();
    NewBlend->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true)) SaveMaterialFunctionAsset(NewBlend);
    FAssetRegistryModule::AssetCreated(NewBlend);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, NewBlend);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// set_material_layer_stack
// --------------------------------------------------------------------------
// Typed wrapper around the parallel-array maintenance that
// UMaterialExpressionMaterialAttributeLayers requires. Shares its body with
// MGIRExpressionEmitter::EmitLayerStack via ApplyMaterialLayerStack (single
// source of truth — if MGIR ever drifts, the imperative RPC stays correct).
REGISTER_RPC_HANDLER("material.authoring.set_material_layer_stack", "material.authoring",
    "Populate a UMaterialExpressionMaterialAttributeLayers node's Layers/Blends arrays from asset paths. Maintains all parallel editor-only arrays (LayerNames, LayerStates, LayerGuids, LayerLinkStates, RestrictTo*) and rebuilds the layer graph.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("expressionId", "string", "MaterialExpressionGuid of the target UMaterialExpressionMaterialAttributeLayers node"),
        RPC_PARAM_REQ("layers", "array", "UMaterialFunctionMaterialLayer asset paths (ordered)"),
        RPC_PARAM_REQ("blends", "array", "UMaterialFunctionMaterialLayerBlend asset paths (length must equal layers.length - 1)"),
        RPC_PARAM_OPT("save", "boolean", "Save after applying (default true)")
    ))
{
    LOAD_MATERIAL_OR_RETURN();

    FString ExpressionId;
    if (!Ctx.RequireString(TEXT("expressionId"), ExpressionId)) return true;

    const TArray<TSharedPtr<FJsonValue>>* LayersJson = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* BlendsJson = nullptr;
    if (!Ctx.GetRawPayload()->TryGetArrayField(TEXT("layers"), LayersJson) || !LayersJson)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("'layers' must be an array of asset paths."));
        return true;
    }
    if (!Ctx.GetRawPayload()->TryGetArrayField(TEXT("blends"), BlendsJson) || !BlendsJson)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("'blends' must be an array of asset paths."));
        return true;
    }

    if (LayersJson->Num() > 0 && BlendsJson->Num() != LayersJson->Num() - 1)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("blends.length must equal layers.length - 1 (got layers=%d, blends=%d)."),
                LayersJson->Num(), BlendsJson->Num()));
        return true;
    }

    UMaterialExpression* Expr = PinWright::Material::ResolveExpressionOrSendError(
        Ctx, ResolveExpressionByIdOrName(Material, ExpressionId), ExpressionId,
        TEXT("EXPRESSION_NOT_FOUND"),
        FString::Printf(TEXT("Could not find expression with id/name '%s'."), *ExpressionId));
    if (!Expr) return true;

    UMaterialExpressionMaterialAttributeLayers* LayersExpression =
        Cast<UMaterialExpressionMaterialAttributeLayers>(Expr);
    if (!LayersExpression)
    {
        Ctx.SendError(TEXT("WRONG_EXPRESSION_TYPE"),
            TEXT("Target expression is not a UMaterialExpressionMaterialAttributeLayers."));
        return true;
    }

    TArray<UMaterialFunctionInterface*> Layers;
    Layers.Reserve(LayersJson->Num());
    for (const TSharedPtr<FJsonValue>& V : *LayersJson)
    {
        FString LayerPath;
        if (!V->TryGetString(LayerPath))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("'layers' entries must be strings."));
            return true;
        }
        UMaterialFunctionInterface* Layer = LoadObject<UMaterialFunctionInterface>(nullptr, *LayerPath);
        if (!Layer)
        {
            Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
                FString::Printf(TEXT("Could not load layer function: %s"), *LayerPath));
            return true;
        }
        Layers.Add(Layer);
    }

    TArray<UMaterialFunctionInterface*> Blends;
    Blends.Reserve(BlendsJson->Num());
    for (const TSharedPtr<FJsonValue>& V : *BlendsJson)
    {
        FString BlendPath;
        if (!V->TryGetString(BlendPath))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("'blends' entries must be strings."));
            return true;
        }
        UMaterialFunctionInterface* Blend = LoadObject<UMaterialFunctionInterface>(nullptr, *BlendPath);
        if (!Blend)
        {
            Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
                FString::Printf(TEXT("Could not load blend function: %s"), *BlendPath));
            return true;
        }
        Blends.Add(Blend);
    }

    ApplyMaterialLayerStack(LayersExpression, Layers, Blends);
    Material->PostEditChange();
    Material->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true))
    {
        Material->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("layersApplied"), Layers.Num());
    Result->SetNumberField(TEXT("blendsApplied"), Blends.Num());
    PinWright::Material::AddMaterialVerification(Result, Material);
    Ctx.SendSuccess(Result);
    return true;
}

// --------------------------------------------------------------------------
// auto_layout
// --------------------------------------------------------------------------
// Standalone wrapper around FMGIRLayoutEngine::Layout. Exposes the same
// auto-layout pass that material.compile_mgir runs under bRunLayout, without
// the surrounding ForceRecompileForRendering / UpdateFromFunctionResource /
// FMaterialUpdateContext machinery — layout-only edits do not invalidate
// shaders, so we skip the recompile path.
//
// Counterfactual: if this handler registration is reverted, the dispatcher
// returns METHOD_NOT_FOUND for material.authoring.auto_layout and the
// regression test's InvokeHandler call returns false.
REGISTER_RPC_HANDLER("material.authoring.auto_layout", "material.authoring",
    "Re-flow expression positions on a UMaterial or UMaterialFunction by running FMGIRLayoutEngine::Layout. Only repositions expressions whose (x,y) are still (0,0); already-positioned nodes are untouched. Layout-only — does not recompile shaders.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material or material-function asset path"))
    ))
{
    FString AssetPath;
    // Layout-only edit: reposition nodes without recompiling, so pass bCheckEditorOpen=false to allow
    // an open Material Editor (repositioning isn't clobbered by its working copy). This is the same
    // Material-then-Function dispatch + class-discrimination the graph-mutating handlers use.
    PinWright::Material::FMaterialMutationTarget Target =
        PinWright::Material::LoadMaterialOrFunctionForMutationOrReportError(
            Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath, /*bCheckEditorOpen=*/false);
    if (!Target.IsValid()) return true;
    UMaterial* Material = Target.Material;
    UMaterialFunction* Function = Target.Function;

    // Count expressions before so we can report what was eligible.
    int32 ExpressionCount = 0;
    {
        TArray<UMaterialExpression*> Expressions;
        if (Material)
            MGIRExpressionUtils::CopyMaterialExpressions(Material, Expressions);
        else
            MGIRExpressionUtils::CopyFunctionExpressions(Function, Expressions);
        ExpressionCount = Expressions.Num();
    }

    const double StartSeconds = FPlatformTime::Seconds();
    if (Material)
        FMGIRLayoutEngine::Layout(Material);
    else
        FMGIRLayoutEngine::Layout(Function);
    const double DurationMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

    UObject* Asset = Target.AssetObject();
    Asset->PreEditChange(nullptr);
    Asset->PostEditChange();
    Asset->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("target"), AssetPath);
    Result->SetNumberField(TEXT("expressionsLaidOut"), ExpressionCount);
    Result->SetNumberField(TEXT("durationMs"), DurationMs);
    Ctx.SendSuccess(Result);
    return true;
}

#undef LOAD_MATERIAL_OR_RETURN
#undef FINALIZE_EXPR_AND_RESPOND
