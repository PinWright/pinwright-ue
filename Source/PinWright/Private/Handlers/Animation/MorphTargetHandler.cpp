// Copyright (c) 2026 Alexander Penkin. MIT License.

// MorphTargetHandler.cpp - Migrated from PinWright_SkeletonHandlers.cpp
// Morph target operations: create, delete, list, set deltas, set value, import
//
// Phase 14 migration to auto-registration system.

#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"


#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/MorphTarget.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "EngineUtils.h"
#include "Misc/Paths.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif

namespace {

static FString DescribeAssetTypeMorph(const UObject* Asset)
{
    if (!Asset || !Asset->GetClass())
    {
        return TEXT("UObject");
    }
    if (Asset->IsA(USkeleton::StaticClass()))
    {
        return TEXT("USkeleton");
    }
    if (Asset->IsA(USkeletalMesh::StaticClass()))
    {
        return TEXT("USkeletalMesh");
    }
    return Asset->GetClass()->GetName();
}

// Helper: Load skeletal mesh asset from path
static USkeletalMesh* LoadSkeletalMeshFromPathMorph(const FString& MeshPath, FString& OutError, bool* bOutWrongType = nullptr)
{
    OutError.Reset();
    if (bOutWrongType)
    {
        *bOutWrongType = false;
    }
    if (MeshPath.IsEmpty())
    {
        OutError = TEXT("Skeletal mesh path is required");
        return nullptr;
    }

    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *MeshPath);
    if (!Asset)
    {
        OutError = FString::Printf(TEXT("Skeletal mesh asset not found: %s"), *MeshPath);
        return nullptr;
    }

    USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset);
    if (!Mesh)
    {
        if (bOutWrongType)
        {
            *bOutWrongType = true;
        }
        OutError = FString::Printf(TEXT("Asset '%s' is a %s, not a USkeletalMesh. Pass a USkeletalMesh asset path."), *MeshPath, *DescribeAssetTypeMorph(Asset));
        return nullptr;
    }

    return Mesh;
}

static void SendMeshPathErrorMorph(FHandlerContext& Ctx, const FString& Error, bool bWrongType)
{
    Ctx.SendError(
        bWrongType ? ErrorCodes::ERR_INVALID_ASSET_TYPE : ErrorCodes::ERR_MESH_NOT_FOUND,
        Error);
}

} // anonymous namespace


// ===========================================================================
// skeleton.create_morph_target - Create a new morph target
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.create_morph_target", "skeleton",
    "Create a UMorphTarget on a SkeletalMesh (the asset that holds per-vertex deltas for blendshapes / facial expressions). Idempotent — returns alreadyExists=true if a morph with that name is already on the mesh. The engine drops morphs that carry no vertex deltas, so a delta-less create fails with MORPH_NOT_PERSISTED; supply the shape by importing a populated FBX via asset.import, or author it as an actual blendshape — skeleton.set_morph_target_deltas only writes into a morph that already persisted.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Asset path to the USkeletalMesh that will host the morph."),
        RPC_PARAM_REQ("morphTargetName", "string", "Identifier for the new morph target; must be unique within the mesh.")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString MorphTargetName = Ctx.GetString(TEXT("morphTargetName"));

    if (SkeletalMeshPath.IsEmpty() || MorphTargetName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("skeletalMeshPath and morphTargetName are required"));
        return true;
    }

    // Build the morph FName once: it drives the existing-morph lookup, the NewObject name, and
    // the verify-after-register lookup below. Each FName(string) hashes + does a name-table
    // lookup, so reusing one build avoids that work three times for one constant name.
    const FName MorphFName(*MorphTargetName);

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathMorph(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorMorph(Ctx, Error, bWrongType);
        return true;
    }

    UMorphTarget* ExistingMorph = Mesh->FindMorphTarget(MorphFName);
    if (ExistingMorph)
    {
        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetStringField(TEXT("morphTargetName"), MorphTargetName);
        Result->SetBoolField(TEXT("alreadyExists"), true);

        Ctx.SendSuccess(Result);
        return true;
    }

    UMorphTarget* NewMorphTarget = NewObject<UMorphTarget>(Mesh, MorphFName);
    if (!NewMorphTarget)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED,
            TEXT("Failed to create morph target object"));
        return true;
    }

    int32 DeltaCount = 0;
    for (int32 LODIndex = 0; LODIndex < NewMorphTarget->GetMorphLODModels().Num(); ++LODIndex)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        DeltaCount += NewMorphTarget->GetNumDeltasForLOD(LODIndex);
