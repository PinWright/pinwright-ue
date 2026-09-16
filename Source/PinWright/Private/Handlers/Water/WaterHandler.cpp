// Copyright (c) 2026 Alexander Penkin. MIT License.

// WaterHandler.cpp — water.* namespace for UE Water plugin actor authoring.
//
// Registration is unconditional so discovery lists water.* even when the Water
// plugin is disabled in the project — only the handler body branches on the
// MCP_HAS_WATER gate. Mirrors the MetaSound conditional-compile pattern in
// MetaSoundDestructiveHandler.cpp (lines 100-113).
//
// Spline-body authoring (River/Lake/Custom): callers should chain
// spline.set_spline_point_position against the returned actor's WaterSpline
// component after spawn. No optional splinePoints param on spawn_water_body
// — that would re-implement spline editing inside the water handler.

#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/AssetUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/PropertyUtils.h"
#include "Compat/JsonKeyCompat.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"

// Water plugin support — conditional on the public WaterBodyActor.h header
// being reachable. Drives the namespace.
#if __has_include("WaterBodyActor.h")
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterBodyCustomActor.h"
#include "WaterBodyRiverComponent.h"
#include "WaterSplineComponent.h"
#include "WaterZoneActor.h"
#include "Utils/ScopedLandscapeLayerDialog.h"
#define MCP_HAS_WATER 1
#else
#define MCP_HAS_WATER 0
#endif

#if MCP_HAS_WATER
namespace
{
    // Resolve the lowercased water body type string to its UClass.
    // Returns nullptr if the string does not match a known short name.
    UClass* ResolveWaterBodyClass(const FString& LowerType)
    {
        if (LowerType == TEXT("river"))  return AWaterBodyRiver::StaticClass();
        if (LowerType == TEXT("lake"))   return AWaterBodyLake::StaticClass();
        if (LowerType == TEXT("ocean"))  return AWaterBodyOcean::StaticClass();
        if (LowerType == TEXT("custom")) return AWaterBodyCustom::StaticClass();
        return nullptr;
    }

    // Parse {x, y, z} JSON object into FVector. Missing fields default to 0.
    FVector ReadVector(const TSharedPtr<FJsonObject>& Obj, const FVector& Default)
    {
        if (!Obj.IsValid()) return Default;
        FVector Out = Default;
        Obj->TryGetNumberField(TEXT("x"), Out.X);
        Obj->TryGetNumberField(TEXT("y"), Out.Y);
        Obj->TryGetNumberField(TEXT("z"), Out.Z);
        return Out;
    }

    // Parse {pitch, yaw, roll} JSON object into FRotator. Missing fields default to 0.
    FRotator ReadRotator(const TSharedPtr<FJsonObject>& Obj)
    {
        FRotator Out = FRotator::ZeroRotator;
        if (!Obj.IsValid()) return Out;
        Obj->TryGetNumberField(TEXT("pitch"), Out.Pitch);
        Obj->TryGetNumberField(TEXT("yaw"),   Out.Yaw);
        Obj->TryGetNumberField(TEXT("roll"),  Out.Roll);
        return Out;
    }

    // Load a material by asset path. Returns nullptr if the path is empty,
    // invalid, or does not resolve to a UMaterialInterface.
    UMaterialInterface* LoadMaterialOrNull(const FString& Path)
    {
        if (Path.IsEmpty()) return nullptr;
        const FString Resolved = ResolveAssetPath(Path);
        if (Resolved.IsEmpty()) return nullptr;
        return Cast<UMaterialInterface>(
            StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *Resolved));
    }

    UWorld* GetEditorWorldOrError(FHandlerContext& Ctx)
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!IsValid(World))
        {
            Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE, TEXT("Editor world not available"));
            return nullptr;
        }
        return World;
    }

    AWaterBody* ResolveWaterBodyActor(FHandlerContext& Ctx, const FString& ActorRef)
    {
        UWorld* World = GetEditorWorldOrError(Ctx);
        if (!World) return nullptr;
        AActor* Actor = McpActorUtils::FindActorByName(World, ActorRef);
        if (!Actor)
        {
            Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                FString::Printf(TEXT("No actor matches '%s'"), *ActorRef));
            return nullptr;
        }
        AWaterBody* Body = Cast<AWaterBody>(Actor);
        if (!Body)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_WATER_BODY_TYPE,
                FString::Printf(TEXT("Actor '%s' is not an AWaterBody"), *ActorRef));
            return nullptr;
        }
        return Body;
    }
}
#endif

