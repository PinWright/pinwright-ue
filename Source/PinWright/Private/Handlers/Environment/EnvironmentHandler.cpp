// Copyright (c) 2026 Alexander Penkin. MIT License.

// EnvironmentHandler.cpp (editor half) - level-authoring environment.build.* / spawn verbs,
// procedural terrain, and the editor-viewport / selection inspectors. environment.control.*
// and the object/actor/class system.inspect.* verbs live in the runtime module's
// EnvironmentHandler.cpp.
#include "Compat/SceneComponentCompat.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/JsonBuilders.h"
#include "Utils/NameMatchFilter.h"
#include "Utils/PropertyExport.h"
#include "Utils/PropertyUtils.h"
#include "Handlers/Editor/EditorHandlerUtils.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Handlers/Environment/EnvironmentSpawnHelpers.h"
#include "Dom/JsonObject.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EditorViewportClient.h"
#include "Slate/SceneViewport.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/Selection.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#if __has_include("Subsystems/UnrealEditorSubsystem.h")
#include "Subsystems/UnrealEditorSubsystem.h"
#elif __has_include("UnrealEditorSubsystem.h")
#include "UnrealEditorSubsystem.h"
#endif
#if __has_include("Subsystems/LevelEditorSubsystem.h")
#include "Subsystems/LevelEditorSubsystem.h"
#elif __has_include("LevelEditorSubsystem.h")
#include "LevelEditorSubsystem.h"
#endif
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Developer/AssetTools/Public/AssetToolsModule.h"
#include "EditorValidatorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GeneralProjectSettings.h"
#include "KismetProceduralMeshLibrary.h"
#include "Misc/FileHelper.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "ProceduralMeshComponent.h"

// Landscape includes
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeGrassType.h"
#include "AssetRegistry/AssetRegistryModule.h"

// =====================================================================
// environment.build.* sub-actions
// =====================================================================

// Helper: dispatch to another auto-registered handler by method name via the subsystem
static bool CrossDispatchEnv(FHandlerContext& Ctx, const FString& MethodName, const TSharedPtr<FJsonObject>& Payload)
{
    auto* Sub = Ctx.GetSubsystem();
    if (Sub)
    {
        return Sub->DispatchMethod(MethodName, Ctx.GetRequestId(), Payload);
    }
    Ctx.SendError(TEXT("SUBSYSTEM_NOT_FOUND"), TEXT("Subsystem not available for dispatch"));
    return true;
}

// Resolve the engine sky-sphere blueprint's generated class across UE versions.
// The asset moved between releases: older engines shipped
// /Engine/Maps/Templates/SkySphere (class SkySphere_C), while modern UE 5.x ships
// /Engine/EngineSky/BP_Sky_Sphere (class BP_Sky_Sphere_C). Try the legacy path
// first, then the modern one, so create_sky_sphere works on UE 5.3-5.7. Returns
// null only if neither path resolves on the running engine.
static UClass* LoadSkySphereClass()
{
    static const TCHAR* const SkySphereClassPaths[] = {
        TEXT("/Script/Engine.Blueprint'/Engine/Maps/Templates/SkySphere.SkySphere_C'"),
        TEXT("/Engine/EngineSky/BP_Sky_Sphere.BP_Sky_Sphere_C"),
    };
    for (const TCHAR* ClassPath : SkySphereClassPaths)
    {
        if (UClass* Resolved = LoadClass<AActor>(nullptr, ClassPath))
        {
            return Resolved;
        }
    }
    return nullptr;
}

// Recognize a sky-sphere actor regardless of which engine variant created it.
// The legacy template's class is "SkySphere_C"; the modern engine's is
// "BP_Sky_Sphere_C". A plain Contains("SkySphere") only catches the legacy name,
// so also accept the underscored "Sky_Sphere" spelling used by BP_Sky_Sphere.
static bool IsSkySphereActor(const AActor* Actor)
{
    if (!Actor)
    {
        return false;
    }
    const FString ClassName = Actor->GetClass()->GetName();
    return ClassName.Contains(TEXT("SkySphere")) || ClassName.Contains(TEXT("Sky_Sphere"));
}

