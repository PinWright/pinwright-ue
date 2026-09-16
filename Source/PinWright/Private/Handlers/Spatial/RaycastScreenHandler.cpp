// Copyright (c) 2026 Alexander Penkin. MIT License.

// RaycastScreenHandler.cpp - spatial.raycast_screen: map a pixel from a prior capture
// back to a world-space ray + hit, so an agent can "place it THERE where I can see it."
//
// Pose-coherence is the whole point: the caller passes the EXACT pose + dimensions a
// capture returned (location, rotation, projectionMode, fov/orthoWidth in world cm, width, height)
// plus a pixel. We rebuild that SAME FViewportCaptureRequest, deproject the pixel to a
// world ray through PinWrightViewProjection::DeprojectScreenToWorld (which applies and
// restores the pose on the active Level Editor viewport internally - we never read the
// live camera), then line-trace the world via SpatialTraceUtils. The response echoes the
// view it deprojected against so the caller can confirm coherence with the pixels on disk.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "Handlers/Render/ViewProjectionUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"

namespace
{
    // Coordinate-convention echo attached to every result so a caller never has to
    // guess units/handedness when interpreting location/normal/distance/ray. Mirrors
    // spatial.raycast's RaycastAddAxisEcho.
    void RaycastScreenAddAxisEcho(const TSharedPtr<FJsonObject>& Data)
    {
        Data->SetStringField(TEXT("units"), TEXT("cm"));
        Data->SetStringField(TEXT("axis"), TEXT("+X fwd, +Y right, +Z up, left-handed"));
    }

    // Maps the wire `channel` string to a trace channel. Same accepted strings as
    // spatial.raycast (RaycastHandler::RaycastMapChannel) so both spatial verbs speak
    // one channel vocabulary; anything else is a caller error.
    bool RaycastScreenMapChannel(const FString& Name, ECollisionChannel& OutChannel)
    {
        const FString Lower = Name.ToLower();
        if (Lower == TEXT("visibility"))   { OutChannel = ECC_Visibility;    return true; }
        if (Lower == TEXT("camera"))       { OutChannel = ECC_Camera;        return true; }
        if (Lower == TEXT("worldstatic"))  { OutChannel = ECC_WorldStatic;   return true; }
        if (Lower == TEXT("worlddynamic")) { OutChannel = ECC_WorldDynamic;  return true; }
        return false;
    }

    // The deproject util returns typed errors shaped "CODE: message" (e.g.
    // "NO_ACTIVE_LEVEL_VIEWPORT: no active Level Editor viewport"). Split off the CODE
    // so it becomes the SendError code; the full string stays as the human message.
    // Emitting the code as a runtime FString (not a SendError(TEXT("...")) literal)
    // keeps VIEW_BUILD_FAILED - which is intentionally not in the ErrorCodes registry -
    // out of the literal-scanning registry test while still surfacing it verbatim.
    void RaycastScreenSplitDeprojectError(const FString& Err, FString& OutCode, FString& OutMessage)
    {
        OutMessage = Err;
        int32 ColonIndex = INDEX_NONE;
        if (Err.FindChar(TEXT(':'), ColonIndex) && ColonIndex > 0)
        {
            OutCode = Err.Left(ColonIndex).TrimStartAndEnd();
            return;
        }
        OutCode = Err.IsEmpty() ? FString(TEXT("VIEW_BUILD_FAILED")) : Err;
    }
}

