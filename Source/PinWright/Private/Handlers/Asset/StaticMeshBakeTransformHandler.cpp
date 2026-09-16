// Copyright (c) 2026 Alexander Penkin. MIT License.

// StaticMeshBakeTransformHandler.cpp - Bake a rotation/translation/uniform-scale transform
// into a StaticMesh asset in place (static_mesh.bake_transform).
//
// An asset authored on the wrong axis or at the wrong origin previously had to be countered
// per instance (actor.set_transform on every placement) — the asset itself was unreachable.
// This verb rewrites the asset once: every source-model LOD's mesh description, the hi-res
// (Nanite) source, the simple-collision primitives, and the sockets are all transformed,
// then the mesh rebuilds and (by default) saves. Mirror (negative) and zero scales are
// rejected — a rotator + translation + uniform scale cannot encode a reflection, and the
// collision radii/extents scale as plain positive factors.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "Compat/EngineVersionCompat.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "MeshDescription.h"
#include "StaticMeshOperations.h"
#include "StaticMeshCompiler.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "Utils/AssetUtils.h"
#include "Utils/MeshRebuildRenderGuard.h"
#include "Dom/JsonObject.h"

// UE 5.4 added the bApplyCorrectNormalTransform parameter to FStaticMeshOperations::ApplyTransform;
// 5.3 has only the 2-arg overload.
static void ApplyBakeTransformToMeshDescription(FMeshDescription& MeshDescription, const FTransform& Transform)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    FStaticMeshOperations::ApplyTransform(MeshDescription, Transform, /*bApplyCorrectNormalTransform*/ true);
#else
    FStaticMeshOperations::ApplyTransform(MeshDescription, Transform);
#endif
}

