// Copyright (c) 2026 Alexander Penkin. MIT License.

// EffectHandler.cpp - Migrated from PinWright_EffectHandlers.cpp
// effect.set_niagara_parameter, effect.step_and_capture, effect.spawn_niagara (editor-only: the label
// lookup needs Handlers/Niagara/NiagaraInstanceUtils, the capture drives the level editor viewport,
// spawn waits on Niagara compiles). The game-safe effect.* verbs (debug shapes, activate/deactivate/
// advance, cleanup) moved to PinWrightRuntime Private/Handlers/VFX/EffectHandler.cpp.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraCompileVerdict.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/OpenLevelCapture.h"
#include "Handlers/VFX/EffectRuntimeUtils.h"
#include "Handlers/VFX/EffectSpawnTestHooks.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/AssetUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/ScopedWorldTimeDilation.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#if __has_include("NiagaraActor.h")
#include "NiagaraActor.h"
#endif
#if __has_include("NiagaraComponent.h")
#include "NiagaraComponent.h"
#endif
#if __has_include("NiagaraSystem.h")
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#endif
#if __has_include("NiagaraEmitter.h")
#include "NiagaraEmitter.h"
#endif
#if __has_include("NiagaraScript.h")
#include "NiagaraScript.h"
#endif
#if __has_include("NiagaraDataInterface.h")
#include "NiagaraDataInterface.h"
#endif
#if __has_include("NiagaraSimulationStageBase.h")
#include "NiagaraSimulationStageBase.h"
#endif
#if __has_include("NiagaraRendererProperties.h")
#include "NiagaraRendererProperties.h"
#endif
#if __has_include("Engine/PointLight.h")
#include "Engine/PointLight.h"
#endif
#if __has_include("Engine/SpotLight.h")
#include "Engine/SpotLight.h"
#endif
#if __has_include("Engine/DirectionalLight.h")
#include "Engine/DirectionalLight.h"
#endif
#if __has_include("Engine/RectLight.h")
#include "Engine/RectLight.h"
#endif
#if __has_include("Components/LightComponent.h")
#include "Components/LightComponent.h"
#endif
#if __has_include("Components/PointLightComponent.h")
#include "Components/PointLightComponent.h"
#endif
#if __has_include("Components/SpotLightComponent.h")
#include "Components/SpotLightComponent.h"
#endif
#if __has_include("Components/RectLightComponent.h")
#include "Components/RectLightComponent.h"
#endif
#if __has_include("Components/DirectionalLightComponent.h")
#include "Components/DirectionalLightComponent.h"
#endif
#include "Utils/RenderingAvailability.h"

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

// Parse scale from payload - supports array [x,y,z] or single number
static FVector ParseScale(const TSharedPtr<FJsonObject>& Payload)
{
    TArray<double> ScaleArr = {1, 1, 1};
    const TArray<TSharedPtr<FJsonValue>>* ScaleJsonArr = nullptr;
    if (Payload->TryGetArrayField(TEXT("scale"), ScaleJsonArr) && ScaleJsonArr && ScaleJsonArr->Num() >= 3)
    {
        ScaleArr[0] = (*ScaleJsonArr)[0]->AsNumber();
        ScaleArr[1] = (*ScaleJsonArr)[1]->AsNumber();
        ScaleArr[2] = (*ScaleJsonArr)[2]->AsNumber();
    }
    else if (Payload->TryGetNumberField(TEXT("scale"), ScaleArr[0]))
    {
        ScaleArr[1] = ScaleArr[2] = ScaleArr[0];
    }
    return FVector(ScaleArr[0], ScaleArr[1], ScaleArr[2]);
}


