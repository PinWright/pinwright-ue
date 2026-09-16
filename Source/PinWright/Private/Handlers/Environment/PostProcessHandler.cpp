// Copyright (c) 2026 Alexander Penkin. MIT License.

// PostProcessHandler.cpp - Typed PostProcessVolume setters (color grading, bloom,
// Lumen GI/reflections, anti-aliasing, motion blur).
//
// Each setter flips the paired bOverride_<Field> = true alongside every value
// write — the foot-gun that `property.set` exposes to callers is the entire
// reason these typed handlers exist. See the generated wiki page post_process.md.
//
// `set_anti_aliasing` is the one exception: the AA method and screen percentage
// live as CVars (`r.AntiAliasingMethod`, `r.ScreenPercentage`), not as PPV
// override fields, so the handler writes to IConsoleManager directly and does
// not find or spawn any PostProcessVolume — its response carries only the
// resolved values, not actorName / actor verification fields.
//
// The shared find-or-spawn-unbound-PPV helper lives in
// `Handlers/Environment/PostProcessVolumeUtils.h` and is consumed here plus by
// `lighting.set_exposure` / `lighting.set_ambient_occlusion`.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Handlers/Environment/PostProcessVolumeUtils.h"
#include "PinWrightHelpers.h"

#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
#include "Engine/EngineTypes.h"
#include "HAL/IConsoleManager.h"
#include "Compat/EngineVersionCompat.h"
#include "Editor.h"

using PinWright::FindOrSpawnUnboundPPV;

// ---------------------------------------------------------------------------
// File-local helpers.
// ---------------------------------------------------------------------------
namespace
{
    // Standard success response shape — actorName + AddActorVerification.
    void SendPPVSuccess(FHandlerContext& Ctx, APostProcessVolume* PPV)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("actorName"), PPV->GetActorLabel());
        AddActorVerification(Resp, PPV);
        Ctx.SendSuccess(Resp);
    }

    // Color grading saturation/contrast/gamma/gain are FVector4 (not float).
    // Accept either:
    //   - a scalar number → broadcast as FVector4(s, s, s, 1.0)  (luminance=1)
    //   - a 4-element number array → FVector4(r, g, b, luminance)
    // Returns false if the field is not present or has an unsupported shape.
    bool TryReadColorGradingFVector4(const FJsonObject& Payload,
        const FString& Field, FVector4& Out)
    {
        if (!Payload.HasField(Field))
            return false;

        // Scalar form first — TryGetNumberField only succeeds if the value is a number.
        double Scalar = 0.0;
        if (Payload.TryGetNumberField(Field, Scalar))
        {
            Out = FVector4((float)Scalar, (float)Scalar, (float)Scalar, 1.0f);
            return true;
        }

        // Array form: [r, g, b, luminance].
        const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
        if (Payload.TryGetArrayField(Field, ArrPtr) && ArrPtr && ArrPtr->Num() == 4)
        {
            const auto& Arr = *ArrPtr;
            Out = FVector4(
                (float)Arr[0]->AsNumber(),
                (float)Arr[1]->AsNumber(),
                (float)Arr[2]->AsNumber(),
                (float)Arr[3]->AsNumber());
            return true;
        }

        return false;
    }
}

