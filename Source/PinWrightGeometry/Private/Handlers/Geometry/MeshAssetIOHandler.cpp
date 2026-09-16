// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshAssetIOHandler.cpp - StaticMesh ASSET -> editable DynamicMeshActor.
//
// The inverse of geometry.convert_to_static_mesh, and the file-format sibling of
// MeshIOHandler.cpp (which does OBJ/STL): that one moves geometry across the filesystem
// boundary, this one moves it across the asset boundary.
//
// Before this verb the geometry namespace was write-only with respect to assets. ~85 ops
// could build and mutate a live ADynamicMeshActor, convert_to_static_mesh could bake one
// into a UStaticMesh, and NOTHING could read a UStaticMesh back - so a baked blockout mesh,
// or any pre-existing project mesh, was permanently out of reach of every geometry op.
//
// Engine call: UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh
// (GeometryScriptingCore, MeshAssetFunctions.h). The 6-arg overload is deliberate: on UE
// 5.5+ it is a non-deprecated header-inline forwarder to the exported
// CopyMeshFromStaticMeshV2 with bUseSectionMaterials=true, and on 5.3/5.4 it IS the exported
// function - so it needs no UE_VERSION_* guard, unlike the write direction (see
// MeshOpsHandler.cpp's convert_to_static_mesh, where the 6-arg CopyMeshToStaticMesh is
// UE_DEPRECATED(5.5)).
//
// The copy REPLACES the destination UDynamicMesh's contents (ToDynamicMesh->SetMesh(
// MoveTemp(NewMesh)) at MeshAssetFunctions.cpp:118), which is what makes reuseExisting
// correct by construction rather than by an extra clear step.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Handlers/Geometry/GeometryNameParamUtils.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMeshActor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "UDynamicMesh.h"
#include "Utils/AssetUtils.h"
#include "Utils/PathUtils.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshAssetFunctions.h"

// Every helper below is prefixed MeshAssetIO. The module builds with bUseUnity = true, so two
// anonymous-namespace statics sharing a name in different .cpp files become an ODR redefinition
// once the TUs are merged; helpers that several files need live in a named-namespace header
// instead (GeometryTarget.h).
namespace
{
    // Cap on the per-slot material detail array. A blockout mesh has 1-3 slots; a Quixel
    // kitbash can have dozens. Bounded like the sibling list handlers so a pathological
    // asset cannot push the response toward the 10,000-char spill threshold. materialSlots
    // always carries the true total.
    constexpr int32 MeshAssetIOMaxMaterialDetail = 32;

