// Copyright (c) 2026 Alexander Penkin. MIT License.

// VolumeHandler.cpp - Migrated from PinWright_VolumeHandlers.cpp
// Phase 24: Volumes & Zones Handlers
//
// Complete volume and trigger system including:
// - Trigger Volumes (trigger_volume, trigger_sphere, trigger_capsule)
// - Gameplay Volumes (blocking, kill_z, pain_causing, physics)
// - Audio Volumes (audio, reverb)
// - Rendering Volumes (cull_distance, precomputed_visibility, lightmass_importance, post_process)
// - Navigation Volumes (nav_mesh_bounds, nav_modifier, camera_blocking)
// - Volume Configuration (set_volume_extent, set_volume_properties, set_volume_bounds)
// - Volume Info (get_volumes_info)
// - Add Volume To Actor (add_trigger_volume, add_blocking_volume, add_kill_z_volume,
//   add_physics_volume, add_cull_distance_volume, add_post_process_volume)

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/TriggerVolume.h"
#include "Engine/TriggerBox.h"
#include "Engine/TriggerSphere.h"
#include "Engine/TriggerCapsule.h"
#include "Engine/BlockingVolume.h"
#include "GameFramework/KillZVolume.h"
#include "GameFramework/PainCausingVolume.h"
#include "GameFramework/PhysicsVolume.h"
#include "Sound/AudioVolume.h"
#include "Sound/ReverbEffect.h"
#include "Engine/CullDistanceVolume.h"
#include "Lightmass/PrecomputedVisibilityVolume.h"
#include "Lightmass/LightmassImportanceVolume.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavModifierVolume.h"
#include "GameFramework/CameraBlockingVolume.h"
#include "Components/BrushComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Model.h"
#include "Builders/CubeBuilder.h"
#include "Engine/PostProcessVolume.h"
#include "BSPOps.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Handlers/Volume/VolumeBrushGeometry.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpVolumeHandlers, Log, All);

// ============================================================================
// Helper Functions
// ============================================================================

namespace VolumeHelpers
{
    UWorld* GetEditorWorld()
    {
        if (GEditor)
        {
            return GEditor->GetEditorWorldContext().World();
        }
        return nullptr;
    }

    FVector GetVectorFromPayload(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName, FVector Default = FVector::ZeroVector)
    {
        return ExtractVectorField(Payload, *FieldName, Default);
    }

    FRotator GetRotatorFromPayload(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName, FRotator Default = FRotator::ZeroRotator)
    {
        return ExtractRotatorField(Payload, *FieldName, Default);
    }

    // Brush-model construction moved to Handlers/Volume/VolumeBrushGeometry.h so the spawn
    // paths outside this namespace reach the same builder: foliage.create_procedural and
    // actor.spawn both landed ABrush-derived volumes with a null Brush UModel
    // (B-spawned-volumes-have-no-brush-geometry). Re-exported here under the names the
    // verbs below already call, so a second copy of the init sequence cannot drift.
    using VolumeBrushGeometry::BuildBoxBrushGeometry;
    using VolumeBrushGeometry::CreateBoxBrushForVolume;
    using VolumeBrushGeometry::CreateSphereBrushForVolume;
    using VolumeBrushGeometry::CreateCapsuleBrushForVolume;

    // ============================================================================
    // Validation Helpers
    // ============================================================================

    bool ValidateVolumeName(const FString& VolumeName, FString& OutError)
    {
        if (VolumeName.IsEmpty())
        {
            OutError = TEXT("volumeName is required");
            return false;
        }

        if (VolumeName.Contains(TEXT("..")) || VolumeName.Contains(TEXT("/")) || VolumeName.Contains(TEXT("\\")))
        {
            OutError = TEXT("volumeName must not contain path separators or traversal sequences");
            return false;
        }

        if (VolumeName.Contains(TEXT(":")))
        {
            OutError = TEXT("volumeName must not contain drive letters");
            return false;
        }

        return true;
    }

    bool ValidateExtent(const FVector& Extent, FString& OutError)
    {
        if (!FMath::IsFinite(Extent.X) || !FMath::IsFinite(Extent.Y) || !FMath::IsFinite(Extent.Z))
        {
            OutError = TEXT("extent contains NaN or Infinity values");
            return false;
        }

        if (Extent.X <= 0.0f || Extent.Y <= 0.0f || Extent.Z <= 0.0f)
        {
            OutError = TEXT("extent values must be positive");
            return false;
        }

        return true;
    }

    bool ValidateRadius(float Radius, FString& OutError)
    {
        if (!FMath::IsFinite(Radius))
        {
            OutError = TEXT("radius contains NaN or Infinity value");
            return false;
        }

        if (Radius <= 0.0f)
        {
            OutError = TEXT("radius must be positive");
            return false;
        }

        return true;
    }

    bool ValidateCapsuleDimensions(float Radius, float HalfHeight, FString& OutError)
    {
        if (!FMath::IsFinite(Radius) || !FMath::IsFinite(HalfHeight))
        {
            OutError = TEXT("capsule dimensions contain NaN or Infinity values");
            return false;
        }

        if (Radius <= 0.0f)
        {
            OutError = TEXT("capsule radius must be positive");
            return false;
        }

        if (HalfHeight <= 0.0f)
        {
            OutError = TEXT("capsule half height must be positive");
            return false;
        }

        return true;
    }

    bool ValidateLocation(const FVector& Location, FString& OutError)
    {
        if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y) || !FMath::IsFinite(Location.Z))
        {
            OutError = TEXT("location contains NaN or Infinity values");
            return false;
        }

        return true;
    }

    AActor* FindVolumeByName(UWorld* World, const FString& VolumeName)
    {
        if (!World || VolumeName.IsEmpty())
        {
            return nullptr;
        }

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor && Actor->GetActorLabel().Equals(VolumeName, ESearchCase::IgnoreCase))
            {
                if (Actor->IsA<AVolume>() || Actor->IsA<ATriggerBase>())
                {
                    return Actor;
                }
            }
        }

        return nullptr;
    }

    // Generic volume spawning template for brush-based volumes
    template<typename TVolumeClass>
    typename TEnableIf<TIsDerivedFrom<TVolumeClass, ABrush>::Value, TVolumeClass*>::Type
    SpawnVolumeActor(
        UWorld* World,
        const FString& VolumeName,
        const FVector& Location,
        const FRotator& Rotation,
        const FVector& Extent)
    {
        if (!World)
        {
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        TVolumeClass* Volume = World->SpawnActor<TVolumeClass>(Location, Rotation, SpawnParams);
        if (Volume)
        {
            // Before the label and the brush build: UWorld::SpawnActor only dirties under a
            // transaction (`if (GUndo) ModifyLevel(...)`, LevelActor.cpp:735-739) and these
            // handlers open none, so the spawn left the level clean and `level.save` discarded
            // the volume. SetActorLabel below dirties the ACTOR (and only when the label really
            // changes) — never the level, and never at all when volumeName is empty.
            PinWright::MarkLevelActorSpawned(Volume);

            if (!VolumeName.IsEmpty())
            {
                Volume->SetActorLabel(VolumeName);
            }

            if (Extent != FVector::ZeroVector)
            {
                CreateBoxBrushForVolume(Volume, Extent);
            }
        }

        return Volume;
    }

    // Overload for non-brush trigger actors
    template<typename TVolumeClass>
    typename TEnableIf<!TIsDerivedFrom<TVolumeClass, ABrush>::Value, TVolumeClass*>::Type
    SpawnVolumeActor(
        UWorld* World,
        const FString& VolumeName,
        const FVector& Location,
        const FRotator& Rotation,
        const FVector& Extent)
    {
        if (!World)
        {
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        TVolumeClass* Volume = World->SpawnActor<TVolumeClass>(Location, Rotation, SpawnParams);
        if (Volume)
        {
            PinWright::MarkLevelActorSpawned(Volume);

            if (!VolumeName.IsEmpty())
            {
                Volume->SetActorLabel(VolumeName);
            }
        }

        return Volume;
    }

    // Helper to find actor by path or name
    AActor* FindActorByPathOrName(UWorld* World, const FString& ActorPath)
    {
        if (!World || ActorPath.IsEmpty())
        {
            return nullptr;
        }

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor)
            {
                if (Actor->GetActorLabel().Equals(ActorPath, ESearchCase::IgnoreCase))
                {
                    return Actor;
                }
                if (Actor->GetName().Equals(ActorPath, ESearchCase::IgnoreCase))
                {
                    return Actor;
                }
                FString ActorPathName = Actor->GetPathName();
                if (ActorPathName.Equals(ActorPath, ESearchCase::IgnoreCase) ||
                    ActorPathName.EndsWith(ActorPath, ESearchCase::IgnoreCase))
                {
                    return Actor;
                }
            }
        }

        return nullptr;
    }
}