// ---- water.spawn_water_body ----
REGISTER_RPC_HANDLER("water.spawn_water_body", "water",
    "Spawn a water body actor (river/lake/ocean/custom). For spline-based bodies "
    "(river/lake/custom), chain spline.set_spline_point_position against the "
    "spawned actor's WaterSpline to author the spline shape.",
    RPC_PARAMS(
        RPC_PARAM_REQ("type",     "string", "WaterBody subtype: River, Lake, Ocean, Custom"),
        RPC_PARAM_OPT("name",     "string", "Label for the spawned actor"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch, yaw, roll}")
    ))
{
#if MCP_HAS_WATER
    FString TypeStr;
    if (!Ctx.RequireString(TEXT("type"), TypeStr)) return true;

    UClass* Cls = ResolveWaterBodyClass(TypeStr.ToLower());
    if (!Cls)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_WATER_BODY_TYPE,
            FString::Printf(TEXT("Unknown water body type '%s'. Use River, Lake, Ocean, or Custom."), *TypeStr));
        return true;
    }

    auto* Payload = Ctx.GetRawPayload().Get();
    const TSharedPtr<FJsonObject>* LocPtr = nullptr;
    Payload->TryGetObjectField(TEXT("location"), LocPtr);
    const TSharedPtr<FJsonObject>* RotPtr = nullptr;
    Payload->TryGetObjectField(TEXT("rotation"), RotPtr);

    const FVector Location = ReadVector(LocPtr ? *LocPtr : nullptr, FVector::ZeroVector);
    const FRotator Rotation = ReadRotator(RotPtr ? *RotPtr : nullptr);

    const FString Label = Ctx.GetString(TEXT("name"));
    // Spawning a landscape-affecting water body synchronously triggers Water's brush
    // auto-setup, which would otherwise raise a blocking modal edit-layer dialog.
    FScopedLandscapeLayerDialog SuppressLayerDialog;
    AWaterBody* Body = SpawnActorInActiveWorld<AWaterBody>(Cls, Location, Rotation, Label);
    if (!Body)
    {
        Ctx.SendError(ErrorCodes::ERR_SPAWN_FAILED,
            FString::Printf(TEXT("Failed to spawn %s"), *Cls->GetName()));
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("actorName"), Body->GetActorLabel());
    Resp->SetStringField(TEXT("className"), Cls->GetName());
    AddActorVerification(Resp, Body);
    Ctx.SendSuccess(Resp);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_WATER_PLUGIN_NOT_AVAILABLE,
        TEXT("Water plugin headers not compiled into PinWright. Enable the Water plugin and rebuild."));
    return true;
#endif
}

