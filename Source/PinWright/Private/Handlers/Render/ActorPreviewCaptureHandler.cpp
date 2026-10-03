// Copyright (c) 2026 Alexander Penkin. MIT License.

// render.capture_actor_preview. The capture itself lives in ActorPreviewCapture.cpp (a utility
// translation unit) so the GPU test calls the same production boundary this handler does.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Render/ActorPreviewCapture.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/OrthoTileCaptureUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/JsonBuilders.h"
#include "Utils/RenderingAvailability.h"

// ---- render.capture_actor_preview ----
// `previewScene` is declared only to be REFUSED: the subject is drawn through a show-only list, so a
// rig's floor and environment could never appear, and an actorPath subject has no FPreviewScene.
// Declared because the shared ParseOffscreenCaptureRequest reads it; refused with a reason rather
// than left to the dispatcher's bare UNKNOWN_PARAMS.
// Tick-unsafe (family B in Dispatch/SafePoint.cpp): the render.capture_mesh scene-capture probe -
// synchronous render-thread flush and readback - plus a transient actor spawn and a private
// FPreviewScene build/destroy (ActorPreviewCapture.cpp).
REGISTER_RPC_HANDLER_TICK_UNSAFE("render.capture_actor_preview", "render", "Capture ONE actor, framed to its own component bounds, as an exact-size PNG without placing anything in the level. Pass `classPath` to spawn a transient instance into a private preview world that is destroyed before the call returns, or `actorPath` to draw an existing actor (editor level or PIE world) in its own world. Only that actor's primitives are drawn (show-only list), under its world's lights; nothing is spawned into, moved in or dirtied in the user's map.",
    RPC_PARAMS(
        RPC_PARAM_OPT("classPath", "classref", "Actor class to spawn transient: UClass name, /Script path, or Blueprint asset path. Exactly one of classPath / actorPath."),
        RPC_PARAM_OPT("actorPath", "path", "Existing actor: object path, internal name or unique label. The PIE world is searched first when a session is running, then the editor world. Exactly one of classPath / actorPath."),
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/ActorPreview. The .png extension is appended if missing."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Default 768."),
        RPC_PARAM_OPT("location", "object", "Camera location {x,y,z}. Omit location and rotation to frame the actor's bounds."),
        RPC_PARAM_OPT("rotation", "object", "Camera rotation {pitch,yaw,roll}. Omit location and rotation to frame the actor's bounds."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'."),
        RPC_PARAM_OPT("fov", "number", "Perspective horizontal field of view in degrees. Default 50."),
        FParamSpec{TEXT("orthoWidth"), TEXT("number"),
            TEXT("Orthographic frame width in world centimetres. When omitted, it is fitted to the actor bounds."),
            false, TEXT(""), TArray<FString>({TEXT("orthoWorldWidth")})},
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC),
        RPC_PARAM_DEF("azimuth", "number", "Framing orbit azimuth in degrees when no explicit pose is given. Default 0.", "0"),
        RPC_PARAM_DEF("elevation", "number", "Framing orbit elevation in degrees, [-90, 90]. Default 20.", "20"),
        RPC_PARAM_DEF("measureCoverage", "boolean", "Draw once more with no primitive at all and report the fraction of pixels the actor changed. Defaults to true.", "true"),
        RPC_PARAM_DEF("padding", "number", "Bounds-fit margin multiplier. Default 1.25.", "1.25"),
        RPC_PARAM_OPT("previewScene", "object", "NOT SUPPORTED on this verb; any value is refused with INVALID_ARGUMENT. Only the subject's primitives are drawn (show-only list), so a rig's floor and environment could never appear, and an actorPath subject is lit by its own world. Use render.capture_mesh for a rigged mesh capture.")
    ))
{
    if (!PinWrightRendering::RequireRenderer(Ctx))
    {
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const FString ClassPath = Ctx.GetString(TEXT("classPath"));
    const FString ActorPath = Ctx.GetString(TEXT("actorPath"));
    if (ClassPath.IsEmpty() == ActorPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("Pass exactly one of classPath (spawn a transient instance) or actorPath (an existing actor)."));
        return true;
    }

    if (Payload.IsValid() && Payload->HasField(TEXT("previewScene")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("render.capture_actor_preview does not accept previewScene: the subject is drawn "
                 "show-only, so a rig's floor and environment never appear, and an actorPath "
                 "subject is lit by its own world. Omit it, or use render.capture_mesh for a "
                 "rigged mesh capture."));
        return true;
    }

    PinWrightActorPreviewCapture::FActorCaptureRequest Request;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightRenderCapture::ParseOffscreenCaptureRequest(
            Payload, Request.Capture, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }
    if (Request.Capture.ViewMode.bRequested
        && !PinWrightOrthoTiles::IsViewModeReachableOnSceneCapture(
            Request.Capture.ViewMode.ViewMode, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage.Replace(
            TEXT("render.capture_ortho_tiles"), TEXT("render.capture_actor_preview")));
        return true;
    }
    Request.Padding = static_cast<float>(Ctx.GetNumber(TEXT("padding"), 1.25));
    Request.OrbitAzimuth = static_cast<float>(Ctx.GetNumber(TEXT("azimuth"), 0.0));
    Request.OrbitElevation = static_cast<float>(Ctx.GetNumber(TEXT("elevation"), 20.0));
    if (!FMath::IsFinite(Request.Padding) || Request.Padding <= 0.0f
        || !FMath::IsFinite(Request.OrbitAzimuth)
        || !FMath::IsFinite(Request.OrbitElevation)
        || Request.OrbitElevation < -90.0f || Request.OrbitElevation > 90.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("padding must be finite and > 0, azimuth finite, elevation finite in [-90, 90]"));
        return true;
    }
    Request.bUseOrbitPose = !Request.Capture.bLocationProvided && !Request.Capture.bRotationProvided;
    Request.bAutoFrameOrthoWidth = !Payload.IsValid()
        || (!Payload->HasField(TEXT("orthoWidth")) && !Payload->HasField(TEXT("orthoWorldWidth")));
    Request.bMeasureCoverage = Ctx.GetBool(TEXT("measureCoverage"), true);

    if (!ClassPath.IsEmpty())
    {
        Request.ActorClass = ResolveClassByName(ClassPath);
        if (!Request.ActorClass)
        {
            Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
                FString::Printf(TEXT("Class not found: %s"), *ClassPath));
            return true;
        }
    }
    else if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, ActorPath, Request.Actor))
    {
        return true;
    }

    PinWrightActorPreviewCapture::FActorCaptureOutput Output;
    if (!PinWrightActorPreviewCapture::CaptureActorPreview(Request, Output, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWrightCameraFrame::AddShotFields(Output.Capture, Output.ResolvedRequest, Result);
    TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
    ActorObj->SetStringField(TEXT("class"), Output.ActorClass);
    ActorObj->SetStringField(TEXT("name"), Output.ActorName);
    ActorObj->SetStringField(TEXT("label"), Output.ActorLabel);
    ActorObj->SetStringField(TEXT("path"), Output.ActorPath);
    ActorObj->SetStringField(TEXT("world"), Output.World);
    Result->SetObjectField(TEXT("actor"), ActorObj);
    Result->SetBoolField(TEXT("spawnedTransient"), Output.bSpawnedTransient);
    if (Output.bSpawnedTransient)
    {
        Result->SetBoolField(TEXT("transientDestroyed"), Output.bTransientDestroyed);
    }
    TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
    BoundsObj->SetObjectField(TEXT("origin"), JsonBuilders::BuildVectorJson(Output.BoundsOrigin));
    BoundsObj->SetNumberField(TEXT("radius"), Output.BoundsRadius);
    Result->SetObjectField(TEXT("bounds"), BoundsObj);
    if (Output.SubjectCoverage.IsSet())
    {
        Result->SetNumberField(TEXT("subjectCoverage"), Output.SubjectCoverage.GetValue());
    }
    if (Request.Capture.Exposure.WantsPin())
    {
        TSharedPtr<FJsonObject> ExposureObj = MakeShared<FJsonObject>();
        ExposureObj->SetBoolField(TEXT("pinned"), Output.Capture.bExposurePinned);
        ExposureObj->SetNumberField(TEXT("ev100Applied"), Output.Capture.Ev100Applied);
        Result->SetObjectField(TEXT("exposure"), ExposureObj);
    }
    if (Output.Capture.bPreviewSceneCaptureIncomplete)
    {
        // Omitted on the healthy path, like capture_mesh's viewport.previewScene.captureIncomplete.
        Result->SetBoolField(TEXT("skyCaptureIncomplete"), true);
        Result->SetStringField(TEXT("skyCaptureWarning"),
            TEXT("A sky capture was still queued after the preview world's sky/reflection update, "
                 "usually because assets were async-compiling, so these pixels may lack the sky "
                 "lighting. The queue is process-wide, so another window's sky light can also "
                 "raise this. Re-shoot once compilation has settled."));
    }
    Result->SetObjectField(TEXT("viewport"),
        PinWrightRenderCapture::MakeViewportInfoObject(Output.Capture));
    Ctx.SendSuccess(Result);
    return true;
}