// ---- environment.build.create_sky_sphere ----
REGISTER_RPC_HANDLER("environment.build.create_sky_sphere", "environment", "Create a sky sphere actor in the level",
    RPC_PARAMS(
        RPC_PARAM_OPT("name", "string", "Label for the spawned actor (defaults to \"SkySphere\")")
    ))
{
    bool bSuccess = false;
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();

    // Optional caller-supplied label; fall back to the historical default when
    // omitted (or sent empty) so the World Outliner name is unchanged for existing callers.
    const FString Label = Ctx.GetStringFirstOf({TEXT("name")}, TEXT("SkySphere"));

    if (GEditor)
    {
        UClass* SkySphereClass = LoadSkySphereClass();
        if (SkySphereClass)
        {
            AActor* SkySphere = SpawnActorInActiveWorld<AActor>(
                SkySphereClass, FVector::ZeroVector, FRotator::ZeroRotator, Label);
            if (SkySphere)
            {
                bSuccess = true;
                Resp->SetStringField(TEXT("actorName"), SkySphere->GetActorLabel());
                Resp->SetBoolField(TEXT("success"), true);
            }
        }
    }
    if (bSuccess)
    {
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create sky sphere"));
    }
    return true;
}

// ---- environment.build.set_time_of_day ----
REGISTER_RPC_HANDLER("environment.build.set_time_of_day", "environment", "Set time of day via sky sphere",
    RPC_PARAMS(
        RPC_PARAM_REQ("time", "number", "Sky sphere time-of-day value as exposed by BP_Sky_Sphere (typically 0..24, defaults to 12).")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("Payload missing"));
        return true;
    }

    float TimeOfDay = 12.0f;
    Payload->TryGetNumberField(TEXT("time"), TimeOfDay);

    bool bSuccess = false;
    if (GEditor)
    {
        UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
        if (ActorSS)
        {
            for (AActor* Actor : ActorSS->GetAllLevelActors())
            {
                if (IsSkySphereActor(Actor))
                {
                    UFunction* SetTimeFunction = Actor->FindFunction(TEXT("SetTimeOfDay"));
                    if (SetTimeFunction)
                    {
                        // ProcessEvent treats the buffer as the function's FULL parameter
                        // frame (SetTimeFunction->ParmsSize), not just one float. SkySphere
                        // variants declare SetTimeOfDay with differing frames, so passing the
                        // address of a lone 4-byte stack float lets ProcessEvent read/write
                        // past it and corrupt the stack. Build a correctly-sized frame, write
                        // the time into the first float input parm, and run under the editor
                        // script guard (editor-world AActors otherwise no-op the BP event).
                        uint8* Parms = nullptr;
                        if (SetTimeFunction->ParmsSize > 0)
                        {
                            Parms = static_cast<uint8*>(
                                FMemory::Malloc(SetTimeFunction->ParmsSize, SetTimeFunction->GetMinAlignment()));
                            FMemory::Memzero(Parms, SetTimeFunction->ParmsSize);
                            for (TFieldIterator<FProperty> It(SetTimeFunction); It && (It->PropertyFlags & CPF_Parm); ++It)
                            {
                                if (It->GetSize() > 0)
                                {
                                    It->InitializeValue_InContainer(Parms);
                                }
                            }
                            for (TFieldIterator<FProperty> It(SetTimeFunction); It && (It->PropertyFlags & CPF_Parm); ++It)
                            {
                                const EPropertyFlags Flags = It->GetPropertyFlags();
                                if ((Flags & (CPF_ReturnParm | CPF_OutParm)) == 0)
                                {
                                    if (FFloatProperty* FloatProp = CastField<FFloatProperty>(*It))
                                    {
                                        FloatProp->SetPropertyValue_InContainer(Parms, TimeOfDay);
                                        break;
                                    }
                                }
                            }
                        }
                        ON_SCOPE_EXIT
                        {
                            if (Parms)
                            {
                                for (TFieldIterator<FProperty> It(SetTimeFunction); It && (It->PropertyFlags & CPF_Parm); ++It)
                                {
                                    if (It->GetSize() > 0)
                                    {
                                        It->DestroyValue_InContainer(Parms);
                                    }
                                }
                                FMemory::Free(Parms);
                            }
                        };

                        FEditorScriptExecutionGuard ScriptGuard;
                        Actor->ProcessEvent(SetTimeFunction, Parms);
                        bSuccess = true;
                        break;
                    }

                    // Modern /Engine/EngineSky/BP_Sky_Sphere has no SetTimeOfDay
                    // UFunction. It drives time of day from its "Sun height" BP
                    // variable (a double, roughly -1=midnight .. 1=noon) that its
                    // construction script re-applies to the sky material and linked
                    // directional light. Map the 0..24 time onto that range and
                    // rerun construction so the sky updates. Engine authors the BP
                    // variable's FName with the literal "Sun height" spelling.
                    // FNumericProperty::SetFloatingPointPropertyValue writes either a
                    // float or double "Sun height" without branching on the concrete
                    // FProperty subclass (IsFloatingPoint() guards out an int variant).
                    FNumericProperty* SunHeightProp =
                        CastField<FNumericProperty>(Actor->GetClass()->FindPropertyByName(TEXT("Sun height")));
                    if (SunHeightProp && SunHeightProp->IsFloatingPoint())
                    {
                        // -cos so midnight (0/24)->-1, noon (12)->+1.
                        const double SunHeight = -FMath::Cos((TimeOfDay / 24.0) * 2.0 * PI);
                        Actor->Modify();
                        SunHeightProp->SetFloatingPointPropertyValue(
                            SunHeightProp->ContainerPtrToValuePtr<void>(Actor), SunHeight);
                        Actor->RerunConstructionScripts();
                        bSuccess = true;
                        break;
                    }
                }
            }
        }
    }

    if (bSuccess)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("action"), TEXT("set_time_of_day"));
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("SET_TIME_FAILED"), TEXT("Sky sphere not found or time function not available"));
    }
    return true;
}