// ---- water.spawn_water_zone ----
REGISTER_RPC_HANDLER("water.spawn_water_zone", "water",
    "Spawn an AWaterZone actor. Required for water rendering and underwater "
    "post-process to function in the level.",
    RPC_PARAMS(
        RPC_PARAM_OPT("name",     "string", "Label for the spawned zone"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("extent",   "object", "Zone extent {x, y} in world units")
    ))
{
#if MCP_HAS_WATER
    auto* Payload = Ctx.GetRawPayload().Get();
    const TSharedPtr<FJsonObject>* LocPtr = nullptr;
    Payload->TryGetObjectField(TEXT("location"), LocPtr);
    const FVector Location = ReadVector(LocPtr ? *LocPtr : nullptr, FVector::ZeroVector);

    const FString Label = Ctx.GetString(TEXT("name"));
    FScopedLandscapeLayerDialog SuppressLayerDialog;
    AWaterZone* Zone = SpawnActorInActiveWorld<AWaterZone>(
        AWaterZone::StaticClass(), Location, FRotator::ZeroRotator, Label);
    if (!Zone)
    {
        Ctx.SendError(ErrorCodes::ERR_SPAWN_FAILED, TEXT("Failed to spawn AWaterZone"));
        return true;
    }

    const TSharedPtr<FJsonObject>* ExtentPtr = nullptr;
    if (Payload->TryGetObjectField(TEXT("extent"), ExtentPtr) && ExtentPtr && (*ExtentPtr).IsValid())
    {
        FVector2D Extent;
        Extent.X = 0.0;
        Extent.Y = 0.0;
        (*ExtentPtr)->TryGetNumberField(TEXT("x"), Extent.X);
        (*ExtentPtr)->TryGetNumberField(TEXT("y"), Extent.Y);
        if (Extent.X > 0.0 && Extent.Y > 0.0)
        {
            Zone->SetZoneExtent(Extent);
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("actorName"), Zone->GetActorLabel());
    Resp->SetStringField(TEXT("className"), TEXT("WaterZone"));
    AddActorVerification(Resp, Zone);
    Ctx.SendSuccess(Resp);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_WATER_PLUGIN_NOT_AVAILABLE,
        TEXT("Water plugin headers not compiled into PinWright. Enable the Water plugin and rebuild."));
    return true;
#endif
}

// ---- water.set_water_body_material ----
REGISTER_RPC_HANDLER("water.set_water_body_material", "water",
    "Set materials on an AWaterBody's UWaterBodyComponent. Each material is "
    "optional; only provided channels are updated. River-to-lake / river-to-ocean "
    "transition materials require an AWaterBodyRiver actor.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actor",                            "string", "Actor label, name, or path"),
        RPC_PARAM_OPT("waterMaterial",                    "string", "Asset path of the surface water material"),
        RPC_PARAM_OPT("underwaterMaterial",               "string", "Asset path of the underwater post-process material"),
        RPC_PARAM_OPT("riverToLakeTransitionMaterial",    "string", "Asset path; river actors only"),
        RPC_PARAM_OPT("riverToOceanTransitionMaterial",   "string", "Asset path; river actors only")
    ))
{
#if MCP_HAS_WATER
    FString ActorRef;
    if (!Ctx.RequireString(TEXT("actor"), ActorRef)) return true;

    AWaterBody* Body = ResolveWaterBodyActor(Ctx, ActorRef);
    if (!Body) return true;

    UWaterBodyComponent* Comp = Body->GetWaterBodyComponent();
    if (!Comp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WATER_BODY_COMPONENT,
            FString::Printf(TEXT("Actor '%s' has no UWaterBodyComponent"), *ActorRef));
        return true;
    }

    auto* Payload = Ctx.GetRawPayload().Get();
    TArray<FString> Applied;

    FString WaterMatPath;
    if (Payload->TryGetStringField(TEXT("waterMaterial"), WaterMatPath))
    {
        if (UMaterialInterface* M = LoadMaterialOrNull(WaterMatPath))
        {
            Comp->SetWaterMaterial(M);
            Applied.Add(TEXT("waterMaterial"));
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("waterMaterial not found: %s"), *WaterMatPath));
            return true;
        }
    }

    FString UnderwaterMatPath;
    if (Payload->TryGetStringField(TEXT("underwaterMaterial"), UnderwaterMatPath))
    {
        if (UMaterialInterface* M = LoadMaterialOrNull(UnderwaterMatPath))
        {
            Comp->SetUnderwaterPostProcessMaterial(M);
            Applied.Add(TEXT("underwaterMaterial"));
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("underwaterMaterial not found: %s"), *UnderwaterMatPath));
            return true;
        }
    }

    FString RiverToLakePath;
    const bool bHasRiverToLake = Payload->TryGetStringField(TEXT("riverToLakeTransitionMaterial"), RiverToLakePath);
    FString RiverToOceanPath;
    const bool bHasRiverToOcean = Payload->TryGetStringField(TEXT("riverToOceanTransitionMaterial"), RiverToOceanPath);

    if (bHasRiverToLake || bHasRiverToOcean)
    {
        UWaterBodyRiverComponent* RiverComp = Cast<UWaterBodyRiverComponent>(Comp);
        if (!RiverComp)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_WATER_BODY_TYPE,
                TEXT("riverToLakeTransitionMaterial / riverToOceanTransitionMaterial require an AWaterBodyRiver actor"));
            return true;
        }
        if (bHasRiverToLake)
        {
            UMaterialInterface* M = LoadMaterialOrNull(RiverToLakePath);
            if (!M)
            {
                Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("riverToLakeTransitionMaterial not found: %s"), *RiverToLakePath));
                return true;
            }
            RiverComp->SetLakeTransitionMaterial(M);
            Applied.Add(TEXT("riverToLakeTransitionMaterial"));
        }
        if (bHasRiverToOcean)
        {
            UMaterialInterface* M = LoadMaterialOrNull(RiverToOceanPath);
            if (!M)
            {
                Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("riverToOceanTransitionMaterial not found: %s"), *RiverToOceanPath));
                return true;
            }
            RiverComp->SetOceanTransitionMaterial(M);
            Applied.Add(TEXT("riverToOceanTransitionMaterial"));
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& F : Applied) AppliedJson.Add(MakeShared<FJsonValueString>(F));
    Resp->SetArrayField(TEXT("applied"), AppliedJson);
    AddActorVerification(Resp, Body);
    Ctx.SendSuccess(Resp);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_WATER_PLUGIN_NOT_AVAILABLE,
        TEXT("Water plugin headers not compiled into PinWright. Enable the Water plugin and rebuild."));
    return true;
#endif
}

