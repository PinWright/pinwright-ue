// Copyright (c) 2026 Alexander Penkin. MIT License.

// MiscHandler.cpp - Migrated from PinWright_MiscHandlers.cpp
// Miscellaneous handlers: post-process volumes, cameras, game speed, and
// actor CDO replication (set_replication)

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "CineCameraActor.h"
#include "Engine/PostProcessVolume.h"
#include "Components/PostProcessComponent.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/WorldSettings.h"
#include "UnrealClient.h"
#include "Engine/LevelStreaming.h"
#include "Compat/EngineVersionCompat.h"
#include "Components/SplineComponent.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif

#if __has_include("Settings/LevelEditorPlaySettings.h")
#include "Settings/LevelEditorPlaySettings.h"
#endif


DEFINE_LOG_CATEGORY_STATIC(LogMcpMiscHandlers, Log, All);

namespace MiscHelpers
{
    UWorld* GetEditorWorld()
    {
        if (GEditor)
            return GEditor->GetEditorWorldContext().World();
        return nullptr;
    }
}

// ---- misc.create_post_process_volume ----
REGISTER_RPC_HANDLER("misc.create_post_process_volume", "misc", "Spawn an APostProcessVolume actor in the active level. Use property.set on the resulting actor to configure post-process settings.",
    RPC_PARAMS(
        RPC_PARAM_OPT("volumeName", "string", "Label for the volume (default PostProcessVolume)"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("extent", "object", "Extent {x, y, z}"),
        RPC_PARAM_OPT("unbound", "boolean", "Whether the volume is unbound"),
        RPC_PARAM_OPT("blendRadius", "number", "Blend radius"),
        RPC_PARAM_OPT("blendWeight", "number", "Blend weight"),
        RPC_PARAM_OPT("priority", "number", "Volume priority"),
        RPC_PARAM_OPT("settings", "object", "Post-process settings: bloomIntensity, exposureCompensation, saturation, contrast, vignetteIntensity")
    ))
{
    FString VolumeName = Ctx.GetString(TEXT("volumeName"), TEXT("PostProcessVolume"));
    FVector Location = Ctx.GetVector(TEXT("location"), FVector::ZeroVector);
    FVector Extent = Ctx.GetVector(TEXT("extent"), FVector(1000.0f, 1000.0f, 500.0f));
    bool bUnbound = Ctx.GetBool(TEXT("unbound"), false);
    double BlendRadius = Ctx.GetNumber(TEXT("blendRadius"), 100.0);
    double BlendWeight = Ctx.GetNumber(TEXT("blendWeight"), 1.0);
    double Priority = Ctx.GetNumber(TEXT("priority"), 0.0);

    UWorld* World = MiscHelpers::GetEditorWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available"));
        return true;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(Location, FRotator::ZeroRotator, SpawnParams);
    if (!Volume)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn PostProcessVolume"));
        return true;
    }

    Volume->SetActorLabel(VolumeName);
    Volume->bUnbound = bUnbound;
    Volume->BlendRadius = static_cast<float>(BlendRadius);
    Volume->BlendWeight = static_cast<float>(BlendWeight);
    Volume->Priority = static_cast<float>(Priority);

    auto* Payload = Ctx.GetRawPayload().Get();
    if (Payload->HasField(TEXT("settings")))
    {
        const TSharedPtr<FJsonObject>* SettingsPtr = nullptr;
        if (Payload->TryGetObjectField(TEXT("settings"), SettingsPtr) && SettingsPtr)
        {
            FPostProcessSettings& Settings = Volume->Settings;

            double Bloom;
            if ((*SettingsPtr)->TryGetNumberField(TEXT("bloomIntensity"), Bloom))
            {
                Settings.bOverride_BloomIntensity = true;
                Settings.BloomIntensity = static_cast<float>(Bloom);
            }
            double Exposure;
            if ((*SettingsPtr)->TryGetNumberField(TEXT("exposureCompensation"), Exposure))
            {
                Settings.bOverride_AutoExposureBias = true;
                Settings.AutoExposureBias = static_cast<float>(Exposure);
            }
            double Saturation;
            if ((*SettingsPtr)->TryGetNumberField(TEXT("saturation"), Saturation))
            {
                Settings.bOverride_ColorSaturation = true;
                Settings.ColorSaturation = FVector4(Saturation, Saturation, Saturation, 1.0f);
            }
            double Contrast;
            if ((*SettingsPtr)->TryGetNumberField(TEXT("contrast"), Contrast))
            {
                Settings.bOverride_ColorContrast = true;
                Settings.ColorContrast = FVector4(Contrast, Contrast, Contrast, 1.0f);
            }
            double VignetteIntensity;
            if ((*SettingsPtr)->TryGetNumberField(TEXT("vignetteIntensity"), VignetteIntensity))
            {
                Settings.bOverride_VignetteIntensity = true;
                Settings.VignetteIntensity = static_cast<float>(VignetteIntensity);
            }
        }
    }

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetStringField(TEXT("volumeName"), Volume->GetActorLabel());
    ResponseJson->SetStringField(TEXT("volumePath"), Volume->GetPathName());
    ResponseJson->SetBoolField(TEXT("unbound"), bUnbound);
    ResponseJson->SetNumberField(TEXT("blendRadius"), BlendRadius);
    ResponseJson->SetNumberField(TEXT("priority"), Priority);
    AddActorVerification(ResponseJson, Volume);
    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- misc.create_camera ----
REGISTER_RPC_HANDLER("misc.create_camera", "misc", "Spawn an ACameraActor (or, with cine=true / cameraClass=\"cine\", an ACineCameraActor) in the active level. Use misc.set_camera_fov afterward to configure FOV.",
    RPC_PARAMS(
        RPC_PARAM_OPT("cameraName", "string", "Label for the camera"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("fov", "number", "Field of view (default 90)"),
        RPC_PARAM_OPT("cine", "boolean", "Spawn an ACineCameraActor (cinematic camera with filmback/focus/exposure) instead of a plain ACameraActor"),
        RPC_PARAM_OPT("cameraClass", "string", "Camera variant to spawn: \"camera\" (default) or \"cine\" (ACineCameraActor). Alias for cine=true")
    ))
{
    FString CameraName = Ctx.GetString(TEXT("cameraName"), TEXT("Camera"));
    FVector Location = Ctx.GetVector(TEXT("location"), FVector::ZeroVector);
    FRotator Rotation = Ctx.GetRotator(TEXT("rotation"), FRotator::ZeroRotator);
    double FOV = Ctx.GetNumber(TEXT("fov"), 90.0);

    // The advertised "(or CineCameraActor)" variant is opt-in via either cine=true
    // or cameraClass="cine"/"cinecamera"/"cinecameraactor" (the camelCase selector
    // a caller naturally reaches for). Without an opt-in we keep the historical
    // plain ACameraActor so existing callers are unaffected.
    const FString CameraClass = Ctx.GetString(TEXT("cameraClass")).TrimStartAndEnd().ToLower();
    const bool bCine = Ctx.GetBool(TEXT("cine"), false)
        || CameraClass == TEXT("cine")
        || CameraClass == TEXT("cinecamera")
        || CameraClass == TEXT("cinecameraactor");

    UWorld* World = MiscHelpers::GetEditorWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available"));
        return true;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    // ACineCameraActor derives from ACameraActor, so a base pointer covers both
    // spawn paths and the shared label/FOV/response code below stays single-source.
    ACameraActor* Camera = bCine
        ? World->SpawnActor<ACineCameraActor>(Location, Rotation, SpawnParams)
        : World->SpawnActor<ACameraActor>(Location, Rotation, SpawnParams);
    if (!Camera)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn camera actor"));
        return true;
    }

    Camera->SetActorLabel(CameraName);
    if (UCameraComponent* CamComp = Camera->GetCameraComponent())
        CamComp->SetFieldOfView(static_cast<float>(FOV));

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetStringField(TEXT("cameraName"), Camera->GetActorLabel());
    ResponseJson->SetStringField(TEXT("cameraPath"), Camera->GetPathName());
    ResponseJson->SetNumberField(TEXT("fov"), FOV);
    // The spawned variant is reported by the shared "actorClass" field that
    // AddActorVerification emits next (CameraActor vs CineCameraActor) — a more
    // precise single source of truth than a separate bool, so no "cine" field here.
    AddActorVerification(ResponseJson, Camera);
    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- misc.set_camera_fov ----
REGISTER_RPC_HANDLER("misc.set_camera_fov", "misc", "Set the FieldOfView property on an existing camera actor (degrees). Targets the actor's camera component.",
    RPC_PARAMS(
        RPC_PARAM_REQ("cameraName", "string", "Camera actor label or name"),
        RPC_PARAM_OPT("fov", "number", "Field of view (default 90)")
    ))
{
    FString CameraName = Ctx.GetString(TEXT("cameraName"));
    double FOV = Ctx.GetNumber(TEXT("fov"), 90.0);

    if (CameraName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("cameraName is required"));
        return true;
    }

    UWorld* World = MiscHelpers::GetEditorWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("Editor world not available"));
        return true;
    }

    ACameraActor* Camera = nullptr;
    for (TActorIterator<ACameraActor> It(World); It; ++It)
    {
        if (It->GetActorLabel().Equals(CameraName, ESearchCase::IgnoreCase) ||
            It->GetName().Equals(CameraName, ESearchCase::IgnoreCase))
        {
            Camera = *It;
            break;
        }
    }

    if (!Camera)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Camera not found: %s"), *CameraName));
        return true;
    }

    if (UCameraComponent* CamComp = Camera->GetCameraComponent())
        CamComp->SetFieldOfView(static_cast<float>(FOV));

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetStringField(TEXT("cameraName"), Camera->GetActorLabel());
    ResponseJson->SetNumberField(TEXT("fov"), FOV);
    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- misc.set_game_speed ----
