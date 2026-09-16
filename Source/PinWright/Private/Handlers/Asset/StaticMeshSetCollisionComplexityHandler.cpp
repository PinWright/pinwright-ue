// Copyright (c) 2026 Alexander Penkin. MIT License.

// StaticMeshSetCollisionComplexityHandler.cpp - Set a StaticMesh asset's shape-complexity mode.
//
// geometry.generate_collision authors simple shapes, but level-authoring workflows also need
// to choose whether the baked StaticMesh uses those shapes, its render triangles, or both.
// static_mesh.describe already exposes CollisionTraceFlag; this typed writer closes the
// read/write gap without forcing callers through a generic nested property mutation.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "BodySetupEnums.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Utils/AssetUtils.h"
#include "Dom/JsonObject.h"
#include "UObject/UObjectIterator.h"

namespace
{
    bool ParseCollisionComplexity(const FString& Value, ECollisionTraceFlag& OutFlag)
    {
        FString Normalized = Value;
        Normalized.TrimStartAndEndInline();
        Normalized.ReplaceInline(TEXT("-"), TEXT("_"));
        Normalized.ReplaceInline(TEXT(" "), TEXT("_"));
        Normalized.ToLowerInline();

        if (Normalized == TEXT("project_default") || Normalized == TEXT("default") ||
            Normalized == TEXT("ctf_usedefault"))
        {
            OutFlag = CTF_UseDefault;
            return true;
        }
        if (Normalized == TEXT("simple_and_complex") || Normalized == TEXT("ctf_usesimpleandcomplex"))
        {
            OutFlag = CTF_UseSimpleAndComplex;
            return true;
        }
        if (Normalized == TEXT("simple_as_complex") || Normalized == TEXT("use_simple_as_complex") ||
            Normalized == TEXT("ctf_usesimpleascomplex"))
        {
            OutFlag = CTF_UseSimpleAsComplex;
            return true;
        }
        if (Normalized == TEXT("complex_as_simple") || Normalized == TEXT("use_complex_as_simple") ||
            Normalized == TEXT("ctf_usecomplexassimple"))
        {
            OutFlag = CTF_UseComplexAsSimple;
            return true;
        }
        return false;
    }

    FString CollisionTraceFlagName(ECollisionTraceFlag Flag)
    {
        if (const UEnum* Enum = StaticEnum<ECollisionTraceFlag>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Flag));
        }
        return FString();
    }
}

REGISTER_RPC_HANDLER("static_mesh.set_collision_complexity", "static_mesh",
    "Set a StaticMesh asset's collision trace mode, recook its physics data, refresh loaded components, and optionally save. Use project_default, simple_and_complex, simple_as_complex, or complex_as_simple.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "StaticMesh asset path"),
        RPC_PARAM_REQ("complexity", "string", "Collision mode: project_default, simple_and_complex, simple_as_complex, or complex_as_simple"),
        RPC_PARAM_OPT("save", "boolean", "Persist the modified StaticMesh to disk (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath)) return true;

    FString Complexity;
    if (!Ctx.RequireString(TEXT("complexity"), Complexity)) return true;

    ECollisionTraceFlag RequestedFlag = CTF_UseDefault;
    if (!ParseCollisionComplexity(Complexity, RequestedFlag))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_COLLISION_COMPLEXITY,
            FString::Printf(TEXT("Unsupported complexity '%s'. Expected project_default, simple_and_complex, simple_as_complex, or complex_as_simple"),
                *Complexity));
        return true;
    }

    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
    if (!Mesh)
    {
        Ctx.SendError(TEXT("MESH_NOT_FOUND"),
            FString::Printf(TEXT("Could not load StaticMesh: %s"), *AssetPath));
        return true;
    }

    const bool bCreatedBodySetup = Mesh->GetBodySetup() == nullptr;
    Mesh->CreateBodySetup();
    UBodySetup* BodySetup = Mesh->GetBodySetup();
    if (!BodySetup)
    {
        Ctx.SendError(ErrorCodes::ERR_BODY_SETUP_FAILED,
            FString::Printf(TEXT("Could not create BodySetup for StaticMesh: %s"), *AssetPath));
        return true;
    }

    const ECollisionTraceFlag PreviousFlag = BodySetup->CollisionTraceFlag;
    const bool bChanged = bCreatedBodySetup || PreviousFlag != RequestedFlag;
    int32 ComponentsRecreated = 0;
    if (bChanged)
    {
        Mesh->Modify();
        BodySetup->Modify();
        BodySetup->CollisionTraceFlag = RequestedFlag;

        // The trace flag controls which simple/triangle data the cook retains. Re-cook
        // before recreating users so a transition from either one-sided mode has the
        // newly-required data available immediately in editor worlds and PIE.
        BodySetup->InvalidatePhysicsData();
        BodySetup->CreatePhysicsMeshes();

        for (UStaticMeshComponent* Component : TObjectRange<UStaticMeshComponent>())
        {
            if (Component && Component->GetStaticMesh() == Mesh && Component->IsPhysicsStateCreated())
            {
                Component->RecreatePhysicsState();
                ++ComponentsRecreated;
            }
        }
        Mesh->MarkPackageDirty();
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    FString PackageName;
    int64 SizeBytes = 0;
    bool bSavedToDisk = false;
    // Threaded so the response carries saveState/saveDetail like every other save in the
    // family: without it a PIE-blocked write answered with a bare pendingFlush the caller was
    // documented to retry (B-asset-save-omits-savestate-pie-block).
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    if (bSave)
    {
        bSavedToDisk = SaveAssetToDiskReportingPresence(Mesh, /*bForce=*/true, &PackageName, &SizeBytes,
            &SaveState);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("previousCollisionTraceFlag"), CollisionTraceFlagName(PreviousFlag));
    Result->SetStringField(TEXT("collisionTraceFlag"), CollisionTraceFlagName(BodySetup->CollisionTraceFlag));
    Result->SetStringField(TEXT("effectiveCollisionTraceFlag"), CollisionTraceFlagName(BodySetup->GetCollisionTraceFlag()));
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetNumberField(TEXT("componentsRecreated"), ComponentsRecreated);
    Result->SetStringField(TEXT("package"), PackageName);
    AddAssetSaveSizeReport(Result, SizeBytes, bSavedToDisk);
    AddAssetSaveReport(Result, /*bSaveRequested=*/bSave, bSavedToDisk, SaveState);
    Ctx.SendSuccess(TEXT("StaticMesh collision complexity updated"), Result);
    return true;
}