// ---- water.set_water_body_underwater_post_process ----
REGISTER_RPC_HANDLER("water.set_water_body_underwater_post_process", "water",
    "Wire up the underwater post-process material and settings on the body's "
    "UWaterBodyComponent. The settings JSON is applied field-by-field via "
    "property reflection onto FUnderwaterPostProcessSettings.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actor",                "string", "Actor label, name, or path"),
        RPC_PARAM_OPT("postProcessMaterial",  "string", "Asset path of the underwater post-process material"),
        RPC_PARAM_OPT("settings",             "object", "FUnderwaterPostProcessSettings field map")
    ))
{
#if MCP_HAS_WATER
    FString ActorRef;
    if (!Ctx.RequireString(TEXT("actor"), ActorRef)) return true;

    AWaterBody* Body = ResolveWaterBodyActor(Ctx, ActorRef);
    if (!Body) return true;

    UWaterBodyComponent* Comp = Body->GetWaterBodyComponent();
    if (!Comp)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WATER_BODY_COMPONENT,
            FString::Printf(TEXT("Actor '%s' has no UWaterBodyComponent"), *ActorRef));
        return true;
    }

    auto* Payload = Ctx.GetRawPayload().Get();
    TArray<FString> Applied;

    FString PPMatPath;
    if (Payload->TryGetStringField(TEXT("postProcessMaterial"), PPMatPath))
    {
        if (UMaterialInterface* M = LoadMaterialOrNull(PPMatPath))
        {
            Comp->SetUnderwaterPostProcessMaterial(M);
            Applied.Add(TEXT("postProcessMaterial"));
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("postProcessMaterial not found: %s"), *PPMatPath));
            return true;
        }
    }

    const TSharedPtr<FJsonObject>* SettingsPtr = nullptr;
    if (Payload->TryGetObjectField(TEXT("settings"), SettingsPtr) && SettingsPtr && (*SettingsPtr).IsValid())
    {
        // Resolve the UnderwaterPostProcessSettings struct property on the component.
        FProperty* SettingsProp = FindPropertyCI(Comp->GetClass(), TEXT("UnderwaterPostProcessSettings"));
        FStructProperty* SettingsStructProp = CastField<FStructProperty>(SettingsProp);
        if (!SettingsStructProp)
        {
            Ctx.SendError(ErrorCodes::ERR_INTERNAL_ERROR,
                TEXT("UnderwaterPostProcessSettings property not found on UWaterBodyComponent"));
            return true;
        }

        void* SettingsContainer = SettingsStructProp->ContainerPtrToValuePtr<void>(Comp);
        UScriptStruct* SettingsStruct = SettingsStructProp->Struct;

        // Validate-before-mutate: pre-resolve every requested settings key against the
        // direct fields of FUnderwaterPostProcessSettings. A key that does not resolve
        // (e.g. a flat FPostProcessSettings field name like SceneColorTint/FogDensity,
        // which lives on the nested PostProcessSettings member — not directly on this
        // struct) used to be silently dropped with a clean ok:true / applied:[]. That
        // is the misleading-success defect class also fixed for ai.configure_slot_behavior
        // (droppedTags) and the GAS tag-write family — reject the whole call with
        // INVALID_PARAMS + a droppedSettings list instead of dropping part of the input.
        // Pass 1 resolves; pass 2 (below) applies and surfaces any apply error.
        // File-local helper so the {key,error} drop-object + droppedSettings array +
        // INVALID_PARAMS SendError shape is defined once and shared by both reject sites.
        TArray<TSharedPtr<FJsonValue>> DroppedSettings;
        auto AddDrop = [&DroppedSettings](const FString& Key, const FString& Error)
        {
            TSharedPtr<FJsonObject> Drop = MakeShared<FJsonObject>();
            Drop->SetStringField(TEXT("key"), Key);
            Drop->SetStringField(TEXT("error"), Error);
            DroppedSettings.Add(MakeShared<FJsonValueObject>(Drop));
        };
        auto SendDropped = [&Ctx, &DroppedSettings](const FString& Message)
        {
            TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
            ErrData->SetArrayField(TEXT("droppedSettings"), DroppedSettings);
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, Message, ErrData);
        };

        struct FResolvedSetting { FProperty* Prop; FString Key; TSharedPtr<FJsonValue> Value; };
        TArray<FResolvedSetting> Resolved;
        for (const auto& Pair : (*SettingsPtr)->Values)
        {
            const FString KeyStr = EARGCompat::JsonKeyToString(Pair.Key);
            FProperty* InnerProp = FindPropertyCI(SettingsStruct, KeyStr);
            if (!InnerProp)
            {
                AddDrop(KeyStr,
                    TEXT("not a direct field of FUnderwaterPostProcessSettings (only bEnabled/Priority/BlendRadius/BlendWeight); FPostProcessSettings knobs (SceneColorTint/FogDensity/etc.) must be nested under a PostProcessSettings object"));
                continue;
            }
            Resolved.Add({ InnerProp, KeyStr, Pair.Value });
        }

        if (DroppedSettings.Num() > 0)
        {
            SendDropped(FString::Printf(TEXT("%d settings key(s) do not resolve to a direct field of FUnderwaterPostProcessSettings and would be dropped; nest FPostProcessSettings color/tint/fog knobs under a 'PostProcessSettings' object. See droppedSettings."), DroppedSettings.Num()));
            return true;
        }

        // Pass 2: all keys resolved — apply them. An apply failure now surfaces the
        // previously-discarded ApplyErr as a droppedSettings {key,error} entry +
        // INVALID_PARAMS rather than evaporating with a clean success. Collect every
        // apply failure (matching pass 1's collect-then-reject contract) so a multi-key
        // failure reports all bad keys in one round-trip rather than only the first.
        for (const FResolvedSetting& Entry : Resolved)
        {
            FString ApplyErr;
            if (ApplyJsonValueToProperty(SettingsContainer, Entry.Prop, Entry.Value, ApplyErr))
            {
                Applied.Add(FString::Printf(TEXT("settings.%s"), *Entry.Key));
            }
            else
            {
                AddDrop(Entry.Key, ApplyErr.IsEmpty() ? TEXT("failed to apply value") : ApplyErr);
            }
        }

        if (DroppedSettings.Num() > 0)
        {
            SendDropped(FString::Printf(TEXT("%d settings key(s) could not be applied; see droppedSettings."), DroppedSettings.Num()));
            return true;
        }
    }

    Comp->MarkRenderStateDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& F : Applied) AppliedJson.Add(MakeShared<FJsonValueString>(F));
    Resp->SetArrayField(TEXT("applied"), AppliedJson);
    AddActorVerification(Resp, Body);
    Ctx.SendSuccess(Resp);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_WATER_PLUGIN_NOT_AVAILABLE,
        TEXT("Water plugin headers not compiled into PinWright. Enable the Water plugin and rebuild."));
    return true;