REGISTER_RPC_HANDLER("misc.set_game_speed", "misc", "Set the global TimeDilation on the active world (1.0 = real-time, 0.5 = half-speed slow-mo, 2.0 = double-speed).",
    RPC_PARAMS(
        RPC_PARAM_OPT("speed", "number", "Time dilation factor (default 1.0, range 0-100)")
    ))
{
    double Speed = Ctx.GetNumber(TEXT("speed"), 1.0);

    if (Speed < 0.0 || Speed > 100.0)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Speed must be between 0.0 and 100.0"));
        return true;
    }

    UWorld* World = nullptr;
    if (GEditor && GEditor->PlayWorld)
        World = GEditor->PlayWorld;
    else
        World = MiscHelpers::GetEditorWorld();

    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available"));
        return true;
    }

    AWorldSettings* WorldSettings = World->GetWorldSettings();
    if (!WorldSettings)
    {
        Ctx.SendError(TEXT("NO_WORLD_SETTINGS"), TEXT("World settings not available"));
        return true;
    }

    WorldSettings->SetTimeDilation(static_cast<float>(Speed));

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetNumberField(TEXT("speed"), Speed);
    ResponseJson->SetNumberField(TEXT("actualTimeDilation"), WorldSettings->TimeDilation);
    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- misc.set_replication ----
REGISTER_RPC_HANDLER("misc.set_replication", "misc", "Configure replication flags (bReplicates, bAlwaysRelevant, bNetLoadOnClient, etc.) on the actor CDO of a Blueprint asset. Asset-level change; recompiles the Blueprint.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path"),
        RPC_PARAM_OPT("replicates", "boolean", "Enable replication (default true)"),
        RPC_PARAM_OPT("replicateMovement", "boolean", "Enable movement replication (default true)")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    bool bReplicates = Ctx.GetBool(TEXT("replicates"), true);
    bool bReplicateMovement = Ctx.GetBool(TEXT("replicateMovement"), true);

    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("blueprintPath is required"));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    if (!Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT"), TEXT("Blueprint has no generated class"));
        return true;
    }

    AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (CDO)
    {
        CDO->SetReplicates(bReplicates);
        CDO->SetReplicateMovement(bReplicateMovement);
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    ResponseJson->SetBoolField(TEXT("replicates"), bReplicates);
    ResponseJson->SetBoolField(TEXT("replicateMovement"), bReplicateMovement);
    AddAssetVerification(ResponseJson, Blueprint);
    Ctx.SendSuccess(ResponseJson);
    return true;
}

