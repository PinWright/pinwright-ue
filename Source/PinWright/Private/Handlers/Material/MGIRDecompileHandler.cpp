// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/IrSidecarRegistry.h"
#include "Dom/JsonValue.h"

#include "MGIR/MGIRDecompiler.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"

namespace
{
    UClass* GetMGIRMaterialClass()
    {
        return UMaterial::StaticClass();
    }

    UClass* GetMGIRMaterialFunctionClass()
    {
        return UMaterialFunction::StaticClass();
    }

    IrSidecarRegistry::FIrSidecarResult BuildMGIRMaterialSidecar(UObject* Asset)
    {
        const FMGIRDecompileResult Result = FMGIRDecompiler::DecompileMaterial(Cast<UMaterial>(Asset));
        return {Result.MGIRText, Result.Warnings, Result.bSuccess};
    }

    IrSidecarRegistry::FIrSidecarResult BuildMGIRMaterialFunctionSidecar(UObject* Asset)
    {
        const FMGIRDecompileResult Result = FMGIRDecompiler::DecompileFunction(Cast<UMaterialFunction>(Asset));
        return {Result.MGIRText, Result.Warnings, Result.bSuccess};
    }
}

REGISTER_DECOMPILE_IR(TEXT("mgir.material"), DumpFileNames::Mgir,
    &GetMGIRMaterialClass, &BuildMGIRMaterialSidecar, 100);
REGISTER_DECOMPILE_IR(TEXT("mgir.material_function"), DumpFileNames::Mgir,
    &GetMGIRMaterialFunctionClass, &BuildMGIRMaterialFunctionSidecar, 100);

REGISTER_RPC_HANDLER("material.decompile_mgir", "material",
    "Decompile a material or material function asset into MGIR text. A material block carries its material-level properties (blend mode, shading model, two-sided, domain, translucency lighting mode and four more) as `property Name: Value` lines above the graph, so compiling the result back reproduces the material and not just its nodes. Material instances have no graph and are refused with UNSUPPORTED_ASSET_CLASS.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Material or material function asset path"),
        RPC_PARAM_OPT("includeReferencedFunctions", "boolean", "Include material functions referenced by a material"),
        RPC_PARAM_OPT("emitSubstrateSugar", "boolean", "Emit optional Substrate aliases such as slab(...) instead of canonical call form")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    const bool bIncludeReferencedFunctions = Ctx.GetBool(TEXT("includeReferencedFunctions"), false);
    FMGIRDecompileOptions DecompileOptions;
    DecompileOptions.bEmitSubstrateSugar = Ctx.GetBool(TEXT("emitSubstrateSugar"), false);

    FMGIRDecompileResult DecompileResult;
    if (UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath))
    {
        DecompileResult = bIncludeReferencedFunctions
            ? FMGIRDecompiler::DecompileMaterialWithReferences(Material, DecompileOptions)
            : FMGIRDecompiler::DecompileMaterial(Material, DecompileOptions);
    }
    else if (UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *AssetPath))
    {
        DecompileResult = FMGIRDecompiler::DecompileFunction(Function, DecompileOptions);
    }
    else if (UMaterialInstance* Instance = LoadObject<UMaterialInstance>(nullptr, *AssetPath))
    {
        // MGIR is a graph IR and an instance has no graph - it has a parent plus a parameter
        // override set. Decompiling it "successfully" could only ever produce its parent's
        // graph under the instance's own path, which recompiling would then turn into a
        // second master. Refuse, and name the asset it actually is: the bare ASSET_NOT_FOUND
        // this replaces read as "the path is wrong" for an asset that loaded fine.
        Ctx.SendError(TEXT("UNSUPPORTED_ASSET_CLASS"),
            FString::Printf(
                TEXT("%s is a %s, not a material or material function. MGIR describes material ")
                TEXT("graphs; an instance has no graph of its own. Decompile its parent (%s) for ")
                TEXT("the graph, read its overrides with material.authoring.get_material_instance_info ")
                TEXT("or asset.dump, and write them with material.authoring.set_material_instance_parameters."),
                *AssetPath,
                *Instance->GetClass()->GetName(),
                Instance->Parent ? *Instance->Parent->GetPathName() : TEXT("no parent set")));
        return true;
    }
    else
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load material or material function: %s"), *AssetPath));
        return true;
    }

    if (!DecompileResult.bSuccess)
    {
        const FString Message = DecompileResult.Warnings.IsEmpty()
            ? TEXT("MGIR decompile failed.")
            : FString::Join(DecompileResult.Warnings, TEXT("\n"));
        Ctx.SendError(TEXT("MGIR_DECOMPILE_FAILED"), Message);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : DecompileResult.Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("text"), DecompileResult.MGIRText);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Ctx.SendSuccess(Result);
    return true;
}
