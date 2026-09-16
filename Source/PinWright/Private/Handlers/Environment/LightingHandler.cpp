// Copyright (c) 2026 Alexander Penkin. MIT License.

// LightingHandler.cpp - Migrated from PinWright_LightingHandlers.cpp
// Light spawning, sky lights, lighting builds, fog, GI, shadows, exposure, and AO handlers

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Level/LevelBuildBinds.h"
#include "Handlers/Level/MapSwapGuardRefusal.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "UObject/UObjectIterator.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/TextureCube.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Lightmass/LightmassImportanceVolume.h"

#include "Editor/UnrealEd/Public/Editor.h"
#include "FileHelpers.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Handlers/Environment/GIMethodCVarHelper.h"
#include "Handlers/Environment/PostProcessVolumeUtils.h"
#include "Handlers/Volume/VolumeBrushGeometry.h"
#include "Kismet/GameplayStatics.h"
#include "LevelEditor.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Utils/JsonBuilders.h"

using PinWright::FindOrSpawnUnboundPPV;

// ---- lighting.list_light_types ----
REGISTER_RPC_HANDLER("lighting.list_light_types", "lighting", "List all available light types",
    RPC_NO_PARAMS)
{
    TArray<TSharedPtr<FJsonValue>> Types;
    Types.Add(MakeShared<FJsonValueString>(TEXT("DirectionalLight")));
    Types.Add(MakeShared<FJsonValueString>(TEXT("PointLight")));
    Types.Add(MakeShared<FJsonValueString>(TEXT("SpotLight")));
    Types.Add(MakeShared<FJsonValueString>(TEXT("RectLight")));

    TSet<FString> AddedNames;
    AddedNames.Add(TEXT("DirectionalLight"));
    AddedNames.Add(TEXT("PointLight"));
    AddedNames.Add(TEXT("SpotLight"));
    AddedNames.Add(TEXT("RectLight"));

    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->IsChildOf(ALight::StaticClass()) &&
            !It->HasAnyClassFlags(CLASS_Abstract) &&
            !AddedNames.Contains(It->GetName()))
        {
            Types.Add(MakeShared<FJsonValueString>(It->GetName()));
            AddedNames.Add(It->GetName());
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("types"), Types);
    Resp->SetNumberField(TEXT("count"), Types.Num());
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.spawn_light ----
REGISTER_RPC_HANDLER("lighting.spawn_light", "lighting",
    "Spawn a light actor (point, directional, spot, rect, sky, or custom). "
    "`shadowsEnabled` is measured, not echoed: it is true only when the component's CastShadows is "
    "set AND the r.ShadowQuality cvar is above 0, because that cvar force-clears the DynamicShadows "
    "show flag and no read-back on the light records the veto. `castShadows` reports the component "
    "flag, `shadowQualityCVar` the measured cvar, and `cvarWarning` names the remedy when no dynamic "
    "shadow can render. Does not change the cvar.",
    RPC_PARAMS(
        RPC_PARAM_OPT("lightClass", "classref", "Unreal class name of the light (e.g. PointLight)"),
        RPC_PARAM_OPT("lightType", "string", "Short name: point, directional, spot, rect, sky"),
        RPC_PARAM_OPT("name", "string", "Label for the spawned light actor"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("properties", "object", "Light properties: intensity, color, castShadows, attenuationRadius, etc.")
    ))
{
    UEditorActorSubsystem* ActorSS = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    if (!ActorSS)
    {
        Ctx.SendError(TEXT("EDITOR_ACTOR_SUBSYSTEM_MISSING"), TEXT("EditorActorSubsystem not available"));
        return true;
    }

    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!EditorWorld)
    {
        EditorWorld = ActorSS->GetWorld();
    }
    if (!IsValid(EditorWorld))
    {
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"), TEXT("Editor world not available"));
        return true;
    }

    auto* Payload = Ctx.GetRawPayload().Get();

    FString LightClassStr;
    if (!Payload->TryGetStringField(TEXT("lightClass"), LightClassStr) || LightClassStr.IsEmpty())
    {
        FString LightType;
        if (Payload->TryGetStringField(TEXT("lightType"), LightType) && !LightType.IsEmpty())
        {
            const FString LowerType = LightType.ToLower();
            if (LowerType == TEXT("point") || LowerType == TEXT("pointlight"))
                LightClassStr = TEXT("PointLight");
            else if (LowerType == TEXT("directional") || LowerType == TEXT("directionallight"))
                LightClassStr = TEXT("DirectionalLight");
            else if (LowerType == TEXT("spot") || LowerType == TEXT("spotlight"))
                LightClassStr = TEXT("SpotLight");
            else if (LowerType == TEXT("rect") || LowerType == TEXT("rectlight"))
                LightClassStr = TEXT("RectLight");
            else if (LowerType == TEXT("sky") || LowerType == TEXT("skylight"))
                LightClassStr = TEXT("SkyLight");
            else
            {
                Ctx.SendError(TEXT("INVALID_LIGHT_TYPE"),
                    FString::Printf(TEXT("Invalid lightType: %s. Must be one of: point, directional, spot, rect, sky"), *LightType));
                return true;
            }
        }
    }

    if (LightClassStr.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("lightClass or lightType required"));
        return true;
    }

    UClass* LightClass = nullptr;
    const FString LowerClassStr = LightClassStr.ToLower();

    if (LowerClassStr == TEXT("pointlight") || LowerClassStr == TEXT("point"))
        LightClass = APointLight::StaticClass();
    else if (LowerClassStr == TEXT("directionallight") || LowerClassStr == TEXT("directional"))
        LightClass = ADirectionalLight::StaticClass();
    else if (LowerClassStr == TEXT("spotlight") || LowerClassStr == TEXT("spot"))
        LightClass = ASpotLight::StaticClass();
    else if (LowerClassStr == TEXT("rectlight") || LowerClassStr == TEXT("rect"))
        LightClass = ARectLight::StaticClass();
    else if (LowerClassStr == TEXT("skylight") || LowerClassStr == TEXT("sky"))
        LightClass = ASkyLight::StaticClass();
    else
    {
        LightClass = ResolveUClass(LightClassStr);
        if (!LightClass)
            LightClass = ResolveUClass(TEXT("A") + LightClassStr);
    }

    if (!LightClass || !LightClass->IsChildOf(ALight::StaticClass()))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("Invalid light class: %s"), *LightClassStr));
        return true;
    }

    UE_LOG(LogPinWrightSubsystem, Log,
        TEXT("spawn_light: Resolved lightClass '%s' to %s (path: %s)"),
        *LightClassStr, *LightClass->GetName(), *LightClass->GetPathName());

    FVector Location = FVector(0.0f, 0.0f, 300.0f);
    const TSharedPtr<FJsonObject>* LocPtr;
    bool bHasExplicitLocation = Payload->TryGetObjectField(TEXT("location"), LocPtr);
    if (bHasExplicitLocation)
    {
        Location.X = GetJsonNumberField((*LocPtr), TEXT("x"));
        Location.Y = GetJsonNumberField((*LocPtr), TEXT("y"));
        Location.Z = GetJsonNumberField((*LocPtr), TEXT("z"));
    }
    else
    {
        UE_LOG(LogPinWrightSubsystem, Log,
            TEXT("spawn_light: No location provided, using default (0, 0, 300)"));
    }

    FRotator Rotation = FRotator::ZeroRotator;
    const TSharedPtr<FJsonObject>* RotPtr;
    if (Payload->TryGetObjectField(TEXT("rotation"), RotPtr))
    {
        Rotation.Pitch = GetJsonNumberField((*RotPtr), TEXT("pitch"));
        Rotation.Yaw = GetJsonNumberField((*RotPtr), TEXT("yaw"));
        Rotation.Roll = GetJsonNumberField((*RotPtr), TEXT("roll"));
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AActor* NewLight = EditorWorld->SpawnActor(LightClass, &Location, &Rotation, SpawnParams);

    if (NewLight)
    {
        PinWright::MarkLevelActorSpawned(NewLight);
        NewLight->SetActorLabel(LightClassStr);
        NewLight->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
    }

    if (!NewLight)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn light actor"));
        return true;
    }

    FString Name = Ctx.GetString(TEXT("name"));
    if (!Name.IsEmpty())
        NewLight->SetActorLabel(Name);

    // Default to Movable for immediate feedback
    if (ULightComponent* BaseLightComp = NewLight->FindComponentByClass<ULightComponent>())
        BaseLightComp->SetMobility(EComponentMobility::Movable);

    // Apply properties with validation
    const TSharedPtr<FJsonObject>* Props;
    if (Payload->TryGetObjectField(TEXT("properties"), Props))
    {
        ULightComponent* LightComp = NewLight->FindComponentByClass<ULightComponent>();
        if (LightComp)
        {
            double Intensity;
            if ((*Props)->TryGetNumberField(TEXT("intensity"), Intensity))
            {
                if (!FMath::IsFinite(Intensity))
                {
                    UE_LOG(LogPinWrightSubsystem, Warning,
                        TEXT("spawn_light: Invalid intensity (not finite), using 0"));
                    Intensity = 0.0;
                }
                else if (Intensity < 0.0)
                {
                    UE_LOG(LogPinWrightSubsystem, Warning,
                        TEXT("spawn_light: Negative intensity %.2f clamped to 0"), Intensity);
                    Intensity = 0.0;
                }
                LightComp->SetIntensity((float)Intensity);
            }

            const TSharedPtr<FJsonObject>* ColorObj;
            if ((*Props)->TryGetObjectField(TEXT("color"), ColorObj))
            {
                FLinearColor Color;
                Color.R = GetJsonNumberField((*ColorObj), TEXT("r"));
                Color.G = GetJsonNumberField((*ColorObj), TEXT("g"));
                Color.B = GetJsonNumberField((*ColorObj), TEXT("b"));
                Color.A = (*ColorObj)->HasField(TEXT("a")) ? GetJsonNumberField((*ColorObj), TEXT("a")) : 1.0f;
                if (!FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G) ||
                    !FMath::IsFinite(Color.B) || !FMath::IsFinite(Color.A))
                {
                    UE_LOG(LogPinWrightSubsystem, Warning,
                        TEXT("spawn_light: Invalid color components, using white"));
                    Color = FLinearColor::White;
                }
                LightComp->SetLightColor(Color);
            }

            bool bCastShadows;
            if ((*Props)->TryGetBoolField(TEXT("castShadows"), bCastShadows))
                LightComp->SetCastShadows(bCastShadows);

            if (UDirectionalLightComponent* DirComp = Cast<UDirectionalLightComponent>(LightComp))
            {
                bool bUseSun = true;
                if ((*Props)->TryGetBoolField(TEXT("useAsAtmosphereSunLight"), bUseSun))
                    DirComp->SetAtmosphereSunLight(bUseSun);
                else
                    DirComp->SetAtmosphereSunLight(true);
            }

            if (UPointLightComponent* PointComp = Cast<UPointLightComponent>(LightComp))
            {
                double Radius;
                if ((*Props)->TryGetNumberField(TEXT("attenuationRadius"), Radius))
                {
                    if (!FMath::IsFinite(Radius) || Radius <= 0.0)
                    {
                        UE_LOG(LogPinWrightSubsystem, Warning,
                            TEXT("spawn_light: Invalid attenuationRadius %.2f, using 1000"), Radius);
                        Radius = 1000.0;
                    }
                    PointComp->SetAttenuationRadius((float)Radius);
                }
            }

            if (USpotLightComponent* SpotComp = Cast<USpotLightComponent>(LightComp))
            {
                double InnerCone;
                if ((*Props)->TryGetNumberField(TEXT("innerConeAngle"), InnerCone))
                {
                    if (!FMath::IsFinite(InnerCone) || InnerCone < 0.0 || InnerCone > 180.0)
                    {
                        UE_LOG(LogPinWrightSubsystem, Warning,
                            TEXT("spawn_light: Invalid innerConeAngle %.2f, clamping to 0-180"), InnerCone);
                        InnerCone = FMath::Clamp(InnerCone, 0.0, 180.0);
                    }
                    SpotComp->SetInnerConeAngle((float)InnerCone);
                }
                double OuterCone;
                if ((*Props)->TryGetNumberField(TEXT("outerConeAngle"), OuterCone))
                {
                    if (!FMath::IsFinite(OuterCone) || OuterCone < 0.0 || OuterCone > 180.0)
                    {
                        UE_LOG(LogPinWrightSubsystem, Warning,
                            TEXT("spawn_light: Invalid outerConeAngle %.2f, clamping to 0-180"), OuterCone);
                        OuterCone = FMath::Clamp(OuterCone, 0.0, 180.0);
                    }
                    SpotComp->SetOuterConeAngle((float)OuterCone);
                }
            }

            if (URectLightComponent* RectComp = Cast<URectLightComponent>(LightComp))
            {
                double Width;
                if ((*Props)->TryGetNumberField(TEXT("sourceWidth"), Width))
                {
                    if (!FMath::IsFinite(Width) || Width <= 0.0)
                    {
                        UE_LOG(LogPinWrightSubsystem, Warning,
                            TEXT("spawn_light: Invalid sourceWidth %.2f, using 100"), Width);
                        Width = 100.0;
                    }
                    RectComp->SetSourceWidth((float)Width);
                }
                double Height;
                if ((*Props)->TryGetNumberField(TEXT("sourceHeight"), Height))
                {
                    if (!FMath::IsFinite(Height) || Height <= 0.0)
                    {
                        UE_LOG(LogPinWrightSubsystem, Warning,
                            TEXT("spawn_light: Invalid sourceHeight %.2f, using 100"), Height);
                        Height = 100.0;
                    }
                    RectComp->SetSourceHeight((float)Height);
                }
            }
        }
    }

    // r.ShadowQuality vetoes every dynamic shadow in the frame regardless of the
    // component: at <= 0 EngineShowFlagOverride clears the DynamicShadows show flag
    // wholesale (ShowFlags.cpp:490-496), which sits above any per-light test, so
    // CastShadows is never consulted. The component still reads back true — property.get
    // CastShadows answers the same either way — so nothing on the light records the veto.
    // BaseScalability.ini zeroes it in [ShadowQuality@0] and restores it at @1 and above,
    // so a low-shadow-quality editor casts no dynamic shadows at all while the write here
    // still succeeds. `castShadows` reports what is on the component, `shadowsEnabled`
    // what will render; reporting them apart is the point — a caller that sees
    // shadowsEnabled:false with a cvarWarning knows to fix the renderer, not the light.
    int32 ShadowQualityValue = 0;
    bool bShadowQualityFound = false;
    if (const IConsoleVariable* ShadowQualityCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShadowQuality")))
    {
        ShadowQualityValue = ShadowQualityCVar->GetInt();
        bShadowQualityFound = true;
    }

    // Read back off the component, never echoed from the request: castShadows is optional
    // and a fresh light component defaults it to true, so the unrequested case still has a
    // shadow state worth reporting.
    ULightComponent* SpawnedLightComp = NewLight->FindComponentByClass<ULightComponent>();
    const bool bComponentCastShadows = SpawnedLightComp && SpawnedLightComp->CastShadows != 0;
    // The engine's own test is <= 0, not == 0.
    const bool bShadowsVetoed = bShadowQualityFound && ShadowQualityValue <= 0;

    TSharedPtr<FJsonObject> ShadowCVarInfo = MakeShared<FJsonObject>();
    ShadowCVarInfo->SetStringField(TEXT("cvar"), TEXT("r.ShadowQuality"));
    ShadowCVarInfo->SetBoolField(TEXT("found"), bShadowQualityFound);
    // Absent, not 0, when the registry did not carry it: an unmeasured value must not be
    // readable as a measured one.
    if (bShadowQualityFound)
    {
        ShadowCVarInfo->SetNumberField(TEXT("value"), ShadowQualityValue);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorName"), NewLight->GetActorLabel());
    Resp->SetBoolField(TEXT("castShadows"), bComponentCastShadows);
    Resp->SetBoolField(TEXT("shadowsEnabled"), bComponentCastShadows && !bShadowsVetoed);
    Resp->SetObjectField(TEXT("shadowQualityCVar"), ShadowCVarInfo);

    // Gated on the component actually casting shadows: a light with CastShadows off under a
    // zeroed cvar has nothing misreported to warn about.
    if (bShadowsVetoed && bComponentCastShadows)
    {
        Resp->SetStringField(TEXT("cvarWarning"),
            TEXT("CastShadows is set on this light, but r.ShadowQuality is 0, so the ")
            TEXT("DynamicShadows show flag is force-cleared and NO dynamic shadow renders ")
            TEXT("anywhere in the frame: this light's shadow settings — ShadowBias, ")
            TEXT("ShadowSlopeBias, ShadowSharpen, ContactShadowLength, CastVolumetricShadow ")
            TEXT("and every cascade setting on a directional light — are all inert. A capture ")
            TEXT("taken now measures the wrong renderer. Turn shadows on for this session with ")
            TEXT("system.console_command \"r.ShadowQuality 3\" (a scalability group can zero it ")
            TEXT("again — BaseScalability.ini sets it to 0 in ShadowQuality@0, so ")
            TEXT("sg.ShadowQuality 1 or higher also lifts it), or pin it durably with ")
            TEXT("r.ShadowQuality=3 under [SystemSettings] in the project's DefaultEngine.ini."));
    }
    else if (!bShadowQualityFound)
    {
        Resp->SetStringField(TEXT("cvarWarning"),
            TEXT("r.ShadowQuality is not in this host's console registry, so whether dynamic ")
            TEXT("shadows can render was not measured. shadowsEnabled here reports only the ")
            TEXT("component flag."));
    }

    AddActorVerification(Resp, NewLight);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.spawn_sky_light ----
REGISTER_RPC_HANDLER("lighting.spawn_sky_light", "lighting",
    "Spawn a sky light actor. The r.SkylightIntensityMultiplier cvar RESCALES rather than vetoes: "
    "the sky light's scene proxy multiplies the written intensity by it before the renderer sees "
    "it, and the component still reads back the unscaled value. `intensity` is the component "
    "read-back, `effectiveIntensity` the measured product, `skylightIntensityMultiplierCVar` the "
    "measured cvar, and `cvarWarning` names the remedy when the two differ. Does not change the "
    "cvar.",
    RPC_PARAMS(
        RPC_PARAM_OPT("name", "string", "Label for the sky light"),
        RPC_PARAM_OPT("location", "object", "Location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("sourceType", "string", "CapturedScene or SpecifiedCubemap"),
        RPC_PARAM_OPT("cubemapPath", "path", "Asset path for cubemap texture"),
        RPC_PARAM_OPT("intensity", "number", "Sky light intensity"),
        RPC_PARAM_OPT("recapture", "boolean", "Recapture sky after spawning")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();

    FVector Location = FVector(0.0f, 0.0f, 500.0f);
    const TSharedPtr<FJsonObject>* LocPtr;
    bool bHasExplicitLocation = Payload->TryGetObjectField(TEXT("location"), LocPtr);
    if (bHasExplicitLocation)
    {
        Location.X = GetJsonNumberField((*LocPtr), TEXT("x"));
        Location.Y = GetJsonNumberField((*LocPtr), TEXT("y"));
        Location.Z = GetJsonNumberField((*LocPtr), TEXT("z"));
    }
    else
    {
        UE_LOG(LogPinWrightSubsystem, Log,
            TEXT("spawn_sky_light: No location provided, using default (0, 0, 500)"));
    }

    FRotator Rotation = FRotator::ZeroRotator;
    const TSharedPtr<FJsonObject>* RotPtr;
    if (Payload->TryGetObjectField(TEXT("rotation"), RotPtr))
    {
        Rotation.Pitch = GetJsonNumberField((*RotPtr), TEXT("pitch"));
        Rotation.Yaw = GetJsonNumberField((*RotPtr), TEXT("yaw"));
        Rotation.Roll = GetJsonNumberField((*RotPtr), TEXT("roll"));
    }

    AActor* SkyLight = SpawnActorInActiveWorld<AActor>(ASkyLight::StaticClass(), Location, Rotation);
    if (!SkyLight)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn SkyLight"));
        return true;
    }

    PinWright::MarkLevelActorSpawned(SkyLight);

    FString Name = Ctx.GetString(TEXT("name"));
    if (!Name.IsEmpty())
        SkyLight->SetActorLabel(Name);

    USkyLightComponent* SkyComp = SkyLight->FindComponentByClass<USkyLightComponent>();
    if (SkyComp)
    {
        SkyComp->Modify();

        FString SourceType = Ctx.GetString(TEXT("sourceType"));
        if (!SourceType.IsEmpty())
        {
            if (SourceType == TEXT("SpecifiedCubemap"))
            {
                SkyComp->SourceType = ESkyLightSourceType::SLS_SpecifiedCubemap;
                FString CubemapPath = Ctx.GetString(TEXT("cubemapPath"));
                if (!CubemapPath.IsEmpty())
                {
                    FString SanitizedCubemapPath = SanitizeProjectRelativePath(CubemapPath);
                    if (SanitizedCubemapPath.IsEmpty())
                    {
                        UE_LOG(LogPinWrightSubsystem, Warning,
                            TEXT("spawn_sky_light: Invalid cubemapPath rejected: %s"), *CubemapPath);
                    }
                    else
                    {
                        UTextureCube* Cubemap = Cast<UTextureCube>(
                            StaticLoadObject(UTextureCube::StaticClass(), nullptr, *SanitizedCubemapPath));
                        if (Cubemap)
                            SkyComp->Cubemap = Cubemap;
                    }
                }
            }
            else
            {
                SkyComp->SourceType = ESkyLightSourceType::SLS_CapturedScene;
            }
        }

        double Intensity;
        if (Payload->TryGetNumberField(TEXT("intensity"), Intensity))
            SkyComp->SetIntensity((float)Intensity);

        bool bRecapture;
        if (Payload->TryGetBoolField(TEXT("recapture"), bRecapture) && bRecapture)
            SkyComp->RecaptureSky();

        // SourceType / Cubemap above are raw field writes with no render-state push
        // (unlike SetIntensity, which runs UpdateLimitedRenderingStateFast itself).
        PinWright::MarkComponentRenderStateDirty(SkyComp);
    }

    // r.SkylightIntensityMultiplier does not veto the write, it RESCALES it — the shape a
    // caller is least likely to suspect, because the verb succeeds and the component reads
    // back exactly what was asked for. FSkyLightSceneProxy::GetEffectiveLightColor returns
    // LightColor * GSkylightIntensityMultiplier (SkyLightComponent.cpp:85-91, 231-234), so
    // the renderer never sees the written intensity; BaseScalability.ini sets the cvar to
    // 0.8 in [GlobalIlluminationQuality@0] and 1.0 at @1 and above, so a low-GI-quality
    // editor lights the scene at 80% of every intensity this verb reports. `intensity` is
    // the component read-back, `effectiveIntensity` what the proxy applies. The spawned
    // ASkyLight's component is Stationary (USkyLightComponent constructor,
    // SkyLightComponent.cpp:312), so the proxy — and therefore the scale — is in play.
    float SkylightMultiplier = 1.0f;
    bool bSkylightMultiplierFound = false;
    if (const IConsoleVariable* SkylightMultiplierCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkylightIntensityMultiplier")))
    {
        SkylightMultiplier = SkylightMultiplierCVar->GetFloat();
        bSkylightMultiplierFound = true;
    }
    const bool bRescaled =
        bSkylightMultiplierFound && !FMath::IsNearlyEqual(SkylightMultiplier, 1.0f);

    TSharedPtr<FJsonObject> SkylightCVarInfo = MakeShared<FJsonObject>();
    SkylightCVarInfo->SetStringField(TEXT("cvar"), TEXT("r.SkylightIntensityMultiplier"));
    SkylightCVarInfo->SetBoolField(TEXT("found"), bSkylightMultiplierFound);
    // Absent, not 1.0, when the registry did not carry it: an unmeasured value must not be
    // readable as a measured one.
    if (bSkylightMultiplierFound)
    {
        SkylightCVarInfo->SetNumberField(TEXT("value"), SkylightMultiplier);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorName"), SkyLight->GetActorLabel());
    if (SkyComp)
    {
        Resp->SetNumberField(TEXT("intensity"), SkyComp->Intensity);
        // Omitted rather than echoed unscaled when the cvar was not measured — an
        // unmeasured effective value would read as a measured one.
        if (bSkylightMultiplierFound)
        {
            Resp->SetNumberField(TEXT("effectiveIntensity"), SkyComp->Intensity * SkylightMultiplier);
        }
    }
    Resp->SetObjectField(TEXT("skylightIntensityMultiplierCVar"), SkylightCVarInfo);

    if (bRescaled)
    {
        Resp->SetStringField(TEXT("cvarWarning"), FString::Printf(
            TEXT("r.SkylightIntensityMultiplier is %.4g, not 1, so the intensity written to this ")
            TEXT("sky light is NOT the intensity that lights the scene: ")
            TEXT("FSkyLightSceneProxy::GetEffectiveLightColor scales the component's light colour ")
            TEXT("by it before the renderer sees it, and the component still reads back the ")
            TEXT("unscaled value. Tuning sky intensity against a capture taken now tunes against a ")
            TEXT("scaled renderer. Restore the scale for this session with system.console_command ")
            TEXT("\"r.SkylightIntensityMultiplier 1\" (a scalability group can change it again — ")
            TEXT("BaseScalability.ini sets it to 0.8 in GlobalIlluminationQuality@0, so ")
            TEXT("sg.GlobalIlluminationQuality 1 or higher also restores it), or pin it durably ")
            TEXT("with r.SkylightIntensityMultiplier=1 under [SystemSettings] in the project's ")
            TEXT("DefaultEngine.ini."),
            SkylightMultiplier));
    }
    else if (!bSkylightMultiplierFound)
    {
        Resp->SetStringField(TEXT("cvarWarning"),
            TEXT("r.SkylightIntensityMultiplier is not in this host's console registry, so whether ")
            TEXT("the written intensity is the effective one was not measured. `intensity` here ")
            TEXT("reports only the component value, and effectiveIntensity is omitted."));
    }

    AddActorVerification(Resp, SkyLight);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.build_lighting ----
REGISTER_RPC_HANDLER("lighting.build_lighting", "lighting", "Build/bake lighting for the current level",
    RPC_PARAMS(
        RPC_PARAM_OPT("quality", "string", "Quality: preview/0, medium/1, high/2, production/3")
    ))
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"), TEXT("Editor world not available"));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (AWorldSettings* WS = World->GetWorldSettings())
    {
        if (WS->bForceNoPrecomputedLighting)
        {
            Ctx.SendError(TEXT("OPERATION_SKIPPED"),
                TEXT("Lighting build skipped - precomputed lighting disabled in WorldSettings"));
            return true;
        }
    }

    FString Quality = Ctx.GetString(TEXT("quality"));
    FString QualityCmd = TEXT("Production");
    if (!Quality.IsEmpty())
    {
        const FString LowerQuality = Quality.ToLower();
        if (LowerQuality == TEXT("preview") || LowerQuality == TEXT("0"))
            QualityCmd = TEXT("Preview");
        else if (LowerQuality == TEXT("medium") || LowerQuality == TEXT("1"))
            QualityCmd = TEXT("Medium");
        else if (LowerQuality == TEXT("high") || LowerQuality == TEXT("2"))
            QualityCmd = TEXT("High");
        else if (LowerQuality == TEXT("production") || LowerQuality == TEXT("3"))
            QualityCmd = TEXT("Production");
        else
        {
            Ctx.SendError(TEXT("UNKNOWN_QUALITY"),
                FString::Printf(TEXT("Unknown lighting quality: %s. Valid: preview/0, medium/1, high/2, production/3"), *Quality));
            return true;
        }
    }

    FJobBindArgs Args;
    Args.Method = TEXT("lighting.build_lighting");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("quality"), QualityCmd);

    Args.BindNativeDelegate =
        [QualityCmd](FJobOnComplete OnComplete)
    {
        BindLightingBuildCompletion()(OnComplete);
        if (GEditor)
        {
            FString Command = FString::Printf(TEXT("BuildLighting %s"), *QualityCmd);
            GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command);
        }
    };
    Ctx.StartJob(Args);
    return true;
}

// ---- lighting.ensure_single_sky_light ----
REGISTER_RPC_HANDLER("lighting.ensure_single_sky_light", "lighting", "Ensure only one sky light exists, removing duplicates",
    RPC_PARAMS(
        RPC_PARAM_OPT("name", "string", "Name of the sky light to keep (default SkyLight)"),
        RPC_PARAM_OPT("recapture", "boolean", "Recapture sky after ensuring single")
    ))
{
    UEditorActorSubsystem* ActorSS = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    if (!ActorSS)
    {
        Ctx.SendError(TEXT("EDITOR_ACTOR_SUBSYSTEM_MISSING"), TEXT("EditorActorSubsystem not available"));
        return true;
    }

    TArray<AActor*> AllActors = ActorSS->GetAllLevelActors();
    TArray<AActor*> SkyLights;
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->IsA<ASkyLight>())
            SkyLights.Add(Actor);
    }

    FString TargetName = Ctx.GetString(TEXT("name"), TEXT("SkyLight"));

    int32 RemovedCount = 0;
    AActor* KeptActor = nullptr;

    for (AActor* SkyLightActor : SkyLights)
    {
        if (!KeptActor && (SkyLightActor->GetActorLabel() == TargetName || TargetName.IsEmpty()))
        {
            KeptActor = SkyLightActor;
            if (!TargetName.IsEmpty())
                SkyLightActor->SetActorLabel(TargetName);
        }
        else if (!KeptActor)
        {
            KeptActor = SkyLightActor;
            if (!TargetName.IsEmpty())
                SkyLightActor->SetActorLabel(TargetName);
        }
        else
        {
            ActorSS->DestroyActor(SkyLightActor);
            RemovedCount++;
        }
    }

    if (!KeptActor)
    {
        KeptActor = SpawnActorInActiveWorld<AActor>(
            ASkyLight::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, TargetName);
        PinWright::MarkLevelActorSpawned(KeptActor);
    }

    if (KeptActor)
    {
        bool bRecapture = Ctx.GetBool(TEXT("recapture"), false);
        if (bRecapture)
        {
            if (USkyLightComponent* Comp = KeptActor->FindComponentByClass<USkyLightComponent>())
                Comp->RecaptureSky();
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("removed"), RemovedCount);
    if (KeptActor)
        AddActorVerification(Resp, KeptActor);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.create_lightmass_volume ----
REGISTER_RPC_HANDLER("lighting.create_lightmass_volume", "lighting",
    "Create a Lightmass Importance Volume with real box brush geometry. `measuredSize`, "
    "`measuredExtent` and `measuredCenter` are MEASURED off the spawned volume's brush bounds, "
    "never echoed from the request; `requestedSize` / `requestedLocation` name what was asked "
    "for, and `sizeWarning` fires when the two disagree. An ALightmassImportanceVolume keeps "
    "its shape in a UModel brush rather than in its transform, so one spawned without a brush "
    "has extent (0,0,0) and no actor scale can fix it. When the brush cannot be built this "
    "verb DESTROYS the actor and REFUSES: FStaticLightingSystem::GatherScene synthesizes a "
    "scene-bounds importance region only while the importance-volume COUNT is zero, so an "
    "extent-less volume suppresses that fallback and the warning inside it and makes the next "
    "bake strictly worse than one with no importance volume at all.",
    RPC_PARAMS(
        RPC_PARAM_OPT("location", "object", "Center of the volume {x, y, z}"),
        RPC_PARAM_OPT("size", "object",
            "FULL size {x, y, z} on each axis, not a half-extent (default 1000 each); the "
            "volume spans location +/- size/2. Every axis must be positive and finite"),
        RPC_PARAM_OPT("name", "string", "Label for the volume actor")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();

    FVector Location = FVector::ZeroVector;
    const TSharedPtr<FJsonObject>* LocObj;
    if (Payload->TryGetObjectField(TEXT("location"), LocObj))
    {
        Location.X = GetJsonNumberField((*LocObj), TEXT("x"));
        Location.Y = GetJsonNumberField((*LocObj), TEXT("y"));
        Location.Z = GetJsonNumberField((*LocObj), TEXT("z"));
    }

    FVector Size = FVector(1000, 1000, 1000);
    const TSharedPtr<FJsonObject>* SizeObj;
    if (Payload->TryGetObjectField(TEXT("size"), SizeObj))
    {
        Size.X = GetJsonNumberField((*SizeObj), TEXT("x"));
        Size.Y = GetJsonNumberField((*SizeObj), TEXT("y"));
        Size.Z = GetJsonNumberField((*SizeObj), TEXT("z"));
    }

    // Refused BEFORE the spawn, so nothing is left in the level to clean up. A degenerate or
    // non-finite axis builds a flat (or no) brush, and a zero-extent importance volume is
    // worse for a bake than no volume at all - see the destroy path below for why.
    const bool bSizeIsUsable =
        FMath::IsFinite(Size.X) && FMath::IsFinite(Size.Y) && FMath::IsFinite(Size.Z) &&
        Size.GetMin() > UE_KINDA_SMALL_NUMBER;
    if (!bSizeIsUsable)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(
            TEXT("size must be positive and finite on every axis (got %s). A degenerate axis ")
            TEXT("produces an importance region of zero volume, and an extent-less ")
            TEXT("LightmassImportanceVolume is worse for a lighting build than no volume at ")
            TEXT("all: the engine synthesizes a scene-bounds importance region only while the ")
            TEXT("importance-volume COUNT is zero, so one with no extent suppresses that ")
            TEXT("fallback. Nothing was spawned."),
            *Size.ToString()));
        return true;
    }

    ALightmassImportanceVolume* Volume = SpawnActorInActiveWorld<ALightmassImportanceVolume>(
        ALightmassImportanceVolume::StaticClass(), Location, FRotator::ZeroRotator);
    if (!Volume)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn LightmassImportanceVolume"));
        return true;
    }

    PinWright::MarkLevelActorSpawned(Volume);

    // ALightmassImportanceVolume is an ABrush: its shape lives in a UModel, not in its
    // transform, and the raw spawn above leaves that model null. UBrushComponent::CalcBounds
    // then falls to its final else and returns
    // FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.f) - a literal zero
    // extent that no actor scale can multiply, which is why the SetActorScale3D(Size / 200)
    // this verb used to write was inert, and why leaving it in place after building a brush
    // would instead overshoot by Size/200 per axis. Build the caller's box through the shared
    // builder and keep the actor at identity scale. BuildBoxBrushGeometry takes FULL size per
    // axis, which is already what `size` means here, so no conversion is needed.
    // B-lightmass-volume-no-brush-geometry.
    const bool bBrushBuilt = VolumeBrushGeometry::BuildBoxBrushGeometry(Volume, Size);

    // MEASURED off the actor, never echoed from the request: AVolume::GetBounds runs
    // UBrushComponent::CalcBounds over the model that was just built, which is the same
    // computation behind the cached component bounds that
    // FLightmassExporter::AddImportanceVolume stores (GetComponentsBoundingBox(true)) as the
    // bake's importance region. A phantom is therefore detectable from the response alone.
    const FBoxSphereBounds MeasuredBounds = Volume->GetBounds();
    const FVector MeasuredExtent = MeasuredBounds.BoxExtent;

    if (!bBrushBuilt || MeasuredExtent.GetMin() <= UE_KINDA_SMALL_NUMBER)
    {
        // Destroy it rather than leave it standing. FStaticLightingSystem::GatherScene
        // (Engine: Editor/UnrealEd/Private/StaticLightingSystem/StaticLightingSystem.cpp)
        // synthesizes an importance region from the scene bounds only
        // `if (LightmassExporter->GetImportanceVolumes().Num() == 0)` - a COUNT, with no
        // extent test anywhere on the path - and FLightmassExporter::AddImportanceVolume
        // (Lightmass.h) stores GetComponentsBoundingBox(true) unchecked. A zero-extent volume
        // therefore counts as one, suppresses that fallback AND the "No importance volume
        // found" warning inside it, and ships a point as the entire importance region. Failing
        // with an unchanged level is strictly better than succeeding with a phantom in it.
        UWorld* VolumeWorld = Volume->GetWorld();
        if (UEditorActorSubsystem* ActorSS =
            GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr)
        {
            ActorSS->DestroyActor(Volume);
        }
        else if (VolumeWorld)
        {
            VolumeWorld->EditorDestroyActor(Volume, /*bShouldModifyLevel=*/true);
        }

        Ctx.SendError(TEXT("SPAWN_FAILED"), FString::Printf(
            TEXT("Brush geometry could not be built for the LightmassImportanceVolume ")
            TEXT("(requested size %s, measured brush extent %s), so the volume was destroyed ")
            TEXT("instead of being left in the level. An extent-less importance volume is ")
            TEXT("worse than none: the engine synthesizes a scene-bounds importance region ")
            TEXT("only while the importance-volume COUNT is zero, so a spawned-but-empty one ")
            TEXT("suppresses that fallback and its warning, and the whole level then bakes ")
            TEXT("against a point. Retry, or place the volume through ")
            TEXT("volume.create_lightmass_importance_volume, before running a lighting build."),
            *Size.ToString(), *MeasuredExtent.ToString()));
        return true;
    }

    FString Name = Ctx.GetString(TEXT("name"));
    if (!Name.IsEmpty())
        Volume->SetActorLabel(Name);

    const FVector MeasuredSize = MeasuredExtent * 2.0;

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorName"), Volume->GetActorLabel());
    Resp->SetObjectField(TEXT("measuredSize"), JsonBuilders::BuildVectorJson(MeasuredSize));
    Resp->SetObjectField(TEXT("measuredExtent"), JsonBuilders::BuildVectorJson(MeasuredExtent));
    Resp->SetObjectField(TEXT("measuredCenter"), JsonBuilders::BuildVectorJson(MeasuredBounds.Origin));
    // The request, named separately so it can never be mistaken for a reading.
    Resp->SetObjectField(TEXT("requestedSize"), JsonBuilders::BuildVectorJson(Size));
    Resp->SetObjectField(TEXT("requestedLocation"), JsonBuilders::BuildVectorJson(Location));

    // 1 uu tolerance, matching the volume brush regression tests: BSP vertex rounding moves a
    // face by far less than that, so anything wider is a real disagreement the caller must be
    // told about rather than left to infer from two numbers that look alike.
    if (!MeasuredSize.Equals(Size, 1.0) || !MeasuredBounds.Origin.Equals(Location, 1.0))
    {
        Resp->SetStringField(TEXT("sizeWarning"), FString::Printf(
            TEXT("The volume's measured brush bounds (size %s centred at %s) differ from the ")
            TEXT("request (size %s at %s). The MEASURED values are the importance region the ")
            TEXT("next lighting build will use."),
            *MeasuredSize.ToString(), *MeasuredBounds.Origin.ToString(),
            *Size.ToString(), *Location.ToString()));
    }

    AddActorVerification(Resp, Volume);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.setup_volumetric_fog ----
REGISTER_RPC_HANDLER("lighting.setup_volumetric_fog", "lighting",
    "Enable volumetric fog on the level's ExponentialHeightFog, spawning one if the level has none. "
    "`enabled` is measured, not echoed: it is true only when the component flag is set AND the "
    "r.VolumetricFog cvar is non-zero, because that cvar vetoes the whole volumetric pass and no "
    "read-back on the actor records the veto. `componentFlag` reports the write, `volumetricFogCVar` "
    "the measured cvar, and `cvarWarning` names the remedy when the pass cannot run. Does not change "
    "the cvar. Inside `viewDistance` the volumetric integration REPLACES the height fog's analytic "
    "inscattering, so `albedo` / `emissive` are the fog colour there and FogInscatteringLuminance "
    "applies only beyond that distance.",
    RPC_PARAMS(
        RPC_PARAM_OPT("viewDistance", "number", "Volumetric fog view distance"),
        RPC_PARAM_OPT("albedo", "object",
            "Particle reflectiveness {r, g, b} in linear 0-1 (VolumetricFogAlbedo). This is the "
            "fog colour inside viewDistance, not FogInscatteringLuminance"),
        RPC_PARAM_OPT("emissive", "object",
            "Light emitted by the fog itself {r, g, b, a} in linear units, unclamped "
            "(VolumetricFogEmissive)"),
        RPC_PARAM_OPT("extinctionScale", "number",
            "Scales how much light fog particles absorb; 1 is neutral, UI range 0.1-10 "
            "(VolumetricFogExtinctionScale)"),
        RPC_PARAM_OPT("scatteringDistribution", "number",
            "Scattering phase anisotropy; 0 scatters equally in all directions, 0.9 scatters "
            "predominantly forward, UI range -0.9 to 0.9 (VolumetricFogScatteringDistribution)")
    ))
{
    UEditorActorSubsystem* ActorSS = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    if (!ActorSS)
    {
        Ctx.SendError(TEXT("EDITOR_ACTOR_SUBSYSTEM_MISSING"), TEXT("EditorActorSubsystem not available"));
        return true;
    }

    AExponentialHeightFog* FogActor = nullptr;
    TArray<AActor*> AllActors = ActorSS->GetAllLevelActors();
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->IsA<AExponentialHeightFog>())
        {
            FogActor = Cast<AExponentialHeightFog>(Actor);
            break;
        }
    }

    if (!FogActor)
    {
        FogActor = Cast<AExponentialHeightFog>(SpawnActorInActiveWorld<AActor>(
            AExponentialHeightFog::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator));
        PinWright::MarkLevelActorSpawned(FogActor);
    }

    if (FogActor && FogActor->GetComponent())
    {
        UExponentialHeightFogComponent* FogComp = FogActor->GetComponent();

        PinWright::MarkLevelActorModified(FogActor, FogComp);

        FogComp->bEnableVolumetricFog = true;

        double Distance = Ctx.GetNumber(TEXT("viewDistance"), -1.0);
        if (Distance >= 0.0)
            FogComp->VolumetricFogDistance = (float)Distance;

        // The four fields below are the only colour controls the volumetric pass reads.
        // Within VolumetricFogDistance the volumetric integration replaces the height
        // fog's analytic inscattering, so FogInscatteringLuminance and
        // DirectionalInscatteringLuminance apply only BEYOND that distance — an author
        // reaching for "the fog colour" through the obvious field is writing one the
        // renderer does not sample in the region they are looking at.
        //
        // Presence is read off the raw payload rather than through a sentinel default:
        // scatteringDistribution is legitimately negative and extinctionScale is
        // legitimately any positive value, so no out-of-band value means "absent".
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

        // Initialized because the short-circuited `IsValid() && TryGet...` leaves the compiler
        // unable to prove the out-param was written on every path into the body.
        const TSharedPtr<FJsonObject>* AlbedoObj = nullptr;
        if (Payload.IsValid() && Payload->TryGetObjectField(TEXT("albedo"), AlbedoObj))
        {
            FLinearColor Albedo;
            Albedo.R = GetJsonNumberField((*AlbedoObj), TEXT("r"));
            Albedo.G = GetJsonNumberField((*AlbedoObj), TEXT("g"));
            Albedo.B = GetJsonNumberField((*AlbedoObj), TEXT("b"));
            Albedo.A = 1.0f;
            if (!FMath::IsFinite(Albedo.R) || !FMath::IsFinite(Albedo.G) || !FMath::IsFinite(Albedo.B))
            {
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("setup_volumetric_fog: Invalid albedo components, using white"));
                Albedo = FLinearColor::White;
            }
            // VolumetricFogAlbedo is an FColor the renderer decodes as sRGB
            // (Renderer/Private/SceneCore.cpp builds FLinearColor(FColor) from it), so the
            // linear {r, g, b} on the wire is sRGB-encoded here — the same conversion the
            // details-panel colour picker applies. ToFColor clamps to 0-1 for us.
            FogComp->VolumetricFogAlbedo = Albedo.ToFColor(/*bSRGB=*/true);
        }

        const TSharedPtr<FJsonObject>* EmissiveObj = nullptr;
        if (Payload.IsValid() && Payload->TryGetObjectField(TEXT("emissive"), EmissiveObj))
        {
            FLinearColor Emissive;
            Emissive.R = GetJsonNumberField((*EmissiveObj), TEXT("r"));
            Emissive.G = GetJsonNumberField((*EmissiveObj), TEXT("g"));
            Emissive.B = GetJsonNumberField((*EmissiveObj), TEXT("b"));
            Emissive.A = (*EmissiveObj)->HasField(TEXT("a")) ? GetJsonNumberField((*EmissiveObj), TEXT("a")) : 1.0f;
            if (!FMath::IsFinite(Emissive.R) || !FMath::IsFinite(Emissive.G) ||
                !FMath::IsFinite(Emissive.B) || !FMath::IsFinite(Emissive.A))
            {
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("setup_volumetric_fog: Invalid emissive components, using black"));
                Emissive = FLinearColor::Black;
            }
            // Emissive is a density in linear units, not a reflectance: no clamp to 0-1.
            FogComp->VolumetricFogEmissive = Emissive;
        }

        double ExtinctionScale = 0.0;
        if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("extinctionScale"), ExtinctionScale))
        {
            if (FMath::IsFinite(ExtinctionScale))
                FogComp->VolumetricFogExtinctionScale = (float)ExtinctionScale;
            else
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("setup_volumetric_fog: Non-finite extinctionScale ignored"));
        }

        double ScatteringDistribution = 0.0;
        if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("scatteringDistribution"), ScatteringDistribution))
        {
            if (FMath::IsFinite(ScatteringDistribution))
                FogComp->VolumetricFogScatteringDistribution = (float)ScatteringDistribution;
            else
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("setup_volumetric_fog: Non-finite scatteringDistribution ignored"));
        }

        // Raw field writes: neither bEnableVolumetricFog nor VolumetricFogDistance
        // has an engine setter that pushes render state, and the four colour fields
        // above are written the same way, so this single push is what carries all six
        // to the renderer.
        PinWright::MarkComponentRenderStateDirty(FogComp);

        // r.VolumetricFog vetoes the whole volumetric pass regardless of the component
        // flag, and nothing on the actor records that: property.get bEnableVolumetricFog
        // reads true either way. So `enabled` is MEASURED as (flag AND no veto) rather
        // than echoed back from the write, and `componentFlag` carries what was written.
        // Reporting them apart is the point — a caller that sees enabled:false with a
        // cvarWarning knows to fix the renderer, not the fog values. A scalability group
        // is the usual source of the veto (BaseScalability.ini puts r.VolumetricFog=0 in
        // ShadowQuality@1, restoring it at ShadowQuality@3), so a low-quality editor
        // renders no volumetric fog at all while every fog write still succeeds.
        int32 CVarValue = 0;
        bool bCVarFound = false;
        if (const IConsoleVariable* VolumetricFogCVar =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog")))
        {
            CVarValue = VolumetricFogCVar->GetInt();
            bCVarFound = true;
        }

        const bool bComponentFlag = FogComp->bEnableVolumetricFog;
        const bool bVetoed = bCVarFound && CVarValue == 0;

        TSharedPtr<FJsonObject> CVarInfo = MakeShared<FJsonObject>();
        CVarInfo->SetStringField(TEXT("cvar"), TEXT("r.VolumetricFog"));
        CVarInfo->SetBoolField(TEXT("found"), bCVarFound);
        // Absent, not 0, when the registry did not carry it: an unmeasured value must not
        // be readable as a measured one.
        if (bCVarFound)
        {
            CVarInfo->SetNumberField(TEXT("value"), CVarValue);
        }

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("actorName"), FogActor->GetActorLabel());
        Resp->SetBoolField(TEXT("enabled"), bComponentFlag && !bVetoed);
        Resp->SetBoolField(TEXT("componentFlag"), bComponentFlag);
        Resp->SetObjectField(TEXT("volumetricFogCVar"), CVarInfo);

        if (bVetoed)
        {
            Resp->SetStringField(TEXT("cvarWarning"),
                TEXT("bEnableVolumetricFog is set on the fog component, but r.VolumetricFog is 0, ")
                TEXT("so the volumetric pass does not run and NOTHING on this component renders: ")
                TEXT("VolumetricFogAlbedo, VolumetricFogEmissive, VolumetricFogDistance, ")
                TEXT("VolumetricFogExtinctionScale, VolumetricFogScatteringDistribution and every ")
                TEXT("light's VolumetricScatteringIntensity are all inert. Tuning fog values ")
                TEXT("against a capture taken now measures the wrong renderer. Turn the pass on ")
                TEXT("for this session with system.console_command \"r.VolumetricFog 1\" (a ")
                TEXT("scalability group can zero it again — BaseScalability.ini sets it to 0 in ")
                TEXT("ShadowQuality@1), or pin it durably with r.VolumetricFog=1 under ")
                TEXT("[SystemSettings] in the project's DefaultEngine.ini."));
        }
        else if (!bCVarFound)
        {
            Resp->SetStringField(TEXT("cvarWarning"),
                TEXT("r.VolumetricFog is not in this host's console registry, so whether the ")
                TEXT("volumetric pass can run was not measured. `enabled` here reports only the ")
                TEXT("component flag."));
        }

        AddActorVerification(Resp, FogActor);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("EXECUTION_ERROR"), TEXT("Failed to find or spawn ExponentialHeightFog"));
    }
    return true;
}