#endif
}

// ============================================================================
// River spline metadata — the AUTHORITATIVE width/depth setters
// ============================================================================
//
// A river's width and depth live in UWaterSplineMetadata (RiverWidth / Depth), and the
// spline's point Scale is DERIVED from them: UWaterSplineComponent::SynchronizeWaterProperties
// assigns Scale.X = RiverWidth and Scale.Y = Depth (WaterSplineComponent.cpp:231-246),
// and PostLoad calls it (:26-39). Writing the Scale curve instead — which
// spline.set_spline_point_scale used to allow — produced a value that read back, saved,
// and reverted on the next level load. spline.set_spline_point_scale now refuses on a
// water spline with DERIVED_PROPERTY and names these verbs.
//
// UWaterBodyRiverComponent::SetRiverWidthAtSplineInputKey / SetRiverDepthAtSplineInputKey
// (WaterBodyRiverComponent.h:51,54, WATER_API) write the metadata and nothing else — the
// spline Scale and the generated river mesh stay stale until something synchronizes. That
// is what K2_SynchronizeAndBroadcastDataChange (WaterSplineComponent.h:62) is for: it
// runs SynchronizeWaterProperties and broadcasts WaterSplineDataChangedEvent, which
// UWaterBodyComponent::OnWaterSplineDataChanged (WaterBodyComponent.cpp:1382, bound at
// :1453) turns into a body rebuild.
//
// ENGINE VERSION.
// All four {Get,Set}River{Width,Depth}AtSplineInputKey accessors are 5.6+
// (WaterBodyRiverComponent.h:45,48,51,54); they are absent on 5.3-5.5. MCP_HAS_WATER does
// NOT cover this — it gates on the Water plugin being present, not on engine version. The
// pre-5.6 path in WriteRiverCurveAtPoint / ReadRiverCurveAtPoint below reproduces the 5.6
// accessor bodies (WaterBodyRiverComponent.cpp:379-405) against
// UWaterSplineMetadata::RiverWidth / Depth, which are public UPROPERTY FInterpCurveFloat on
// 5.5 (WaterSplineMetadata.h:81,89) and are reached through the public
// UWaterBodyComponent::GetWaterSplineMetadata() (WaterBodyComponent.h:271). Everything
// after the write — SynchronizeWaterProperties and the change broadcast — is unversioned.