// ---- post_process.set_color_grading ----
REGISTER_RPC_HANDLER("post_process.set_color_grading", "post_process",
    "Set white balance and global color-grading values on the unbound PostProcessVolume. "
    "Saturation/contrast/gamma/gain accept a scalar (broadcast as RGB with luminance=1) "
    "or a 4-element [r,g,b,luminance] array.",
    RPC_PARAMS(
        RPC_PARAM_OPT("whiteTemp", "number", "White balance temperature (1500-15000)"),
        RPC_PARAM_OPT("tint", "number", "White balance tint (-1 to 1)"),
        RPC_PARAM_OPT("saturation", "number|array", "Global color saturation (scalar or [r,g,b,lum])"),
        RPC_PARAM_OPT("contrast", "number|array", "Global color contrast"),
        RPC_PARAM_OPT("gamma", "number|array", "Global color gamma"),
        RPC_PARAM_OPT("gain", "number|array", "Global color gain")
    ))
{
    APostProcessVolume* PPV = FindOrSpawnUnboundPPV(Ctx);
    if (!PPV) return true;

    // FPostProcessSettings lives on the actor (PostProcessVolume.h:27-28), not on a
    // component — actor ceremony only. Before the writes so the Modify() is
    // state-neutral under a future transaction.
    PinWright::MarkLevelActorModified(PPV);

    auto* Payload = Ctx.GetRawPayload().Get();

    double Temp;
    if (Payload->TryGetNumberField(TEXT("whiteTemp"), Temp))
    {
        PPV->Settings.bOverride_WhiteTemp = true;
        PPV->Settings.WhiteTemp = (float)Temp;
    }
    double Tint;
    if (Payload->TryGetNumberField(TEXT("tint"), Tint))
    {
        PPV->Settings.bOverride_WhiteTint = true;
        PPV->Settings.WhiteTint = (float)Tint;
    }

    FVector4 V;
    if (TryReadColorGradingFVector4(*Payload, TEXT("saturation"), V))
    {
        PPV->Settings.bOverride_ColorSaturation = true;
        PPV->Settings.ColorSaturation = V;
    }
    if (TryReadColorGradingFVector4(*Payload, TEXT("contrast"), V))
    {
        PPV->Settings.bOverride_ColorContrast = true;
        PPV->Settings.ColorContrast = V;
    }
    if (TryReadColorGradingFVector4(*Payload, TEXT("gamma"), V))
    {
        PPV->Settings.bOverride_ColorGamma = true;
        PPV->Settings.ColorGamma = V;
    }
    if (TryReadColorGradingFVector4(*Payload, TEXT("gain"), V))
    {
        PPV->Settings.bOverride_ColorGain = true;
        PPV->Settings.ColorGain = V;
    }

    SendPPVSuccess(Ctx, PPV);
    return true;
}

// ---- post_process.set_bloom ----
REGISTER_RPC_HANDLER("post_process.set_bloom", "post_process",
    "Set bloom settings on the unbound PostProcessVolume.",
    RPC_PARAMS(
        RPC_PARAM_OPT("intensity", "number", "Bloom intensity"),
        RPC_PARAM_OPT("threshold", "number", "Bloom threshold"),
        RPC_PARAM_OPT("method", "string", "Bloom method: 'Standard' or 'Convolution'"),
        RPC_PARAM_OPT("convolutionScatterDispersion", "number", "Convolution scatter dispersion")
    ))
{
    APostProcessVolume* PPV = FindOrSpawnUnboundPPV(Ctx);
    if (!PPV) return true;

    // FPostProcessSettings lives on the actor (PostProcessVolume.h:27-28), not on a
    // component — actor ceremony only. Before the writes so the Modify() is
    // state-neutral under a future transaction.
    PinWright::MarkLevelActorModified(PPV);

    auto* Payload = Ctx.GetRawPayload().Get();

    double Intensity;
    if (Payload->TryGetNumberField(TEXT("intensity"), Intensity))
    {
        PPV->Settings.bOverride_BloomIntensity = true;
        PPV->Settings.BloomIntensity = (float)Intensity;
    }

    double Threshold;
    if (Payload->TryGetNumberField(TEXT("threshold"), Threshold))
    {
        PPV->Settings.bOverride_BloomThreshold = true;
        PPV->Settings.BloomThreshold = (float)Threshold;
    }

    FString Method;
    if (Payload->TryGetStringField(TEXT("method"), Method))
    {
        EBloomMethod Resolved = BM_SOG;
        if (Method.Equals(TEXT("Convolution"), ESearchCase::IgnoreCase))
        {
            Resolved = BM_FFT;
        }
        else if (Method.Equals(TEXT("Standard"), ESearchCase::IgnoreCase))
        {
            Resolved = BM_SOG;
        }
        PPV->Settings.bOverride_BloomMethod = true;
        PPV->Settings.BloomMethod = Resolved;
    }

    double Scatter;
    if (Payload->TryGetNumberField(TEXT("convolutionScatterDispersion"), Scatter))
    {
        PPV->Settings.bOverride_BloomConvolutionScatterDispersion = true;
        PPV->Settings.BloomConvolutionScatterDispersion = (float)Scatter;
    }

    SendPPVSuccess(Ctx, PPV);
    return true;
}