// ===========================================================================
// effect.set_niagara_parameter
// ===========================================================================
REGISTER_RPC_HANDLER("effect.set_niagara_parameter", "effect", "Set a parameter on a Niagara component",
    RPC_PARAMS(
        RPC_PARAM_OPT("systemName", "string", "Actor label of the Niagara system"),
        RPC_PARAM_REQ("parameterName", "string", "Name of the parameter to set"),
        RPC_PARAM_OPT("parameterType", "string", "Type: Float, Vector, Color, Bool (default Float)"),
        RPC_PARAM_OPT("value", "any", "Value to set")
    ))
{
    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();

    FString SystemName = Ctx.GetString(TEXT("systemName"));
    FString ParameterName = Ctx.GetString(TEXT("parameterName"));
    FString ParameterType = Ctx.GetString(TEXT("parameterType"));
    if (ParameterName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("parameterName required"));
        return true;
    }
    if (ParameterType.IsEmpty())
        ParameterType = TEXT("Float");

    UE_LOG(LogPinWrightSubsystem, Verbose,
           TEXT("SetNiagaraParameter: Looking for actor '%s' to set param '%s'"),
           *SystemName, *ParameterName);

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
    UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (!ActorSS)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING, TEXT("EditorActorSubsystem not available"));
        return true;
    }

    const FName ParamName(*ParameterName);
    const TSharedPtr<FJsonValue> ValueField = LocalPayload->TryGetField(TEXT("value"));

    TArray<AActor*> AllActors = ActorSS->GetAllLevelActors();
    bool bApplied = false;
    bool bActorFound = false;
    bool bComponentFound = false;
    bool bParameterExists = false;

    for (AActor* Actor : AllActors)
    {
        if (!Actor)
            continue;
        if (!Actor->GetActorLabel().Equals(SystemName, ESearchCase::IgnoreCase))
            continue;

        bActorFound = true;
        UNiagaraComponent* NiComp = Actor->FindComponentByClass<UNiagaraComponent>();
        if (!NiComp)
        {
            bComponentFound = false;
            break;
        }
        bComponentFound = true;

        // Reject names that aren't declared on the system. SetVariable* forwards to
        // OverrideParameters.SetParameterValue(..., bAdd=true), which silently CREATES an
        // entry for any unknown name, so without this check a typo'd / wrong-prefix /
        // wrong-type name would write nothing yet still report applied:true. A single
        // classification distinguishes a bad type from an unknown name for the error ladder
        // below, so neither needs to re-run the type allow-list.
        const PinWrightNiagara::EParameterLookup ParamLookup =
            PinWrightNiagara::ClassifyComponentParameter(NiComp, ParamName, ParameterType);
        if (ParamLookup != PinWrightNiagara::EParameterLookup::Found)
        {
            break;
        }
        bParameterExists = true;

        if (ParameterType.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
        {
            double NumberValue = 0.0;
            bool bHasNumber = LocalPayload->TryGetNumberField(TEXT("value"), NumberValue);
            if (!bHasNumber && ValueField.IsValid())
            {
                if (ValueField->Type == EJson::Number)
                {
                    NumberValue = ValueField->AsNumber();
                    bHasNumber = true;
                }
                else if (ValueField->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject> Obj = ValueField->AsObject();
                    if (Obj.IsValid())
                        bHasNumber = Obj->TryGetNumberField(TEXT("v"), NumberValue);
                }
            }
            if (bHasNumber)
            {
                NiComp->SetVariableFloat(ParamName, static_cast<float>(NumberValue));
                bApplied = true;
            }
        }
        else if (ParameterType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
        {
            const TArray<TSharedPtr<FJsonValue>>* ArrValue = nullptr;
            const TSharedPtr<FJsonObject>* ObjValue = nullptr;
            if (LocalPayload->TryGetArrayField(TEXT("value"), ArrValue) && ArrValue && ArrValue->Num() >= 3)
            {
                const float X = static_cast<float>((*ArrValue)[0]->AsNumber());
                const float Y = static_cast<float>((*ArrValue)[1]->AsNumber());
                const float Z = static_cast<float>((*ArrValue)[2]->AsNumber());
                NiComp->SetVariableVec3(ParamName, FVector(X, Y, Z));
                bApplied = true;
            }
            else if (LocalPayload->TryGetObjectField(TEXT("value"), ObjValue) && ObjValue)
            {
                double VX = 0, VY = 0, VZ = 0;
                (*ObjValue)->TryGetNumberField(TEXT("x"), VX);
                (*ObjValue)->TryGetNumberField(TEXT("y"), VY);
                (*ObjValue)->TryGetNumberField(TEXT("z"), VZ);
                NiComp->SetVariableVec3(ParamName, FVector((float)VX, (float)VY, (float)VZ));
                bApplied = true;
            }
        }
        else if (ParameterType.Equals(TEXT("Color"), ESearchCase::IgnoreCase))
        {
            const TArray<TSharedPtr<FJsonValue>>* ArrValue = nullptr;
            if (LocalPayload->TryGetArrayField(TEXT("value"), ArrValue) && ArrValue && ArrValue->Num() >= 3)
            {
                const float R = static_cast<float>((*ArrValue)[0]->AsNumber());
                const float G = static_cast<float>((*ArrValue)[1]->AsNumber());
                const float B = static_cast<float>((*ArrValue)[2]->AsNumber());
                const float Alpha = ArrValue->Num() > 3 ? static_cast<float>((*ArrValue)[3]->AsNumber()) : 1.0f;
                NiComp->SetVariableLinearColor(ParamName, FLinearColor(R, G, B, Alpha));
                bApplied = true;
            }
        }
        else if (ParameterType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
        {
            bool bValue = false;
            bool bHasBool = LocalPayload->TryGetBoolField(TEXT("value"), bValue);
            if (bHasBool)
            {
                NiComp->SetVariableBool(ParamName, bValue);
                bApplied = true;
            }
        }

        break;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), bApplied);
    Resp->SetBoolField(TEXT("applied"), bApplied);
    Resp->SetStringField(TEXT("actorName"), SystemName);
    Resp->SetStringField(TEXT("parameterName"), ParameterName);
    Resp->SetStringField(TEXT("parameterType"), ParameterType);

    if (bApplied)
    {
        Ctx.SendSuccess(Resp);
    }
    else
    {
        FString ErrMsg = TEXT("Niagara parameter not applied");
        FString ErrCode = ErrorCodes::ERR_SET_NIAGARA_PARAM_FAILED;
        if (!bActorFound)
        {
            ErrMsg = FString::Printf(TEXT("Actor '%s' not found"), *SystemName);
            ErrCode = ErrorCodes::ERR_ACTOR_NOT_FOUND;
        }
        else if (!bComponentFound)
        {
            ErrMsg = FString::Printf(TEXT("Actor '%s' has no Niagara component"), *SystemName);
            ErrCode = ErrorCodes::ERR_COMPONENT_NOT_FOUND;
        }
        else if (!PinWrightNiagara::IsSupportedRuntimeParameterType(ParameterType))
        {
            ErrMsg = FString::Printf(TEXT("Invalid parameter type: %s"), *ParameterType);
            ErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        }
        else if (!bParameterExists)
        {
            ErrMsg = FString::Printf(
                TEXT("Niagara system on '%s' has no %s user parameter named '%s'"),
                *SystemName, *ParameterType, *ParameterName);
            ErrCode = ErrorCodes::ERR_PARAMETER_NOT_FOUND;
        }
        Ctx.SendError(ErrCode, ErrMsg);
    }
    return true;
}

// ===========================================================================
// effect.step_and_capture
// ===========================================================================
REGISTER_RPC_HANDLER("effect.step_and_capture", "effect",
    "Reset and activate a placed Niagara system, settle viewport exposure, freeze editor-world "
    "time, advance the system to one requested instant, capture that exact frame, and restore time.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("systemName", "string", "Actor label of the Niagara system (also accepts actorName)", "actorName"),
        RPC_PARAM_OPT("reset", "boolean", "Reset the Niagara system before sampling (default true)."),
        RPC_PARAM_OPT("seconds", "number", "Requested simulation age in seconds, up to 60. Mutually exclusive with frames."),
        RPC_PARAM_OPT("frames", "integer", "Whole simulation frames to advance, from 1 through 10000. Mutually exclusive with seconds; default 1."),
        RPC_PARAM_OPT("deltaTime", "number", "Seconds per simulation step. Default 1/60; the actual per-step delta must be at least 0.0001 seconds and frames * deltaTime must not exceed 60 seconds. With seconds, the uniform step is reduced when needed."),
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/OpenLevel. The .png extension is appended if missing."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Default 768."),
        RPC_PARAM_OPT("location", "object", "Level viewport camera location {x, y, z}."),
        RPC_PARAM_OPT("rotation", "object", "Level viewport camera rotation {pitch, yaw, roll}."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'."),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees. Default 50."),
        RPC_PARAM_OPT("allowBlank", "boolean", "Accept an intentionally near-uniform black frame. Default false."),
        FParamSpec{TEXT("orthoWidth"), TEXT("number"),
            TEXT("Orthographic frame width in world centimetres. Default 2000. Alias: orthoWorldWidth."),
            false, TEXT("2000"), TArray<FString>({TEXT("orthoWorldWidth")})},
        RPC_PARAM_OPT("viewDistanceScale", "number", "Scoped r.ViewDistanceScale override; omit to use the capture default."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("hideEditorSprites", "boolean", PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC),
        RPC_PARAM_OPT("subject", "object", "Optional world or actor framing subject, matching render.capture_open_level.")
    ))
{
    if (!PinWrightRendering::RequireRenderer(Ctx))
    {
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SystemName = Ctx.GetString(TEXT("systemName"));
    if (SystemName.IsEmpty())
    {
        Payload->TryGetStringField(TEXT("actorName"), SystemName);
    }

    const bool bHasSeconds = Payload->HasField(TEXT("seconds"));
    const bool bHasFrames = Payload->HasField(TEXT("frames"));
    if (bHasSeconds && bHasFrames)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("seconds and frames are mutually exclusive; provide at most one time form"));
        return true;
    }

    const auto SendInvalidTimeParams = [&Ctx](const TCHAR* Reason)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("%s %s"), Reason,
                *PinWrightEffectStepAndCaptureLimits::Describe()));
    };

    double RequestedSeconds = 0.0;
    double RequestedDeltaTime = 1.0 / 60.0;
    Payload->TryGetNumberField(TEXT("deltaTime"), RequestedDeltaTime);
    if (!FMath::IsFinite(RequestedDeltaTime) ||
        RequestedDeltaTime < PinWrightEffectStepAndCaptureLimits::MinActualStepDeltaSeconds)
    {
        SendInvalidTimeParams(TEXT("deltaTime is non-finite or below the supported minimum."));
        return true;
    }

    double RequestedSteps = 1.0;
    double StepDeltaSeconds = RequestedDeltaTime;
    FString TimeMode = TEXT("frames");
    if (bHasSeconds)
    {
        Payload->TryGetNumberField(TEXT("seconds"), RequestedSeconds);
        if (!(RequestedSeconds > 0.0) || !FMath::IsFinite(RequestedSeconds))
        {
            SendInvalidTimeParams(TEXT("seconds must be a finite number greater than zero."));
            return true;
        }
        if (RequestedSeconds > PinWrightEffectStepAndCaptureLimits::MaxSimulatedSeconds)
        {
            SendInvalidTimeParams(TEXT("seconds exceeds the supported total simulated duration."));
            return true;
        }

        const double StepRatio = RequestedSeconds / RequestedDeltaTime;
        const double RoundedStepRatio = FMath::RoundToDouble(StepRatio);
        const double RequiredSteps = FMath::IsNearlyEqual(StepRatio, RoundedStepRatio, 1e-9)
            ? RoundedStepRatio
            : FMath::CeilToDouble(StepRatio);
        RequestedSteps = FMath::Max(1.0, RequiredSteps);
        StepDeltaSeconds = RequestedSeconds / RequestedSteps;
        TimeMode = TEXT("seconds");
    }
    else
    {
        double RequestedFrames = 1.0;
        Payload->TryGetNumberField(TEXT("frames"), RequestedFrames);
        RequestedSteps = RequestedFrames;
        RequestedSeconds = StepDeltaSeconds * RequestedSteps;
    }

    PinWrightEffectStepAndCaptureLimits::FPreparedSimulation PreparedSimulation;
    FString ValidationError;
    if (!PinWrightEffectStepAndCaptureLimits::Prepare(
            RequestedSteps, StepDeltaSeconds, PreparedSimulation, ValidationError))
    {
        SendInvalidTimeParams(*ValidationError);
        return true;
    }
    const int32 Steps = PreparedSimulation.Steps;
    const float SimulationDeltaTime = PreparedSimulation.SimulationDeltaTime;

    PinWrightEffectRuntime::FNiagaraTarget Target;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightEffectRuntime::ResolveTarget(
            SystemName, Target, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    UWorld* World = Target.Actor.IsValid() ? Target.Actor->GetWorld() : nullptr;
    FScopedWorldTimeDilation TimeGuard(World);
    if (!TimeGuard.IsAvailable())
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD_SETTINGS,
            TEXT("The Niagara actor's world has no WorldSettings"));
        return true;
    }

    const bool bReset = Ctx.GetBool(TEXT("reset"), true);
    PinWrightEffectRuntime::FAdvanceResult AdvanceResult;
    PinWrightRenderCapture::FViewportCaptureHooks Hooks;
    Hooks.BeforeWarmup = [&Target, bReset](FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (!PinWrightEffectRuntime::Activate(
                Target, bReset, OutErrorCode, OutErrorMessage))
        {
            return false;
        }
        UNiagaraComponent* Component = Target.Component.Get();
        if (!Component || !Component->IsActive())
        {
            OutErrorCode = ErrorCodes::ERR_ACTION_FAILED;
            OutErrorMessage = TEXT("Niagara system did not become active after reset/activation");
            return false;
        }
        return true;
    };
    Hooks.BeforeFinalFrame =
        [&Target, &TimeGuard, &AdvanceResult, Steps, SimulationDeltaTime](
            const PinWrightRenderCapture::FViewportCaptureOutput& SettledCapture,
            FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (!SettledCapture.bWarmupSettled)
        {
            OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
            OutErrorMessage = TEXT("Viewport exposure did not settle before the world-time freeze");
            return false;
        }
        if (!TimeGuard.Freeze())
        {
            OutErrorCode = ErrorCodes::ERR_NO_WORLD_SETTINGS;
            OutErrorMessage = TEXT("Failed to freeze the Niagara actor's world time");
            return false;
        }
        return PinWrightEffectRuntime::Advance(
            Target, Steps, SimulationDeltaTime, AdvanceResult,
            OutErrorCode, OutErrorMessage);
    };
    Hooks.AfterFinalFrame = [&TimeGuard]()
    {
        TimeGuard.Restore();
    };

    const PinWrightOpenLevelCapture::FSuccessDecorator DecorateSuccess =
        [&Target, &TimeGuard, SystemName, bReset, TimeMode, Steps, SimulationDeltaTime,
            RequestedSeconds, &AdvanceResult](
            const PinWrightRenderCapture::FViewportCaptureOutput& Capture,
            const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Effect = MakeShared<FJsonObject>();
        Effect->SetStringField(TEXT("actorName"), SystemName);
        Effect->SetBoolField(TEXT("reset"), bReset);
        Effect->SetStringField(TEXT("timeMode"), TimeMode);
        Effect->SetNumberField(TEXT("steps"), Steps);
        Effect->SetNumberField(TEXT("deltaTime"), static_cast<double>(SimulationDeltaTime));
        Effect->SetNumberField(TEXT("sampledSeconds"), RequestedSeconds);
        Effect->SetNumberField(TEXT("requestedSeconds"), RequestedSeconds);
        Effect->SetNumberField(TEXT("simulatedSeconds"), AdvanceResult.SimulatedSeconds);
        if (AdvanceResult.bStoppedEarly)
        {
            Effect->SetBoolField(TEXT("stoppedEarly"), true);
            Effect->SetStringField(TEXT("stopReason"), AdvanceResult.StopReason);
        }
        Effect->SetBoolField(TEXT("exposureSettledBeforeFreeze"), Capture.bWarmupSettled);
        if (UNiagaraComponent* Component = Target.Component.Get())
        {
            Effect->SetBoolField(TEXT("active"), Component->IsActive());
        }

        TSharedPtr<FJsonObject> WorldTime = MakeShared<FJsonObject>();
        WorldTime->SetNumberField(TEXT("before"), TimeGuard.GetOriginalTimeDilation());
        WorldTime->SetNumberField(TEXT("frozen"), TimeGuard.GetFrozenTimeDilation());
        WorldTime->SetBoolField(TEXT("restored"), TimeGuard.WasRestored());
        Effect->SetObjectField(TEXT("worldTimeDilation"), WorldTime);
        Result->SetObjectField(TEXT("effect"), Effect);
    };

    return PinWrightOpenLevelCapture::Handle(Ctx, Hooks, DecorateSuccess);
}