// ---- lighting.setup_light_shafts ----
REGISTER_RPC_HANDLER("lighting.setup_light_shafts", "lighting",
    "Set bEnableLightShaftBloom and/or bEnableLightShaftOcclusion on a directional light. "
    "`bloomEnabled` and `occlusionEnabled` are measured, not echoed: each is true only when the "
    "component flag is set AND the r.LightShaftQuality cvar is non-zero, because that cvar skips "
    "both light-shaft passes wholesale and no read-back on the light records the veto. "
    "`componentFlags` reports the flags read back off the component, `lightShaftQualityCVar` the "
    "measured cvar, and `cvarWarning` names the remedy when the passes cannot run. Does not change "
    "the cvar.",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Label or object name of the directional light to write; required when the level has more than one"),
        RPC_PARAM_OPT("bloom", "boolean", "Set bEnableLightShaftBloom (ULightComponent)"),
        RPC_PARAM_OPT("occlusion", "boolean", "Set bEnableLightShaftOcclusion (UDirectionalLightComponent)")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();

    // Argument validation before any world lookup: a call that names nothing to write is malformed
    // whatever the editor's state is.
    bool bRequestedBloom = false;
    const bool bHasBloom = Payload->TryGetBoolField(TEXT("bloom"), bRequestedBloom);
    bool bRequestedOcclusion = false;
    const bool bHasOcclusion = Payload->TryGetBoolField(TEXT("occlusion"), bRequestedOcclusion);
    if (!bHasBloom && !bHasOcclusion)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("At least one of bloom or occlusion is required. With neither there is nothing to "
                 "write, and a success carrying the light's current state would read as a write that "
                 "happened."));
        return true;
    }

    UEditorActorSubsystem* ActorSS = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    if (!ActorSS)
    {
        Ctx.SendError(TEXT("EDITOR_ACTOR_SUBSYSTEM_MISSING"), TEXT("EditorActorSubsystem not available"));
        return true;
    }

    // Selected on the COMPONENT class rather than on ADirectionalLight: a Blueprint sun carrying a
    // UDirectionalLightComponent is the same renderer case and the same veto.
    const FString ActorName = Ctx.GetString(TEXT("actorName"));
    TArray<AActor*> CandidateActors;
    TArray<UDirectionalLightComponent*> CandidateComps;
    for (AActor* Actor : ActorSS->GetAllLevelActors())
    {
        if (!IsValid(Actor))
        {
            continue;
        }
        UDirectionalLightComponent* Comp = Actor->FindComponentByClass<UDirectionalLightComponent>();
        if (!Comp)
        {
            continue;
        }
        if (!ActorName.IsEmpty() && Actor->GetActorLabel() != ActorName && Actor->GetName() != ActorName)
        {
            continue;
        }
        CandidateActors.Add(Actor);
        CandidateComps.Add(Comp);
    }

    if (CandidateActors.Num() == 0)
    {
        // Not a silent no-op: light shafts render for directional lights only
        // (ShouldRenderLightShaftsForLight, LightShaftRendering.cpp:122-130 returns false for every
        // other light type), so there is no honest way to service the request against another light.
        const FString Message = ActorName.IsEmpty()
            ? FString(TEXT("No actor with a UDirectionalLightComponent in the level. Light shafts "
                           "render for directional lights only — bEnableLightShaftBloom does nothing "
                           "on a point, spot or rect light despite being declared on ULightComponent."))
            : FString::Printf(TEXT("No actor named '%s' with a UDirectionalLightComponent in the level."),
                *ActorName);
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), Message);
        return true;
    }

    if (CandidateActors.Num() > 1)
    {
        FString Labels;
        for (const AActor* Candidate : CandidateActors)
        {
            if (!Labels.IsEmpty())
            {
                Labels += TEXT(", ");
            }
            Labels += Candidate->GetActorLabel();
        }
        const FString Qualifier = ActorName.IsEmpty()
            ? FString()
            : FString::Printf(TEXT(" '%s'"), *ActorName);
        Ctx.SendError(TEXT("AMBIGUOUS_ACTOR_NAME"), FString::Printf(
            TEXT("%d directional lights match%s: %s. Pass actorName to name the one to write — "
                 "picking one silently would write to whichever the level happens to list first."),
            CandidateActors.Num(), *Qualifier, *Labels));
        return true;
    }

    AActor* LightActor = CandidateActors[0];
    UDirectionalLightComponent* DirComp = CandidateComps[0];

    // The engine's light-shaft setters refuse outright on a registered Static-mobility component
    // (USceneComponent::AreDynamicDataChangesAllowed, SceneComponent.h:1363) and report nothing when
    // they do. Refuse at the input instead of writing, reading back the unchanged value and calling
    // the difference a warning.
    if (DirComp->IsRegistered() && DirComp->Mobility == EComponentMobility::Static)
    {
        Ctx.SendError(TEXT("EXECUTION_ERROR"), FString::Printf(
            TEXT("'%s' has Mobility=Static, and ULightComponent::SetEnableLightShaftBloom / "
                 "UDirectionalLightComponent::SetEnableLightShaftOcclusion silently no-op on a "
                 "registered static component. Set the light to Movable or Stationary first."),
            *LightActor->GetActorLabel()));
        return true;
    }

    PinWright::MarkLevelActorModified(LightActor, DirComp);

    // Engine setters, not raw field writes: both push render state themselves
    // (SetEnableLightShaftBloom -> MarkRenderStateDirty, LightComponent.cpp:1268-1276;
    // SetEnableLightShaftOcclusion -> UpdateProxy, DirectionalLightComponent.cpp:1315-1327), so no
    // MarkComponentRenderStateDirty here. A raw field write would reach neither proxy.
    if (bHasBloom)
    {
        DirComp->SetEnableLightShaftBloom(bRequestedBloom);
    }
    if (bHasOcclusion)
    {
        DirComp->SetEnableLightShaftOcclusion(bRequestedOcclusion);
    }

    // r.LightShaftQuality gates BOTH light-shaft passes at their entry:
    // FDeferredShadingSceneRenderer::RenderLightShaftOcclusion (LightShaftRendering.cpp:436) and
    // ::RenderLightShaftBloom (:541) each open with ShouldRenderLightShafts(ViewFamily), whose first
    // term is GLightShafts — the int the cvar is bound to (:18-25). The test sits ABOVE the loop over
    // Scene->Lights, so at 0 the component flags are never even read: bEnableLightShaftBloom,
    // bEnableLightShaftOcclusion, BloomScale, BloomThreshold, BloomMaxBrightness, BloomTint,
    // OcclusionMaskDarkness, OcclusionDepthRange and LightShaftOverrideDirection are all decorative,
    // and nothing on the light says so. Unlike r.LightFunctionQuality and r.ShadowQuality, this cvar
    // has no EngineShowFlagOverride entry (ShowFlags.cpp:448-518), so the LightShafts show flag stays
    // on and the editor UI gives no hint either. BaseScalability.ini zeroes it in
    // [PostProcessQuality@0] and [PostProcessQuality@1] and restores it at @2/@3/@Cine, so a
    // low-post-process-quality editor renders no shafts at all while every write here still succeeds.
    int32 CVarValue = 0;
    bool bCVarFound = false;
    if (const IConsoleVariable* LightShaftQualityCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightShaftQuality")))
    {
        CVarValue = LightShaftQualityCVar->GetInt();
        bCVarFound = true;
    }

    // Read back off the component, never echoed from the request.
    const bool bComponentBloom = DirComp->bEnableLightShaftBloom != 0;
    const bool bComponentOcclusion = DirComp->bEnableLightShaftOcclusion != 0;
    const bool bVetoed = bCVarFound && CVarValue == 0;

    TSharedPtr<FJsonObject> CVarInfo = MakeShared<FJsonObject>();
    CVarInfo->SetStringField(TEXT("cvar"), TEXT("r.LightShaftQuality"));
    CVarInfo->SetBoolField(TEXT("found"), bCVarFound);
    // Absent, not 0, when the registry did not carry it: an unmeasured value must not be readable as
    // a measured one.
    if (bCVarFound)
    {
        CVarInfo->SetNumberField(TEXT("value"), CVarValue);
    }

    TSharedPtr<FJsonObject> ComponentFlags = MakeShared<FJsonObject>();
    ComponentFlags->SetBoolField(TEXT("bloom"), bComponentBloom);
    ComponentFlags->SetBoolField(TEXT("occlusion"), bComponentOcclusion);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorName"), LightActor->GetActorLabel());
    Resp->SetBoolField(TEXT("bloomEnabled"), bComponentBloom && !bVetoed);
    Resp->SetBoolField(TEXT("occlusionEnabled"), bComponentOcclusion && !bVetoed);
    Resp->SetObjectField(TEXT("componentFlags"), ComponentFlags);
    Resp->SetObjectField(TEXT("lightShaftQualityCVar"), CVarInfo);

    // Gated on a flag actually being set: a call that turned both flags OFF under a zeroed cvar has
    // nothing misreported to warn about, and claiming otherwise would be its own false statement.
    if (bVetoed && (bComponentBloom || bComponentOcclusion))
    {
        Resp->SetStringField(TEXT("cvarWarning"),
            TEXT("A light-shaft flag is set on this component, but r.LightShaftQuality is 0, so ")
            TEXT("neither light-shaft pass runs and NOTHING they control renders: BloomScale, ")
            TEXT("BloomThreshold, BloomMaxBrightness, BloomTint, OcclusionMaskDarkness, ")
            TEXT("OcclusionDepthRange and LightShaftOverrideDirection are all inert, and fog reads a ")
            TEXT("white dummy occlusion texture. Tuning shaft values against a capture taken now ")
            TEXT("measures the wrong renderer, and the LightShafts show flag stays on, so the editor ")
            TEXT("UI does not show the veto either. Turn the passes on for this session with ")
            TEXT("system.console_command \"r.LightShaftQuality 1\" (a scalability group can zero it ")
            TEXT("again — BaseScalability.ini sets it to 0 in PostProcessQuality@0 and ")
            TEXT("PostProcessQuality@1, so sg.PostProcessQuality 2 or higher also lifts it), or pin ")
            TEXT("it durably with r.LightShaftQuality=1 under [SystemSettings] in the project's ")
            TEXT("DefaultEngine.ini."));
    }
    else if (!bCVarFound)
    {
        Resp->SetStringField(TEXT("cvarWarning"),
            TEXT("r.LightShaftQuality is not in this host's console registry, so whether the ")
            TEXT("light-shaft passes can run was not measured. bloomEnabled and occlusionEnabled here ")
            TEXT("report only the component flags."));
    }

    AddActorVerification(Resp, LightActor);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.setup_global_illumination ----
REGISTER_RPC_HANDLER("lighting.setup_global_illumination", "lighting", "Configure global illumination method",
    RPC_PARAMS(
        RPC_PARAM_REQ("method", "string", "GI method: LumenGI, ScreenSpace, None, RayTraced, Lightmass")
    ))
{
    FString Method = Ctx.GetString(TEXT("method"));
    if (Method.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("method parameter is required. Valid values: LumenGI, ScreenSpace, None, RayTraced, Lightmass"));
        return true;
    }

    const bool bValidMethod = ApplyDynamicGIMethodToCVars(Method);
    if (!bValidMethod)
    {
        Ctx.SendError(TEXT("INVALID_GI_METHOD"),
            FString::Printf(TEXT("Invalid GI method: %s. Valid values: LumenGI, ScreenSpace, None, RayTraced, Lightmass"), *Method));
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), bValidMethod);
    Resp->SetStringField(TEXT("method"), Method);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.configure_shadows ----