// ---- post_process.set_lumen_gi ----
REGISTER_RPC_HANDLER("post_process.set_lumen_gi", "post_process",
    "Toggle Lumen Global Illumination + tune the per-volume Lumen GI knobs.",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Toggle DynamicGlobalIlluminationMethod to Lumen (true) or None (false)"),
        RPC_PARAM_OPT("sceneDetail", "number", "Lumen scene detail"),
        RPC_PARAM_OPT("finalGatherQuality", "number", "Lumen final gather quality"),
        RPC_PARAM_OPT("maxTraceDistance", "number", "Lumen maximum trace distance")
    ))
{
    APostProcessVolume* PPV = FindOrSpawnUnboundPPV(Ctx);
    if (!PPV) return true;

    // FPostProcessSettings lives on the actor (PostProcessVolume.h:27-28), not on a
    // component — actor ceremony only. Before the writes so the Modify() is
    // state-neutral under a future transaction.
    PinWright::MarkLevelActorModified(PPV);

    auto* Payload = Ctx.GetRawPayload().Get();

    bool bEnabled;
    if (Payload->TryGetBoolField(TEXT("enabled"), bEnabled))
    {
        PPV->Settings.bOverride_DynamicGlobalIlluminationMethod = true;
        PPV->Settings.DynamicGlobalIlluminationMethod = bEnabled
            ? EDynamicGlobalIlluminationMethod::Lumen
            : EDynamicGlobalIlluminationMethod::None;
    }

    double SceneDetail;
    if (Payload->TryGetNumberField(TEXT("sceneDetail"), SceneDetail))
    {
        PPV->Settings.bOverride_LumenSceneDetail = true;
        PPV->Settings.LumenSceneDetail = (float)SceneDetail;
    }

    double FinalGather;
    if (Payload->TryGetNumberField(TEXT("finalGatherQuality"), FinalGather))
    {
        PPV->Settings.bOverride_LumenFinalGatherQuality = true;
        PPV->Settings.LumenFinalGatherQuality = (float)FinalGather;
    }

    double MaxTrace;
    if (Payload->TryGetNumberField(TEXT("maxTraceDistance"), MaxTrace))
    {
        PPV->Settings.bOverride_LumenMaxTraceDistance = true;
        PPV->Settings.LumenMaxTraceDistance = (float)MaxTrace;
    }

    SendPPVSuccess(Ctx, PPV);
    return true;
}