#if MCP_HAS_WATER
namespace
{
    // Both river-metadata verbs are the same verb with a different curve, so the whole
    // resolve / validate / write / synchronize / measure sequence lives here once and
    // the two handlers differ only in which setter+getter pair they pass in.
    enum class ERiverMetadataCurve : uint8
    {
        Width,
        Depth
    };

    // Resolve the river component, or send the error explaining which body types have
    // this curve at all. Returns nullptr after sending.
    UWaterBodyRiverComponent* ResolveRiverComponentOrError(
        FHandlerContext& Ctx, AWaterBody* Body, const FString& ActorRef)
    {
        UWaterBodyComponent* Comp = Body ? Body->GetWaterBodyComponent() : nullptr;
        if (!Comp)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_WATER_BODY_COMPONENT,
                FString::Printf(TEXT("Actor '%s' has no UWaterBodyComponent"), *ActorRef));
            return nullptr;
        }

        UWaterBodyRiverComponent* RiverComp = Cast<UWaterBodyRiverComponent>(Comp);
        if (!RiverComp)
        {
            // Not a fake success and not a silent clamp: RiverWidth/Depth are only
            // reachable through UWaterBodyRiverComponent's accessors, so a lake or ocean
            // genuinely has no supported route here.
            Ctx.SendError(ErrorCodes::ERR_INVALID_WATER_BODY_TYPE,
                FString::Printf(
                    TEXT("Actor '%s' is a %s, not an AWaterBodyRiver. River width and depth "
                         "are only settable on river bodies (UWaterSplineMetadata::CanEditRiverWidth "
                         "is river-only, WaterSplineMetadata.cpp:21-24)."),
                    *ActorRef, *Body->GetClass()->GetName()));
            return nullptr;
        }
        return RiverComp;
    }

    // Write one curve value at one spline point. Metadata only — the caller synchronizes
    // once for the whole batch.
    void WriteRiverCurveAtPoint(
        UWaterBodyRiverComponent* RiverComp, ERiverMetadataCurve Curve, float InputKey, float Value)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        if (Curve == ERiverMetadataCurve::Width)
        {
            RiverComp->SetRiverWidthAtSplineInputKey(InputKey, Value);
        }
        else
        {
            RiverComp->SetRiverDepthAtSplineInputKey(InputKey, Value);
        }
#else
        UWaterSplineMetadata* Metadata = RiverComp->GetWaterSplineMetadata();
        if (!Metadata)
        {
            return;
        }
        FInterpCurveFloat& CurveRef =
            (Curve == ERiverMetadataCurve::Width) ? Metadata->RiverWidth : Metadata->Depth;
        const int32 PointIndexForKey = CurveRef.GetPointIndexForInputValue(InputKey);
        if (CurveRef.Points.IsValidIndex(PointIndexForKey))
        {
            CurveRef.Points[PointIndexForKey].OutVal = Value;
        }
#endif
    }

    float ReadRiverCurveAtPoint(
        UWaterBodyRiverComponent* RiverComp, ERiverMetadataCurve Curve, float InputKey)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        return (Curve == ERiverMetadataCurve::Width)
            ? RiverComp->GetRiverWidthAtSplineInputKey(InputKey)
            : RiverComp->GetRiverDepthAtSplineInputKey(InputKey);
#else
        const UWaterSplineMetadata* Metadata = RiverComp->GetWaterSplineMetadata();
        if (!Metadata)
        {
            return 0.0f;
        }
        return (Curve == ERiverMetadataCurve::Width)
            ? Metadata->RiverWidth.Eval(InputKey, 0.f)
            : Metadata->Depth.Eval(InputKey, 0.f);