REGISTER_RPC_HANDLER("lighting.configure_shadows", "lighting",
    "Set r.Shadow.Virtual.Enable. `virtualShadowMaps` is measured, not echoed: it is read back off "
    "the cvar AFTER the write, so a write a higher-priority setter refused reports the state that is "
    "actually in force rather than the one that was asked for. It is OMITTED — never zeroed — when "
    "the cvar is absent from this host's console registry, `requested` carries what the call asked "
    "for, `shadowVirtualEnableCVar` the measurement, and `cvarWarning` names the remedy when the "
    "request could not take effect.",
    RPC_PARAMS(
        RPC_PARAM_OPT("virtualShadowMaps", "boolean", "Enable virtual shadow maps"),
        RPC_PARAM_OPT("rayTracedShadows", "boolean", "Enable ray-traced shadows (maps to VSM)")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    bool bRequested = false;
    const bool bHasRequest =
        Payload->TryGetBoolField(TEXT("virtualShadowMaps"), bRequested) ||
        Payload->TryGetBoolField(TEXT("rayTracedShadows"), bRequested);

    // The cvar is the only state this verb writes, so it is also the only thing that can be read
    // back — and two paths drop the write while returning nothing. FindConsoleVariable is null on a
    // host whose registry does not carry the name, and FConsoleVariableBase::CanChange admits a Set
    // only when the incoming priority is >= the one already recorded on the variable; this verb
    // writes at the default ECVF_SetByCode, so a setter above it (in practice a console entry, from
    // the editor console or -ExecCmds) bounces the write silently. Both cases used to report the
    // REQUEST back as `virtualShadowMaps` beside success:true, so a shadow configuration that was
    // never applied read exactly like one that was.
    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Enable"));
    if (CVar && bHasRequest)
    {
        CVar->Set(bRequested ? 1 : 0);
    }

    const bool bCVarFound = CVar != nullptr;
    const int32 CVarValue = bCVarFound ? CVar->GetInt() : 0;
    const bool bMeasured = bCVarFound && CVarValue != 0;

    TSharedPtr<FJsonObject> CVarInfo = MakeShared<FJsonObject>();
    CVarInfo->SetStringField(TEXT("cvar"), TEXT("r.Shadow.Virtual.Enable"));
    CVarInfo->SetBoolField(TEXT("found"), bCVarFound);
    // Absent, not 0, when the registry did not carry it: an unmeasured value must not be readable as
    // a measured one.
    if (bCVarFound)
    {
        CVarInfo->SetNumberField(TEXT("value"), CVarValue);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    if (bHasRequest)
    {
        Resp->SetBoolField(TEXT("requested"), bRequested);
    }
    // Same rule as the cvar value above: with nothing measured there is no honest boolean to publish
    // here, and a false would be indistinguishable from a measured "shadow maps are off".
    if (bCVarFound)
    {
        Resp->SetBoolField(TEXT("virtualShadowMaps"), bMeasured);
    }
    Resp->SetObjectField(TEXT("shadowVirtualEnableCVar"), CVarInfo);

    if (!bCVarFound)
    {
        Resp->SetStringField(TEXT("cvarWarning"),
            TEXT("r.Shadow.Virtual.Enable is not in this host's console registry, so nothing was ")
            TEXT("written and the virtual-shadow-map state was not measured. virtualShadowMaps is ")
            TEXT("omitted rather than reported as false — a false here would be indistinguishable ")
            TEXT("from a measured \"off\"."));
    }
    else if (bHasRequest && bMeasured != bRequested)
    {
        Resp->SetStringField(TEXT("cvarWarning"), FString::Printf(
            TEXT("Requested virtualShadowMaps=%s, but r.Shadow.Virtual.Enable reads %d after the ")
            TEXT("write, so NOTHING was applied and shadows render at the value reported here. ")
            TEXT("FConsoleVariableBase::CanChange admits a Set only at a priority >= the one already ")
            TEXT("recorded on the variable, and this verb writes at ECVF_SetByCode: a setter above ")
            TEXT("it — in practice a console entry, from the editor console or -ExecCmds, made ")
            TEXT("earlier in this session — holds the value for the rest of the run. Set it back ")
            TEXT("from the console, or pin the value you want durably with ")
            TEXT("r.Shadow.Virtual.Enable=%d under [SystemSettings] in the project's ")
            TEXT("DefaultEngine.ini."),
            bRequested ? TEXT("true") : TEXT("false"), CVarValue, bRequested ? 1 : 0));
    }

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.set_exposure ----
REGISTER_RPC_HANDLER("lighting.set_exposure", "lighting", "Configure auto-exposure settings via a PostProcessVolume",
    RPC_PARAMS(
        RPC_PARAM_OPT("minBrightness", "number", "Auto-exposure minimum brightness"),
        RPC_PARAM_OPT("maxBrightness", "number", "Auto-exposure maximum brightness"),
        RPC_PARAM_OPT("compensationValue", "number", "Exposure bias / compensation")
    ))
{
    APostProcessVolume* PPV = FindOrSpawnUnboundPPV(Ctx);
    if (!PPV) return true;

    // FPostProcessSettings is a UPROPERTY on APostProcessVolume itself, not on a
    // component, and the renderer samples the volume list per frame — actor ceremony
    // only, no MarkRenderStateDirty.
    PinWright::MarkLevelActorModified(PPV);

    auto* Payload = Ctx.GetRawPayload().Get();

    bool bSetMin = false, bSetMax = false, bSetComp = false;

    // A PostProcessVolume only blends a setting into the scene when its paired
    // bOverride_<Field> bit is set, so flip the override alongside every write (mirroring
    // set_ambient_occlusion). Without the bit the value is stored but never applied.
    double MinB = 0.0, MaxB = 0.0;
    if (Payload->TryGetNumberField(TEXT("minBrightness"), MinB))
    {
        PPV->Settings.bOverride_AutoExposureMinBrightness = true;
        PPV->Settings.AutoExposureMinBrightness = (float)MinB;
        bSetMin = true;
    }
    if (Payload->TryGetNumberField(TEXT("maxBrightness"), MaxB))
    {
        PPV->Settings.bOverride_AutoExposureMaxBrightness = true;
        PPV->Settings.AutoExposureMaxBrightness = (float)MaxB;
        bSetMax = true;
    }

    double Comp = 0.0;
    if (Payload->TryGetNumberField(TEXT("compensationValue"), Comp))
    {
        PPV->Settings.bOverride_AutoExposureBias = true;
        PPV->Settings.AutoExposureBias = (float)Comp;
        bSetComp = true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorName"), PPV->GetActorLabel());
    // Echo the values actually applied so the caller can confirm the write without
    // a separate property.get round-trip on FPostProcessSettings (board
    // E-lighting-set-ao-exposure-no-echo). Only echo params present in the call —
    // these are optional-param "update settings" RPCs.
    if (bSetMin)
        Resp->SetNumberField(TEXT("minBrightness"), PPV->Settings.AutoExposureMinBrightness);
    if (bSetMax)
        Resp->SetNumberField(TEXT("maxBrightness"), PPV->Settings.AutoExposureMaxBrightness);
    if (bSetComp)
        Resp->SetNumberField(TEXT("compensationValue"), PPV->Settings.AutoExposureBias);
    AddActorVerification(Resp, PPV);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.set_ambient_occlusion ----
REGISTER_RPC_HANDLER("lighting.set_ambient_occlusion", "lighting",
    "Configure ambient occlusion settings via a PostProcessVolume. `enabled` is measured, not "
    "echoed: it is true only when the applied AmbientOcclusionIntensity is above 0 AND the "
    "r.AmbientOcclusionLevels cvar is non-zero, because that cvar vetoes the whole screen-space AO "
    "pass and no read-back on the volume records the veto. `intensity` and `radius` report the "
    "writes, `ambientOcclusionLevelsCVar` the measured cvar, and `cvarWarning` names the remedy "
    "when the pass cannot run. Does not change the cvar.",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Enable AO (sets intensity to 0.5 or 0.0)"),
        RPC_PARAM_OPT("intensity", "number", "AO intensity value"),
        RPC_PARAM_OPT("radius", "number", "AO radius")
    ))
{
    APostProcessVolume* PPV = FindOrSpawnUnboundPPV(Ctx);
    if (!PPV) return true;

    PinWright::MarkLevelActorModified(PPV);

    auto* Payload = Ctx.GetRawPayload().Get();

    bool bSetIntensity = false, bSetRadius = false;

    bool bEnabled = true;
    if (Payload->TryGetBoolField(TEXT("enabled"), bEnabled))
    {
        PPV->Settings.bOverride_AmbientOcclusionIntensity = true;
        PPV->Settings.AmbientOcclusionIntensity = bEnabled ? 0.5f : 0.0f;
        bSetIntensity = true;
    }

    double Intensity;
    if (Payload->TryGetNumberField(TEXT("intensity"), Intensity))
    {
        PPV->Settings.bOverride_AmbientOcclusionIntensity = true;
        PPV->Settings.AmbientOcclusionIntensity = (float)Intensity;
        bSetIntensity = true;
    }

    double Radius;
    if (Payload->TryGetNumberField(TEXT("radius"), Radius))
    {
        PPV->Settings.bOverride_AmbientOcclusionRadius = true;
        PPV->Settings.AmbientOcclusionRadius = (float)Radius;
        bSetRadius = true;
    }

    // r.AmbientOcclusionLevels=0 vetoes SSAO outright:
    // ShouldRenderScreenSpaceAmbientOcclusion requires
    // FSSAOHelper::GetNumAmbientOcclusionLevels() != 0 (CompositionLighting.cpp:81-95),
    // which is a straight read of the cvar (PostProcessAmbientOcclusion.cpp:193-195). At 0
    // the pass never runs, so AmbientOcclusionIntensity, AmbientOcclusionRadius and every
    // other AO field on the volume are decorative — and the settings read back exactly as
    // written, so nothing on the volume records the veto. BaseScalability.ini zeroes it in
    // [PostProcessQuality@0] and restores it to -1 ("decide from the post-process
    // settings") at @1 and above, so a low-post-process-quality editor renders no SSAO at
    // all while every write here still succeeds. `intensity` reports the write; `enabled`
    // is MEASURED as (applied intensity > 0 AND no veto).
    int32 AOLevelsValue = 0;
    bool bAOLevelsFound = false;
    if (const IConsoleVariable* AOLevelsCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.AmbientOcclusionLevels")))
    {
        AOLevelsValue = AOLevelsCVar->GetInt();
        bAOLevelsFound = true;
    }
    // Only 0 disables; a negative value means "decide from the post-process settings".
    const bool bAOVetoed = bAOLevelsFound && AOLevelsValue == 0;

    TSharedPtr<FJsonObject> AOCVarInfo = MakeShared<FJsonObject>();
    AOCVarInfo->SetStringField(TEXT("cvar"), TEXT("r.AmbientOcclusionLevels"));
    AOCVarInfo->SetBoolField(TEXT("found"), bAOLevelsFound);
    // Absent, not 0, when the registry did not carry it: an unmeasured value must not be
    // readable as a measured one — and here 0 is the veto value, so publishing it as a
    // default would invent one.
    if (bAOLevelsFound)
    {
        AOCVarInfo->SetNumberField(TEXT("value"), AOLevelsValue);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorName"), PPV->GetActorLabel());
    // Echo the AO values actually applied so the caller confirms the write without a
    // property.get round-trip on FPostProcessSettings (board
    // E-lighting-set-ao-exposure-no-echo). Only echo params present in the call.
    // `enabled` is MEASURED, not echoed: it is the applied intensity (AO is on iff
    // intensity > 0) AND the absence of the cvar veto. It is not read off
    // bOverride_AmbientOcclusionIntensity: every bSetIntensity path forces the override bit
    // true, so echoing it would be an invariant `true` that misreports the enabled:false /
    // intensity-0 case the caller just requested.
    if (bSetIntensity)
    {
        Resp->SetNumberField(TEXT("intensity"), PPV->Settings.AmbientOcclusionIntensity);
        Resp->SetBoolField(TEXT("enabled"),
            PPV->Settings.AmbientOcclusionIntensity > 0.f && !bAOVetoed);
    }
    if (bSetRadius)
        Resp->SetNumberField(TEXT("radius"), PPV->Settings.AmbientOcclusionRadius);
    Resp->SetObjectField(TEXT("ambientOcclusionLevelsCVar"), AOCVarInfo);

    // Read off the volume, not off bSetIntensity: a radius-only write against a volume that
    // already has AO on is just as inert, and a call that turned AO off has nothing
    // misreported to warn about.
    if (bAOVetoed && PPV->Settings.AmbientOcclusionIntensity > 0.f)
    {
        Resp->SetStringField(TEXT("cvarWarning"),
            TEXT("AmbientOcclusionIntensity is above 0 on this volume, but ")
            TEXT("r.AmbientOcclusionLevels is 0, so the screen-space AO pass does not run and ")
            TEXT("NOTHING it controls renders: AmbientOcclusionIntensity, AmbientOcclusionRadius, ")
            TEXT("AmbientOcclusionPower, AmbientOcclusionBias, AmbientOcclusionQuality, ")
            TEXT("AmbientOcclusionFadeDistance and AmbientOcclusionStaticFraction are all inert. ")
            TEXT("Tuning AO values against a capture taken now measures the wrong renderer. Turn ")
            TEXT("the pass on for this session with system.console_command ")
            TEXT("\"r.AmbientOcclusionLevels -1\" (a scalability group can zero it again — ")
            TEXT("BaseScalability.ini sets it to 0 in PostProcessQuality@0, so ")
            TEXT("sg.PostProcessQuality 1 or higher also lifts it), or pin it durably with ")
            TEXT("r.AmbientOcclusionLevels=-1 under [SystemSettings] in the project's ")
            TEXT("DefaultEngine.ini."));
    }
    else if (!bAOLevelsFound)
    {
        Resp->SetStringField(TEXT("cvarWarning"),
            TEXT("r.AmbientOcclusionLevels is not in this host's console registry, so whether the ")
            TEXT("screen-space AO pass can run was not measured. `enabled` here reports only the ")
            TEXT("applied intensity."));
    }

    AddActorVerification(Resp, PPV);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- lighting.create_lighting_enabled_level ----
REGISTER_RPC_HANDLER("lighting.create_lighting_enabled_level", "lighting", "Create a new level with basic directional and sky lighting",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Save path for the new level")
    ))
{
    FString Path = Ctx.GetString(TEXT("path"));
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path required"));
        return true;
    }

    FString SanitizedPath = SanitizeProjectRelativePath(Path);
    if (SanitizedPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PATH"), TEXT("Invalid path: contains traversal or invalid characters"));
        return true;
    }
    Path = SanitizedPath;

    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor not available"));
        return true;
    }

    // `lighting.create_lighting_enabled_level` is listed in the tick-unsafe method
    // table in Dispatch/SafePoint.cpp: UEditorEngine::NewMap (EditorServer.cpp:2187)
    // calls EditorDestroyWorld (:2206) -> Cleanse (:2080) -> CollectGarbage
    // (EditorEngine.cpp:2859) -> ~ULevel -> FreeTickTaskLevel, which asserts
    // !LevelList.Contains(TickTaskLevel) if this runs inside UWorld::Tick — and the
    // two SpawnActorInActiveWorld calls below then mutate the freshly installed
    // world. The dispatcher re-queues the whole request onto the core ticker; do not
    // add a second gate here.
    //
    // The same EditorDestroyWorld also ends in CheckForWorldGCLeaks, which logs Fatal by
    // default on any dead world still resident after its cleanse — the tick gate above does
    // nothing about that, so this verb needs the shared pre-swap probe exactly as
    // level.create does. False for the transaction-buffer argument: NewMap resets the undo
    // buffer only after that check has already fired.
    if (RefuseIfWorldsSurviveMapSwap(Ctx, Path, /*bTransactionBufferWillBeCleared=*/false))
    {
        return true;
    }

    GEditor->NewMap();

    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    if (!EditorWorld || !EditorWorld->PersistentLevel)
    {
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"), TEXT("Editor world not available"));
        return true;
    }

    SpawnActorInActiveWorld<AActor>(ADirectionalLight::StaticClass(),
        FVector(0, 0, 500), FRotator(-45, 0, 0), TEXT("Sun"));
    SpawnActorInActiveWorld<AActor>(ASkyLight::StaticClass(),
        FVector::ZeroVector, FRotator::ZeroRotator, TEXT("SkyLight"));

    const bool bSaveReported = McpSafeLevelSave(EditorWorld->PersistentLevel, *Path, 5);

    // McpSafeLevelSave's shared ShouldTreatLevelSaveAsSuccess OR-policy accepts a clean /
    // registered package as success even when no .umap landed on disk — for this freshly
    // NewMap()'d in-memory-only world (CreatePackage'd but never written) that yields a
    // false success:true/existsAfter:true. This verb promises an on-disk lighting level at
    // Path, so the only honest persistence signal is the .umap on disk. Re-gate the
    // reported-success boolean through the shared VerifyLevelSavedToDisk helper (mount-aware
    // resolve + FileExists probe + the stricter ShouldTreatCreateLevelSaveAsSuccess predicate),
    // exactly as level.save / level.save_as / level.structure.create_level already do
    // (B-lighting-create-level-false-success-no-umap).
    FString SavedFilename;
    FString SaveErrorCode;
    const bool bSaved = VerifyLevelSavedToDisk(Path, bSaveReported, SavedFilename, SaveErrorCode);
    if (bSaved)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("path"), Path);
        Resp->SetStringField(TEXT("message"), TEXT("Level created with lighting"));
        Resp->SetBoolField(TEXT("existsAfter"), true);
        Resp->SetStringField(TEXT("levelPath"), Path);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        UE_LOG(LogPinWrightSubsystem, Error,
            TEXT("lighting.create_lighting_enabled_level: save reported=%s but no .umap on disk: %s (file=%s)"),
            bSaveReported ? TEXT("true") : TEXT("false"), *Path, *SavedFilename);
        Ctx.SendError(SaveErrorCode,
            bSaveReported
                ? FString::Printf(TEXT("Level created with lighting but no .umap was written to disk — the world exists only in memory and was never persisted: %s"), *Path)
                : FString::Printf(TEXT("Failed to save level: %s"), *Path));
    }
    return true;
}