// ---- post_process.set_lumen_reflections ----
REGISTER_RPC_HANDLER("post_process.set_lumen_reflections", "post_process",
    "Toggle Lumen reflections + tune per-volume Lumen reflection knobs.",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Toggle ReflectionMethod to Lumen (true) or ScreenSpace (false)"),
        RPC_PARAM_OPT("quality", "number", "Lumen reflection quality"),
        RPC_PARAM_OPT("rayLightingMode", "string", "Default | SurfaceCache | HitLightingForReflections | HitLighting"),
        RPC_PARAM_OPT("maxRoughnessToTrace", "number", "Max roughness for dedicated reflection rays (0-1)")
    ))
{
    APostProcessVolume* PPV = FindOrSpawnUnboundPPV(Ctx);
    if (!PPV) return true;

    // FPostProcessSettings lives on the actor (PostProcessVolume.h:27-28), not on a
    // component — actor ceremony only. Before the writes so the Modify() is
    // state-neutral under a future transaction.
    PinWright::MarkLevelActorModified(PPV);

    auto* Payload = Ctx.GetRawPayload().Get();

    bool bEnabled;
    if (Payload->TryGetBoolField(TEXT("enabled"), bEnabled))
    {
        PPV->Settings.bOverride_ReflectionMethod = true;
        PPV->Settings.ReflectionMethod = bEnabled
            ? EReflectionMethod::Lumen
            : EReflectionMethod::ScreenSpace;
    }

    double Quality;
    if (Payload->TryGetNumberField(TEXT("quality"), Quality))
    {
        PPV->Settings.bOverride_LumenReflectionQuality = true;
        PPV->Settings.LumenReflectionQuality = (float)Quality;
    }

    FString Mode;
    if (Payload->TryGetStringField(TEXT("rayLightingMode"), Mode))
    {
        ELumenRayLightingModeOverride Resolved = ELumenRayLightingModeOverride::Default;
        if (Mode.Equals(TEXT("SurfaceCache"), ESearchCase::IgnoreCase))
            Resolved = ELumenRayLightingModeOverride::SurfaceCache;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        // HitLightingForReflections was added in UE 5.5 as a separate value distinct from
        // HitLighting (which already existed in 5.4 under the same name but different semantics)
        else if (Mode.Equals(TEXT("HitLightingForReflections"), ESearchCase::IgnoreCase))
            Resolved = ELumenRayLightingModeOverride::HitLightingForReflections;
#endif
        else if (Mode.Equals(TEXT("HitLighting"), ESearchCase::IgnoreCase))
            Resolved = ELumenRayLightingModeOverride::HitLighting;

        PPV->Settings.bOverride_LumenRayLightingMode = true;
        PPV->Settings.LumenRayLightingMode = Resolved;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // FPostProcessSettings::LumenMaxRoughnessToTraceReflections was added in UE 5.4; the knob
    // has no equivalent on 5.3, so the maxRoughnessToTrace parameter is ignored there.
    double MaxRough;
    if (Payload->TryGetNumberField(TEXT("maxRoughnessToTrace"), MaxRough))
    {
        // Real engine field name has no "For" — ticket text drift.
        PPV->Settings.bOverride_LumenMaxRoughnessToTraceReflections = true;
        PPV->Settings.LumenMaxRoughnessToTraceReflections = (float)MaxRough;
    }
#endif

    SendPPVSuccess(Ctx, PPV);
    return true;
}

// ---- post_process.set_anti_aliasing ----
// Anti-aliasing method and ScreenPercentage live as CVars (r.AntiAliasingMethod,
// r.ScreenPercentage), not as PostProcessVolume override fields. The PPV
// `ScreenPercentage` UPROPERTY exists only as ScreenPercentage_DEPRECATED and
// the AA method has no per-volume override at all. This handler writes only to
// IConsoleManager — it does not touch any actor, so the response carries the
// resolved values rather than actorName / AddActorVerification.
REGISTER_RPC_HANDLER("post_process.set_anti_aliasing", "post_process",
    "Configure anti-aliasing via CVars (no per-volume override exists in FPostProcessSettings). "
    "Does not touch any PostProcessVolume actor.",
    RPC_PARAMS(
        RPC_PARAM_OPT("method", "string", "None | FXAA | TAA | MSAA | TSR"),
        RPC_PARAM_OPT("screenPercentage", "number", "Primary screen percentage (writes r.ScreenPercentage)")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);

    FString Method;
    if (Payload->TryGetStringField(TEXT("method"), Method))
    {
        // Mapping per Engine: 0=None, 1=FXAA, 2=TAA, 3=MSAA, 4=TSR.
        int32 MethodValue = -1;
        FString ResolvedMethod;
        if (Method.Equals(TEXT("None"), ESearchCase::IgnoreCase))      { MethodValue = 0; ResolvedMethod = TEXT("None"); }
        else if (Method.Equals(TEXT("FXAA"), ESearchCase::IgnoreCase)) { MethodValue = 1; ResolvedMethod = TEXT("FXAA"); }
        else if (Method.Equals(TEXT("TAA"), ESearchCase::IgnoreCase))  { MethodValue = 2; ResolvedMethod = TEXT("TAA"); }
        else if (Method.Equals(TEXT("MSAA"), ESearchCase::IgnoreCase)) { MethodValue = 3; ResolvedMethod = TEXT("MSAA"); }
        else if (Method.Equals(TEXT("TSR"), ESearchCase::IgnoreCase))  { MethodValue = 4; ResolvedMethod = TEXT("TSR"); }

        // Validate-before-mutate / fail-loud: an unrecognized documented-enum token
        // must not be a clean success that changed nothing (board
        // B-set-aa-invalid-method-silent-noop; same misleading-success class as an
        // unsupported target reported as an applied write). Reject the whole call before
        // writing any CVar, so a rejected request leaves both r.AntiAliasingMethod and
        // r.ScreenPercentage untouched. `method` is optional, so a call that omits it
        // entirely (e.g. screenPercentage-only) never enters this block and still succeeds.
        if (MethodValue < 0)
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("Unrecognized anti-aliasing method '%s'; must be one of None|FXAA|TAA|MSAA|TSR."),
                    *Method));
            return true;
        }

        if (IConsoleVariable* CVar =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.AntiAliasingMethod")))
        {
            CVar->Set(MethodValue, ECVF_SetByCode);
        }
        Resp->SetStringField(TEXT("antiAliasingMethod"), ResolvedMethod);
    }

    double ScreenPct;
    if (Payload->TryGetNumberField(TEXT("screenPercentage"), ScreenPct))
    {
        if (IConsoleVariable* CVar =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
        {
            CVar->Set((float)ScreenPct, ECVF_SetByCode);
        }
        Resp->SetNumberField(TEXT("screenPercentage"), ScreenPct);
    }

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- post_process.set_motion_blur ----
REGISTER_RPC_HANDLER("post_process.set_motion_blur", "post_process",
    "Configure motion blur on the unbound PostProcessVolume.",
    RPC_PARAMS(
        RPC_PARAM_OPT("amount", "number", "Motion blur amount (0=off, 1=max)"),
        RPC_PARAM_OPT("max", "number", "Max distortion (% of screen width)"),
        RPC_PARAM_OPT("targetFps", "number", "Target FPS for motion blur normalization (0-120)"),
        RPC_PARAM_OPT("perObjectSize", "number", "Min projected screen radius for per-object motion blur")
    ))
{
    APostProcessVolume* PPV = FindOrSpawnUnboundPPV(Ctx);
    if (!PPV) return true;

    // FPostProcessSettings lives on the actor (PostProcessVolume.h:27-28), not on a
    // component — actor ceremony only. Before the writes so the Modify() is
    // state-neutral under a future transaction.
    PinWright::MarkLevelActorModified(PPV);

    auto* Payload = Ctx.GetRawPayload().Get();

    double Amount;
    if (Payload->TryGetNumberField(TEXT("amount"), Amount))
    {
        PPV->Settings.bOverride_MotionBlurAmount = true;
        PPV->Settings.MotionBlurAmount = (float)Amount;
    }

    double Max;
    if (Payload->TryGetNumberField(TEXT("max"), Max))
    {
        PPV->Settings.bOverride_MotionBlurMax = true;
        PPV->Settings.MotionBlurMax = (float)Max;
    }

    double TargetFps;
    if (Payload->TryGetNumberField(TEXT("targetFps"), TargetFps))
    {
        PPV->Settings.bOverride_MotionBlurTargetFPS = true;
        PPV->Settings.MotionBlurTargetFPS = (int32)TargetFps;
    }

    double PerObj;
    if (Payload->TryGetNumberField(TEXT("perObjectSize"), PerObj))
    {
        PPV->Settings.bOverride_MotionBlurPerObjectSize = true;
        PPV->Settings.MotionBlurPerObjectSize = (float)PerObj;
    }

    SendPPVSuccess(Ctx, PPV);
    return true;
}