#else
        // UMorphTarget::GetNumDeltasForLOD arrived in 5.7; it reads exactly these LOD model
        // fields (MorphTarget.cpp:367-379).
        const FMorphTargetLODModel& LODModel = NewMorphTarget->GetMorphLODModels()[LODIndex];
#if WITH_EDITOR
        DeltaCount += LODModel.Vertices.Num();
#else
        DeltaCount += LODModel.NumVertices;
#endif
#endif
    }

    // RegisterMorphTarget ensures on this exact precondition in UE 5.8. Reject an
    // empty target before entering engine code so normal bad input cannot produce
    // a crash report. Check both the engine predicate and its underlying count.
    if (!NewMorphTarget->HasValidData() || DeltaCount == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_MORPH_NOT_PERSISTED,
            FString::Printf(
                TEXT("Morph target '%s' cannot be registered: it has no valid morph data ")
                TEXT("(%d vertex deltas). Supply deltas at creation, or import a populated ")
                TEXT("morph by importing an FBX via asset.import."),
                *MorphTargetName,
                DeltaCount));
        return true;
    }

    Mesh->RegisterMorphTarget(NewMorphTarget);

    // Keep the postcondition too: valid input must still refuse rather than fake
    // success if the engine declines to retain the target for another reason.
    if (!Mesh->FindMorphTarget(MorphFName))
    {
        Ctx.SendError(ErrorCodes::ERR_MORPH_NOT_PERSISTED,
            FString::Printf(
                TEXT("Morph target '%s' passed data validation but was not persisted by the engine."),
                *MorphTargetName));
        return true;
    }

    McpSafeAssetSave(Mesh);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("morphTargetName"), MorphTargetName);
    Result->SetNumberField(TEXT("morphTargetCount"), Mesh->GetMorphTargets().Num());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.set_morph_target_deltas - Set vertex deltas for a morph target
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.set_morph_target_deltas", "skeleton",
    "Write per-vertex position (and optional normal) deltas into an existing UMorphTarget. Vertices not listed retain their current deltas. Use after skeleton.create_morph_target to populate the morph shape.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_REQ("morphTargetName", "string", "Morph target name"),
        RPC_PARAM_REQ("deltas", "array", "Array of delta objects {vertexIndex, positionDelta:{x,y,z}, tangentDelta:{x,y,z}}")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString MorphTargetName = Ctx.GetString(TEXT("morphTargetName"));

    if (SkeletalMeshPath.IsEmpty() || MorphTargetName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("skeletalMeshPath and morphTargetName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathMorph(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorMorph(Ctx, Error, bWrongType);
        return true;
    }

    UMorphTarget* MorphTarget = Mesh->FindMorphTarget(FName(*MorphTargetName));
    if (!MorphTarget)
    {
        Ctx.SendError(ErrorCodes::ERR_MORPH_NOT_FOUND,
            FString::Printf(TEXT("Morph target '%s' not found"), *MorphTargetName));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* DeltasArray = nullptr;
    if (!Payload->TryGetArrayField(TEXT("deltas"), DeltasArray) || !DeltasArray)
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("deltas array is required"));
        return true;
    }

    TArray<FMorphTargetDelta> Deltas;
    for (const TSharedPtr<FJsonValue>& DeltaValue : *DeltasArray)
    {
        const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
        if (DeltaValue->TryGetObject(DeltaObj) && DeltaObj && DeltaObj->IsValid())
        {
            FMorphTargetDelta Delta;

            double VertexIndex = 0;
            (*DeltaObj)->TryGetNumberField(TEXT("vertexIndex"), VertexIndex);
            Delta.SourceIdx = static_cast<uint32>(VertexIndex);

            const TSharedPtr<FJsonObject>* PositionDelta = nullptr;
            if ((*DeltaObj)->TryGetObjectField(TEXT("positionDelta"), PositionDelta) && PositionDelta && PositionDelta->IsValid())
            {
                double X = 0, Y = 0, Z = 0;
                (*PositionDelta)->TryGetNumberField(TEXT("x"), X);
                (*PositionDelta)->TryGetNumberField(TEXT("y"), Y);
                (*PositionDelta)->TryGetNumberField(TEXT("z"), Z);
                Delta.PositionDelta = FVector3f(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
            }

            const TSharedPtr<FJsonObject>* TangentDelta = nullptr;
            if ((*DeltaObj)->TryGetObjectField(TEXT("tangentDelta"), TangentDelta) && TangentDelta && TangentDelta->IsValid())
            {
                double X = 0, Y = 0, Z = 0;
                (*TangentDelta)->TryGetNumberField(TEXT("x"), X);
                (*TangentDelta)->TryGetNumberField(TEXT("y"), Y);
                (*TangentDelta)->TryGetNumberField(TEXT("z"), Z);
                Delta.TangentZDelta = FVector3f(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
            }

            Deltas.Add(Delta);
        }
    }

    if (Deltas.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("deltas must contain at least one valid delta object; no morph data was changed"));
        return true;
    }

    TArray<FSkelMeshSection> EmptySections;
    MorphTarget->PopulateDeltas(Deltas, 0, EmptySections, false, false);

    McpSafeAssetSave(Mesh);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("morphTargetName"), MorphTargetName);
    Result->SetNumberField(TEXT("deltaCount"), Deltas.Num());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.list_morph_targets - List all morph targets on a skeletal mesh
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.list_morph_targets", "skeleton",
    "Enumerate every UMorphTarget on a SkeletalMesh by name, with each morph target's vertex-delta count. Use to discover authored blendshapes before driving them via skeleton.set_morph_target_value.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("skeletalMeshPath"), TEXT("path"),
            TEXT("Path to the skeletal mesh (or meshPath)"),
            /*bRequired=*/true, TArray<FString>({TEXT("skeletalMeshPath"), TEXT("meshPath")}))
    ))
{
    FString SkeletalMeshPath = Ctx.GetStringFirstOf({TEXT("skeletalMeshPath"), TEXT("meshPath")});

    if (SkeletalMeshPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM, TEXT("skeletalMeshPath is required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathMorph(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorMorph(Ctx, Error, bWrongType);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> MorphTargetArray;
    for (const UMorphTarget* MT : Mesh->GetMorphTargets())
    {
        if (MT)
        {
            TSharedPtr<FJsonObject> MTObj = MakeShareable(new FJsonObject());
            MTObj->SetStringField(TEXT("name"), MT->GetName());
            MTObj->SetNumberField(TEXT("numDeltas"), MT->GetMorphLODModels().Num() > 0 ?
                MT->GetMorphLODModels()[0].Vertices.Num() : 0);
            MorphTargetArray.Add(MakeShareable(new FJsonValueObject(MTObj)));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetArrayField(TEXT("morphTargets"), MorphTargetArray);
    Result->SetNumberField(TEXT("count"), MorphTargetArray.Num());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.delete_morph_target - Remove a morph target from skeletal mesh
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.delete_morph_target", "skeleton",
    "Delete a UMorphTarget from a SkeletalMesh by name. Counterpart to skeleton.create_morph_target. Animation tracks driving the deleted morph will silently no-op at runtime.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path", "Path to the skeletal mesh"),
        RPC_PARAM_REQ("morphTargetName", "string", "Name of the morph target to delete")
    ))
{
    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString MorphTargetName = Ctx.GetString(TEXT("morphTargetName"));

    if (SkeletalMeshPath.IsEmpty() || MorphTargetName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("skeletalMeshPath and morphTargetName are required"));
        return true;
    }

    FString Error;
    bool bWrongType = false;
    USkeletalMesh* Mesh = LoadSkeletalMeshFromPathMorph(SkeletalMeshPath, Error, &bWrongType);
    if (!Mesh)
    {
        SendMeshPathErrorMorph(Ctx, Error, bWrongType);
        return true;
    }

    UMorphTarget* TargetToRemove = nullptr;
    int32 Index = INDEX_NONE;
    for (int32 i = 0; i < Mesh->GetMorphTargets().Num(); ++i)
    {
        if (Mesh->GetMorphTargets()[i] && Mesh->GetMorphTargets()[i]->GetFName() == FName(*MorphTargetName))
        {
            TargetToRemove = Mesh->GetMorphTargets()[i];
            Index = i;
            break;
        }
    }

    if (!TargetToRemove || Index == INDEX_NONE)
    {
        Ctx.SendError(ErrorCodes::ERR_MORPH_NOT_FOUND,
            FString::Printf(TEXT("Morph target '%s' not found"), *MorphTargetName));
        return true;
    }

    Mesh->Modify();
    Mesh->UnregisterMorphTarget(TargetToRemove);
    Mesh->MarkPackageDirty();
    McpSafeAssetSave(Mesh);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Result->SetStringField(TEXT("morphTargetName"), MorphTargetName);
    Result->SetNumberField(TEXT("remainingMorphTargets"), Mesh->GetMorphTargets().Num());

    Ctx.SendSuccess(Result);
    return true;
}


// ===========================================================================
// skeleton.set_morph_target_value - Set morph target weight on actor
// ===========================================================================

REGISTER_RPC_HANDLER("skeleton.set_morph_target_value", "skeleton",
    "Drive a morph target weight (0..1) on a USkeletalMeshComponent of a level actor. Affects the runtime preview only — not the asset. Useful for authoring tests; persistent values belong on animation tracks.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor"),
        RPC_PARAM_REQ("morphTargetName", "string", "Morph target name"),
        RPC_PARAM_OPT("value", "number", "Weight value 0.0-1.0 (default 0.0)"),
        RPC_PARAM_OPT("addMissing", "boolean", "Set even if morph target not found on mesh (default false)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString MorphTargetName = Ctx.GetString(TEXT("morphTargetName"));
    double Value = Ctx.GetNumber(TEXT("value"), 0.0);
    bool bAddMissing = Ctx.GetBool(TEXT("addMissing"), false);

    if (ActorName.IsEmpty() || MorphTargetName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("actorName and morphTargetName are required"));
        return true;
    }

    Value = FMath::Clamp(Value, 0.0, 1.0);

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No world available"));
        return true;
    }

    AActor* FoundActor = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
        {
            FoundActor = *It;
            break;
        }
    }

    if (!FoundActor)
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
            FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    USkeletalMeshComponent* SkelMeshComp = FoundActor->FindComponentByClass<USkeletalMeshComponent>();
    if (!SkelMeshComp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_SKEL_MESH_COMP,
            TEXT("Actor does not have a SkeletalMeshComponent"));
        return true;
    }

    USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
    if (SkelMesh)
    {
        bool bHasMorphTarget = false;
        for (const UMorphTarget* MT : SkelMesh->GetMorphTargets())
        {
            if (MT && MT->GetFName() == FName(*MorphTargetName))
            {
                bHasMorphTarget = true;
                break;
            }
        }

        if (!bHasMorphTarget && !bAddMissing)
        {
            Ctx.SendError(ErrorCodes::ERR_MORPH_TARGET_NOT_FOUND,
                FString::Printf(TEXT("Morph target '%s' not found on mesh"), *MorphTargetName));
            return true;
        }
    }

    SkelMeshComp->SetMorphTarget(FName(*MorphTargetName), static_cast<float>(Value));

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetStringField(TEXT("morphTargetName"), MorphTargetName);
    Result->SetNumberField(TEXT("value"), Value);

    TArray<TSharedPtr<FJsonValue>> ActiveMorphs;
    const TMap<FName, float>& MorphCurves = SkelMeshComp->GetMorphTargetCurves();
    for (const auto& Pair : MorphCurves)
    {
        if (Pair.Value > 0.0f)
        {
            TSharedPtr<FJsonObject> MorphObj = MakeShareable(new FJsonObject());
            MorphObj->SetStringField(TEXT("name"), Pair.Key.ToString());
            MorphObj->SetNumberField(TEXT("weight"), Pair.Value);
            ActiveMorphs.Add(MakeShareable(new FJsonValueObject(MorphObj)));
        }
    }
    Result->SetArrayField(TEXT("activeMorphTargets"), ActiveMorphs);

    Ctx.SendSuccess(Result);
    return true;
}