// ===========================================================================
// effect.spawn_niagara
// ===========================================================================
REGISTER_RPC_MUTATING_HANDLER("effect.spawn_niagara", "effect",
    "Spawn and activate a compiled Niagara system, then report the measured component state",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "string", "Asset path to the Niagara system"),
        RPC_PARAM_OPT("location", "array|object", "Location [x,y,z] or {x,y,z}"),
        RPC_PARAM_OPT("rotation", "array", "Rotation [pitch,yaw,roll]"),
        RPC_PARAM_OPT("scale", "array|number", "Scale [x,y,z] or uniform"),
        RPC_PARAM_OPT("autoDestroy", "boolean", "Auto destroy after effect (default false)"),
        RPC_PARAM_OPT("attachToActor", "string", "Actor label to attach to"),
        RPC_PARAM_OPT_ALIAS("name", "string", "Actor label (also accepts actorName)", "actorName")
    ))
{
    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();

    FString SystemPath = Ctx.GetString(TEXT("systemPath"));
    if (SystemPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("systemPath required"));
        return true;
    }

    FVector Loc = PinWrightEffectRuntime::ParseLocationFromPayload(LocalPayload, TEXT("location"));

    // Rotation
    TArray<double> RotArr = {0, 0, 0};
    const TArray<TSharedPtr<FJsonValue>>* RA = nullptr;
    if (LocalPayload->TryGetArrayField(TEXT("rotation"), RA) && RA && RA->Num() >= 3)
    {
        RotArr[0] = (*RA)[0]->AsNumber();
        RotArr[1] = (*RA)[1]->AsNumber();
        RotArr[2] = (*RA)[2]->AsNumber();
    }

    // Scale
    FVector Scale = ParseScale(LocalPayload);

    const bool bAutoDestroy = LocalPayload->HasField(TEXT("autoDestroy"))
        ? GetJsonBoolField(LocalPayload, TEXT("autoDestroy"))
        : false;
    FString AttachToActor;
    LocalPayload->TryGetStringField(TEXT("attachToActor"), AttachToActor);

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
    FString ResolvedWorldMode;
    UWorld* TargetWorld = McpActorUtils::ResolveQueryWorld(TEXT("auto"), ResolvedWorldMode);
    if (!TargetWorld)
    {
        Ctx.SendError(ErrorCodes::ERR_WORLD_NOT_FOUND, TEXT("No valid editor or PIE world available for spawn"));
        return true;
    }

    const FResolvedAsset ResolvedSystem = ResolveAsset(SystemPath, /*bLoadObject=*/true);
    if (!ResolvedSystem.Object)
    {
        Ctx.SendError(ErrorCodes::ERR_SYSTEM_NOT_FOUND,
            FString::Printf(TEXT("Niagara system asset not found: %s"), *SystemPath));
        return true;
    }
    UNiagaraSystem* System = Cast<UNiagaraSystem>(ResolvedSystem.Object);
    if (!System)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE,
            FString::Printf(TEXT("'%s' is a %s, but effect.spawn_niagara requires a NiagaraSystem"),
                *SystemPath, *ResolvedSystem.Object->GetClass()->GetName()));
        return true;
    }

    AActor* Parent = nullptr;
    if (!AttachToActor.IsEmpty())
    {
        Parent = McpActorUtils::FindActorByName(TargetWorld, AttachToActor);
        if (!Parent)
        {
            AActor* OtherWorldParent = McpActorUtils::FindActorByName(nullptr, AttachToActor);
            UWorld* OtherWorld = OtherWorldParent ? OtherWorldParent->GetWorld() : nullptr;
            if (OtherWorld && OtherWorld != TargetWorld)
            {
                TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
                State->SetStringField(TEXT("requestedAttachmentParent"), AttachToActor);
                State->SetStringField(TEXT("targetWorld"), TargetWorld->GetPathName());
                State->SetStringField(TEXT("parentWorld"), OtherWorld->GetPathName());
                Ctx.SendError(ErrorCodes::ERR_ATTACH_FAILED,
                    TEXT("Requested attachment parent belongs to a different world"), State);
                return true;
            }
            Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                FString::Printf(TEXT("Requested attachment parent not found: %s"), *AttachToActor));
            return true;
        }
        if (Parent->GetWorld() != TargetWorld)
        {
            Ctx.SendError(ErrorCodes::ERR_ATTACH_FAILED,
                TEXT("Requested attachment parent belongs to a different world"));
            return true;
        }
    }

    // Spawning is the demand that on-demand compilation defers to, so this wait may drain a
    // request that is only queued - UNiagaraComponent::Activate flushes the same one itself. A
    // system whose compile was deferred at load reports the request outstanding forever while
    // nothing is actively compiling, and refusing on it made the asset permanently unspawnable.
    const PinWrightNiagara::FCompileWaitOutcome CompileWait =
        PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    const TSharedPtr<FJsonObject> Compile = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);
    const PinWrightNiagara::FCompileVerdict CompileVerdict =
        PinWrightNiagara::ReadCompileVerdict(Compile);
    // Measured with the predicate the refusal below is decided on. The diagnostics block reports
    // the GPU-inclusive queue, which is a wider question than whether this system can be run.
    const bool bCompilePending = PinWrightNiagara::HasPendingCompileWork(*System);
    const bool bCompileReady = PinWrightNiagara::IsSystemReadyAfterCompileWait(
        *System,
        CompileWait,
        CompileVerdict.Check == PinWrightNiagara::EScriptCompileCheck::Failed);
    FString CompileStatus = PinWrightNiagara::ScriptCompileCheckToString(CompileVerdict.Check);
    if (bCompileReady)
    {
        // On-demand compilation may leave duplicated script status fields uninitialized even
        // though Niagara's executable state is ready. Engine readiness is authoritative there.
        CompileStatus = TEXT("passed");
    }
    else if (CompileWait.bTimedOut)
    {
        CompileStatus = TEXT("timedOut");
    }
    else if (bCompilePending)
    {
        CompileStatus = TEXT("outstanding");
    }

    TSharedPtr<FJsonObject> CompileState = MakeShared<FJsonObject>();
    CompileState->SetStringField(TEXT("systemPath"), System->GetPathName());
    CompileState->SetStringField(TEXT("compileStatus"), CompileStatus);
    CompileState->SetBoolField(TEXT("compileWaited"), CompileWait.bWaited);
    CompileState->SetNumberField(TEXT("compileWaitedMs"), CompileWait.WaitedSeconds * 1000.0);
    CompileState->SetBoolField(TEXT("compileTimedOut"), CompileWait.bTimedOut);
    CompileState->SetBoolField(TEXT("outstandingCompilationRequests"), bCompilePending);
    CompileState->SetObjectField(TEXT("compile"), Compile);
    if (!bCompileReady)
    {
        FString Message = FString::Printf(
            TEXT("Niagara system '%s' is not compiled and ready to spawn (compileStatus=%s)"),
            *SystemPath, *CompileStatus);
        if (CompileVerdict.FailedScripts.Num() > 0)
        {
            Message += TEXT(": ") +
                PinWrightNiagara::DescribeScriptCompileFailure(CompileVerdict.FailedScripts[0]);
        }
        Ctx.SendError(ErrorCodes::ERR_SYSTEM_NOT_COMPILED, Message, CompileState);
        return true;
    }

    const FRotator SpawnRot(static_cast<float>(RotArr[0]), static_cast<float>(RotArr[1]), static_cast<float>(RotArr[2]));
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    AActor* Spawned = TargetWorld->SpawnActor<AActor>(
        ANiagaraActor::StaticClass(), Loc, SpawnRot, SpawnParams);
    if (!Spawned)
    {
        Ctx.SendError(ErrorCodes::ERR_SPAWN_FAILED, TEXT("Failed to spawn NiagaraActor"));
        return true;
    }

    UNiagaraComponent* NiComp = Spawned->FindComponentByClass<UNiagaraComponent>();
    if (!NiComp)
    {
        const bool bDestroyedAfterFailure = TargetWorld->DestroyActor(Spawned);
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("systemPath"), System->GetPathName());
        State->SetBoolField(TEXT("componentFound"), false);
        State->SetBoolField(TEXT("active"), false);
        State->SetBoolField(TEXT("destroyedAfterFailure"), bDestroyedAfterFailure);
        Ctx.SendError(ErrorCodes::ERR_EFFECT_NOT_ACTIVE,
            TEXT("Spawned NiagaraActor has no Niagara component to activate"), State);
        return true;
    }

    // ANiagaraActor registers its component while spawning. SetAsset on that registered component
    // auto-activates it, so prepare it while unregistered and explicitly activate exactly once only
    // after every requested property and attachment has been applied.
    NiComp->UnregisterComponent();
    NiComp->SetAutoActivate(false);
    NiComp->SetAsset(System);
    NiComp->SetWorldScale3D(Scale);
    NiComp->SetAutoDestroy(bAutoDestroy);

    if (Parent && !Spawned->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform))
    {
        const bool bDestroyedAfterFailure = TargetWorld->DestroyActor(Spawned);
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("requestedAttachmentParent"), AttachToActor);
        State->SetBoolField(TEXT("attached"), false);
        State->SetBoolField(TEXT("destroyedAfterFailure"), bDestroyedAfterFailure);
        Ctx.SendError(ErrorCodes::ERR_ATTACH_FAILED,
            FString::Printf(TEXT("Failed to attach Niagara actor to '%s'"), *AttachToActor), State);
        return true;
    }

    // Set actor label
    FString Name;
    LocalPayload->TryGetStringField(TEXT("name"), Name);
    if (Name.IsEmpty())
        LocalPayload->TryGetStringField(TEXT("actorName"), Name);

    if (!Name.IsEmpty())
    {
        Spawned->SetActorLabel(Name);
    }
    else
    {
        Spawned->SetActorLabel(FString::Printf(TEXT("Niagara_%lld"), FDateTime::Now().ToUnixTimestamp()));
    }

    NiComp->RegisterComponent();
    const FString ComponentPath = NiComp->GetPathName();
    NiComp->Activate(true);
