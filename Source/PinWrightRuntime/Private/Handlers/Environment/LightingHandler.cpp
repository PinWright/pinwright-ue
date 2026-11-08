// Copyright (c) 2026 Alexander Penkin. MIT License.

// LightingHandler.cpp (runtime half) - live-world lighting setters that only write CVars or the
// unbound PostProcessVolume (GI method, shadows, exposure, AO) plus the light-type listing.
// Light spawning, lighting builds, lightmass/fog/light-shaft setup and level creation stay in
// the editor module's LightingHandler.cpp.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Handlers/Environment/GIMethodCVarHelper.h"
#include "Handlers/Environment/PostProcessVolumeUtils.h"
#include "PinWrightHelpers.h"
#include "Utils/JsonBuilders.h"

#include "Engine/Light.h"
#include "Engine/PostProcessVolume.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

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
    Resp->SetStringField(TEXT("actorName"), ActorNameParamUtils::ActorLabelOrName(PPV));
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
    Resp->SetStringField(TEXT("actorName"), ActorNameParamUtils::ActorLabelOrName(PPV));
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