// ============================================================================
// Trigger Volume Handlers
// ============================================================================

REGISTER_RPC_HANDLER("volume.create_trigger_volume", "volume", "Create a trigger volume in the editor world",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume (default: TriggerVolume)"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation {pitch,yaw,roll}"),
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z} (default: 100,100,100)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError))
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError);
        return true;
    }

    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError);
        return true;
    }

    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(100.0f, 100.0f, 100.0f));
    if (!ValidateExtent(Extent, ValidationError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError);
        return true;
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available"));
        return true;
    }

    ATriggerVolume* Volume = SpawnVolumeActor<ATriggerVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn TriggerVolume"));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ATriggerVolume"));
    AddActorVerification(Result, Volume);

    TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
    LocationObj->SetNumberField(TEXT("x"), Volume->GetActorLocation().X);
    LocationObj->SetNumberField(TEXT("y"), Volume->GetActorLocation().Y);
    LocationObj->SetNumberField(TEXT("z"), Volume->GetActorLocation().Z);
    Result->SetObjectField(TEXT("location"), LocationObj);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_trigger_sphere", "volume", "Create a trigger sphere in the editor world",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation {pitch,yaw,roll}"),
        RPC_PARAM_OPT("sphereRadius", "number", "Sphere radius (default: 100)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError))
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError);
        return true;
    }

    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError);
        return true;
    }

    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    float Radius = Ctx.GetNumber(TEXT("sphereRadius"), 100.0);
    if (!ValidateRadius(Radius, ValidationError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError);
        return true;
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available"));
        return true;
    }

    ATriggerSphere* Volume = SpawnVolumeActor<ATriggerSphere>(World, VolumeName, Location, Rotation, FVector::ZeroVector);
    if (!Volume)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn TriggerSphere"));
        return true;
    }

    if (USphereComponent* SphereComp = Volume->GetCollisionComponent() ? Cast<USphereComponent>(Volume->GetCollisionComponent()) : nullptr)
    {
        SphereComp->SetSphereRadius(Radius);
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ATriggerSphere"));
    Result->SetNumberField(TEXT("radius"), Radius);
    AddActorVerification(Result, Volume);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_trigger_capsule", "volume", "Create a trigger capsule in the editor world",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation {pitch,yaw,roll}"),
        RPC_PARAM_OPT("capsuleRadius", "number", "Capsule radius (default: 50)"),
        RPC_PARAM_OPT("capsuleHalfHeight", "number", "Capsule half height (default: 100)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError))
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError);
        return true;
    }

    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError);
        return true;
    }

    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    float Radius = Ctx.GetNumber(TEXT("capsuleRadius"), 50.0);
    float HalfHeight = Ctx.GetNumber(TEXT("capsuleHalfHeight"), 100.0);
    if (!ValidateCapsuleDimensions(Radius, HalfHeight, ValidationError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError);
        return true;
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available"));
        return true;
    }

    ATriggerCapsule* Volume = SpawnVolumeActor<ATriggerCapsule>(World, VolumeName, Location, Rotation, FVector::ZeroVector);
    if (!Volume)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn TriggerCapsule"));
        return true;
    }

    if (UCapsuleComponent* CapsuleComp = Volume->GetCollisionComponent() ? Cast<UCapsuleComponent>(Volume->GetCollisionComponent()) : nullptr)
    {
        CapsuleComp->SetCapsuleSize(Radius, HalfHeight);
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ATriggerCapsule"));
    Result->SetNumberField(TEXT("radius"), Radius);
    Result->SetNumberField(TEXT("halfHeight"), HalfHeight);
    AddActorVerification(Result, Volume);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Gameplay Volume Macro - reduces boilerplate for simple volume creation
// ============================================================================

// Helper macro for simple brush-based volume creation handlers
#define DEFINE_SIMPLE_VOLUME_HANDLER(MethodName, Category, Summary, VolumeType, VolumeClassName, DefaultExtent) \
REGISTER_RPC_HANDLER(MethodName, Category, Summary, \
    RPC_PARAMS( \
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"), \
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"), \
        RPC_PARAM_OPT("rotation", "object", "World rotation {pitch,yaw,roll}"), \
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z}") \
    )) \
{ \
    using namespace VolumeHelpers; \
    auto Payload = Ctx.GetRawPayload(); \
    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume")); \
    FString ValidationError; \
    if (!ValidateVolumeName(VolumeName, ValidationError)) \
    { \
        Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); \
        return true; \
    } \
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector); \
    if (!ValidateLocation(Location, ValidationError)) \
    { \
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); \
        return true; \
    } \
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator); \
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), DefaultExtent); \
    if (!ValidateExtent(Extent, ValidationError)) \
    { \
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); \
        return true; \
    } \
    UWorld* World = GetEditorWorld(); \
    if (!World) \
    { \
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); \
        return true; \
    } \
    VolumeType* Volume = SpawnVolumeActor<VolumeType>(World, VolumeName, Location, Rotation, Extent); \
    if (!Volume) \
    { \
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn " VolumeClassName)); \
        return true; \
    } \
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject()); \
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel()); \
    Result->SetStringField(TEXT("volumeClass"), TEXT(VolumeClassName)); \
    AddActorVerification(Result, Volume); \
    Ctx.SendSuccess(Result); \
    return true; \
}

// The simple volume types that have no extra params use explicit handlers below.

// ============================================================================
// Gameplay Volume Handlers
// ============================================================================

REGISTER_RPC_HANDLER("volume.create_blocking_volume", "volume", "Create a blocking volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z}")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }

    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(100.0f, 100.0f, 100.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    ABlockingVolume* Volume = SpawnVolumeActor<ABlockingVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn BlockingVolume")); return true; }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ABlockingVolume"));
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_kill_z_volume", "volume", "Create a kill-Z volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z} (default: 10000,10000,100)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }

    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(10000.0f, 10000.0f, 100.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AKillZVolume* Volume = SpawnVolumeActor<AKillZVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn KillZVolume")); return true; }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("AKillZVolume"));
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_pain_causing_volume", "volume", "Create a pain-causing volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z}"),
        RPC_PARAM_OPT("bPainCausing", "boolean", "Whether pain is active (default: true)"),
        RPC_PARAM_OPT("damagePerSec", "number", "Damage per second (default: 10)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }

    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(100.0f, 100.0f, 100.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    bool bPainCausing = Ctx.GetBool(TEXT("bPainCausing"), true);
    float DamagePerSec = Ctx.GetNumber(TEXT("damagePerSec"), 10.0);

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    APainCausingVolume* Volume = SpawnVolumeActor<APainCausingVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn PainCausingVolume")); return true; }

    Volume->bPainCausing = bPainCausing;
    Volume->DamagePerSec = DamagePerSec;

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("APainCausingVolume"));
    Result->SetBoolField(TEXT("bPainCausing"), bPainCausing);
    Result->SetNumberField(TEXT("damagePerSec"), DamagePerSec);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_physics_volume", "volume", "Create a physics volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z}"),
        RPC_PARAM_OPT("bWaterVolume", "boolean", "Whether this is a water volume"),
        RPC_PARAM_OPT("fluidFriction", "number", "Fluid friction (default: 0.3)"),
        RPC_PARAM_OPT("terminalVelocity", "number", "Terminal velocity (default: 4000)"),
        RPC_PARAM_OPT("priority", "number", "Volume priority (default: 0)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }

    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(100.0f, 100.0f, 100.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    bool bWaterVolume = Ctx.GetBool(TEXT("bWaterVolume"), false);
    float FluidFriction = Ctx.GetNumber(TEXT("fluidFriction"), 0.3);
    float TerminalVelocity = Ctx.GetNumber(TEXT("terminalVelocity"), 4000.0);
    int32 Priority = Ctx.GetInt(TEXT("priority"), 0);

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    APhysicsVolume* Volume = SpawnVolumeActor<APhysicsVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn PhysicsVolume")); return true; }

    Volume->bWaterVolume = bWaterVolume;
    Volume->FluidFriction = FluidFriction;
    Volume->TerminalVelocity = TerminalVelocity;
    Volume->Priority = Priority;

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("APhysicsVolume"));
    Result->SetBoolField(TEXT("bWaterVolume"), bWaterVolume);
    Result->SetNumberField(TEXT("fluidFriction"), FluidFriction);
    Result->SetNumberField(TEXT("terminalVelocity"), TerminalVelocity);
    Result->SetNumberField(TEXT("priority"), Priority);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Audio Volume Handlers
// ============================================================================

REGISTER_RPC_HANDLER("volume.create_audio_volume", "volume", "Create an audio volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 500,500,200)"),
        RPC_PARAM_OPT("bEnabled", "boolean", "Whether the volume is enabled (default: true)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(500.0f, 500.0f, 200.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    bool bEnabled = Ctx.GetBool(TEXT("bEnabled"), true);

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AAudioVolume* Volume = SpawnVolumeActor<AAudioVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn AudioVolume")); return true; }

    Volume->SetEnabled(bEnabled);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("AAudioVolume"));
    Result->SetBoolField(TEXT("bEnabled"), bEnabled);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_reverb_volume", "volume", "Create a reverb volume (AudioVolume with reverb settings)",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 500,500,200)"),
        RPC_PARAM_OPT("bEnabled", "boolean", "Whether the volume is enabled"),
        RPC_PARAM_OPT("reverbVolume", "number", "Reverb volume level (default: 0.5)"),
        RPC_PARAM_OPT("fadeTime", "number", "Reverb fade time (default: 0.5)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(500.0f, 500.0f, 200.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    bool bEnabled = Ctx.GetBool(TEXT("bEnabled"), true);
    float ReverbVolumeLevel = Ctx.GetNumber(TEXT("reverbVolume"), 0.5);
    float FadeTime = Ctx.GetNumber(TEXT("fadeTime"), 0.5);

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AAudioVolume* Volume = SpawnVolumeActor<AAudioVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn ReverbVolume (AudioVolume)")); return true; }

    Volume->SetEnabled(bEnabled);
    FReverbSettings ReverbSettings = Volume->GetReverbSettings();
    ReverbSettings.bApplyReverb = true;
    ReverbSettings.Volume = ReverbVolumeLevel;
    ReverbSettings.FadeTime = FadeTime;
    Volume->SetReverbSettings(ReverbSettings);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("AAudioVolume (Reverb)"));
    Result->SetBoolField(TEXT("bEnabled"), bEnabled);
    Result->SetNumberField(TEXT("reverbVolume"), ReverbVolumeLevel);
    Result->SetNumberField(TEXT("fadeTime"), FadeTime);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Rendering Volume Handlers