#endif
    }

    // The derived component of the spline point scale that this curve feeds:
    // Scale.X for width, Scale.Y for depth (WaterSplineComponent.cpp:231-244).
    double ReadDerivedScaleComponent(
        const UWaterSplineComponent* Spline, ERiverMetadataCurve Curve, int32 PointIndex)
    {
        const FVector Scale = Spline->GetScaleAtSplinePoint(PointIndex);
        return (Curve == ERiverMetadataCurve::Width) ? Scale.X : Scale.Y;
    }

    // Shared body for water.set_river_width_at_spline_point / set_river_depth_at_spline_point.
    bool HandleSetRiverCurve(
        FHandlerContext& Ctx, ERiverMetadataCurve Curve, const TCHAR* ValueParamName)
    {
        FString ActorRef;
        if (!Ctx.RequireString(TEXT("actor"), ActorRef)) return true;

        double Value = 0.0;
        if (!Ctx.RequireNumber(ValueParamName, Value)) return true;
        if (!(Value > 0.0))
        {
            // No safe default and no useful clamp: SynchronizeWaterProperties floors the
            // value at KINDA_SMALL_NUMBER (WaterSplineComponent.cpp:235), so a
            // zero/negative request would silently become a degenerate 1e-4 river.
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("%s must be greater than 0 (received %f). The engine floors "
                                     "the stored value at KINDA_SMALL_NUMBER, so a non-positive "
                                     "request would produce a degenerate river rather than the "
                                     "value asked for."), ValueParamName, Value));
            return true;
        }

        AWaterBody* Body = ResolveWaterBodyActor(Ctx, ActorRef);
        if (!Body) return true;

        UWaterBodyRiverComponent* RiverComp = ResolveRiverComponentOrError(Ctx, Body, ActorRef);
        if (!RiverComp) return true;

        UWaterSplineComponent* Spline = Body->GetWaterSpline();
        if (!Spline)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_SPLINE,
                FString::Printf(TEXT("Actor '%s' has no water spline component"), *ActorRef));
            return true;
        }

        const int32 NumPoints = Spline->GetNumberOfSplinePoints();
        if (NumPoints <= 0)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_SPLINE,
                FString::Printf(TEXT("Water spline on '%s' has no points"), *ActorRef));
            return true;
        }

        // Exactly one target selector. Neither has a safe default: silently defaulting to
        // point 0 would edit one point of a river the caller meant to widen end to end,
        // and defaulting to all points would rewrite a taper the caller was preserving.
        const bool bAllPoints = Ctx.GetBool(TEXT("allPoints"), false);
        const TOptional<int32> RequestedIndex = Ctx.GetIntFirstOf({ TEXT("pointIndex") });
        if (bAllPoints == RequestedIndex.IsSet())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                TEXT("Provide exactly one of pointIndex (a single spline point) or "
                     "allPoints:true (every point on the spline)."));
            return true;
        }

        TArray<int32> TargetPoints;
        if (bAllPoints)
        {
            TargetPoints.Reserve(NumPoints);
            for (int32 Index = 0; Index < NumPoints; ++Index)
            {
                TargetPoints.Add(Index);
            }
        }
        else
        {
            const int32 Index = RequestedIndex.GetValue();
            if (Index < 0 || Index >= NumPoints)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX,
                    FString::Printf(TEXT("pointIndex %d is out of range; the spline has %d points"),
                        Index, NumPoints));
                return true;
            }
            TargetPoints.Add(Index);
        }

        PinWright::MarkLevelActorModified(Body, Spline);

        TArray<float> InputKeys;
        InputKeys.Reserve(TargetPoints.Num());
        for (int32 PointIndex : TargetPoints)
        {
            const float InputKey = Spline->GetInputKeyValueAtSplinePoint(PointIndex);
            InputKeys.Add(InputKey);
            WriteRiverCurveAtPoint(RiverComp, Curve, InputKey, static_cast<float>(Value));
        }

        // Metadata is now the new truth; this re-derives the spline scale from it and
        // rebuilds the body. Without it the write is real but invisible until reload.
        Spline->K2_SynchronizeAndBroadcastDataChange();

        // Verdict from a re-measurement, not from the write. derivedScale is written by
        // the engine's SynchronizeWaterProperties, not by this handler, so it is the
        // half of the check the write path cannot fake (docs/rpc-design.md §4).
        const double Tolerance = 0.01;
        int32 AppliedCount = 0;
        TArray<TSharedPtr<FJsonValue>> PointsJson;
        for (int32 Slot = 0; Slot < TargetPoints.Num(); ++Slot)
        {
            const int32 PointIndex = TargetPoints[Slot];
            const double StoredValue = ReadRiverCurveAtPoint(RiverComp, Curve, InputKeys[Slot]);
            const double DerivedScale = ReadDerivedScaleComponent(Spline, Curve, PointIndex);
            const bool bPointApplied =
                FMath::IsNearlyEqual(StoredValue, Value, Tolerance) &&
                FMath::IsNearlyEqual(DerivedScale, StoredValue, Tolerance);
            if (bPointApplied)
            {
                ++AppliedCount;
            }

            TSharedPtr<FJsonObject> PointJson = MakeShared<FJsonObject>();
            PointJson->SetNumberField(TEXT("pointIndex"), PointIndex);
            PointJson->SetNumberField(TEXT("inputKey"), InputKeys[Slot]);
            PointJson->SetNumberField(TEXT("requested"), Value);
            PointJson->SetNumberField(TEXT("stored"), StoredValue);
            PointJson->SetNumberField(TEXT("derivedScale"), DerivedScale);
            PointJson->SetBoolField(TEXT("applied"), bPointApplied);
            PointsJson.Add(MakeShared<FJsonValueObject>(PointJson));
        }

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("curve"),
            Curve == ERiverMetadataCurve::Width ? TEXT("RiverWidth") : TEXT("Depth"));
        Resp->SetNumberField(TEXT("pointsTargeted"), TargetPoints.Num());
        Resp->SetNumberField(TEXT("pointsApplied"), AppliedCount);
        Resp->SetBoolField(TEXT("applied"), AppliedCount == TargetPoints.Num());
        Resp->SetArrayField(TEXT("points"), PointsJson);
        // Name what was written and what the engine derives from it. Deliberately NOT a
        // survivesReload:true boolean — this handler did not reload anything, and a
        // literal under a measurement name is the defect class this verb exists to close
        // (docs/rpc-design.md §1).
        Resp->SetStringField(TEXT("authoritativeState"),
            Curve == ERiverMetadataCurve::Width
                ? TEXT("UWaterSplineMetadata::RiverWidth")
                : TEXT("UWaterSplineMetadata::Depth"));
        Resp->SetStringField(TEXT("derivedProperty"),
            Curve == ERiverMetadataCurve::Width
                ? TEXT("spline point Scale.X")
                : TEXT("spline point Scale.Y"));
        AddActorVerification(Resp, Body);

        if (AppliedCount != TargetPoints.Num())
        {
            Ctx.SendError(ErrorCodes::ERR_APPLY_FAILED,
                FString::Printf(
                    TEXT("%d of %d spline points did not take the requested value; see points[] "
                         "for the stored metadata value and the scale the engine derived from it."),
                    TargetPoints.Num() - AppliedCount, TargetPoints.Num()),
                Resp);
            return true;
        }

        Ctx.SendSuccess(Resp);
        return true;
    }
}
#endif