#if WITH_DEV_AUTOMATION_TESTS
    if (PinWrightEffectSpawnTestHooks::ForceInactiveAfterActivation())
    {
        NiComp->DeactivateImmediate();
    }
#endif

    const bool bActorAlive = IsValid(Spawned);
    const bool bComponentAlive = IsValid(NiComp);
    const bool bAssignedSystem = bComponentAlive && NiComp->GetAsset() == System;
    const bool bActive = bComponentAlive && NiComp->IsActive();
    const FBoolProperty* AutoDestroyProperty =
        FindFProperty<FBoolProperty>(UNiagaraComponent::StaticClass(), TEXT("bAutoDestroy"));
    const bool bAutoDestroyMeasured = bComponentAlive && AutoDestroyProperty != nullptr;
    const bool bMeasuredAutoDestroy = bAutoDestroyMeasured &&
        AutoDestroyProperty->GetPropertyValue_InContainer(NiComp);
    const bool bAttached = bActorAlive && Parent && Spawned->GetAttachParentActor() == Parent;
    if (!bActorAlive || !bComponentAlive || !bAssignedSystem || !bActive || !bAutoDestroyMeasured ||
        bMeasuredAutoDestroy != bAutoDestroy ||
        (Parent && !bAttached))
    {
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("systemPath"), System->GetPathName());
        State->SetStringField(TEXT("componentPath"), ComponentPath);
        State->SetBoolField(TEXT("componentAlive"), bComponentAlive);
        State->SetBoolField(TEXT("systemAssigned"), bAssignedSystem);
        State->SetBoolField(TEXT("active"), bActive);
        State->SetBoolField(TEXT("autoDestroy"), bMeasuredAutoDestroy);
        State->SetBoolField(TEXT("autoDestroyMeasured"), bAutoDestroyMeasured);
        State->SetBoolField(TEXT("attached"), bAttached);
        State->SetStringField(TEXT("compileStatus"), CompileStatus);
        const bool bDestroyedAfterFailure = !bActorAlive || TargetWorld->DestroyActor(Spawned);
        State->SetBoolField(TEXT("destroyedAfterFailure"), bDestroyedAfterFailure);
        Ctx.SendError(ErrorCodes::ERR_EFFECT_NOT_ACTIVE,
            TEXT("Spawned Niagara component did not reach the requested active state"), State);
        return true;
    }

    UE_LOG(LogPinWrightSubsystem, Display,
           TEXT("spawn_niagara: Spawned actor '%s' (ID: %u)"),
           *Spawned->GetActorLabel(), Spawned->GetUniqueID());

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    AddActorVerification(Resp, Spawned);
    Resp->SetStringField(TEXT("systemPath"), System->GetPathName());
    Resp->SetStringField(TEXT("componentPath"), ComponentPath);
    Resp->SetBoolField(TEXT("systemAssigned"), bAssignedSystem);
    Resp->SetBoolField(TEXT("active"), bActive);
    Resp->SetBoolField(TEXT("autoDestroy"), bMeasuredAutoDestroy);
    Resp->SetBoolField(TEXT("autoDestroyMeasured"), bAutoDestroyMeasured);
    Resp->SetBoolField(TEXT("attached"), bAttached);
    if (Parent)
    {
        Resp->SetStringField(TEXT("attachmentParent"), Parent->GetPathName());
    }
    Resp->SetStringField(TEXT("compileStatus"), CompileStatus);
    Resp->SetBoolField(TEXT("compileWaited"), CompileWait.bWaited);
    Resp->SetNumberField(TEXT("compileWaitedMs"), CompileWait.WaitedSeconds * 1000.0);
    Resp->SetBoolField(TEXT("compileTimedOut"), CompileWait.bTimedOut);
    Resp->SetBoolField(TEXT("outstandingCompilationRequests"), bCompilePending);
    Ctx.SendSuccess(Resp);
    return true;
}