    bool MeshAssetIOParseLodType(const FString& Token, EGeometryScriptLODType& OutType)
    {
        if (Token.IsEmpty() || Token.Equals(TEXT("MaxAvailable"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptLODType::MaxAvailable;
            return true;
        }
        if (Token.Equals(TEXT("HiResSourceModel"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptLODType::HiResSourceModel;
            return true;
        }
        if (Token.Equals(TEXT("SourceModel"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptLODType::SourceModel;
            return true;
        }
        if (Token.Equals(TEXT("RenderData"), ESearchCase::IgnoreCase))
        {
            OutType = EGeometryScriptLODType::RenderData;
            return true;
        }
        return false;
    }

    const TCHAR* MeshAssetIOLodTypeName(EGeometryScriptLODType Type)
    {
        switch (Type)
        {
        case EGeometryScriptLODType::HiResSourceModel: return TEXT("HiResSourceModel");
        case EGeometryScriptLODType::SourceModel:      return TEXT("SourceModel");
        case EGeometryScriptLODType::RenderData:       return TEXT("RenderData");
        default:                                       return TEXT("MaxAvailable");
        }
    }

    // Resolve a placed actor's StaticMesh asset plus its world transform.
    //
    // Deliberately reads the component's ASSET rather than going through
    // UGeometryScriptLibrary_SceneUtilityFunctions::CopyMeshFromComponent. Two reasons:
    // one engine API instead of two, and the mesh stays in LOCAL space, which is the
    // load-bearing convention every geometry verb assumes (GeometryTarget::Spawn -
    // baking a transform into both the vertices AND the actor double-applies it and
    // silently breaks the boolean verbs). The actor transform is carried onto the new
    // DynamicMeshActor instead. Cost: ISM/HISM per-instance transforms are NOT baked - the
    // caller gets the source mesh once, at the component's own transform. Documented.
    UStaticMesh* MeshAssetIOResolveActorMesh(
        UWorld* World,
        const FString& Label,
        AActor*& OutActor,
        FTransform& OutTransform,
        McpActorUtils::FActorResolution* OutResolution)
    {
        OutActor = nullptr;
        if (OutResolution)
        {
            *OutResolution = McpActorUtils::FActorResolution();
        }
        if (!IsValid(World) || Label.IsEmpty())
        {
            return nullptr;
        }
        const McpActorUtils::FActorResolution Resolution =
            GeometryTarget::ResolveAnyActor(World, Label);
        if (OutResolution)
        {
            *OutResolution = Resolution;
        }
        OutActor = Resolution.IsResolved() ? Resolution.Actor : nullptr;
        if (!OutActor)
        {
            return nullptr;  // actor not found at all
        }

        TArray<UStaticMeshComponent*> Components;
        OutActor->GetComponents<UStaticMeshComponent>(Components);
        for (UStaticMeshComponent* Component : Components)
        {
            if (Component && Component->GetStaticMesh())
            {
                OutTransform = Component->GetComponentTransform();
                return Component->GetStaticMesh();
            }
        }
        return nullptr;      // actor found, but it carries no static mesh
    }

    // Echo the source asset's material slots so the caller can restore them after a
    // create-new bake (convert_to_static_mesh without overwrite drops materials -
    // StaticMeshSetMaterialHandler.cpp:5-11). Read off UStaticMesh::GetStaticMaterials(),
    // which is stable across UE 5.3-5.8, rather than
    // GetSectionMaterialListFromStaticMesh: see the section-vs-slot caveat in the wiki -
    // on UE 5.5+ the copied dynamic mesh's MaterialIDs are LOD SECTION indices, which
    // coincide with slot indices only for the common one-section-per-slot asset.
    void MeshAssetIOAddMaterialFields(const UStaticMesh* Mesh, const TSharedPtr<FJsonObject>& Result)
    {
        if (!Mesh || !Result.IsValid())
        {
            return;
        }
        const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
        Result->SetNumberField(TEXT("materialSlots"), Slots.Num());

        TArray<TSharedPtr<FJsonValue>> Materials;
        const int32 DetailCount = FMath::Min(Slots.Num(), MeshAssetIOMaxMaterialDetail);
        for (int32 Index = 0; Index < DetailCount; ++Index)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("index"), Index);
            Entry->SetStringField(TEXT("slot"), Slots[Index].MaterialSlotName.ToString());
            Entry->SetStringField(TEXT("path"),
                Slots[Index].MaterialInterface ? Slots[Index].MaterialInterface->GetPathName() : FString());
            Materials.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetArrayField(TEXT("materials"), Materials);
        if (DetailCount < Slots.Num())
        {
            Result->SetBoolField(TEXT("materialsTruncated"), true);
        }
    }
}

// ============================================================================
// create_from_static_mesh
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_from_static_mesh", "geometry",
    "Load an existing StaticMesh asset (or the mesh of a placed actor) into an editable DynamicMeshActor, so every geometry.* op applies to it. The exact inverse of geometry.convert_to_static_mesh. Idempotent by default: a second call with the same name reloads the SAME actor (reuseExisting) instead of leaving two identically-labelled ones.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("assetPath"), TEXT("path"),
            TEXT("StaticMesh asset to load, e.g. /Game/Meshes/SM_Tower. Provide exactly one of assetPath or sourceActor. The meshPath spelling the asset.* mesh verbs use is also accepted."),
            /*bRequired=*/false, TArray<FString>({TEXT("assetPath"), TEXT("meshPath")})),
        RPC_PARAM_OPT("sourceActor", "string", "Label of a placed actor to read instead; its first StaticMeshComponent's mesh AND world transform are used. Per-instance ISM/HISM transforms are NOT baked."),
        GeometryNameParamUtils::CreateNameParamOpt(
            TEXT("Label for the editable DynamicMeshActor (accepts the 'actorName' alias the operate verbs use). Default '<MeshName>_Edit'.")),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}; overrides the transform inherited from sourceActor"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch, yaw, roll}; overrides the transform inherited from sourceActor"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale {x, y, z}; overrides the transform inherited from sourceActor"),
        RPC_PARAM_DEF("lodType", "string", "Which mesh to read: MaxAvailable | HiResSourceModel | SourceModel | RenderData. SourceModel variants read the editable MeshDescription and are unaffected by Nanite; RenderData reads the built render mesh (split at UV seams and hard-normal creases)", "MaxAvailable"),
        RPC_PARAM_DEF("lodIndex", "integer", "LOD index for SourceModel/RenderData. The engine SILENTLY CLAMPS this to the available LOD count, so the echoed lodIndex is what you asked for, not necessarily what was read", "0"),
        RPC_PARAM_DEF("applyBuildSettings", "boolean", "Apply the asset's Build Settings during the copy (default true)", "true"),
        RPC_PARAM_DEF("useBuildScale", "boolean", "Scale the copied mesh by the asset's Build Scale (default true)", "true"),
        RPC_PARAM_DEF("requestTangents", "boolean", "Request tangents on the copied mesh (default true)", "true"),
        RPC_PARAM_DEF("reuseExisting", "boolean", "When a DynamicMeshActor already carries the target label, reload into it instead of spawning a duplicate (default true). Deliberately unlike the geometry.create_* family, which always spawns", "true")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString AssetPath = Ctx.GetStringFirstOf({ TEXT("assetPath"), TEXT("meshPath") }, FString());
    const FString SourceActorLabel = Ctx.GetString(TEXT("sourceActor"));

    // Exactly-one-source contract, mirroring actor.spawn_batch's shape/meshPath/classPath/
    // sourceActor rule. Rejecting both is not pedantry: silently preferring one would make
    // a typo'd assetPath look like a successful load of the wrong mesh.
    if (AssetPath.IsEmpty() == SourceActorLabel.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("Provide exactly one of assetPath or sourceActor"));
        return true;
    }

    EGeometryScriptLODType LodType = EGeometryScriptLODType::MaxAvailable;
    const FString LodTypeToken = Ctx.GetString(TEXT("lodType"));
    if (!MeshAssetIOParseLodType(LodTypeToken, LodType))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Unknown lodType '%s'. Valid: MaxAvailable, HiResSourceModel, SourceModel, RenderData"),
                *LodTypeToken));
        return true;
    }
    const int32 LodIndex = FMath::Max(0, Ctx.GetInt(TEXT("lodIndex"), 0));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!IsValid(World))
    {
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"), TEXT("Editor world not available"));
        return true;
    }

    // Resolve the source mesh + the transform the new actor should inherit.
    UStaticMesh* SourceMesh = nullptr;
    FTransform SourceTransform = FTransform::Identity;
    FString DefaultName;
    AActor* SourceActor = nullptr;

    if (!SourceActorLabel.IsEmpty())
    {
        McpActorUtils::FActorResolution SourceResolution;
        SourceMesh = MeshAssetIOResolveActorMesh(
            World, SourceActorLabel, SourceActor, SourceTransform, &SourceResolution);
        if (SourceResolution.IsAmbiguous())
        {
            ActorNameParamUtils::SendAmbiguousActorError(Ctx, SourceActorLabel, SourceResolution);
            return true;
        }
        if (!SourceActor)
        {
            Ctx.SendError(TEXT("ACTOR_NOT_FOUND"),
                FString::Printf(TEXT("Actor not found: %s"), *SourceActorLabel));
            return true;
        }
        if (!SourceMesh)
        {
            Ctx.SendError(TEXT("MESH_NOT_FOUND"),
                FString::Printf(TEXT("Actor '%s' has no StaticMeshComponent with a mesh assigned"),
                    *SourceActorLabel));
            return true;
        }
        AssetPath = SourceMesh->GetPathName();
        DefaultName = FString::Printf(TEXT("%s_Edit"), *SourceActorLabel);
    }
    else
    {
        // Same sanitizer convert_to_static_mesh uses, so the two halves of the round trip
        // accept exactly the same path vocabulary.
        const FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ASSET_PATH"),
                TEXT("Invalid assetPath - rejected due to security validation"));
            return true;
        }
        AssetPath = SanitizedAssetPath;

        SourceMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
        if (!SourceMesh)
        {
            Ctx.SendError(TEXT("MESH_NOT_FOUND"),
                FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));
            return true;
        }
        DefaultName = FString::Printf(TEXT("%s_Edit"), *SourceMesh->GetName());
    }

    const FString ActorLabel = GeometryNameParamUtils::ResolveCreateName(Ctx, DefaultName);

    // Guard before allocating: StaticMeshToDynamicMesh builds the whole FDynamicMesh3 up
    // front, so a Nanite hero asset can be a multi-hundred-MB spike. Same pre-flight the
    // heavy mesh ops use.
    if (!GeometryUtils::IsMemoryPressureSafe())
    {
        Ctx.SendError(TEXT("MEMORY_PRESSURE"),
            TEXT("Insufficient memory headroom to copy a StaticMesh into a dynamic mesh"));
        return true;
    }

    // reuseExisting is what makes a re-run idempotent. ActorLabel is a CREATE LABEL, not an
    // actor identity: resolve it by exact display label among DynamicMeshActors only, so a
    // coincident internal name or object path cannot capture the request and duplicate labels
    // remain an explicit ambiguity. Default true because this verb's identity is the SOURCE
    // ASSET, not the actor: asking for the editable form of /Game/.../SM_Tower twice means the
    // same thing twice. Pass reuseExisting:false (or a distinct name) for N independent copies.
    const bool bReuseExisting = Ctx.GetBool(TEXT("reuseExisting"), true);
    McpActorUtils::FActorResolution ExistingResolution;
    if (bReuseExisting)
    {
        ExistingResolution = McpActorUtils::ResolveActorFiltered(World, ActorLabel,
            [](AActor* Actor)
            {
                return Actor && Actor->IsA(ADynamicMeshActor::StaticClass());
            }, McpActorUtils::EActorResolvePolicy::ExactLabel);
        if (ExistingResolution.IsAmbiguous())
        {
            ActorNameParamUtils::SendAmbiguousActorError(Ctx, ActorLabel, ExistingResolution);
            return true;
        }
    }
    ADynamicMeshActor* ExistingActor = ExistingResolution.IsResolved()
        ? Cast<ADynamicMeshActor>(ExistingResolution.Actor)
        : nullptr;
    UDynamicMeshComponent* ExistingComponent =
        ExistingActor ? ExistingActor->GetDynamicMeshComponent() : nullptr;
    if (ExistingActor && !ExistingComponent)
    {
        // Pathological: a DynamicMeshActor with no component. Fall through to a fresh spawn
        // rather than erroring, so a corrupt actor cannot wedge the verb.
        ExistingActor = nullptr;
    }

    // On the reuse path we copy straight into the component's own UDynamicMesh, because
    // CopyMeshFromStaticMesh REPLACES the destination's contents wholesale
    // (ToDynamicMesh->SetMesh(MoveTemp(NewMesh))) - no clear step needed.
    UDynamicMesh* TargetMesh = ExistingComponent
        ? ExistingComponent->GetDynamicMesh()
        : GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
    if (!TargetMesh)
    {
        Ctx.SendError(TEXT("MESH_NOT_FOUND"), TEXT("Could not allocate a destination dynamic mesh"));
        return true;
    }

    FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
    CopyOptions.bApplyBuildSettings = Ctx.GetBool(TEXT("applyBuildSettings"), true);
    // FGeometryScriptCopyMeshFromAssetOptions::bUseBuildScale arrived in UE 5.4. On 5.3 the
    // converter applies the LOD's BuildScale3D unconditionally (GeometryScriptingCore
    // MeshAssetFunctions.cpp:156-159), i.e. it behaves exactly as bUseBuildScale=true, which is
    // the default here. A caller who asked for false is refused by name rather than silently
    // served a build-scaled mesh under a success.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    CopyOptions.bUseBuildScale = Ctx.GetBool(TEXT("useBuildScale"), true);