// ============================================================================

REGISTER_RPC_HANDLER("volume.create_post_process_volume", "volume", "Create a post-process volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 500,500,500)"),
        RPC_PARAM_OPT("priority", "number", "Priority (default: 0)"),
        RPC_PARAM_OPT("blendRadius", "number", "Blend radius (default: 100)"),
        RPC_PARAM_OPT("blendWeight", "number", "Blend weight (default: 1)"),
        RPC_PARAM_OPT("enabled", "boolean", "Whether enabled (default: true)"),
        RPC_PARAM_OPT("unbound", "boolean", "Whether unbound (default: false)"),
        RPC_PARAM_OPT("postProcessSettings", "object", "Post-process settings object")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(500.0f, 500.0f, 500.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    float Priority = Ctx.GetNumber(TEXT("priority"), 0.0);
    float BlendRadius = Ctx.GetNumber(TEXT("blendRadius"), 100.0);
    float BlendWeight = Ctx.GetNumber(TEXT("blendWeight"), 1.0);
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);
    bool bUnbound = Ctx.GetBool(TEXT("unbound"), false);

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    APostProcessVolume* Volume = SpawnVolumeActor<APostProcessVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn PostProcessVolume")); return true; }

    Volume->Priority = Priority;
    Volume->BlendRadius = BlendRadius;
    Volume->BlendWeight = BlendWeight;
    Volume->bEnabled = bEnabled;
    Volume->bUnbound = bUnbound;

    if (Payload->HasTypedField<EJson::Object>(TEXT("postProcessSettings")))
    {
        TSharedPtr<FJsonObject> SettingsJson = Payload->GetObjectField(TEXT("postProcessSettings"));

        if (SettingsJson->HasTypedField<EJson::Boolean>(TEXT("bloomEnabled")))
        {
            Volume->Settings.bOverride_BloomIntensity = true;
            Volume->Settings.BloomIntensity = SettingsJson->GetBoolField(TEXT("bloomEnabled")) ? 1.0f : 0.0f;
        }
        if (SettingsJson->HasTypedField<EJson::Number>(TEXT("exposureBias")))
        {
            Volume->Settings.bOverride_AutoExposureBias = true;
            Volume->Settings.AutoExposureBias = SettingsJson->GetNumberField(TEXT("exposureBias"));
        }
        if (SettingsJson->HasTypedField<EJson::Number>(TEXT("vignetteIntensity")))
        {
            Volume->Settings.bOverride_VignetteIntensity = true;
            Volume->Settings.VignetteIntensity = SettingsJson->GetNumberField(TEXT("vignetteIntensity"));
        }
        if (SettingsJson->HasTypedField<EJson::Number>(TEXT("saturation")))
        {
            Volume->Settings.bOverride_ColorSaturation = true;
            FVector4 Saturation = Volume->Settings.ColorSaturation;
            Saturation.X = SettingsJson->GetNumberField(TEXT("saturation"));
            Saturation.Y = Saturation.X;
            Saturation.Z = Saturation.X;
            Volume->Settings.ColorSaturation = Saturation;
        }
        if (SettingsJson->HasTypedField<EJson::Number>(TEXT("contrast")))
        {
            Volume->Settings.bOverride_ColorContrast = true;
            FVector4 Contrast = Volume->Settings.ColorContrast;
            Contrast.X = SettingsJson->GetNumberField(TEXT("contrast"));
            Contrast.Y = Contrast.X;
            Contrast.Z = Contrast.X;
            Volume->Settings.ColorContrast = Contrast;
        }
        if (SettingsJson->HasTypedField<EJson::Number>(TEXT("gamma")))
        {
            Volume->Settings.bOverride_ColorGamma = true;
            FVector4 Gamma = Volume->Settings.ColorGamma;
            Gamma.X = SettingsJson->GetNumberField(TEXT("gamma"));
            Gamma.Y = Gamma.X;
            Gamma.Z = Gamma.X;
            Volume->Settings.ColorGamma = Gamma;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("APostProcessVolume"));
    Result->SetNumberField(TEXT("priority"), Priority);
    Result->SetNumberField(TEXT("blendRadius"), BlendRadius);
    Result->SetNumberField(TEXT("blendWeight"), BlendWeight);
    Result->SetBoolField(TEXT("enabled"), bEnabled);
    Result->SetBoolField(TEXT("unbound"), bUnbound);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_cull_distance_volume", "volume", "Create a cull distance volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 1000,1000,500)"),
        RPC_PARAM_OPT("cullDistances", "array", "Array of {size, cullDistance} pairs")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(1000.0f, 1000.0f, 500.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    ACullDistanceVolume* Volume = SpawnVolumeActor<ACullDistanceVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn CullDistanceVolume")); return true; }

    if (Payload->HasTypedField<EJson::Array>(TEXT("cullDistances")))
    {
        TArray<TSharedPtr<FJsonValue>> CullDistancesJson = Payload->GetArrayField(TEXT("cullDistances"));
        TArray<FCullDistanceSizePair> CullDistances;
        for (const TSharedPtr<FJsonValue>& Entry : CullDistancesJson)
        {
            if (Entry->Type == EJson::Object)
            {
                TSharedPtr<FJsonObject> EntryObj = Entry->AsObject();
                FCullDistanceSizePair Pair;
                Pair.Size = GetJsonNumberField(EntryObj, TEXT("size"), 100.0f);
                Pair.CullDistance = GetJsonNumberField(EntryObj, TEXT("cullDistance"), 5000.0f);
                CullDistances.Add(Pair);
            }
        }
        if (CullDistances.Num() > 0)
        {
            Volume->CullDistances = CullDistances;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ACullDistanceVolume"));
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_precomputed_visibility_volume", "volume", "Create a precomputed visibility volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 1000,1000,500)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(1000.0f, 1000.0f, 500.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    APrecomputedVisibilityVolume* Volume = SpawnVolumeActor<APrecomputedVisibilityVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn PrecomputedVisibilityVolume")); return true; }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("APrecomputedVisibilityVolume"));
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_lightmass_importance_volume", "volume", "Create a lightmass importance volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 5000,5000,2000)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(5000.0f, 5000.0f, 2000.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    ALightmassImportanceVolume* Volume = SpawnVolumeActor<ALightmassImportanceVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn LightmassImportanceVolume")); return true; }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ALightmassImportanceVolume"));
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Navigation Volume Handlers
// ============================================================================

REGISTER_RPC_HANDLER("volume.create_nav_mesh_bounds_volume", "volume", "Create a nav mesh bounds volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 2000,2000,500)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(2000.0f, 2000.0f, 500.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    ANavMeshBoundsVolume* Volume = SpawnVolumeActor<ANavMeshBoundsVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn NavMeshBoundsVolume")); return true; }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ANavMeshBoundsVolume"));
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_nav_modifier_volume", "volume", "Create a nav modifier volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 500,500,200)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(500.0f, 500.0f, 200.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    ANavModifierVolume* Volume = SpawnVolumeActor<ANavModifierVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn NavModifierVolume")); return true; }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ANavModifierVolume"));
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.create_camera_blocking_volume", "volume", "Create a camera blocking volume",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Name for the volume"),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 200,200,200)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("TriggerVolume"));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }
    FVector Location = GetVectorFromPayload(Payload, TEXT("location"), FVector::ZeroVector);
    if (!ValidateLocation(Location, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }
    FRotator Rotation = GetRotatorFromPayload(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(200.0f, 200.0f, 200.0f));
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    ACameraBlockingVolume* Volume = SpawnVolumeActor<ACameraBlockingVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn CameraBlockingVolume")); return true; }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ACameraBlockingVolume"));
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Volume Configuration Handlers
// ============================================================================