REGISTER_RPC_HANDLER("static_mesh.bake_transform", "static_mesh",
    "Bake a rotation/translation/uniform-scale transform into a StaticMesh asset in place: all source-model LODs, hi-res source, simple collision, sockets; rebuilds and optionally saves",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "StaticMesh asset path"),
        RPC_PARAM_OPT("rotation", "object", "Rotator {pitch,yaw,roll} degrees baked into the asset (default zero)"),
        RPC_PARAM_OPT("translation", "object", "Vector {x,y,z} cm baked into the asset (default zero)"),
        RPC_PARAM_OPT("scale", "number", "Uniform scale factor > 0 (default 1)"),
        RPC_PARAM_OPT("save", "boolean", "Persist to disk after rebuild (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath)) return true;

    const FRotator Rotation = Ctx.GetRotator(TEXT("rotation"), FRotator::ZeroRotator);
    const FVector Translation = Ctx.GetVector(TEXT("translation"), FVector::ZeroVector);
    const double UniformScale = Ctx.GetNumber(TEXT("scale"), 1.0);
    const bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (UniformScale <= UE_SMALL_NUMBER)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("scale must be a positive uniform factor; got %g (mirror/zero scale is unsupported)"), UniformScale));
        return true;
    }

    if (Rotation.IsZero() && Translation.IsZero() && UniformScale == 1.0)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("noop"), true);
        Ctx.SendSuccess(TEXT("Identity transform - nothing to bake, asset untouched"), Result);
        return true;
    }

    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
    if (!Mesh)
    {
        Ctx.SendError(TEXT("MESH_NOT_FOUND"),
            FString::Printf(TEXT("Could not load StaticMesh: %s"), *AssetPath));
        return true;
    }

    const FTransform BakeTransform(Rotation, Translation, FVector(UniformScale));

    // Cache once — the source-model accessors run an async-property wait per call.
    const int32 NumSourceModels = Mesh->GetNumSourceModels();

    // "Nothing to transform" guard: no source geometry, no simple collision, no sockets.
    bool bAnySourceMeshDescription = false;
    for (int32 LodIndex = 0; LodIndex < NumSourceModels; ++LodIndex)
    {
        if (Mesh->IsMeshDescriptionValid(LodIndex))
        {
            bAnySourceMeshDescription = true;
            break;
        }
    }
    const UBodySetup* GuardBodySetup = Mesh->GetBodySetup();
    const bool bAnySimpleCollision = GuardBodySetup && GuardBodySetup->AggGeom.GetElementCount() > 0;
    if (!bAnySourceMeshDescription && !bAnySimpleCollision && Mesh->Sockets.Num() == 0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("StaticMesh has no source mesh description, no simple collision, and no sockets - nothing to transform: %s"), *AssetPath));
        return true;
    }

    TArray<FString> RebuildPaths;
    RebuildPaths.Add(AssetPath);
    return PinWrightMeshRebuild::RunGuardedStaticMeshRebuild(Ctx,
        TEXT("static_mesh.bake_transform"), RebuildPaths,
        [AssetPath, BakeTransform, UniformScale, bSave, NumSourceModels](
            const PinWrightSafePoint::FSafePointResponder& Responder,
            const TArray<UStaticMesh*>& Meshes)
    {
        if (Meshes.Num() == 0)
        {
            Responder.SendError(TEXT("MESH_NOT_FOUND"),
                FString::Printf(TEXT("Could not load StaticMesh: %s"), *AssetPath));
            return;
        }

        UStaticMesh* Mesh = Meshes[0];

        // PreEditChange finishes any in-flight async compilation and releases render
        // resources — required before mutating the source models.
        Mesh->PreEditChange(nullptr);
        Mesh->Modify();

    // Render LODs: transform every source model that owns a mesh description. Generated
    // (reduction) LODs have none of their own — CommitMeshDescription check-asserts on
    // them, and the PostEditChange rebuild regenerates them from the transformed source.
    int32 LodsTransformed = 0;
    int32 GeneratedLods = 0;
    int64 Verts = 0;
    for (int32 LodIndex = 0; LodIndex < NumSourceModels; ++LodIndex)
    {
        if (!Mesh->IsMeshDescriptionValid(LodIndex))
        {
            ++GeneratedLods;
            continue;
        }
        FMeshDescription* MeshDescription = Mesh->GetMeshDescription(LodIndex);
        if (!MeshDescription)
        {
            ++GeneratedLods;
            continue;
        }
        ApplyBakeTransformToMeshDescription(*MeshDescription, BakeTransform);
        Mesh->CommitMeshDescription(LodIndex);
        ++LodsTransformed;
        Verts += MeshDescription->Vertices().Num();
    }

    // Hi-res (Nanite) source: mandatory — leaving it untouched would revert the mesh on
    // the next Nanite rebuild from the stale hi-res geometry.
    bool bHiResTransformed = false;
    if (Mesh->IsHiResMeshDescriptionValid())
    {
        if (FMeshDescription* HiResDescription = Mesh->GetHiResMeshDescription())
        {
            ApplyBakeTransformToMeshDescription(*HiResDescription, BakeTransform);
            Mesh->CommitHiResMeshDescription();
            bHiResTransformed = true;
        }
    }

    // Simple collision: transform the analytic primitives in place. Float shape
    // dimensions scale by the float-narrowed factor; centers/rotations use the full
    // double transform.
    const float UniformScaleFloat = static_cast<float>(UniformScale);
    int32 ConvexTransformed = 0;
    int32 BoxTransformed = 0;
    int32 SphereTransformed = 0;
    int32 SphylTransformed = 0;
    int32 TaperedCapsuleTransformed = 0;
    int32 SkippedElems = 0;
    if (UBodySetup* BodySetup = Mesh->GetBodySetup())
    {
        BodySetup->Modify();
        FKAggregateGeom& AggGeom = BodySetup->AggGeom;

        for (FKConvexElem& Elem : AggGeom.ConvexElems)
        {
            // Fold any authored elem transform into VertexData first (BakeTransformToVerts
            // resets it to identity) — otherwise hulls with a non-identity transform would
            // double-transform when the elem transform re-applies on top of the baked verts.
            if (!Elem.GetTransform().Equals(FTransform::Identity))
            {
                Elem.BakeTransformToVerts();
            }
            for (FVector& Vertex : Elem.VertexData)
            {
                Vertex = BakeTransform.TransformPosition(Vertex);
            }
            // Recomputes ElemBox AND rebuilds the Chaos convex index data from VertexData.
            Elem.UpdateElemBox();
            ++ConvexTransformed;
        }
        for (FKBoxElem& Elem : AggGeom.BoxElems)
        {
            Elem.Center = BakeTransform.TransformPosition(Elem.Center);
            // FQuat composition applies the RIGHT operand first: elem rotation, then bake rotation.
            Elem.Rotation = (BakeTransform.GetRotation() * Elem.Rotation.Quaternion()).Rotator();
            Elem.X *= UniformScaleFloat;
            Elem.Y *= UniformScaleFloat;
            Elem.Z *= UniformScaleFloat;
            ++BoxTransformed;
        }
        for (FKSphereElem& Elem : AggGeom.SphereElems)
        {
            Elem.Center = BakeTransform.TransformPosition(Elem.Center);
            Elem.Radius *= UniformScaleFloat;
            ++SphereTransformed;
        }
        for (FKSphylElem& Elem : AggGeom.SphylElems)
        {
            Elem.Center = BakeTransform.TransformPosition(Elem.Center);
            Elem.Rotation = (BakeTransform.GetRotation() * Elem.Rotation.Quaternion()).Rotator();
            Elem.Radius *= UniformScaleFloat;
            Elem.Length *= UniformScaleFloat;
            ++SphylTransformed;
        }
        for (FKTaperedCapsuleElem& Elem : AggGeom.TaperedCapsuleElems)
        {
            Elem.Center = BakeTransform.TransformPosition(Elem.Center);
            Elem.Rotation = (BakeTransform.GetRotation() * Elem.Rotation.Quaternion()).Rotator();
            Elem.Radius0 *= UniformScaleFloat;
            Elem.Radius1 *= UniformScaleFloat;
            Elem.Length *= UniformScaleFloat;
            ++TaperedCapsuleTransformed;
        }
        // Level-set / skinned elems carry baked volumetric data that an analytic transform
        // cannot rewrite — leave them untouched and report them as skipped.
        SkippedElems += AggGeom.LevelSetElems.Num();
        SkippedElems += AggGeom.SkinnedLevelSetElems.Num();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        SkippedElems += AggGeom.MLLevelSetElems.Num();
        SkippedElems += AggGeom.SkinnedTriangleMeshElems.Num();
#endif

        // Drop stale cooked physics so the transformed primitives recook; the
        // PostEditChange rebuild handles the recook (no CreatePhysicsMeshes here).
        BodySetup->InvalidatePhysicsData();
    }

    // Sockets ride the same transform so attachments stay glued to the geometry.
    int32 SocketsTransformed = 0;
    for (UStaticMeshSocket* Socket : Mesh->Sockets)
    {
        if (!Socket)
        {
            continue;
        }
        Socket->Modify();
        Socket->RelativeLocation = BakeTransform.TransformPosition(Socket->RelativeLocation);
        Socket->RelativeRotation = (BakeTransform.GetRotation() * Socket->RelativeRotation.Quaternion()).Rotator();
        Socket->RelativeScale *= UniformScale;
        ++SocketsTransformed;
    }

    // Bounds extensions are axis-aligned padding, not geometry — they scale but cannot
    // be rotated exactly, so flag the approximation in the response.
    bool bBoundsExtensionsScaledOnly = false;
    if (Mesh->GetPositiveBoundsExtension() != FVector::ZeroVector ||
        Mesh->GetNegativeBoundsExtension() != FVector::ZeroVector)
    {
        Mesh->SetPositiveBoundsExtension(Mesh->GetPositiveBoundsExtension() * UniformScale);
        Mesh->SetNegativeBoundsExtension(Mesh->GetNegativeBoundsExtension() * UniformScale);
        bBoundsExtensionsScaledOnly = true;
    }

    // PostEditChange runs the full static-mesh Build internally — do NOT also call
    // Mesh->Build(), that would double-build. Finishing compilation makes the bounds
    // read and the save below deterministic.
    Mesh->PostEditChange();
    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    // Persist so the SAVED asset carries the baked geometry; report persistence
    // honestly via the shared save-report contract, mirroring static_mesh.set_material.
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

    const FBox NewBounds = Mesh->GetBoundingBox();
    auto MakeVectorJson = [](const FVector& V)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), V.X);
        Obj->SetNumberField(TEXT("y"), V.Y);
        Obj->SetNumberField(TEXT("z"), V.Z);
        return Obj;
    };

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetNumberField(TEXT("lodsTransformed"), LodsTransformed);
    Result->SetNumberField(TEXT("generatedLodsRebuilt"), GeneratedLods);
    Result->SetBoolField(TEXT("hiResTransformed"), bHiResTransformed);
    Result->SetNumberField(TEXT("verts"), static_cast<double>(Verts));

    TSharedPtr<FJsonObject> Collision = MakeShared<FJsonObject>();
    Collision->SetNumberField(TEXT("convex"), ConvexTransformed);
    Collision->SetNumberField(TEXT("box"), BoxTransformed);
    Collision->SetNumberField(TEXT("sphere"), SphereTransformed);
    Collision->SetNumberField(TEXT("sphyl"), SphylTransformed);
    Collision->SetNumberField(TEXT("taperedCapsule"), TaperedCapsuleTransformed);
    Collision->SetNumberField(TEXT("skipped"), SkippedElems);
    Result->SetObjectField(TEXT("collision"), Collision);

    Result->SetNumberField(TEXT("socketsTransformed"), SocketsTransformed);
    if (bBoundsExtensionsScaledOnly)
    {
        Result->SetBoolField(TEXT("boundsExtensionsScaledOnly"), true);
    }

    TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
    BoundsJson->SetObjectField(TEXT("min"), MakeVectorJson(NewBounds.Min));
    BoundsJson->SetObjectField(TEXT("max"), MakeVectorJson(NewBounds.Max));
    Result->SetObjectField(TEXT("newBounds"), BoundsJson);

    Result->SetStringField(TEXT("package"), PackageName);
    AddAssetSaveSizeReport(Result, SizeBytes, bSavedToDisk);
    AddAssetSaveReport(Result, /*bSaveRequested=*/bSave, bSavedToDisk, SaveState);
    Responder.SendSuccess(TEXT("Transform baked into StaticMesh"), Result);
    });
}