// ---- environment.build (legacy dispatcher) ----
// Preserves backward compatibility for "environment.build.*" compound dispatch
REGISTER_RPC_HANDLER("environment.build", "environment", "Build environment actions (legacy dispatcher)",
    RPC_PARAMS(
        RPC_PARAM_REQ("action", "string", "The environment build sub-action to execute")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("build_environment payload missing."));
        return true;
    }

    FString SubAction;
    Payload->TryGetStringField(TEXT("action"), SubAction);
    const FString LowerSub = SubAction.ToLower();

    auto* Sub = Ctx.GetSubsystem();
    if (!Sub)
    {
        Ctx.SendError(TEXT("SUBSYSTEM_NOT_FOUND"), TEXT("Subsystem not available"));
        return true;
    }

    // Cross-dispatch foliage sub-actions to their dedicated handlers
    if (LowerSub == TEXT("add_foliage_instances"))
    {
        // Transform from build_environment schema to foliage handler schema
        FString FoliageTypePath;
        Payload->TryGetStringField(TEXT("foliageType"), FoliageTypePath);
        const TArray<TSharedPtr<FJsonValue>>* Transforms = nullptr;
        Payload->TryGetArrayField(TEXT("transforms"), Transforms);
        TSharedPtr<FJsonObject> FoliagePayload = MakeShared<FJsonObject>();
        if (!FoliageTypePath.IsEmpty())
        {
            FoliagePayload->SetStringField(TEXT("foliageTypePath"), FoliageTypePath);
        }
        TArray<TSharedPtr<FJsonValue>> Locations;
        if (Transforms)
        {
            for (const TSharedPtr<FJsonValue>& V : *Transforms)
            {
                if (!V.IsValid() || V->Type != EJson::Object) continue;
                const TSharedPtr<FJsonObject>* TObj = nullptr;
                if (!V->TryGetObject(TObj) || !TObj) continue;
                const TSharedPtr<FJsonObject>* LocObj = nullptr;
                if (!(*TObj)->TryGetObjectField(TEXT("location"), LocObj) || !LocObj) continue;
                double X = 0, Y = 0, Z = 0;
                (*LocObj)->TryGetNumberField(TEXT("x"), X);
                (*LocObj)->TryGetNumberField(TEXT("y"), Y);
                (*LocObj)->TryGetNumberField(TEXT("z"), Z);
                TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
                L->SetNumberField(TEXT("x"), X);
                L->SetNumberField(TEXT("y"), Y);
                L->SetNumberField(TEXT("z"), Z);
                Locations.Add(MakeShared<FJsonValueObject>(L));
            }
        }
        FoliagePayload->SetArrayField(TEXT("locations"), Locations);
        return CrossDispatchEnv(Ctx, TEXT("foliage.paint"), FoliagePayload);
    }
    else if (LowerSub == TEXT("get_foliage_instances"))
    {
        FString FoliageTypePath;
        Payload->TryGetStringField(TEXT("foliageType"), FoliageTypePath);
        TSharedPtr<FJsonObject> FoliagePayload = MakeShared<FJsonObject>();
        if (!FoliageTypePath.IsEmpty())
            FoliagePayload->SetStringField(TEXT("foliageTypePath"), FoliageTypePath);
        return CrossDispatchEnv(Ctx, TEXT("foliage.get_instances"), FoliagePayload);
    }
    else if (LowerSub == TEXT("remove_foliage"))
    {
        FString FoliageTypePath;
        Payload->TryGetStringField(TEXT("foliageType"), FoliageTypePath);
        bool bRemoveAll = false;
        Payload->TryGetBoolField(TEXT("removeAll"), bRemoveAll);
        TSharedPtr<FJsonObject> FoliagePayload = MakeShared<FJsonObject>();
        if (!FoliageTypePath.IsEmpty())
            FoliagePayload->SetStringField(TEXT("foliageTypePath"), FoliageTypePath);
        FoliagePayload->SetBoolField(TEXT("removeAll"), bRemoveAll);
        return CrossDispatchEnv(Ctx, TEXT("foliage.remove"), FoliagePayload);
    }
    else if (LowerSub == TEXT("paint_foliage"))
    {
        return CrossDispatchEnv(Ctx, TEXT("foliage.paint"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("create_procedural_foliage"))
    {
        return CrossDispatchEnv(Ctx, TEXT("foliage.create_procedural"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("add_foliage_type") || LowerSub == TEXT("add_foliage"))
    {
        return CrossDispatchEnv(Ctx, TEXT("foliage.add_type"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("create_landscape"))
    {
        return CrossDispatchEnv(Ctx, TEXT("landscape.create"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("paint_landscape") || LowerSub == TEXT("paint_landscape_layer"))
    {
        // The old HandlePaintLandscapeLayer was part of landscape edit -- dispatch to landscape.edit
        return CrossDispatchEnv(Ctx, TEXT("landscape.edit"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("sculpt_landscape") || LowerSub == TEXT("sculpt"))
    {
        return CrossDispatchEnv(Ctx, TEXT("landscape.sculpt"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("modify_heightmap"))
    {
        // modify_heightmap was handled by landscape edit handler
        return CrossDispatchEnv(Ctx, TEXT("landscape.edit"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("set_landscape_material"))
    {
        return CrossDispatchEnv(Ctx, TEXT("landscape.set_material"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("create_landscape_grass_type"))
    {
        return CrossDispatchEnv(Ctx, TEXT("landscape.create_grass_type"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("create_procedural_terrain"))
    {
        return CrossDispatchEnv(Ctx, TEXT("landscape.create_procedural_terrain"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("generate_lods"))
    {
        // generate_lods migrated to Handlers/Asset/AssetWorkflowHandler.cpp as asset.generate_lods
        return CrossDispatchEnv(Ctx, TEXT("asset.generate_lods"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("bake_lightmap"))
    {
        // environment.build.bake_lightmap was a pure forwarding shim to
        // level.build_lighting; dispatch straight through now that the shim is gone
        // (the quality field passes untouched in the raw payload).
        return CrossDispatchEnv(Ctx, TEXT("level.build_lighting"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("delete"))
    {
        // actor.delete supersedes environment.build.delete; translate the legacy
        // "names" array into actor.delete's "actorNames".
        TSharedPtr<FJsonObject> DeletePayload = MakeShared<FJsonObject>();
        const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
        if (Payload->TryGetArrayField(TEXT("names"), NamesArray) && NamesArray)
        {
            DeletePayload->SetArrayField(TEXT("actorNames"), *NamesArray);
        }
        return CrossDispatchEnv(Ctx, TEXT("actor.delete"), DeletePayload);
    }
    else if (LowerSub == TEXT("create_sky_sphere"))
    {
        return CrossDispatchEnv(Ctx, TEXT("environment.build.create_sky_sphere"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("set_time_of_day"))
    {
        return CrossDispatchEnv(Ctx, TEXT("environment.build.set_time_of_day"), Ctx.GetRawPayload());
    }
    else if (LowerSub == TEXT("create_fog_volume"))
    {
        // actor.spawn supersedes environment.build.create_fog_volume (which only ever
        // spawned an ExponentialHeightFog); translate the legacy flat x/y/z + name.
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.ExponentialHeightFog"));
        FString FogName;
        if (Payload->TryGetStringField(TEXT("name"), FogName) && !FogName.IsEmpty())
        {
            SpawnPayload->SetStringField(TEXT("actorName"), FogName);
        }
        double FogX = 0.0, FogY = 0.0, FogZ = 0.0;
        Payload->TryGetNumberField(TEXT("x"), FogX);
        Payload->TryGetNumberField(TEXT("y"), FogY);
        Payload->TryGetNumberField(TEXT("z"), FogZ);
        TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
        LocationObj->SetNumberField(TEXT("x"), FogX);
        LocationObj->SetNumberField(TEXT("y"), FogY);
        LocationObj->SetNumberField(TEXT("z"), FogZ);
        SpawnPayload->SetObjectField(TEXT("location"), LocationObj);
        return CrossDispatchEnv(Ctx, TEXT("actor.spawn"), SpawnPayload);
    }

    // Unknown sub-action
    Ctx.SendError(TEXT("NOT_IMPLEMENTED"),
        FString::Printf(TEXT("Environment action '%s' not implemented"), *SubAction));
    return true;
}


// =====================================================================
// Procedural terrain
// =====================================================================

// ---- environment.build.create_procedural_terrain ----
REGISTER_RPC_HANDLER("environment.build.create_procedural_terrain", "environment", "Create a procedural terrain mesh actor",
    RPC_PARAMS(
        RPC_PARAM_OPT("sizeX", "number", "Grid size X (default 100)"),
        RPC_PARAM_OPT("sizeY", "number", "Grid size Y (default 100)"),
        RPC_PARAM_OPT("spacing", "number", "Vertex spacing (default 100)"),
        RPC_PARAM_OPT("heightScale", "number", "Height scale factor (default 500)"),
        RPC_PARAM_OPT("subdivisions", "number", "Grid subdivision count (default 50)"),
        RPC_PARAM_OPT("actorName", "string", "Name for the terrain actor"),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("material", "path", "Material asset path to apply")
    ))
{
    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor not available"));
        return true;
    }

    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("create_procedural_terrain payload missing"));
        return true;
    }

    // Get terrain parameters
    int32 SizeX = 100;
    int32 SizeY = 100;
    double Spacing = 100.0;
    double HeightScale = 500.0;
    int32 Subdivisions = 50;
    FString ActorName = TEXT("ProceduralTerrain");

    Payload->TryGetNumberField(TEXT("sizeX"), SizeX);
    Payload->TryGetNumberField(TEXT("sizeY"), SizeY);
    Payload->TryGetNumberField(TEXT("spacing"), Spacing);
    Payload->TryGetNumberField(TEXT("heightScale"), HeightScale);
    Payload->TryGetNumberField(TEXT("subdivisions"), Subdivisions);
    Payload->TryGetStringField(TEXT("actorName"), ActorName);

    // Strict validation
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName parameter is required for create_procedural_terrain"));
        return true;
    }

    if (ActorName.Contains(TEXT("/")) || ActorName.Contains(TEXT("\\")) ||
        ActorName.Contains(TEXT(":")) || ActorName.Contains(TEXT("*")) ||
        ActorName.Contains(TEXT("?")) || ActorName.Contains(TEXT("\"")) ||
        ActorName.Contains(TEXT("<")) || ActorName.Contains(TEXT(">")) ||
        ActorName.Contains(TEXT("|")))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("actorName contains invalid characters (/, \\, :, *, ?, \", <, >, |)"));
        return true;
    }

    if (ActorName.Len() > 128)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("actorName exceeds maximum length of 128 characters"));
        return true;
    }

    // Clamp values to reasonable limits
    SizeX = FMath::Clamp(SizeX, 2, 1000);
    SizeY = FMath::Clamp(SizeY, 2, 1000);
    Subdivisions = FMath::Clamp(Subdivisions, 2, 200);
    Spacing = FMath::Max(Spacing, 1.0);
    HeightScale = FMath::Max(HeightScale, 0.0);

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(TEXT("WORLD_NOT_AVAILABLE"), TEXT("World not available"));
        return true;
    }

    // Spawn the actor
    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = FName(*ActorName);
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;

    FVector Location(0, 0, 0);
    const TSharedPtr<FJsonObject>* LocObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
    {
        double X = 0, Y = 0, Z = 0;
        (*LocObj)->TryGetNumberField(TEXT("x"), X);
        (*LocObj)->TryGetNumberField(TEXT("y"), Y);
        (*LocObj)->TryGetNumberField(TEXT("z"), Z);
        Location = FVector(X, Y, Z);
    }

    FRotator Rotation(0, 0, 0);
    const TSharedPtr<FJsonObject>* RotObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
    {
        double Pitch = 0, Yaw = 0, Roll = 0;
        (*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
        (*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
        (*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
        Rotation = FRotator(Pitch, Yaw, Roll);
    }

    AActor* TerrainActor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, Rotation, SpawnParams);
    if (!TerrainActor)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn terrain actor"));
        return true;
    }

    // Apply the caller's name to the editor display label, not just the internal object
    // name (SpawnParams.Name above). Without this the World Outliner label — and the
    // AddActorVerification `actorName` echo, which reads GetActorLabel() — fall back to the
    // generic class default "Actor". Mirrors the sibling create verb
    // create_sky_sphere, which labels via SpawnActorInActiveWorld(..., Label).
    TerrainActor->SetActorLabel(ActorName);

    // Add procedural mesh component
    UProceduralMeshComponent* ProcMesh = NewObject<UProceduralMeshComponent>(TerrainActor);
    if (!ProcMesh)
    {
        TerrainActor->Destroy();
        Ctx.SendError(TEXT("COMPONENT_CREATION_FAILED"), TEXT("Failed to create procedural mesh component"));
        return true;
    }

    ProcMesh->RegisterComponent();
    TerrainActor->AddInstanceComponent(ProcMesh);
    TerrainActor->SetRootComponent(ProcMesh);

    // Generate terrain mesh
    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<FProcMeshTangent> Tangents;

    // Create grid of vertices
    for (int32 Y = 0; Y <= Subdivisions; ++Y)
    {
        for (int32 X = 0; X <= Subdivisions; ++X)
        {
            double NormX = static_cast<double>(X) / Subdivisions;
            double NormY = static_cast<double>(Y) / Subdivisions;

            double WorldX = (NormX - 0.5) * SizeX * Spacing;
            double WorldY = (NormY - 0.5) * SizeY * Spacing;

            double WorldZ = FMath::Sin(NormX * 4.0 * PI) * FMath::Cos(NormY * 4.0 * PI) * HeightScale * 0.3 +
                            FMath::Sin(NormX * 8.0 * PI) * FMath::Cos(NormY * 8.0 * PI) * HeightScale * 0.15 +
                            FMath::Sin(NormX * 2.0 * PI + NormY * 3.0 * PI) * HeightScale * 0.25;

            Vertices.Add(FVector(WorldX, WorldY, WorldZ));
            UVs.Add(FVector2D(NormX, NormY));
        }
    }

    // Generate triangles
    for (int32 Y = 0; Y < Subdivisions; ++Y)
    {
        for (int32 X = 0; X < Subdivisions; ++X)
        {
            int32 Current = Y * (Subdivisions + 1) + X;
            int32 Next = Current + Subdivisions + 1;

            Triangles.Add(Current);
            Triangles.Add(Next);
            Triangles.Add(Current + 1);

            Triangles.Add(Current + 1);
            Triangles.Add(Next);
            Triangles.Add(Next + 1);
        }
    }

    // Calculate normals and tangents
    UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Vertices, Triangles, UVs, Normals, Tangents);

    // Create the mesh section
    ProcMesh->CreateMeshSection(0, Vertices, Triangles, Normals, UVs, TArray<FColor>(), Tangents, true);

    // Apply material if specified. Track what actually landed so the success
    // response can echo it — a caller who passed `material` would otherwise have
    // to fire a separate actor.describe readback to confirm the material took
    // (board E-create-procedural-terrain-no-material-echo).
    FString MaterialPath;
    bool bMaterialApplied = false;
    FString MaterialWarning;
    if (Payload->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.IsEmpty())
    {
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        if (Material)
        {
            ProcMesh->SetMaterial(0, Material);
            bMaterialApplied = true;
        }
        else
        {
            // Don't swallow a failed load silently: the call still succeeds (the
            // terrain mesh is valid) but the requested material was skipped, so
            // surface a warning instead of a clean success that hides the miss.
            MaterialWarning = FString::Printf(
                TEXT("material '%s' could not be loaded and was not applied"), *MaterialPath);
        }
    }

    TerrainActor->MarkPackageDirty();

    // Build response
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("actorName"), TerrainActor->GetName());
    Resp->SetStringField(TEXT("actorPath"), TerrainActor->GetPathName());
    Resp->SetNumberField(TEXT("vertices"), Vertices.Num());
    Resp->SetNumberField(TEXT("triangles"), Triangles.Num() / 3);
    Resp->SetNumberField(TEXT("sizeX"), SizeX);
    Resp->SetNumberField(TEXT("sizeY"), SizeY);
    Resp->SetNumberField(TEXT("subdivisions"), Subdivisions);
    // Echo the applied material so confirmation lives in the create response.
    // `material_applied` is always present (false when no material was requested);
    // `materialPath` carries the requested path only when one was given. Field
    // names match the other material-echoing handlers — `materialPath` as in
    // asset/landscape/spline/render, `material_applied` as in the SCS handler.
    Resp->SetBoolField(TEXT("material_applied"), bMaterialApplied);
    if (!MaterialPath.IsEmpty())
    {
        Resp->SetStringField(TEXT("materialPath"), MaterialPath);
    }
    if (!MaterialWarning.IsEmpty())
    {
        Resp->SetStringField(TEXT("materialWarning"), MaterialWarning);
    }
    AddActorVerification(Resp, TerrainActor);

    Ctx.SendSuccess(Resp);
    return true;
}

// =====================================================================
// system.inspect.* sub-actions
// =====================================================================

// ---- system.inspect.get_viewport_info ----
REGISTER_RPC_HANDLER("system.inspect.get_viewport_info", "system.inspect", "Return the active viewport's pixel width/height plus the level-editor camera transform (cameraLocation {x,y,z}, cameraRotation {pitch,yaw,roll}, fov) read from the level-editor viewport client. `pie` says whether the measured viewport is PIE's game viewport and `activeViewport` names it; the camera fields always describe the EDITOR camera, never the play camera, and are omitted when no level-editor viewport is open. Returns success with no dimensions if no viewport is currently active.",
    RPC_NO_PARAMS)
{
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    if (GEditor && GEditor->GetActiveViewport())
    {
        FViewport* Viewport = GEditor->GetActiveViewport();
        Resp->SetNumberField(TEXT("width"), Viewport->GetSizeXY().X);
        Resp->SetNumberField(TEXT("height"), Viewport->GetSizeXY().Y);

        // Which viewport those dimensions were measured on. While PIE runs inside the level
        // viewport the active viewport is the game one, so the geometry is the play window's
        // while the camera below is still the editor's; saying so is the only way a caller can
        // tell the two apart.
        const bool bPlayInEditorViewport = EditorHandlerUtils::IsActiveViewportPlayInEditor();
        Resp->SetBoolField(TEXT("pie"), bPlayInEditorViewport);
        Resp->SetStringField(TEXT("activeViewport"),
            bPlayInEditorViewport ? TEXT("pieGameViewport") : TEXT("levelEditorViewport"));

        // The camera comes from the typed level-editor client, never from the active viewport's
        // client: under PIE that client is a UGameViewportClient and reading FEditorViewportClient
        // camera members through it is an out-of-bounds read (see ResolveActiveLevelViewportClient).
        if (FEditorViewportClient* ViewportClient =
                EditorHandlerUtils::ResolveActiveLevelViewportClient())
        {
            Resp->SetObjectField(TEXT("cameraLocation"),
                JsonBuilders::BuildVectorJson(ViewportClient->GetViewLocation()));
            Resp->SetObjectField(TEXT("cameraRotation"),
                JsonBuilders::BuildRotatorJson(ViewportClient->GetViewRotation()));
            Resp->SetNumberField(TEXT("fov"), ViewportClient->ViewFOV);
        }
        Resp->SetBoolField(TEXT("success"), true);
    }
    else
    {
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("message"), TEXT("Viewport info not available in this context"));
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- system.inspect.get_selected_actors ----
REGISTER_RPC_HANDLER("system.inspect.get_selected_actors", "system.inspect", "Return the actors currently selected in the level editor as {name, path, class} entries. Read-only; for write-side selection use the editor.* selection methods.",
    RPC_NO_PARAMS)
{
    TArray<TSharedPtr<FJsonValue>> ActorsArray;
    if (GEditor)
    {
        TArray<AActor*> SelectedActors;
        GEditor->GetSelectedActors()->GetSelectedObjects(SelectedActors);
        for (AActor* Actor : SelectedActors)
        {
            if (Actor)
            {
                TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
                ActorObj->SetStringField(TEXT("name"), Actor->GetName());
                ActorObj->SetStringField(TEXT("path"), Actor->GetPathName());
                ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
                ActorsArray.Add(MakeShared<FJsonValueObject>(ActorObj));
            }
        }
    }
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("actors"), ActorsArray);
    Resp->SetNumberField(TEXT("count"), ActorsArray.Num());
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
}


// ---- environment.spawn_sky_atmosphere ----
REGISTER_RPC_HANDLER("environment.spawn_sky_atmosphere", "environment", "Spawn an ASkyAtmosphere actor and apply properties to its USkyAtmosphereComponent",
    RPC_PARAMS(
        RPC_PARAM_OPT("name", "string", "Label for the spawned actor"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("properties", "object", "Component properties (e.g. RayleighScatteringScale, MieScatteringScale)")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();

    FVector Location;
    FRotator Rotation;
    ReadLocationRotationFromPayload(*Payload, Location, Rotation);

    const FString Name = Ctx.GetString(TEXT("name"));
    AActor* NewActor = SpawnActorInActiveWorld<AActor>(
        ASkyAtmosphere::StaticClass(), Location, Rotation, Name);
    if (!NewActor)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn SkyAtmosphere actor"));
        return true;
    }

    USkyAtmosphereComponent* Comp = NewActor->FindComponentByClass<USkyAtmosphereComponent>();
    if (!Comp)
    {
        Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"), TEXT("Spawned actor missing USkyAtmosphereComponent"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> RejectedProps;
    const TSharedPtr<FJsonObject>* Props = nullptr;
    Payload->TryGetObjectField(TEXT("properties"), Props);
    if (ApplyPropertiesJsonToComponent(Comp, Props ? *Props : TSharedPtr<FJsonObject>(), RejectedProps) > 0)
    {
        Comp->MarkRenderStateDirty();
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    AddActorVerification(Resp, NewActor);
    AddChainableActorFields(Resp, NewActor);
    AddComponentVerification(Resp, Comp);
    if (RejectedProps.Num() > 0)
    {
        Resp->SetArrayField(TEXT("rejected"), RejectedProps);
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- environment.spawn_volumetric_cloud ----
REGISTER_RPC_HANDLER("environment.spawn_volumetric_cloud", "environment", "Spawn an AVolumetricCloud actor and apply properties to its UVolumetricCloudComponent",
    RPC_PARAMS(
        RPC_PARAM_OPT("name", "string", "Label for the spawned actor"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("properties", "object", "Component properties (e.g. LayerBottomAltitude, LayerHeight, Material)")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();

    FVector Location;
    FRotator Rotation;
    ReadLocationRotationFromPayload(*Payload, Location, Rotation);

    const FString Name = Ctx.GetString(TEXT("name"));
    AActor* NewActor = SpawnActorInActiveWorld<AActor>(
        AVolumetricCloud::StaticClass(), Location, Rotation, Name);
    if (!NewActor)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn VolumetricCloud actor"));
        return true;
    }

    UVolumetricCloudComponent* Comp = NewActor->FindComponentByClass<UVolumetricCloudComponent>();
    if (!Comp)
    {
        Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"), TEXT("Spawned actor missing UVolumetricCloudComponent"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> RejectedProps;
    const TSharedPtr<FJsonObject>* Props = nullptr;
    Payload->TryGetObjectField(TEXT("properties"), Props);
    if (ApplyPropertiesJsonToComponent(Comp, Props ? *Props : TSharedPtr<FJsonObject>(), RejectedProps) > 0)
    {
        Comp->MarkRenderStateDirty();
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    AddActorVerification(Resp, NewActor);
    AddChainableActorFields(Resp, NewActor);
    AddComponentVerification(Resp, Comp);
    if (RejectedProps.Num() > 0)
    {
        Resp->SetArrayField(TEXT("rejected"), RejectedProps);
    }
    Ctx.SendSuccess(Resp);
    return true;
}