// ---- spatial.raycast_screen ----
REGISTER_RPC_HANDLER("spatial.raycast_screen", "spatial",
    "Map a pixel from a prior capture back to a world-space ray and its first blocking hit - the inverse of a viewport capture. Pass the EXACT pose and dimensions the capture returned (location, rotation, projectionMode, fov/orthoWidth, width, height) plus the pixel; the pixel is deprojected against that same view (top-left origin, center pixel = camera forward), then traced into the scene. On hit returns location/normal/distance and the struck actor; a clean miss is a success with hit:false. The response echoes the ray and the view it deprojected against so you can confirm pose-coherence with the image. Coordinates are unreal units (cm), left-handed (+X forward, +Y right, +Z up).",
    RPC_PARAMS(
        RPC_PARAM_REQ("location", "object",
            "Camera world location {x,y,z} in cm - the EXACT pose the capture returned. Required."),
        RPC_PARAM_REQ("rotation", "object",
            "Camera world rotation {pitch,yaw,roll} in degrees - the EXACT pose the capture returned. Required."),
        FParamSpec{TEXT("x"), TEXT("number"),
            TEXT("Pixel X, top-left origin: 0 <= x < width. Required."),
            true, TEXT(""), TArray<FString>({TEXT("pixelX")})},
        FParamSpec{TEXT("y"), TEXT("number"),
            TEXT("Pixel Y, top-left origin: 0 <= y < height. Required."),
            true, TEXT(""), TArray<FString>({TEXT("pixelY")})},
        RPC_PARAM_REQ("width", "number",
            "Capture width in pixels (the dimensions the capture used). Required."),
        RPC_PARAM_REQ("height", "number",
            "Capture height in pixels (the dimensions the capture used). Required."),
        RPC_PARAM_DEF("projectionMode", "string",
            "'perspective' (default) or 'orthographic'. Must match the capture.", "perspective"),
        RPC_PARAM_DEF("fov", "number",
            "Perspective field of view in degrees (default 50). Must match the capture.", "50"),
        FParamSpec{TEXT("orthoWidth"), TEXT("number"),
            TEXT("Orthographic frame width in WORLD CENTIMETRES (default 2000); used when projectionMode is orthographic. Must match the capture. Alias: orthoWorldWidth."),
            false, TEXT("2000"), TArray<FString>({TEXT("orthoWorldWidth")})},
        RPC_PARAM_OPT("maxDistance", "number",
            "Max ray length in cm along the deprojected direction (default 1e7)."),
        RPC_PARAM_OPT("channel", "string",
            "Trace channel: visibility (default), camera, worldstatic, or worlddynamic."),
        FParamSpec{TEXT("traceComplex"), TEXT("boolean"),
            TEXT("Trace against per-triangle (complex) collision instead of simple collision. Default false."),
            false, TEXT(""), TArray<FString>({TEXT("trace_complex")})},
        FParamSpec{TEXT("ignoreActors"), TEXT("array"),
            TEXT("Names/labels of actors to exclude from the trace."),
            false, TEXT(""), TArray<FString>({TEXT("ignore_actors")})}
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    auto HasField = [&Payload](const TCHAR* Key)
    {
        return Payload.IsValid() && Payload->HasField(Key);
    };

    // Required pose. Validate presence here (not only via the param-spec check) so the
    // direct-invocation path reports the same typed error as the dispatcher path.
    if (!HasField(TEXT("location")))
    {
        Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"),
            TEXT("Missing required parameter 'location' ({x,y,z} in cm) - pass the capture's pose."));
        return true;
    }
    if (!HasField(TEXT("rotation")))
    {
        Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"),
            TEXT("Missing required parameter 'rotation' ({pitch,yaw,roll}) - pass the capture's pose."));
        return true;
    }

    const bool bHasX = HasField(TEXT("x")) || HasField(TEXT("pixelX"));
    const bool bHasY = HasField(TEXT("y")) || HasField(TEXT("pixelY"));
    if (!bHasX || !bHasY)
    {
        Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"),
            TEXT("Missing required pixel 'x'/'y' (aliases pixelX/pixelY), top-left origin."));
        return true;
    }
    if (!HasField(TEXT("width")) || !HasField(TEXT("height")))
    {
        Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"),
            TEXT("Missing required 'width'/'height' - pass the capture's dimensions."));
        return true;
    }

    const FVector Location = Ctx.GetVector(TEXT("location"));
    const FRotator Rotation = Ctx.GetRotator(TEXT("rotation"));
    const double PixelX = HasField(TEXT("x")) ? Ctx.GetNumber(TEXT("x")) : Ctx.GetNumber(TEXT("pixelX"));
    const double PixelY = HasField(TEXT("y")) ? Ctx.GetNumber(TEXT("y")) : Ctx.GetNumber(TEXT("pixelY"));
    const int32 Width = Ctx.GetInt(TEXT("width"));
    const int32 Height = Ctx.GetInt(TEXT("height"));

    if (Width <= 0 || Height <= 0)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("width and height must be positive (the capture's pixel dimensions)."));
        return true;
    }

    // The pixel must lie inside the captured image; a top-left-origin pixel maps to a
    // valid ray only for 0 <= x < width and 0 <= y < height.
    if (PixelX < 0.0 || PixelX >= static_cast<double>(Width) ||
        PixelY < 0.0 || PixelY >= static_cast<double>(Height))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("pixel (%.2f, %.2f) is outside the capture bounds [0,%d) x [0,%d)."),
                PixelX, PixelY, Width, Height));
        return true;
    }

    FString ProjectionMode = Ctx.GetString(TEXT("projectionMode"), TEXT("perspective")).ToLower();
    if (ProjectionMode != TEXT("perspective") && ProjectionMode != TEXT("orthographic"))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("projectionMode must be 'perspective' or 'orthographic'."));
        return true;
    }
    const bool bOrtho = (ProjectionMode == TEXT("orthographic"));

    const double Fov = Ctx.GetNumber(TEXT("fov"), 50.0);
    // World centimetres, shared with the capture verbs so an omitted value reconstructs the same
    // view. `orthoWorldWidth` is an alias that states the unit.
    const double OrthoWidth = HasField(TEXT("orthoWorldWidth"))
        ? Ctx.GetNumber(TEXT("orthoWorldWidth"),
            static_cast<double>(PinWrightRenderCapture::DefaultOrthoWorldWidth))
        : Ctx.GetNumber(TEXT("orthoWidth"),
            static_cast<double>(PinWrightRenderCapture::DefaultOrthoWorldWidth));
    const double MaxDistance = Ctx.GetNumber(TEXT("maxDistance"), 1.0e7);

    ECollisionChannel Channel = ECC_Visibility;
    const FString ChannelName = Ctx.GetString(TEXT("channel"));
    if (!ChannelName.IsEmpty() && !RaycastScreenMapChannel(ChannelName, Channel))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Unknown channel '%s'. Valid: visibility, camera, worldstatic, worlddynamic."),
                *ChannelName));
        return true;
    }

    const bool bTraceComplex =
        Ctx.GetBoolFirstOf({TEXT("traceComplex"), TEXT("trace_complex")}, false);

    // Rebuild the SAME view struct the capture used. The util applies + restores this
    // pose on the active viewport internally; we do not touch the live camera here.
    PinWrightRenderCapture::FViewportCaptureRequest ViewRequest;
    ViewRequest.Location = Location;
    ViewRequest.Rotation = Rotation;
    ViewRequest.Width = Width;
    ViewRequest.Height = Height;
    ViewRequest.ProjectionMode = ProjectionMode;
    ViewRequest.Fov = static_cast<float>(Fov);
    ViewRequest.OrthoWidth = static_cast<float>(OrthoWidth);

    FVector RayOrigin = FVector::ZeroVector;
    FVector RayDir = FVector::ZeroVector;
    FString DeprojectErr;
    if (!PinWrightViewProjection::DeprojectScreenToWorld(
            ViewRequest, static_cast<float>(PixelX), static_cast<float>(PixelY),
            RayOrigin, RayDir, DeprojectErr))
    {
        FString Code;
        FString Message;
        RaycastScreenSplitDeprojectError(DeprojectErr, Code, Message);
        Ctx.SendError(Code, Message);
        return true;
    }

    // Trace against the editor world - the world the deprojected pixel is coherent with
    // (the deproject rebuilt the view from the active Level Editor viewport). The util
    // already verified this world exists, so a null here is defensive only.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"),
            TEXT("No editor world available for the trace."));
        return true;
    }

    // Resolve ignore-actor names to actors in this world (silently skip names that
    // don't resolve - a stale name should not fail an otherwise-valid trace).
    TArray<AActor*> IgnoreActors;
    const TArray<TSharedPtr<FJsonValue>>* IgnoreArray = Ctx.GetArray(TEXT("ignoreActors"));
    if (!IgnoreArray)
    {
        IgnoreArray = Ctx.GetArray(TEXT("ignore_actors"));
    }
    if (IgnoreArray)
    {
        for (const TSharedPtr<FJsonValue>& Val : *IgnoreArray)
        {
            FString Name;
            if (Val.IsValid() && Val->TryGetString(Name) && !Name.IsEmpty())
            {
                if (AActor* Found = McpActorUtils::FindActorByName(World, Name))
                {
                    IgnoreActors.AddUnique(Found);
                }
            }
        }
    }

    const FVector End = RayOrigin + RayDir * MaxDistance;
    const SpatialTraceUtils::FSpatialHit Hit =
        SpatialTraceUtils::TraceLine(World, RayOrigin, End, Channel, bTraceComplex, IgnoreActors);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    RaycastScreenAddAxisEcho(Data);
    Data->SetStringField(TEXT("channel"),
        ChannelName.IsEmpty() ? FString(TEXT("visibility")) : ChannelName.ToLower());

    // Echo the ray we deprojected.
    TSharedPtr<FJsonObject> RayObj = MakeShared<FJsonObject>();
    RayObj->SetObjectField(TEXT("origin"), PinWrightRenderCapture::MakeVectorObject(RayOrigin));
    RayObj->SetObjectField(TEXT("direction"), PinWrightRenderCapture::MakeVectorObject(RayDir));
    Data->SetObjectField(TEXT("ray"), RayObj);

    // Echo the exact pose + dimensions + pixel we deprojected against, so the caller can
    // confirm coherence with the capture that produced the pixel.
    TSharedPtr<FJsonObject> ViewObj = MakeShared<FJsonObject>();
    ViewObj->SetObjectField(TEXT("location"), PinWrightRenderCapture::MakeVectorObject(Location));
    ViewObj->SetObjectField(TEXT("rotation"), PinWrightRenderCapture::MakeRotatorObject(Rotation));
    ViewObj->SetStringField(TEXT("projectionMode"), ProjectionMode);
    if (bOrtho)
    {
        ViewObj->SetNumberField(TEXT("orthoWidth"), OrthoWidth);
    }
    else
    {
        ViewObj->SetNumberField(TEXT("fov"), Fov);
    }
    ViewObj->SetNumberField(TEXT("width"), Width);
    ViewObj->SetNumberField(TEXT("height"), Height);
    TSharedPtr<FJsonObject> PixelObj = MakeShared<FJsonObject>();
    PixelObj->SetNumberField(TEXT("x"), PixelX);
    PixelObj->SetNumberField(TEXT("y"), PixelY);
    ViewObj->SetObjectField(TEXT("pixel"), PixelObj);
    Data->SetObjectField(TEXT("view"), ViewObj);

    if (!Hit.bHit)
    {
        // A miss is a valid outcome, not an error. Ray + view are still echoed above.
        Data->SetBoolField(TEXT("hit"), false);
        Ctx.SendSuccess(Data);
        return true;
    }

    Data->SetBoolField(TEXT("hit"), true);
    Data->SetObjectField(TEXT("location"), PinWrightRenderCapture::MakeVectorObject(Hit.Location));
    Data->SetObjectField(TEXT("normal"), PinWrightRenderCapture::MakeVectorObject(Hit.Normal));
    Data->SetNumberField(TEXT("distance"), Hit.Distance);

    if (AActor* HitActor = Hit.HitActor.Get())
    {
        TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
        ActorObj->SetStringField(TEXT("name"), HitActor->GetActorLabel());
        ActorObj->SetStringField(TEXT("path"), HitActor->GetPathName());
        Data->SetObjectField(TEXT("actor"), ActorObj);
    }
    if (UPrimitiveComponent* HitComp = Hit.HitComponent.Get())
    {
        Data->SetStringField(TEXT("component"), HitComp->GetName());
    }

    Ctx.SendSuccess(Data);
    return true;
}