#else
    if (!Ctx.GetBool(TEXT("useBuildScale"), true))
    {
        Ctx.SendError(TEXT("UNSUPPORTED_ENGINE_VERSION"), TEXT(
            "useBuildScale=false needs FGeometryScriptCopyMeshFromAssetOptions::bUseBuildScale, "
            "added in UE 5.4. This engine always applies the LOD's build scale; omit the "
            "parameter or pass true."));
        return true;
    }
#endif
    CopyOptions.bRequestTangents = Ctx.GetBool(TEXT("requestTangents"), true);

    FGeometryScriptMeshReadLOD ReadLOD;
    ReadLOD.LODType = LodType;
    ReadLOD.LODIndex = LodIndex;

    EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

    // The 6-arg overload on purpose - version-stable across 5.3-5.8, see the file header.
    // Debug is nullptr to match every other geometry callsite; the cost is that the
    // engine's FText reason is discarded and we report a generic CONVERSION_FAILED.
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
        SourceMesh, TargetMesh, CopyOptions, ReadLOD, Outcome, nullptr);

    if (Outcome != EGeometryScriptOutcomePins::Success)
    {
        if (!ExistingComponent && TargetMesh)
        {
            TargetMesh->MarkAsGarbage();
        }
        Ctx.SendError(TEXT("CONVERSION_FAILED"),
            FString::Printf(TEXT("Could not copy '%s' (lodType %s, lodIndex %d) into a dynamic mesh"),
                *AssetPath, MeshAssetIOLodTypeName(LodType), LodIndex));
        return true;
    }

    // Post-copy budget check. Done AFTER the copy because neither GetNumTriangles nor the
    // source models give a reliable pre-count for every LODType, and enforcing it here
    // still prevents the expensive part: spawning a renderer + collision for a mesh the
    // rest of the namespace refuses to operate on anyway.
    const int32 TriangleCount = TargetMesh->GetTriangleCount();
    if (TriangleCount > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH)
    {
        if (!ExistingComponent && TargetMesh)
        {
            TargetMesh->MarkAsGarbage();
        }
        Ctx.SendError(TEXT("POLYGON_LIMIT_EXCEEDED"),
            FString::Printf(TEXT("'%s' has %d triangles at this LOD, over the %d dynamic-mesh limit. Load a coarser lodIndex, or use lodType RenderData with a higher lodIndex."),
                *AssetPath, TriangleCount, GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH));
        return true;
    }

    // Spawn transform: the source actor's transform is the default when sourcing from one,
    // and any explicitly-supplied location/rotation/scale overrides it component-wise.
    FTransform SpawnTransform = SourceTransform;
    if (Payload.IsValid())
    {
        if (Payload->HasField(TEXT("location")))
        {
            SpawnTransform.SetLocation(
                GeometryUtils::ReadVectorFromPayload(Payload, TEXT("location"), SpawnTransform.GetLocation()));
        }
        if (Payload->HasField(TEXT("rotation")))
        {
            SpawnTransform.SetRotation(FQuat(
                GeometryUtils::ReadRotatorFromPayload(Payload, TEXT("rotation"), SpawnTransform.Rotator())));
        }
        if (Payload->HasField(TEXT("scale")))
        {
            SpawnTransform.SetScale3D(
                GeometryUtils::ReadVectorFromPayload(Payload, TEXT("scale"), SpawnTransform.GetScale3D()));
        }
    }

    AActor* ResultActor = ExistingActor;
    if (ExistingComponent)
    {
        GeometryUtils::MarkGeometryActorModified(ExistingComponent);
    }
    else
    {
        // This verb never fell back to the EditorActorSubsystem's world the way the create_*
        // verbs do; keep it that way.
        GeometryTarget::FSpawnOptions SpawnOptions;
        SpawnOptions.bFallBackToActorSubsystemWorld = false;

        ResultActor = GeometryTarget::Spawn(Ctx, TargetMesh, SpawnTransform, ActorLabel, SpawnOptions);
        if (!ResultActor)
        {
            return true;  // spawn helper already sent the error
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ResultActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    if (!SourceActorLabel.IsEmpty())
    {
        Result->SetStringField(TEXT("sourceActor"), SourceActorLabel);
    }
    // reused:true means no new actor was created - the caller can tell an idempotent
    // re-run from a first run without diffing the outliner.
    Result->SetBoolField(TEXT("reused"), ExistingComponent != nullptr);
    Result->SetStringField(TEXT("lodType"), MeshAssetIOLodTypeName(LodType));
    Result->SetNumberField(TEXT("lodIndex"), LodIndex);
    // Same key names geometry.get_mesh_info reports, so a load is self-verifying in one
    // call instead of load -> get_mesh_info.
    GeometryUtils::SetMeshCountFields(TargetMesh, Result);
    {
        const UE::Geometry::FDynamicMesh3& ReadMesh = TargetMesh->GetMeshRef();
        Result->SetBoolField(TEXT("hasNormals"), ReadMesh.HasAttributes() && ReadMesh.Attributes()->PrimaryNormals() != nullptr);
        Result->SetBoolField(TEXT("hasUVs"), GeometryUtils::MeshHasUsableUVs(TargetMesh));
        Result->SetBoolField(TEXT("hasColors"), ReadMesh.HasAttributes() && ReadMesh.Attributes()->HasPrimaryColors());
        Result->SetBoolField(TEXT("hasPolygroups"), ReadMesh.HasTriangleGroups());
    }
    MeshAssetIOAddMaterialFields(SourceMesh, Result);
    AddActorVerification(Result, ResultActor);
    GeometryUtils::AddResolvedActorIdentity(Result, SourceActor, TEXT("source"));
    Ctx.SendSuccess(TEXT("StaticMesh loaded into an editable DynamicMesh"), Result);
    return true;
}