// ---- water.set_river_width_at_spline_point ----
REGISTER_RPC_HANDLER("water.set_river_width_at_spline_point", "water",
    "Set a river's width at one spline point (pointIndex) or at every point (allPoints), "
    "writing the authoritative UWaterSplineMetadata::RiverWidth curve. This is the setter "
    "to use - the spline point's Scale.X is DERIVED from this value and re-derived on "
    "every load, so writing Scale.X instead reads back, saves, and then reverts. Reports "
    "the stored metadata value and the scale the engine derived from it per point, so a "
    "write that did not take cannot report success.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actor",      "string",  "Actor label, name, or path of the AWaterBodyRiver"),
        RPC_PARAM_REQ("width",      "number",  "River width in world units (must be > 0)"),
        RPC_PARAM_OPT("pointIndex", "integer",  "Spline point to set; mutually exclusive with allPoints"),
        RPC_PARAM_OPT("allPoints",  "boolean", "Set every point on the spline; mutually exclusive with pointIndex")
    ))
{
#if MCP_HAS_WATER
    return HandleSetRiverCurve(Ctx, ERiverMetadataCurve::Width, TEXT("width"));
#else
    Ctx.SendError(ErrorCodes::ERR_WATER_PLUGIN_NOT_AVAILABLE,
        TEXT("Water plugin headers not compiled into PinWright. Enable the Water plugin and rebuild."));
    return true;
#endif
}

// ---- water.set_river_depth_at_spline_point ----
REGISTER_RPC_HANDLER("water.set_river_depth_at_spline_point", "water",
    "Set a river's depth at one spline point (pointIndex) or at every point (allPoints), "
    "writing the authoritative UWaterSplineMetadata::Depth curve. The spline point's "
    "Scale.Y is DERIVED from this value and re-derived on every load, so writing Scale.Y "
    "instead reads back, saves, and then reverts. River bodies only: the depth accessors "
    "live on UWaterBodyRiverComponent.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actor",      "string",  "Actor label, name, or path of the AWaterBodyRiver"),
        RPC_PARAM_REQ("depth",      "number",  "River depth in world units (must be > 0)"),
        RPC_PARAM_OPT("pointIndex", "integer",  "Spline point to set; mutually exclusive with allPoints"),
        RPC_PARAM_OPT("allPoints",  "boolean", "Set every point on the spline; mutually exclusive with pointIndex")
    ))
{
#if MCP_HAS_WATER
    return HandleSetRiverCurve(Ctx, ERiverMetadataCurve::Depth, TEXT("depth"));
#else
    Ctx.SendError(ErrorCodes::ERR_WATER_PLUGIN_NOT_AVAILABLE,
        TEXT("Water plugin headers not compiled into PinWright. Enable the Water plugin and rebuild."));
    return true;
#endif
}