// {x,y,z} object — the shape every volume verb already emits for a vector field.
static TSharedPtr<FJsonObject> MakeVolumeVectorJson(const FVector& Value)
{
    TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
    Json->SetNumberField(TEXT("x"), Value.X);
    Json->SetNumberField(TEXT("y"), Value.Y);
    Json->SetNumberField(TEXT("z"), Value.Z);
    return Json;
}

// {min:[x,y,z], max:[x,y,z]} — the shape volume.set_volume_bounds emits for a corner pair.
static TSharedPtr<FJsonObject> MakeVolumeBoundsJson(const FVector& MinBound, const FVector& MaxBound)
{
    TArray<TSharedPtr<FJsonValue>> MinArray, MaxArray;
    MinArray.Add(MakeShareable(new FJsonValueNumber(MinBound.X)));
    MinArray.Add(MakeShareable(new FJsonValueNumber(MinBound.Y)));
    MinArray.Add(MakeShareable(new FJsonValueNumber(MinBound.Z)));
    MaxArray.Add(MakeShareable(new FJsonValueNumber(MaxBound.X)));
    MaxArray.Add(MakeShareable(new FJsonValueNumber(MaxBound.Y)));
    MaxArray.Add(MakeShareable(new FJsonValueNumber(MaxBound.Z)));

    TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
    Json->SetArrayField(TEXT("min"), MinArray);
    Json->SetArrayField(TEXT("max"), MaxArray);
    return Json;
}

REGISTER_RPC_HANDLER("volume.set_volume_extent", "volume",
    "Set the extent of an existing volume. `extent` is a WORLD half-extent: on a brush volume the "
    "actor's scale is normalized to (1,1,1) before the box is built, so the volume ends up the size "
    "that was asked for instead of that size multiplied by whatever scale it was already carrying. "
    "`newExtent` is measured, not echoed — it is GetActorBounds() read back AFTER the apply, so a "
    "rotated volume (whose world AABB is larger than the box it was given) and the non-brush "
    "scale-based branch both report what the volume actually has. `requestedExtent` carries what "
    "the call asked for, and `clearedScale` names the non-unit scale that was reset — omitted, "
    "never zeroed, when there was none to reset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("volumeName", "string", "Name of the volume to modify"),
        RPC_PARAM_OPT("extent", "object", "New WORLD half-extent {x,y,z}")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT(""));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }

    FVector NewExtent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(100.0f, 100.0f, 100.0f));
    if (!ValidateExtent(NewExtent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* VolumeActor = FindVolumeByName(World, VolumeName);
    if (!VolumeActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Volume not found: %s"), *VolumeName)); return true; }

    // Covers the non-brush branch, which BuildBoxBrushGeometry never sees: SetActorScale3D
    // writes RootComponent->RelativeScale3D and dirties nothing on its own.
    PinWright::MarkLevelActorModified(VolumeActor);

    ABrush* BrushVolume = Cast<ABrush>(VolumeActor);
    FVector ClearedScale = FVector::OneVector;
    if (BrushVolume)
    {
        // The brush branch establishes LOCAL geometry — BuildBoxBrushGeometry builds a box of
        // exactly NewExtent*2 — and the actor transform then multiplies it, so a volume that
        // arrives carrying a scale ended up at NewExtent*Scale in world space while the response
        // reported NewExtent. Nothing on this path used to write the transform at all: the
        // SetActorScale3D below is in the mutually exclusive non-brush `else`, so for an ABrush
        // the scale was simply whatever the spawning verb left behind (foliage.create_procedural
        // writes Size/200 on the volume it spawns, which stays inert until geometry exists and
        // then starts multiplying). `extent` means a world half-extent here — set_volume_bounds
        // shares this branch and derives its extent from world min/max corners, where nothing
        // else is coherent — so normalize the transform the geometry is rendered through.
        //
        // Order is load-bearing, and it is why this does NOT need the shape-component treatment
        // (B-shape-extent-stale-physics: a bare BoxExtent store leaves the Chaos body stale and
        // wants FBodyInstance::UpdateBodyScale(..., bForceUpdate=true)). Writing the scale FIRST
        // means the geometry build that follows ends in csgPrepMovingBrush ->
        // UBrushComponent::BuildSimpleBrushCollision -> RecreatePhysicsState()
        // (BrushComponent.cpp:754), which destroys and rebuilds the body from the transform in
        // force at build time. That is strictly stronger than an UpdateBodyScale, and it leaves
        // no window in which the body disagrees with the model.
        ClearedScale = VolumeActor->GetActorScale3D();
        if (!ClearedScale.Equals(FVector::OneVector))
        {
            VolumeActor->SetActorScale3D(FVector::OneVector);
        }
        CreateBoxBrushForVolume(BrushVolume, NewExtent);
    }
    else
    {
        VolumeActor->SetActorScale3D(FVector(NewExtent.X / 100.0f, NewExtent.Y / 100.0f, NewExtent.Z / 100.0f));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), VolumeName);
    AddActorVerification(Result, VolumeActor);

    // Measured, not echoed. `newExtent` used to re-emit the request unconditionally, and
    // AddActorVerification publishes neither bounds nor scale, so a volume left at extent*scale
    // reported the extent it was asked for with no field in the response contradicting it — the
    // caller had to make a second actor.get_bounding_box call to find out. It is now read back
    // off the actor AFTER the apply; the request is named separately.
    FVector MeasuredOrigin, MeasuredExtent;
    VolumeActor->GetActorBounds(/*bOnlyCollidingComponents=*/false, MeasuredOrigin, MeasuredExtent);

    Result->SetObjectField(TEXT("newExtent"), MakeVolumeVectorJson(MeasuredExtent));
    Result->SetObjectField(TEXT("requestedExtent"), MakeVolumeVectorJson(NewExtent));
    // Omitted rather than reported as (1,1,1) when nothing was reset: a unit value here would be
    // indistinguishable from "the verb cleared a scale that happened to be unit".
    if (!ClearedScale.Equals(FVector::OneVector))
    {
        Result->SetObjectField(TEXT("clearedScale"), MakeVolumeVectorJson(ClearedScale));
    }

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.set_volume_properties", "volume", "Set properties on an existing volume",
    RPC_PARAMS(
        RPC_PARAM_REQ("volumeName", "string", "Name of the volume to modify"),
        RPC_PARAM_OPT("bWaterVolume", "boolean", "PhysicsVolume: is water"),
        RPC_PARAM_OPT("fluidFriction", "number", "PhysicsVolume: fluid friction"),
        RPC_PARAM_OPT("terminalVelocity", "number", "PhysicsVolume: terminal velocity"),
        RPC_PARAM_OPT("priority", "number", "PhysicsVolume: priority"),
        RPC_PARAM_OPT("bPainCausing", "boolean", "PainCausingVolume: pain active"),
        RPC_PARAM_OPT("damagePerSec", "number", "PainCausingVolume: damage/sec"),
        RPC_PARAM_OPT("bEnabled", "boolean", "AudioVolume: enabled"),
        RPC_PARAM_OPT("reverbVolume", "number", "AudioVolume: reverb volume"),
        RPC_PARAM_OPT("fadeTime", "number", "AudioVolume: reverb fade time")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT(""));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* VolumeActor = FindVolumeByName(World, VolumeName);
    if (!VolumeActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Volume not found: %s"), *VolumeName)); return true; }

    // Track which documented properties the caller actually requested. Each property is
    // class-gated by the apply blocks below (physics fields apply only to an APhysicsVolume,
    // pain to an APainCausingVolume, audio to an AAudioVolume), so any requested property
    // that the apply blocks did not set was dropped purely because the target volume is the
    // wrong class. Deriving the skip set as Requested \ PropertiesSet (after the apply blocks)
    // keeps a single source of truth for which fields exist — a newly-added property
    // automatically participates in skip reporting the moment its apply line is added — and
    // avoids a parallel per-class field list that could drift from the apply path. Without
    // this reporting, a call naming valid props on a class-mismatched volume (e.g. the
    // ATriggerVolume create_trigger_volume makes) would return success with propertiesSet:[]
    // and no signal of the drop. (E-volume-set-properties-class-mismatch-silent-noop)
    static const TCHAR* const DocumentedFields[] = {
        TEXT("bWaterVolume"), TEXT("fluidFriction"), TEXT("terminalVelocity"), TEXT("priority"),
        TEXT("bPainCausing"), TEXT("damagePerSec"),
        TEXT("bEnabled"), TEXT("reverbVolume"), TEXT("fadeTime")
    };

    TArray<FString> Requested;
    for (const TCHAR* Field : DocumentedFields) { if (Payload->HasField(Field)) { Requested.Add(Field); } }

    // Gated on Requested so a zero-argument call — the common probe shape — stays a true
    // no-op and leaves the level clean. Deliberately NOT gated on the actor's class: the
    // apply blocks below are class-gated, so a class predicate here would be a second
    // source of truth that silently loses its dirty the day a fourth volume class is added,
    // which is the data-loss failure this patch exists to close. The gate as written can
    // only over-dirty (a wrong-class call that ends in CLASS_MISMATCH below leaves the
    // package dirty with nothing changed) — cosmetic, and the accepted trade.
    if (Requested.Num() > 0)
    {
        PinWright::MarkLevelActorModified(VolumeActor);
    }

    TArray<FString> PropertiesSet;

    if (APhysicsVolume* PhysicsVol = Cast<APhysicsVolume>(VolumeActor))
    {
        if (Payload->HasField(TEXT("bWaterVolume"))) { PhysicsVol->bWaterVolume = Ctx.GetBool(TEXT("bWaterVolume"), false); PropertiesSet.Add(TEXT("bWaterVolume")); }
        if (Payload->HasField(TEXT("fluidFriction"))) { PhysicsVol->FluidFriction = Ctx.GetNumber(TEXT("fluidFriction"), 0.3); PropertiesSet.Add(TEXT("fluidFriction")); }
        if (Payload->HasField(TEXT("terminalVelocity"))) { PhysicsVol->TerminalVelocity = Ctx.GetNumber(TEXT("terminalVelocity"), 4000.0); PropertiesSet.Add(TEXT("terminalVelocity")); }
        if (Payload->HasField(TEXT("priority"))) { PhysicsVol->Priority = Ctx.GetInt(TEXT("priority"), 0); PropertiesSet.Add(TEXT("priority")); }
    }

    if (APainCausingVolume* PainVol = Cast<APainCausingVolume>(VolumeActor))
    {
        if (Payload->HasField(TEXT("bPainCausing"))) { PainVol->bPainCausing = Ctx.GetBool(TEXT("bPainCausing"), true); PropertiesSet.Add(TEXT("bPainCausing")); }
        if (Payload->HasField(TEXT("damagePerSec"))) { PainVol->DamagePerSec = Ctx.GetNumber(TEXT("damagePerSec"), 10.0); PropertiesSet.Add(TEXT("damagePerSec")); }
    }

    if (AAudioVolume* AudioVol = Cast<AAudioVolume>(VolumeActor))
    {
        if (Payload->HasField(TEXT("bEnabled"))) { AudioVol->SetEnabled(Ctx.GetBool(TEXT("bEnabled"), true)); PropertiesSet.Add(TEXT("bEnabled")); }

        bool bModifiedReverb = false;
        FReverbSettings ReverbSettings = AudioVol->GetReverbSettings();
        if (Payload->HasField(TEXT("reverbVolume"))) { ReverbSettings.Volume = Ctx.GetNumber(TEXT("reverbVolume"), 0.5); PropertiesSet.Add(TEXT("reverbVolume")); bModifiedReverb = true; }
        if (Payload->HasField(TEXT("fadeTime"))) { ReverbSettings.FadeTime = Ctx.GetNumber(TEXT("fadeTime"), 0.5); PropertiesSet.Add(TEXT("fadeTime")); bModifiedReverb = true; }
        if (bModifiedReverb) { AudioVol->SetReverbSettings(ReverbSettings); }
    }

    // A requested property the apply blocks did not set was dropped for class mismatch.
    TArray<FString> SkippedProperties;
    for (const FString& Field : Requested) { if (!PropertiesSet.Contains(Field)) { SkippedProperties.Add(Field); } }

    // Fail loud on a total class-mismatch no-op: the caller named at least one valid,
    // documented property, but the target volume's class supports none of them, so nothing
    // was applied. Returning success here is the misleading shape (propertiesSet:[] with
    // isError:false). Reject with CLASS_MISMATCH naming the dropped props and the actual
    // class, matching the validate-before-mutate / fail-loud convention used for the other
    // silent-drop write verbs (ai.configure_slot_behavior, gas.set_effect_tags).
    if (PropertiesSet.Num() == 0 && SkippedProperties.Num() > 0)
    {
        const FString ActorClassName = VolumeActor->GetClass()->GetName();
        TSharedPtr<FJsonObject> ErrData = MakeShareable(new FJsonObject());
        ErrData->SetStringField(TEXT("volumeName"), VolumeName);
        ErrData->SetStringField(TEXT("actorClass"), ActorClassName);
        ErrData->SetArrayField(TEXT("skipped"), EmitStringArray(SkippedProperties));
        Ctx.SendError(TEXT("CLASS_MISMATCH"),
            FString::Printf(TEXT("%s is a %s; set_volume_properties only configures PhysicsVolume/PainCausingVolume/AudioVolume properties, none of which apply to this class. No properties were set."),
                *VolumeName, *ActorClassName),
            ErrData);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), VolumeName);
    AddActorVerification(Result, VolumeActor);

    Result->SetArrayField(TEXT("propertiesSet"), EmitStringArray(PropertiesSet));

    // Partial application (e.g. a PhysicsVolume given both physics and audio props): the
    // call succeeds for the props that matched, but echo which class-mismatched props were
    // dropped so the omission is explicit rather than inferred from a short propertiesSet.
    if (SkippedProperties.Num() > 0)
    {
        Result->SetArrayField(TEXT("skipped"), EmitStringArray(SkippedProperties));
    }

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.set_volume_bounds", "volume",
    "Set volume bounds using WORLD min/max corners. On a brush volume the actor's scale is "
    "normalized to (1,1,1) before the box is built, so the volume spans the corners that were "
    "asked for instead of those corners multiplied by whatever scale it was already carrying. "
    "`bounds` and `center` are measured, not echoed — both come from GetActorBounds() read back "
    "AFTER the apply, so a rotated volume (whose world AABB is larger than the box it was given) "
    "and the non-brush scale-based branch both report what the volume actually has. "
    "`requestedBounds` carries what the call asked for, and `clearedScale` names the non-unit "
    "scale that was reset — omitted, never zeroed, when there was none to reset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("volumeName", "string", "Name of the volume to modify"),
        RPC_PARAM_REQ("bounds", "array", "Array of 6 WORLD values [minX,minY,minZ,maxX,maxY,maxZ]")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT(""));
    FString ValidationError;
    if (!ValidateVolumeName(VolumeName, ValidationError)) { Ctx.SendError(TEXT("MISSING_PARAMETER"), ValidationError); return true; }

    TArray<float> BoundsValues;
    if (Payload->HasTypedField<EJson::Array>(TEXT("bounds")))
    {
        TArray<TSharedPtr<FJsonValue>> BoundsArray = Payload->GetArrayField(TEXT("bounds"));
        for (const TSharedPtr<FJsonValue>& Value : BoundsArray)
        {
            BoundsValues.Add(static_cast<float>(Value->AsNumber()));
        }
    }

    if (BoundsValues.Num() != 6) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bounds must be an array of 6 values [minX, minY, minZ, maxX, maxY, maxZ]")); return true; }

    for (float Val : BoundsValues)
    {
        if (!FMath::IsFinite(Val)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bounds contains NaN or Infinity values")); return true; }
    }

    FVector MinBound(BoundsValues[0], BoundsValues[1], BoundsValues[2]);
    FVector MaxBound(BoundsValues[3], BoundsValues[4], BoundsValues[5]);
    FVector Center = (MinBound + MaxBound) * 0.5f;
    FVector Extent = (MaxBound - MinBound) * 0.5f;

    if (Extent.X <= 0.0f || Extent.Y <= 0.0f || Extent.Z <= 0.0f) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bounds must define a valid volume (max > min for all axes)")); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* VolumeActor = FindVolumeByName(World, VolumeName);
    if (!VolumeActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Volume not found: %s"), *VolumeName)); return true; }

    PinWright::MarkLevelActorModified(VolumeActor);

    VolumeActor->SetActorLocation(Center);

    ABrush* BrushVolume = Cast<ABrush>(VolumeActor);
    FVector ClearedScale = FVector::OneVector;
    if (BrushVolume)
    {
        // Same omission as volume.set_volume_extent, and the same repair — see the long comment
        // on that handler for why the transform write has to precede the geometry build. `bounds`
        // are world min/max corners, so a residual actor scale meant the volume spanned those
        // corners multiplied by the scale while the response echoed the corners as requested.
        ClearedScale = VolumeActor->GetActorScale3D();
        if (!ClearedScale.Equals(FVector::OneVector))
        {
            VolumeActor->SetActorScale3D(FVector::OneVector);
        }
        CreateBoxBrushForVolume(BrushVolume, Extent);
    }
    else
    {
        VolumeActor->SetActorScale3D(FVector(Extent.X / 100.0f, Extent.Y / 100.0f, Extent.Z / 100.0f));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), VolumeName);
    AddActorVerification(Result, VolumeActor);

    // Measured, not echoed — `bounds` and `center` used to re-emit the request unconditionally,
    // and AddActorVerification publishes neither bounds nor scale, so a volume left at
    // corners*scale reported the corners it was asked for with no field contradicting it.
    FVector MeasuredOrigin, MeasuredExtent;
    VolumeActor->GetActorBounds(/*bOnlyCollidingComponents=*/false, MeasuredOrigin, MeasuredExtent);

    Result->SetObjectField(TEXT("bounds"),
        MakeVolumeBoundsJson(MeasuredOrigin - MeasuredExtent, MeasuredOrigin + MeasuredExtent));
    Result->SetObjectField(TEXT("requestedBounds"), MakeVolumeBoundsJson(MinBound, MaxBound));
    Result->SetObjectField(TEXT("center"), MakeVolumeVectorJson(MeasuredOrigin));
    // Omitted rather than reported as (1,1,1) when nothing was reset: a unit value here would be
    // indistinguishable from "the verb cleared a scale that happened to be unit".
    if (!ClearedScale.Equals(FVector::OneVector))
    {
        Result->SetObjectField(TEXT("clearedScale"), MakeVolumeVectorJson(ClearedScale));
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Utility Handlers
// ============================================================================

REGISTER_RPC_HANDLER("volume.get_volumes_info", "volume", "Get information about all volumes in the level",
    RPC_PARAMS(
        RPC_PARAM_OPT("filter", "string", "Name filter"),
        RPC_PARAM_OPT("volumeType", "string", "Volume type filter")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString Filter = Ctx.GetString(TEXT("filter"), TEXT(""));
    FString VolumeType = Ctx.GetString(TEXT("volumeType"), TEXT(""));

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    TArray<TSharedPtr<FJsonValue>> VolumesArray;
    int32 TotalCount = 0;

    for (TActorIterator<AVolume> It(World); It; ++It)
    {
        AVolume* Volume = *It;
        if (!Volume) continue;

        if (!VolumeType.IsEmpty())
        {
            FString ClassName = Volume->GetClass()->GetName();
            if (!ClassName.Contains(VolumeType)) continue;
        }
        if (!Filter.IsEmpty())
        {
            FString ActorLabel = Volume->GetActorLabel();
            if (!ActorLabel.Contains(Filter)) continue;
        }

        TSharedPtr<FJsonObject> VolumeInfo = MakeShareable(new FJsonObject());
        VolumeInfo->SetStringField(TEXT("name"), Volume->GetActorLabel());
        VolumeInfo->SetStringField(TEXT("class"), Volume->GetClass()->GetName());

        FVector Location = Volume->GetActorLocation();
        TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject());
        LocationJson->SetNumberField(TEXT("x"), Location.X);
        LocationJson->SetNumberField(TEXT("y"), Location.Y);
        LocationJson->SetNumberField(TEXT("z"), Location.Z);
        VolumeInfo->SetObjectField(TEXT("location"), LocationJson);

        FVector Origin, BoxExtent;
        Volume->GetActorBounds(false, Origin, BoxExtent);
        TSharedPtr<FJsonObject> ExtentJson = MakeShareable(new FJsonObject());
        ExtentJson->SetNumberField(TEXT("x"), BoxExtent.X);
        ExtentJson->SetNumberField(TEXT("y"), BoxExtent.Y);
        ExtentJson->SetNumberField(TEXT("z"), BoxExtent.Z);
        VolumeInfo->SetObjectField(TEXT("extent"), ExtentJson);

        VolumesArray.Add(MakeShareable(new FJsonValueObject(VolumeInfo)));
        TotalCount++;
    }

    for (TActorIterator<ATriggerBase> It(World); It; ++It)
    {
        ATriggerBase* Trigger = *It;
        if (!Trigger) continue;

        if (!VolumeType.IsEmpty())
        {
            FString ClassName = Trigger->GetClass()->GetName();
            if (!ClassName.Contains(VolumeType) && !VolumeType.Equals(TEXT("Trigger"), ESearchCase::IgnoreCase)) continue;
        }
        if (!Filter.IsEmpty())
        {
            FString ActorLabel = Trigger->GetActorLabel();
            if (!ActorLabel.Contains(Filter)) continue;
        }

        TSharedPtr<FJsonObject> VolumeInfo = MakeShareable(new FJsonObject());
        VolumeInfo->SetStringField(TEXT("name"), Trigger->GetActorLabel());
        VolumeInfo->SetStringField(TEXT("class"), Trigger->GetClass()->GetName());

        FVector Location = Trigger->GetActorLocation();
        TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject());
        LocationJson->SetNumberField(TEXT("x"), Location.X);
        LocationJson->SetNumberField(TEXT("y"), Location.Y);
        LocationJson->SetNumberField(TEXT("z"), Location.Z);
        VolumeInfo->SetObjectField(TEXT("location"), LocationJson);

        FVector Origin, BoxExtent;
        Trigger->GetActorBounds(false, Origin, BoxExtent);
        TSharedPtr<FJsonObject> ExtentJson = MakeShareable(new FJsonObject());
        ExtentJson->SetNumberField(TEXT("x"), BoxExtent.X);
        ExtentJson->SetNumberField(TEXT("y"), BoxExtent.Y);
        ExtentJson->SetNumberField(TEXT("z"), BoxExtent.Z);
        VolumeInfo->SetObjectField(TEXT("extent"), ExtentJson);

        VolumesArray.Add(MakeShareable(new FJsonValueObject(VolumeInfo)));
        TotalCount++;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    TSharedPtr<FJsonObject> VolumesInfo = MakeShareable(new FJsonObject());
    VolumesInfo->SetNumberField(TEXT("totalCount"), TotalCount);
    VolumesInfo->SetArrayField(TEXT("volumes"), VolumesArray);
    Result->SetObjectField(TEXT("volumesInfo"), VolumesInfo);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Add Volume To Actor Handlers
// ============================================================================

// Helper macro for the "add volume to actor" pattern to reduce boilerplate
static bool AddVolumeToActorCommon(FHandlerContext& Ctx, const FString& ActorPath, bool& bOutAttachmentSucceeded)
{
    using namespace VolumeHelpers;

    if (ActorPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("actorPath is required"));
        return false;
    }

    if (ActorPath.Contains(TEXT("..")) || ActorPath.Contains(TEXT("\\")))
    {
        Ctx.SendError(TEXT("SECURITY_VIOLATION"), TEXT("actorPath contains invalid characters"));
        return false;
    }

    return true; // validation passed
}

REGISTER_RPC_HANDLER("volume.add_trigger_volume", "volume", "Add a trigger volume at an existing actor's location",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorPath", "path", "Path or name of the target actor"),
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z} (default: 100,100,100)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString ActorPath = Ctx.GetString(TEXT("actorPath"), TEXT(""));
    bool bAttachmentSucceeded = false;
    if (!AddVolumeToActorCommon(Ctx, ActorPath, bAttachmentSucceeded)) return true;

    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(100.0f, 100.0f, 100.0f));
    FString ValidationError;
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* TargetActor = FindActorByPathOrName(World, ActorPath);
    if (!TargetActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorPath)); return true; }

    FVector Location = TargetActor->GetActorLocation();
    FRotator Rotation = TargetActor->GetActorRotation();
    FString VolumeName = TargetActor->GetActorLabel() + TEXT("_TriggerVolume");

    ATriggerVolume* Volume = SpawnVolumeActor<ATriggerVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn TriggerVolume")); return true; }

    bAttachmentSucceeded = Volume->AttachToActor(TargetActor, FAttachmentTransformRules::KeepWorldTransform);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ATriggerVolume"));
    Result->SetStringField(TEXT("attachedTo"), TargetActor->GetActorLabel());
    Result->SetBoolField(TEXT("attachmentSucceeded"), bAttachmentSucceeded);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.add_blocking_volume", "volume", "Add a blocking volume at an existing actor's location",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorPath", "path", "Path or name of the target actor"),
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z} (default: 200,200,200)")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString ActorPath = Ctx.GetString(TEXT("actorPath"), TEXT(""));
    bool bAttachmentSucceeded = false;
    if (!AddVolumeToActorCommon(Ctx, ActorPath, bAttachmentSucceeded)) return true;

    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(200.0f, 200.0f, 200.0f));
    FString ValidationError;
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* TargetActor = FindActorByPathOrName(World, ActorPath);
    if (!TargetActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorPath)); return true; }

    FVector Location = TargetActor->GetActorLocation();
    FRotator Rotation = TargetActor->GetActorRotation();
    FString VolumeName = TargetActor->GetActorLabel() + TEXT("_BlockingVolume");

    ABlockingVolume* Volume = SpawnVolumeActor<ABlockingVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn BlockingVolume")); return true; }

    bAttachmentSucceeded = Volume->AttachToActor(TargetActor, FAttachmentTransformRules::KeepWorldTransform);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ABlockingVolume"));
    Result->SetStringField(TEXT("attachedTo"), TargetActor->GetActorLabel());
    Result->SetBoolField(TEXT("attachmentSucceeded"), bAttachmentSucceeded);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.add_kill_z_volume", "volume", "Add a kill-Z volume at an existing actor's location",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorPath", "path", "Path or name of the target actor"),
        RPC_PARAM_OPT("extent", "object", "Volume extent {x,y,z} (default: 1000,1000,100)"),
        RPC_PARAM_OPT("killZHeight", "number", "Override Z height for the volume")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString ActorPath = Ctx.GetString(TEXT("actorPath"), TEXT(""));
    bool bAttachmentSucceeded = false;
    if (!AddVolumeToActorCommon(Ctx, ActorPath, bAttachmentSucceeded)) return true;

    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(1000.0f, 1000.0f, 100.0f));
    float KillZHeight = Ctx.GetNumber(TEXT("killZHeight"), 0.0);
    FString ValidationError;
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* TargetActor = FindActorByPathOrName(World, ActorPath);
    if (!TargetActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorPath)); return true; }

    FVector Location = TargetActor->GetActorLocation();
    if (KillZHeight != 0.0f) { Location.Z = KillZHeight; }
    FRotator Rotation = TargetActor->GetActorRotation();
    FString VolumeName = TargetActor->GetActorLabel() + TEXT("_KillZVolume");

    AKillZVolume* Volume = SpawnVolumeActor<AKillZVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn KillZVolume")); return true; }

    bAttachmentSucceeded = Volume->AttachToActor(TargetActor, FAttachmentTransformRules::KeepWorldTransform);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("AKillZVolume"));
    Result->SetStringField(TEXT("attachedTo"), TargetActor->GetActorLabel());
    Result->SetNumberField(TEXT("killZHeight"), Location.Z);
    Result->SetBoolField(TEXT("attachmentSucceeded"), bAttachmentSucceeded);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.add_physics_volume", "volume", "Add a physics volume at an existing actor's location",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorPath", "path", "Path or name of the target actor"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 300,300,300)"),
        RPC_PARAM_OPT("bWaterVolume", "boolean", "Whether this is a water volume"),
        RPC_PARAM_OPT("fluidFriction", "number", "Fluid friction"),
        RPC_PARAM_OPT("terminalVelocity", "number", "Terminal velocity")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString ActorPath = Ctx.GetString(TEXT("actorPath"), TEXT(""));
    bool bAttachmentSucceeded = false;
    if (!AddVolumeToActorCommon(Ctx, ActorPath, bAttachmentSucceeded)) return true;

    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(300.0f, 300.0f, 300.0f));
    FString ValidationError;
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    bool bWaterVolume = Ctx.GetBool(TEXT("bWaterVolume"), false);
    float FluidFriction = Ctx.GetNumber(TEXT("fluidFriction"), 0.3);
    float TerminalVelocity = Ctx.GetNumber(TEXT("terminalVelocity"), 4000.0);

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* TargetActor = FindActorByPathOrName(World, ActorPath);
    if (!TargetActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorPath)); return true; }

    FVector Location = TargetActor->GetActorLocation();
    FRotator Rotation = TargetActor->GetActorRotation();
    FString VolumeName = TargetActor->GetActorLabel() + TEXT("_PhysicsVolume");

    APhysicsVolume* Volume = SpawnVolumeActor<APhysicsVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn PhysicsVolume")); return true; }

    Volume->bWaterVolume = bWaterVolume;
    Volume->FluidFriction = FluidFriction;
    Volume->TerminalVelocity = TerminalVelocity;

    bAttachmentSucceeded = Volume->AttachToActor(TargetActor, FAttachmentTransformRules::KeepWorldTransform);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("APhysicsVolume"));
    Result->SetStringField(TEXT("attachedTo"), TargetActor->GetActorLabel());
    Result->SetBoolField(TEXT("bWaterVolume"), bWaterVolume);
    Result->SetBoolField(TEXT("attachmentSucceeded"), bAttachmentSucceeded);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.add_cull_distance_volume", "volume", "Add a cull distance volume at an existing actor's location",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorPath", "path", "Path or name of the target actor"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 1000,1000,500)"),
        RPC_PARAM_OPT("cullDistances", "array", "Array of {size, cullDistance} pairs")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString ActorPath = Ctx.GetString(TEXT("actorPath"), TEXT(""));
    bool bAttachmentSucceeded = false;
    if (!AddVolumeToActorCommon(Ctx, ActorPath, bAttachmentSucceeded)) return true;

    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(1000.0f, 1000.0f, 500.0f));
    FString ValidationError;
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* TargetActor = FindActorByPathOrName(World, ActorPath);
    if (!TargetActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorPath)); return true; }

    FVector Location = TargetActor->GetActorLocation();
    FRotator Rotation = TargetActor->GetActorRotation();
    FString VolumeName = TargetActor->GetActorLabel() + TEXT("_CullDistanceVolume");

    ACullDistanceVolume* Volume = SpawnVolumeActor<ACullDistanceVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn CullDistanceVolume")); return true; }

    if (Payload->HasTypedField<EJson::Array>(TEXT("cullDistances")))
    {
        TArray<TSharedPtr<FJsonValue>> CullDistancesJson = Payload->GetArrayField(TEXT("cullDistances"));
        TArray<FCullDistanceSizePair> CullDistances;
        for (const TSharedPtr<FJsonValue>& Entry : CullDistancesJson)
        {
            if (Entry->Type == EJson::Object)
            {
                TSharedPtr<FJsonObject> EntryObj = Entry->AsObject();
                FCullDistanceSizePair Pair;
                Pair.Size = GetJsonNumberField(EntryObj, TEXT("size"), 100.0f);
                Pair.CullDistance = GetJsonNumberField(EntryObj, TEXT("cullDistance"), 5000.0f);
                CullDistances.Add(Pair);
            }
        }
        if (CullDistances.Num() > 0) { Volume->CullDistances = CullDistances; }
    }

    bAttachmentSucceeded = Volume->AttachToActor(TargetActor, FAttachmentTransformRules::KeepWorldTransform);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("ACullDistanceVolume"));
    Result->SetStringField(TEXT("attachedTo"), TargetActor->GetActorLabel());
    Result->SetBoolField(TEXT("attachmentSucceeded"), bAttachmentSucceeded);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("volume.add_post_process_volume", "volume", "Add a post-process volume at an existing actor's location",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorPath", "path", "Path or name of the target actor"),
        RPC_PARAM_OPT("extent", "object", "Volume extent (default: 500,500,500)"),
        RPC_PARAM_OPT("priority", "number", "Priority"),
        RPC_PARAM_OPT("blendRadius", "number", "Blend radius"),
        RPC_PARAM_OPT("blendWeight", "number", "Blend weight"),
        RPC_PARAM_OPT("enabled", "boolean", "Whether enabled"),
        RPC_PARAM_OPT("unbound", "boolean", "Whether unbound")
    ))
{
    using namespace VolumeHelpers;
    auto Payload = Ctx.GetRawPayload();

    FString ActorPath = Ctx.GetString(TEXT("actorPath"), TEXT(""));
    bool bAttachmentSucceeded = false;
    if (!AddVolumeToActorCommon(Ctx, ActorPath, bAttachmentSucceeded)) return true;

    FVector Extent = GetVectorFromPayload(Payload, TEXT("extent"), FVector(500.0f, 500.0f, 500.0f));
    FString ValidationError;
    if (!ValidateExtent(Extent, ValidationError)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), ValidationError); return true; }

    float Priority = Ctx.GetNumber(TEXT("priority"), 0.0);
    float BlendRadius = Ctx.GetNumber(TEXT("blendRadius"), 100.0);
    float BlendWeight = Ctx.GetNumber(TEXT("blendWeight"), 1.0);
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);
    bool bUnbound = Ctx.GetBool(TEXT("unbound"), false);

    UWorld* World = GetEditorWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available")); return true; }

    AActor* TargetActor = FindActorByPathOrName(World, ActorPath);
    if (!TargetActor) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorPath)); return true; }

    FVector Location = TargetActor->GetActorLocation();
    FRotator Rotation = TargetActor->GetActorRotation();
    FString VolumeName = TargetActor->GetActorLabel() + TEXT("_PostProcessVolume");

    APostProcessVolume* Volume = SpawnVolumeActor<APostProcessVolume>(World, VolumeName, Location, Rotation, Extent);
    if (!Volume) { Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn PostProcessVolume")); return true; }

    Volume->Priority = Priority;
    Volume->BlendRadius = BlendRadius;
    Volume->BlendWeight = BlendWeight;
    Volume->bEnabled = bEnabled;
    Volume->bUnbound = bUnbound;

    bAttachmentSucceeded = Volume->AttachToActor(TargetActor, FAttachmentTransformRules::KeepWorldTransform);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    Result->SetStringField(TEXT("volumeClass"), TEXT("APostProcessVolume"));
    Result->SetStringField(TEXT("attachedTo"), TargetActor->GetActorLabel());
    Result->SetNumberField(TEXT("priority"), Priority);
    Result->SetBoolField(TEXT("attachmentSucceeded"), bAttachmentSucceeded);
    AddActorVerification(Result, Volume);
    Ctx.SendSuccess(Result);
    return true;
}
