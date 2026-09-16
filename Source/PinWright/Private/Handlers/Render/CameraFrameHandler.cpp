// Copyright (c) 2026 Alexander Penkin. MIT License.

// Camera-capture RPCs that give an AI agent multi-angle "eyes" on the scene by
// reusing the live-viewport capture engine (PreviewViewportCaptureUtils). Both
// verbs compute their own camera poses (spherical placement + bounds-fit distance),
// hand the pose list to PinWrightPoseCapture::CaptureCameraPoses (which moves the
// real editor camera, takes a throwaway warm-up frame and restores everything via
// scope-exit), and return the PNG path (plus optional inline base64) per shot.
//
//   camera.frame_actor  — one framed shot of a single subject from an azimuth/elevation.
//   camera.orbit_shots  — a set of shots around a subject or world point (default: a
//                         3/4 perspective plus top/front/side orthographic; views:"sides"
//                         for the full six axis-aligned set).
//
// The viewport is the active Level Editor one for a placed actor or a world point, and the
// subject's own asset-editor preview when a `subject` names an asset.
//
// Orthographic shots snap their azimuth/elevation onto a cardinal world axis first: the editor
// derives an orthographic view matrix from the viewport TYPE and ignores the camera rotation, so
// only the six axis views exist. The shot echoes the angles it actually used.
//
// SUBJECTS. Both verbs also take an optional `subject` object, which is what lets them reach an
// asset-editor preview (static mesh, skeletal mesh, animation, Niagara) instead of only the Level
// Editor viewport. The subject is resolved through the provider registry in CaptureSubject.h; the
// camera maths above it is unchanged, because a pose list plus a bounding sphere is all either verb
// ever needed from its target. The legacy `actorName` and `point`+`radius` spellings keep their own
// resolution code and their own refusals, so no call shape that already shipped moves.
//
// `subject.radius` MEANS TWO DIFFERENT THINGS IN THIS FILE, on purpose. It is parsed once
// (CaptureSubject.cpp) into one FSubjectRequest::Radius, and then camera.orbit_shots reads it as a
// CAMERA DISTANCE (a bare world point has no size, so the orbit radius IS its framing extent -- the
// rule the legacy point path has always used) while camera.frame_actor reads it as a SPHERE RADIUS
// that feeds the bounds its fit is solved FROM. For subject:{kind:"world", point, radius:R} at fov
// 50 that puts orbit's camera at R and frame_actor's at roughly 2.5*R. Both meanings are correct
// for their verb and neither can move without changing pixels callers already have, so the code
// keeps both; PinWrightCameraFrame::ResolveCameraDistance is the one place that spells the
// precedence, and its declaration carries the same warning for anyone arriving through the header.
//
// PARAMETER PARITY. Both verbs call ComputeFitDistance(Radius, Fov, Padding) and each used to
// expose only one half of it: orbit took an explicit `radius` and welded the margin at 1.15,
// frame_actor took `padding` and had no distance override. Both halves are now on both verbs
// (`padding` on orbit, `distance` on frame_actor), with the shipped defaults unchanged, so an
// existing call gets byte-identical pixels. camera.animation_shots already exposed both and is
// the parity reference rather than a target.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
// The pluggable subject resolver: one acquisition step (viewport client + bounds + optional time
// setter) for every subject kind, registered per kind rather than switched on here.
#include "Handlers/Render/CaptureSubject.h"
// The base multi-shot primitive camera.orbit_shots now runs on: a list of free cameras, one
// restore path, one honest truncation report, one warm-up frame.
#include "Handlers/Render/PoseListCapture.h"
// The scoped preview-scene rig: parse the `previewScene` pin here, hand it to the capture
// primitive, and let the guard apply/restore/measure it. Nothing in this file writes a light.
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/JsonUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Slate/SceneViewport.h"

// The shot-planning helpers this file used to define inline (ComputeFitDistance,
// PlaceOrbitCamera, SnapOrbitAnglesToOrthographicAxis, AddShotFields, ...) now live in
// Handlers/Render/CameraShotPlanUtils.h under the SAME namespace name, so every call site
// below is unchanged. They moved out when camera.animation_shots and
// render.capture_animation_preview needed the identical maths and the identical shot
// serialization: a private copy per handler is how one of them starts framing differently, or
// stops emitting imageStats after the others gained it.

// The `subject` argument, spelled once for both verbs in this file so their two schemas cannot
// drift apart. The nested shape itself is documented in the wiki overlay, which is what makes an
// object-shaped parameter discoverable at all (Tests/Infra/TestFoliageNestedInputSchemaDocs.cpp).
//
// FILE-SCOPED NAME on purpose. Unity merges translation units and a macro defined in one .cpp is
// visible to the next, so a generic PINWRIGHT_CAPTURE_SUBJECT_PARAM_DESC defined here and again in
// another capture handler would be a redefinition. When a second verb family needs the same text,
// the right move is to lift it into CaptureSubject.h next to the shape it describes, not to
// re-#define it per file.
#define PINWRIGHT_CAMERA_SUBJECT_PARAM_DESC \
    "What to point the camera at, as an object: {kind, path, name, point, radius, animation, " \
    "closeAfterCapture}. `kind` is one of world|actor|staticMesh|skeletalMesh|animation|niagara " \
    "and is INFERRED when omitted from the single identifying key present; a payload naming two " \
    "of them is refused with both key names in the message. An asset kind opens that asset's " \
    "editor preview and captures ITS viewport instead of the Level Editor's, closing the window " \
    "afterwards unless closeAfterCapture is false. Omit it and the legacy actorName / point+radius " \
    "spellings below behave exactly as they always have; supplying both is refused rather than " \
    "ranked. The `subject` response block is present only when a subject was actually resolved."

// ---- caller-supplied `subject` -> a viewport to shoot through and a sphere to frame ----
//
// File-unique NAMED namespace, never anonymous: Unity merges these translation units, and an
// anonymous helper here would collide with a same-named one in a sibling Render/*.cpp -- the same
// reason CameraShotPlanUtils.h is a named namespace rather than an anonymous one.
//
// WHY THE LEGACY PATHS ARE NOT ROUTED THROUGH THE RESOLVER. `actorName` and `point`+`radius` keep
// their own resolution code in the handler bodies below, unchanged: their refusals
// (ACTOR_NOT_FOUND for an unresolvable name, the radius-required message for a bare point) are
// asserted by the regression floor and are the two shapes every existing caller uses. A `subject`
// object is the new surface and is the only thing that reaches the provider registry, so nothing
// that already shipped can move behind a resolver defect.
namespace PinWrightCameraFrameSubject
{
    // What either verb needs out of a resolved subject, plus whether there was one at all.
    //
    // NON-COPYABLE by construction: FResolvedSubject deletes its copy constructor because a copy
    // would carry the provider's release responsibility twice. Declared as a handler-body local
    // and only ever passed by reference.
    struct FSubjectBinding
    {
        // A `subject` object was on the wire.
        bool bSupplied = false;
        // ... and the registry resolved it. The `subject` RESPONSE block is emitted if and only if
        // BOTH are true. An empty block on a verb the caller gave no subject to is exactly the
        // absence assertion nobody knows about that this design is meant to avoid.
        bool bResolved = false;

        // Kept, not discarded: `subject.radius` is the one parsed field a camera verb still needs
        // after the resolve, because for a bare world point the orbit radius and the framing extent
        // are the same number and only the request carries the caller's intent about it.
        PinWrightCaptureSubject::FSubjectRequest Request;
        PinWrightCaptureSubject::FResolvedSubject Resolved;
    };

    inline bool HasSubjectField(const TSharedPtr<FJsonObject>& Payload)
    {
        return Payload.IsValid() && Payload->HasField(TEXT("subject"));
    }

    // Parse the wire shape and hand it to the provider registry, in one step, filling the error
    // pair for the caller to send. On success OutBinding.Resolved carries a viewport client, a
    // scene viewport and a bounding sphere; on failure the provider's release has already run and
    // the object is marked released, so the caller cannot double-release it.
    //
    // The time setter is deliberately taken and DISCARDED here. Neither camera verb has a time
    // axis -- camera.animation_shots is the verb that crosses one -- so FCameraPose::SubjectTimeSeconds
    // stays unset for every pose these two build, and a subject kind with no time axis never sees
    // the typed refusal because nothing ever asks it for an instant.
    inline bool BindSubject(const TSharedPtr<FJsonObject>& Payload, FSubjectBinding& OutBinding,
        FString& OutErrCode, FString& OutErrMsg)
    {
        OutBinding.bSupplied = true;

        if (!PinWrightCaptureSubject::ParseSubject(Payload, OutBinding.Request,
                OutErrCode, OutErrMsg))
        {
            return false;
        }

        PinWrightCaptureSubject::FSubjectTimeSetter UnusedTimeSetter;
        if (!PinWrightCaptureSubject::Resolve(OutBinding.Request, OutBinding.Resolved,
                UnusedTimeSetter, OutErrCode, OutErrMsg))
        {
            return false;
        }

        // Stated rather than assumed: the contract says these are never null on success, and a
        // provider that broke it would otherwise crash inside the capture util instead of
        // reporting which subject could not be acquired.
        if (OutBinding.Resolved.ViewportClient == nullptr || !OutBinding.Resolved.SceneViewport.IsValid())
        {
            OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
            OutErrMsg = TEXT("The subject resolved but produced no usable preview viewport, so there "
                             "is nothing to capture. This is a provider defect, not a bad argument.");
            return false;
        }

        OutBinding.bResolved = true;
        return true;
    }

    // The bounds-fit radius for a resolved subject, in the same shape the actor path produces: at
    // least 1 cm so ComputeFitDistance and ComputeOrthoWorldWidth never divide a degenerate span.
    // A subject that reports no bounds (BoundsRadius 0) still frames, at the floor, and its
    // `framing` block reports evaluated=false rather than a guess.
    inline float FitRadius(const PinWrightCaptureSubject::FResolvedSubject& Resolved)
    {
        return FMath::Max(static_cast<float>(Resolved.BoundsRadius), 1.0f);
    }

    // Everything that is not the level itself or an actor in it is shot through an asset editor's
    // preview viewport rather than the Level Editor one. One predicate for both verbs, and it
    // reads FALSE for the legacy `actorName` / `point` spellings too, because a default-constructed
    // binding has bSupplied=false -- which is correct: those always shoot the level viewport.
    inline bool IsAssetSubject(const FSubjectBinding& Binding)
    {
        return Binding.bSupplied &&
            Binding.Request.Kind != PinWrightCaptureSubject::ESubjectKind::World &&
            Binding.Request.Kind != PinWrightCaptureSubject::ESubjectKind::Actor;
    }

    // The refusal a camera verb sends when a `previewScene` rig was asked for against the Level
    // Editor viewport.
    //
    // ERR_UNSUPPORTED_ASSET_EDITOR rather than a new code (no new codes; the request named a
    // viewport that has no advanced preview scene, which is exactly what that code already says).
    // REFUSED rather than accepted-and-ignored: "the call succeeded and nothing happened" is the
    // shape this parameter exists to remove, and it is the same answer render.capture_annotated
    // gives on the same condition, so the two verbs cannot come to mean different things.
    //
    // FLevelEditorViewportClient passes nullptr for its preview scene (UE 5.8
    // Editor/UnrealEd/Private/LevelEditorViewport.cpp:2335), so GetPreviewScene() is null there:
    // there is nothing to write and nothing to restore, not merely nothing worth writing.
    inline FString PreviewSceneRigLevelViewportMessage(const TCHAR* WhatThisVerbDoes)
    {
        return FString::Printf(
            TEXT("`previewScene` rigs the key light, sky and backdrop of an ASSET EDITOR preview "
                 "scene, and this call %s the Level Editor viewport, which has none - a level is "
                 "lit by the actors in it. Name an asset subject (subject: {kind: 'staticMesh', "
                 "path: '/Game/...'}) to rig the preview viewport that opens, or drop "
                 "`previewScene` to shoot the level as it is already lit."),
            WhatThisVerbDoes);
    }
}

// ---- camera.frame_actor ----
REGISTER_RPC_HANDLER("camera.frame_actor", "camera",
    "Frame one subject from a given azimuth/elevation and capture a PNG, fitting its bounds to the FOV. A placed actor by default (actorName, the live Level Editor viewport); pass `subject` to frame a static mesh, skeletal mesh, animation or Niagara asset in its own editor preview instead.",
    RPC_PARAMS(
        // OPTIONAL rather than required since `subject` can stand in for it. The dispatcher's
        // required-param gate can only speak for ONE slot, and there are now two ways to name a
        // target, so the "no target at all" refusal moved into the body -- same INVALID_ARGUMENT
        // code, message extended to name `subject` alongside the actor keys.
        ParamAliasUtils::MakeAliasParamSpec(TEXT("actorName"), TEXT("string"),
            TEXT("Actor to frame. Accepts a display label, internal object name, or object path. Provide this OR subject." ACTORNAME_COLLISION_STEER),
            /*bRequired=*/false, ActorNameParamUtils::ActorNameKeys()),
        RPC_PARAM_OPT("subject", "object", PINWRIGHT_CAMERA_SUBJECT_PARAM_DESC),
        RPC_PARAM_OPT("azimuth", "number", "Horizontal orbit angle in degrees around the actor (default 45)."),
        RPC_PARAM_OPT("elevation", "number", "Vertical orbit angle in degrees above the horizon (default 30)."),
        RPC_PARAM_DEF("padding", "number", PINWRIGHT_FIT_PADDING_PARAM_DESC " Default 1.15.", "1.15"),
        // The other half of ComputeFitDistance, which this verb welded until now. camera.orbit_shots
        // has exposed the same override as `radius` since it shipped, and camera.animation_shots
        // exposes both inputs; this verb exposed only the margin, so a caller who knew the exact
        // distance they wanted had to solve backwards through the fit maths to find the padding
        // that produced it. Named `distance` rather than `radius` because this verb takes ONE shot
        // and has no orbit for a radius to be the radius OF.
        RPC_PARAM_OPT("distance", "number", "Explicit camera distance from the subject, in world centimetres. Skips the bounds fit entirely, so `padding` and `fov` no longer decide where the camera sits (fov still sets the perspective lens angle, and on an orthographic shot `padding` still decides orthoWidth, which is the frame extent rather than the distance). A value <= 0 is treated as absent and the fit runs, matching camera.orbit_shots' `radius`. Omit for the bounds fit, which is unchanged."),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees (default 50)."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels (default 768)."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels (default 768)."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("hideEditorSprites", "boolean", PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC),
        RPC_PARAM_OPT("previewScene", "object", PINWRIGHT_PREVIEW_SCENE_PARAM_DESC " ASSET SUBJECTS ONLY on this verb. It frames the Level Editor viewport for `actorName`, and for a subject of kind world or actor, and a level viewport has no preview scene to rig - a level is lit by its own actors. Passing it on any of those is refused with UNSUPPORTED_ASSET_EDITOR rather than accepted and quietly ignored."),
        RPC_PARAM_OPT("inline", "boolean", "When true, also embed base64 PNG bytes in a 'base64' field (default false).")
    ))
{
    using namespace PinWrightCameraFrame;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bSubjectProvided = PinWrightCameraFrameSubject::HasSubjectField(Payload);

    // ---- target, legacy spelling ----
    // Deliberately still the FIRST thing this verb does, and unchanged: an empty payload keeps
    // returning INVALID_ARGUMENT and an unresolvable name keeps returning ACTOR_NOT_FOUND, both
    // ahead of every other validation. The only difference is that `subject` now also satisfies
    // "name a target", so the refusal names it.
    FString ActorName;
    AActor* Actor = nullptr;
    if (bSubjectProvided)
    {
        // Two answers to "what am I shooting", and ranking one over the other for the caller is
        // how a verb ends up with parameters that contradict each other depending on a mode flag.
        // Refused naming BOTH keys, so the message says which one to remove rather than leaving
        // the caller to discover which spelling won by looking at the picture.
        const FString LegacyName = ActorNameParamUtils::ResolveActorName(Ctx);
        if (!LegacyName.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(
                    TEXT("`subject` and `actorName` both name a target ('%s'); pass one. Drop "
                         "`actorName` to frame the subject, or drop `subject` to frame the placed "
                         "actor."), *LegacyName));
            return true;
        }
    }
    else
    {
        ActorName = ActorNameParamUtils::ResolveActorName(Ctx);
        if (ActorName.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Provide a target: one of %s, or a `subject` object."),
                    *FString::Join(ActorNameParamUtils::ActorNameKeys(), TEXT(", "))));
            return true;
        }

        Actor = McpActorUtils::FindActorByName(nullptr, ActorName);
        if (!Actor)
        {
            Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                FString::Printf(TEXT("Actor not found: %s"), *ActorName));
            return true;
        }
    }

    const float Azimuth = static_cast<float>(Ctx.GetNumber(TEXT("azimuth"), 45.0));
    const float Elevation = static_cast<float>(Ctx.GetNumber(TEXT("elevation"), 30.0));
    // GDefaultFitPadding, not a literal 1.15 -- the two camera verbs in this file used to spell
    // the same margin twice, one of them as a welded constexpr with no way to reach it.
    const bool bPaddingProvided = Payload.IsValid() && Payload->HasField(TEXT("padding"));
    const float Padding = static_cast<float>(Ctx.GetNumber(TEXT("padding"), GDefaultFitPadding));
    // The explicit camera-distance override. Absent -- or non-positive, which is treated as
    // absent the way camera.orbit_shots' `radius` already is -- means the bounds fit runs and
    // this verb behaves exactly as it always has.
    const bool bDistanceProvided = Payload.IsValid() && Payload->HasField(TEXT("distance"));
    const float DistanceOverride = static_cast<float>(Ctx.GetNumber(TEXT("distance"), 0.0));
    const float Fov = static_cast<float>(Ctx.GetNumber(TEXT("fov"), 50.0));
    // Resolved ONCE, here, and never re-read: a capture size that varies inside one session trips
    // FViewport::GetHitProxy's ProxyMap.Num() == TestSizeX * TestSizeY assertion, which has already
    // cost unsaved level state on this project. `resolutionSource` below says which rule applied,
    // and it is DERIVED from this same DefaultEdge rather than from a flag kept beside it, so the
    // verb cannot report a rule it did not apply.
    //
    // All omitted-size capture policies now resolve to the shared 768 edge. The semantic source is
    // still reported as Default rather than inferred from the (now identical) integer values.
    const bool bWidthProvided = Payload.IsValid() && Payload->HasField(TEXT("width"));
    const bool bHeightProvided = Payload.IsValid() && Payload->HasField(TEXT("height"));
    constexpr int32 DefaultEdge = GLegacyDefaultEdge;
    const int32 Width = Ctx.GetInt(TEXT("width"), DefaultEdge);
    const int32 Height = Ctx.GetInt(TEXT("height"), DefaultEdge);
    const FString ProjectionMode = Ctx.GetString(TEXT("projectionMode"), TEXT("perspective")).ToLower();
    const bool bInline = Ctx.GetBool(TEXT("inline"), false);

    // This verb builds its own pose instead of going through ParseViewportCaptureRequest, so
    // `exposure` is read here — through the SAME parser, so the wire vocabulary cannot drift from
    // render.capture_open_level's.
    PinWrightRenderCapture::FExposurePin ExposurePin;
    {
        FString ExposureErrCode;
        FString ExposureErrMsg;
        if (!PinWrightRenderCapture::ParseExposurePin(Ctx.GetRawPayload(), ExposurePin,
                ExposureErrCode, ExposureErrMsg))
        {
            Ctx.SendError(ExposureErrCode, ExposureErrMsg);
            return true;
        }
    }

    if (!IsValidProjectionMode(ProjectionMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("projectionMode must be 'perspective' or 'orthographic'"));
        return true;
    }
    if (!AreDimensionsValid(Width, Height))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("width and height must be in (0, %d]"), GMaxCaptureDimension));
        return true;
    }
    if (Fov <= 0.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("fov must be greater than zero"));
        return true;
    }

    // Both are pure parsers over the payload, so they run HERE -- above anything that opens an
    // asset editor window or acquires a viewport. A malformed `viewMode` is therefore still
    // refused before a preview window is opened for a subject, and the error a caller sees does
    // not depend on whether a viewport happened to be available.
    //
    // No client is passed to ParseViewModePin: a mode needing a pre-selected sub-visualisation is
    // refused here rather than accepted and then found unrenderable - the refusal names what is
    // missing either way.
    PinWrightRenderCapture::FViewModePin ViewModePin;
    {
        FString ViewModeErrCode;
        FString ViewModeErrMsg;
        if (!PinWrightRenderCapture::ParseViewModePin(Ctx.GetRawPayload(), ViewModePin,
                ViewModeErrCode, ViewModeErrMsg))
        {
            Ctx.SendError(ViewModeErrCode, ViewModeErrMsg);
            return true;
        }
    }
    // The preview-scene rig, parsed HERE for the same reason `viewMode` is: it is a pure parser
    // over the payload, so a malformed rig is refused before a preview window is opened for a
    // subject. Applied by the scoped guard inside the capture primitive, restored on every exit
    // path, and MEASURED into viewport.previewScene rather than echoed.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin PreviewSceneRigPin;
    {
        FString RigErrCode;
        FString RigErrMsg;
        if (!PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(Ctx.GetRawPayload(),
                PreviewSceneRigPin, RigErrCode, RigErrMsg))
        {
            Ctx.SendError(RigErrCode, RigErrMsg);
            return true;
        }
    }
    // A rig only has somewhere to land when `subject` names an ASSET kind. The legacy `actorName`
    // spelling always shoots the Level Editor viewport, so that half of the refusal is decidable
    // from the payload alone and is answered HERE -- above the viewport acquisition, so a caller
    // gets the reason their rig cannot be honoured rather than NO_ACTIVE_LEVEL_VIEWPORT, and so
    // the refusal is assertable with no GPU. The other half (a `subject` of kind world or actor)
    // is only knowable after the resolve and is checked once the target has resolved.
    if (PreviewSceneRigPin.bRequested && !bSubjectProvided)
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            PinWrightCameraFrameSubject::PreviewSceneRigLevelViewportMessage(TEXT("frames")));
        return true;
    }
    // Same reason `exposure` is read through the shared parser above: one spelling of the wire
    // field, so this verb cannot come to mean something different from render.capture_open_level's.
    const bool bHideEditorSprites =
        PinWrightRenderCapture::ParseHideEditorSprites(Ctx.GetRawPayload());

    // ---- what is being framed: a bounding sphere, and the viewport that shows it ----
    // A subject goes through the provider registry, which is what makes an asset-editor preview
    // reachable from this verb at all. The legacy actor path keeps its own bounds and its own
    // level-viewport acquisition, so its pixels and its refusals cannot move.
    FVector Origin = FVector::ZeroVector;
    float Radius = 1.0f;
    FEditorViewportClient* ViewportClient = nullptr;
    TSharedPtr<FSceneViewport> SceneViewport;
    PinWrightCameraFrameSubject::FSubjectBinding Subject;
    FString ErrCode;
    FString ErrMsg;

    if (bSubjectProvided)
    {
        if (!PinWrightCameraFrameSubject::BindSubject(Payload, Subject, ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, ErrMsg);
            return true;
        }
        ViewportClient = Subject.Resolved.ViewportClient;
        SceneViewport = Subject.Resolved.SceneViewport;
        Origin = Subject.Resolved.BoundsOrigin;
        Radius = PinWrightCameraFrameSubject::FitRadius(Subject.Resolved);
    }
    else
    {
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent);
        Radius = FMath::Max(static_cast<float>(Extent.Size()), 1.0f);

        if (!GetActiveLevelViewport(ViewportClient, SceneViewport, ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, ErrMsg);
            return true;
        }
    }

    // A rig against the Level Editor viewport is refused, not silently dropped. Checked HERE
    // rather than beside the parse because "which viewport is this" is only known once the target
    // has resolved -- a `subject` can name a world point or a placed actor, both of which shoot
    // the level viewport just as `actorName` does.
    //
    // The subject is released FIRST on this path, the way the success path releases before
    // building the response: both this function's ViewportClient/SceneViewport copies go with it,
    // so a preview FSceneViewport cannot outlive the SEditorViewport the release destroys.
    if (PreviewSceneRigPin.bRequested && !PinWrightCameraFrameSubject::IsAssetSubject(Subject))
    {
        if (Subject.bResolved)
        {
            PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs(
                Subject.Resolved, ViewportClient, SceneViewport);
        }
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            PinWrightCameraFrameSubject::PreviewSceneRigLevelViewportMessage(TEXT("frames")));
        return true;
    }

    // `distance` wins, otherwise the bounds fit. `subject.radius` is deliberately NOT read as a
    // distance here (bSubjectRadiusProvided=false): on THIS verb it is the sphere radius that
    // already fed `Radius` above, so consuming it a second time as a camera distance would
    // double-count it and move pixels every existing caller gets. camera.orbit_shots reads the
    // same field the other way, and ResolveCameraDistance's declaration records why.
    const float Distance = ResolveCameraDistance(bDistanceProvided, DistanceOverride,
        /*bSubjectRadiusProvided=*/false, /*SubjectRadius=*/0.0f, Radius, Fov, Padding);
    // Supplied and inert, said out loud rather than dropped -- the same rule this file already
    // applies to `elevation` under distribution:'sphere'. Padding is an input to the FIT, so an
    // explicit distance leaves it nothing to modify for the camera position; it still decides
    // orthoWidth on an orthographic shot, which the warning says so a caller does not remove a
    // parameter that is still doing something.
    const bool bPaddingInertForDistance = bPaddingProvided && bDistanceProvided &&
        DistanceOverride > 0.0f;

    // An orthographic editor view can only look along a cardinal world axis, so snap the requested
    // orbit angles before placing the camera (see SnapOrbitAnglesToOrthographicAxis).
    float ShotAzimuth = Azimuth;
    float ShotElevation = Elevation;
    bool bOrthoAxisSnapped = false;
    if (ProjectionMode == TEXT("orthographic"))
    {
        bOrthoAxisSnapped = SnapOrbitAnglesToOrthographicAxis(ShotAzimuth, ShotElevation);
    }

    // ---- one pose, through the SAME primitive camera.orbit_shots runs on ----
    //
    // This verb used to call CaptureEditorViewportToPng directly, which meant it was the one
    // camera verb that never took the throwaway warm-up frame - and the first capture into a
    // freshly opened or freshly resized viewport is measurably dark (0.9 stop on a measured pair,
    // 2.26 in another case) while viewport.warmup.settled still reports true. A single still that
    // is a stop darker than the set it will be compared against is the exact defect the primitive
    // already fixed for orbit; routing one pose through it is how this verb inherits the fix
    // instead of documenting the trap.
    //
    // The orbit angles were snapped to a cardinal axis above for orthographic shots, so the pose
    // handed to the capture util always resolves to a real ELevelViewportType. (Before that snap
    // this handler passed a "don't check" flag and every orthographic shot silently rendered the
    // same LVT_OrthoFreelook +X side view regardless of the requested angles.)
    PinWrightPoseCapture::FPoseListCaptureRequest PoseRequest;
    PoseRequest.Width = Width;
    PoseRequest.Height = Height;
    PoseRequest.FilenamePrefix = TEXT("CameraFrame");
    PoseRequest.Subdirectory = TEXT("CameraFrame");
    PoseRequest.Exposure = ExposurePin;
    PoseRequest.bHideEditorSprites = bHideEditorSprites;
    PoseRequest.ViewMode = ViewModePin;
    // Set-level like the exposure pin and for the same reason: one rig for the whole call, applied
    // and restored once around it, so the frame cannot be lit differently from its own report.
    PoseRequest.PreviewSceneRig = PreviewSceneRigPin;
    // One shot, said out loud: poseSet.maxPosesPerCall reads 1 rather than the primitive's own 8,
    // so the bound in force is never implicit for this verb either.
    PoseRequest.MaxPoses = 1;
    // The subject's bounds, for the `framing` verdict. Measured by the primitive against the pose
    // the RENDERER resolved to rather than the one that was requested, which is the whole point:
    // an orbit-mode viewport can keep only the requested location's distance and aim elsewhere,
    // and framing measured off the request would call that frame correct.
    PoseRequest.BoundsOrigin = Origin;
    PoseRequest.BoundsRadius = static_cast<double>(Radius);
    // No SubjectTimeSetter is bound: this verb has no time axis, so every pose is captured with
    // the subject wherever it already is and no FCameraPose carries an instant.

    {
        PinWrightPoseCapture::FCameraPose Pose;
        Pose.ProjectionMode = ProjectionMode;
        Pose.Fov = Fov;
        PlaceOrbitCamera(Origin, ShotAzimuth, ShotElevation, Distance, Pose.Location, Pose.Rotation);
        if (ProjectionMode == TEXT("orthographic"))
        {
            Pose.OrthoWidth = ComputeOrthoWorldWidth(Radius, Padding, Width, Height);
        }
        // Filename left empty on purpose: this verb has always auto-named from the timestamp, and
        // a single still has nothing within the call to collide with. The warm-up frame is named
        // explicitly by the primitive and deleted again.
        PoseRequest.Poses.Add(MoveTemp(Pose));
    }

    PinWrightPoseCapture::FPoseListCaptureOutput PoseResult;
    if (!PinWrightPoseCapture::CaptureCameraPoses(*ViewportClient, SceneViewport, PoseRequest,
            PoseResult, ErrCode, ErrMsg))
    {
        // RELEASED THE SAME WAY THE SUCCESS PATH RELEASES, and for a reason that is invisible from
        // this line: `Subject` is declared AFTER ViewportClient/SceneViewport, so it is destroyed
        // FIRST, and ~FResolvedSubject therefore runs while this function's copies of the preview
        // FSceneViewport are still alive. CloseAssetEditor sees the extra holder, refuses the close
        // (CaptureSubject.cpp's CountPreviewSceneViewportHolders guard) and leaves the asset editor
        // OPEN -- which no longer aborts mid-run, but faults at editor shutdown in
        // ~FStaticMeshEditor (docs/lessons.md:166). A capture failure must not also leak a window.
        if (Subject.bResolved)
        {
            PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs(
                Subject.Resolved, ViewportClient, SceneViewport);
        }
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }
    if (PoseResult.Captures.Num() == 0 || PoseResult.Requests.Num() == 0)
    {
        // Unreachable by the primitive's own contract (it fails rather than returning an empty
        // set), and reported rather than indexed into: an out-of-bounds read here would be a
        // crash on a path whose whole purpose is diagnostics.
        //
        // Releases anyway. "Unreachable" is a claim about the primitive's contract, not about this
        // function, and the one thing an unreachable path must not do is leak the window that makes
        // the editor fault at shutdown.
        if (Subject.bResolved)
        {
            PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs(
                Subject.Resolved, ViewportClient, SceneViewport);
        }
        Ctx.SendError(ErrorCodes::ERR_CAPTURE_FAILED,
            TEXT("The pose capture reported success with no frames, so there is no image to report on."));
        return true;
    }

    const PinWrightRenderCapture::FViewportCaptureOutput& Capture = PoseResult.Captures[0];
    const PinWrightRenderCapture::FViewportCaptureRequest& FrameRequest = PoseResult.Requests[0];

    // Released BEFORE the response is built, not left to the destructor, so what the `subject`
    // block says about the window is MEASURED: assetEditorClosed reports what actually happened
    // rather than the state as it stood while the shot was still being taken. Idempotent, so the
    // destructor's own release is a no-op afterwards. Nothing below touches the viewport client.
    //
    // ...AND THIS FUNCTION'S OWN COPIES GO WITH IT. ViewportClient/SceneViewport were copied out of
    // the resolved subject above, so releasing while they are alive leaves a second reference to a
    // preview FSceneViewport whose SEditorViewport the release is about to destroy - which asserts
    // rather than leaks. Both are dead to this function from here on either way.
    if (Subject.bResolved)
    {
        PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs(
            Subject.Resolved, ViewportClient, SceneViewport);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddShotFields(Capture, FrameRequest, Result);
    Result->SetObjectField(TEXT("viewport"),
        PinWrightRenderCapture::MakeViewportInfoObject(Capture));
    // Did this frame contain the subject at all? `blank` cannot answer that - it catches BLACK
    // frames, and a lit backdrop is not black. Written by the primitive, which owns both the
    // measurement and the rule that an unmeasurable verdict says NOTHING rather than emitting an
    // `evaluated: false` block; one function so the seven adopting verbs cannot each decide that
    // rule differently.
    PinWrightPoseCapture::AddPoseFramingField(PoseResult, 0, Result);
    // What the base primitive did: the bound in force, and whether the throwaway warm-up frame was
    // taken and deleted. Unconditional, because a shot that silently skipped its warm-up is
    // indistinguishable from one that never needed it.
    Result->SetObjectField(TEXT("poseSet"),
        PinWrightPoseCapture::MakePoseSetInfoObject(PoseResult));
    // Present IF AND ONLY IF a subject was actually resolved. A caller who framed a placed actor
    // through `actorName` gave this verb no subject, and an empty block would say otherwise.
    if (Subject.bResolved)
    {
        Result->SetObjectField(TEXT("subject"),
            PinWrightCaptureSubject::MakeSubjectInfoObject(Subject.Resolved));
    }
    if (!ActorName.IsEmpty())
    {
        Result->SetStringField(TEXT("actorName"), ActorName);
    }
    // Which rule picked the pixel size, in the SAME vocabulary every other capture verb uses
    // (CameraShotPlanUtils.h). This verb published nothing at all before, which is how one concept
    // came to have three word sets across three verbs. Classified from the DefaultEdge actually
    // applied, so "caller" and "default" are the only two reachable here and neither can be
    // reported by a verb that did something else.
    Result->SetStringField(TEXT("resolutionSource"),
        ResolveResolutionSource(bWidthProvided || bHeightProvided, EResolutionSource::Default));
    // A supplied parameter that did nothing, named rather than dropped. Same rule and same field
    // shape as camera.orbit_shots' `elevationIgnored` under distribution:'sphere'; present only
    // when it actually happened, so its absence is not an assertion nobody knows about.
    if (bPaddingInertForDistance)
    {
        Result->SetBoolField(TEXT("paddingIgnored"), true);
        Result->SetStringField(TEXT("paddingWarning"),
            TEXT("`padding` was supplied and did NOT move the camera: an explicit `distance` "
                 "skips the bounds fit, and `padding` is only an input to that fit. Drop "
                 "`distance` to let padding pull the camera back. On an ORTHOGRAPHIC shot "
                 "`padding` still decides orthoWidth, which is the frame extent rather than the "
                 "camera distance, so it is not inert there."));
    }
    // The angles the shot was actually taken from; the request is echoed alongside when an
    // orthographic shot had to be snapped onto a world axis.
    Result->SetNumberField(TEXT("azimuth"), ShotAzimuth);
    Result->SetNumberField(TEXT("elevation"), ShotElevation);
    if (bOrthoAxisSnapped)
    {
        Result->SetBoolField(TEXT("orthoAxisSnapped"), true);
        Result->SetNumberField(TEXT("requestedAzimuth"), Azimuth);
        Result->SetNumberField(TEXT("requestedElevation"), Elevation);
    }
    MaybeAddBase64(Capture.Path, bInline, Result);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- camera.orbit_shots ----
REGISTER_RPC_HANDLER("camera.orbit_shots", "camera",
    "Capture a set of shots around a placed actor, a world point, or a `subject` asset in its own editor preview (static mesh, skeletal mesh, animation, Niagara). views:'sides' gives the six axis-aligned orthographic views in one call; count/angles give arbitrary poses (projectionMode selects perspective or orthographic); omit all three for the default canonical set (3/4 perspective + top/front/side orthographic).",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("actorName"), TEXT("string"),
            TEXT("Actor to orbit (label / internal name / object path). Provide this OR point OR subject."),
            /*bRequired=*/false, ActorNameParamUtils::ActorNameKeys()),
        RPC_PARAM_OPT("point", "object", "World-space orbit center {x, y, z}. Alternative to actorName; requires radius."),
        RPC_PARAM_OPT("subject", "object", PINWRIGHT_CAMERA_SUBJECT_PARAM_DESC),
        RPC_PARAM_OPT("count", "integer", "Number of evenly-spaced perspective shots. Omit for the 4-shot canonical set."),
        RPC_PARAM_OPT("angles", "array", "Explicit shot poses, each an object {azimuth, elevation} in degrees (perspective)."),
        RPC_PARAM_OPT("elevation", "number", "Default elevation in degrees for count/angles shots (default 30)."),
        RPC_PARAM_OPT("radius", "number", "Orbit radius / camera distance. Defaults to a bounds-fit distance; required when target is a bare point. Supplying it skips the bounds fit, so `padding` then has nothing to modify for the camera position and the response says so."),
        // The other half of ComputeFitDistance, which this verb welded as `constexpr float
        // Padding = 1.15f` until now -- the only 1.15f literal in the whole Private/ tree.
        // camera.frame_actor has exposed the margin since it shipped and camera.animation_shots
        // exposes both inputs, so this verb was the one end of the helper no caller could reach.
        // The default is unchanged, so no existing call shape moves a pixel.
        RPC_PARAM_DEF("padding", "number", PINWRIGHT_FIT_PADDING_PARAM_DESC " Default 1.15, the value this verb used to weld. Read once for the whole set, so every shot in one call is framed with the same margin.", "1.15"),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees (default 50)."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. If omitted, existing count/angles/canonical calls use 1024 and views:'sides' uses 640; explicit sizes win."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. If omitted, existing count/angles/canonical calls use 1024 and views:'sides' uses 640; explicit sizes win."),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC " Read once for the whole set, so every shot is rendered in the same mode, and restored once after the last one."),
        RPC_PARAM_DEF("distribution", "string", "How `count` spreads its shots. 'ring' (default, and the pre-existing behaviour) puts them at evenly-spaced azimuths on ONE horizontal circle at `elevation`, which never sees the top or the underside. 'sphere' uses a golden-angle (Fibonacci) spiral over the whole viewing sphere - deterministic, near-optimal even coverage for any N, no two shots on the same great circle - and IGNORES `elevation`, which the response says out loud rather than dropping silently. Only affects `count`; `angles` and `views` name their own poses.", "ring"),
        RPC_PARAM_OPT("seed", "number", "Jitter the shot distribution by a seeded azimuth offset, so a set can vary between runs without becoming irreproducible. Omitted, output is deterministic (offset 0). Supplied, output is STILL deterministic - the same seed always gives the same poses - and the seed is echoed in the response so any set can be retaken exactly. There is deliberately no unseeded random mode: uniformly random points on a sphere clump, which is worse coverage than an even spread, and an acceptance shot that cannot be reproduced cannot be compared against."),
        RPC_PARAM_OPT("projectionMode", "string", "Projection for count/angles shots: 'perspective' (default) or 'orthographic'. Orthographic poses are snapped onto the nearest cardinal world axis."),
        RPC_PARAM_OPT("views", "string", "Canned shot plan. 'sides' captures the six axis-aligned views (front/back/left/right/top/bottom), orthographic by default. Cannot be combined with count or angles."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC " Read once for the whole set, so every shot in one call is exposed identically."),
        RPC_PARAM_OPT("hideEditorSprites", "boolean", PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC " Read once for the whole set, so every shot in one call shows the same viewport decoration."),
        RPC_PARAM_OPT("previewScene", "object", PINWRIGHT_PREVIEW_SCENE_PARAM_DESC " Read once for the whole set, so every shot in one call is lit identically, and restored once after the last one. ASSET SUBJECTS ONLY on this verb: orbiting `actorName`, a bare `point`, or a subject of kind world or actor shoots the Level Editor viewport, which has no preview scene to rig, and is refused with UNSUPPORTED_ASSET_EDITOR rather than accepted and quietly ignored."),
        RPC_PARAM_OPT("inline", "boolean", "When true, also embed base64 PNG bytes per shot (default false).")
    ))
{
    using namespace PinWrightCameraFrame;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasField = Payload.IsValid();

    const float DefaultElevation = static_cast<float>(Ctx.GetNumber(TEXT("elevation"), 30.0));
    const bool bElevationProvided = Payload.IsValid() && Payload->HasField(TEXT("elevation"));
    const float Fov = static_cast<float>(Ctx.GetNumber(TEXT("fov"), 50.0));
    const bool bInline = Ctx.GetBool(TEXT("inline"), false);
    // Read ONCE for the whole set, above the shot loop, for the same reason the exposure pin is:
    // every shot in one set must be framed with the same margin or the set is not a set.
    //
    // This was `constexpr float Padding = 1.15f` -- the only 1.15f literal in the whole Private/
    // tree -- with the comment "Orbit has no padding arg; use the same default margin as
    // frame_actor". It has one now, and the default is still that same margin, so a call that
    // omits it gets byte-identical pixels to before.
    const bool bPaddingProvided = bHasField && Payload->HasField(TEXT("padding"));
    const float Padding = static_cast<float>(Ctx.GetNumber(TEXT("padding"), GDefaultFitPadding));

    // ---- shot distribution + seeded jitter ----
    // Both are pure pose-generation inputs and are resolved here, above the plan, because the
    // plan is the only thing they touch. `ring` is the default and reproduces the previous
    // behaviour byte-for-byte, so no existing caller's pixels move.
    const FString DistributionArg = Ctx.GetString(TEXT("distribution"), TEXT("ring")).ToLower();
    EShotDistribution Distribution = EShotDistribution::Ring;
    if (DistributionArg == TEXT("ring"))
    {
        Distribution = EShotDistribution::Ring;
    }
    else if (DistributionArg == TEXT("sphere"))
    {
        Distribution = EShotDistribution::Sphere;
    }
    else
    {
        // Rejected rather than defaulted, for the reason `projection` on editor.set_view_mode is:
        // a typo that quietly becomes the default is indistinguishable from the default having
        // been asked for.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unrecognised distribution '%s'. Valid: ring (default), sphere."),
                *DistributionArg));
        return true;
    }

    const bool bSeedProvided = bHasField && Payload->HasField(TEXT("seed"));
    const int32 Seed = Ctx.GetInt(TEXT("seed"), 0);
    const double AzimuthOffsetDegrees =
        bSeedProvided ? SeedToAzimuthOffsetDegrees(Seed) : 0.0;

    // Read ONCE, above the shot loop, and copied into every per-shot request below. Re-reading it
    // per shot could not differ today, but hoisting it makes "every shot in this set was exposed
    // identically" a property of the code rather than of the payload staying constant.
    PinWrightRenderCapture::FExposurePin ExposurePin;
    {
        FString ExposureErrCode;
        FString ExposureErrMsg;
        if (!PinWrightRenderCapture::ParseExposurePin(Ctx.GetRawPayload(), ExposurePin,
                ExposureErrCode, ExposureErrMsg))
        {
            Ctx.SendError(ExposureErrCode, ExposureErrMsg);
            return true;
        }
    }

    // Hoisted for the same reason as the exposure pin, and a pure payload parser like it, so a
    // malformed rig is refused before an asset editor is opened for a subject. Applied and
    // restored ONCE around the whole set by the capture primitive's scoped guard, and MEASURED
    // into viewport.previewScene rather than echoed.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin PreviewSceneRigPin;
    {
        FString RigErrCode;
        FString RigErrMsg;
        if (!PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(Ctx.GetRawPayload(),
                PreviewSceneRigPin, RigErrCode, RigErrMsg))
        {
            Ctx.SendError(RigErrCode, RigErrMsg);
            return true;
        }
    }
    // A rig only lands when `subject` names an ASSET kind. The legacy `actorName` and bare-`point`
    // spellings always orbit the Level Editor viewport, so that half of the refusal is decidable
    // from the payload alone and is answered HERE -- above the plan, above the target resolve and
    // above the viewport acquisition, so a caller gets the reason their rig cannot be honoured
    // rather than NO_ACTIVE_LEVEL_VIEWPORT, and so the refusal is assertable with no GPU. The
    // other half (a `subject` of kind world or actor) is checked once the target has resolved.
    if (PreviewSceneRigPin.bRequested &&
        !PinWrightCameraFrameSubject::HasSubjectField(Payload))
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            PinWrightCameraFrameSubject::PreviewSceneRigLevelViewportMessage(TEXT("orbits")));
        return true;
    }

    // Hoisted for the same reason as the exposure pin: every shot in one set must show the same
    // viewport decoration, or the set is not a set.
    const bool bHideEditorSprites =
        PinWrightRenderCapture::ParseHideEditorSprites(Ctx.GetRawPayload());

    // ---- Projection for count/angles shots. Default perspective, which is what every shot
    // built from count/angles has always been (the literals this replaces). ----
    const bool bProjectionProvided = bHasField && Payload->HasField(TEXT("projectionMode"));
    const FString RequestedProjection =
        Ctx.GetString(TEXT("projectionMode"), TEXT("perspective")).ToLower();
    if (bProjectionProvided && !IsValidProjectionMode(RequestedProjection))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("projectionMode must be 'perspective' or 'orthographic'"));
        return true;
    }

    const bool bViewsProvided = bHasField && Payload->HasField(TEXT("views"));
    const FString Views = Ctx.GetString(TEXT("views")).ToLower();

    // ---- Resolution. Existing call shapes keep the 1024 default byte-for-byte; only the new
    // opt-in `views` plan takes the multi-image budget (long edge 640), because it is the only
    // shape that never existed before and so has no output to change. Dropping 1024 -> 640
    // globally would silently coarsen every archived comparison, and for an ORTHOGRAPHIC shot it
    // also rescales world-units-per-pixel (unitsPerPixel = orthoWidth / width), quietly degrading
    // any measurement recipe built on the old size. resolutionSource says which rule applied. ----
    const bool bWidthProvided = bHasField && Payload->HasField(TEXT("width"));
    const bool bHeightProvided = bHasField && Payload->HasField(TEXT("height"));
    const bool bBudgetSize = bViewsProvided && !bWidthProvided && !bHeightProvided;
    const int32 DefaultEdge = bBudgetSize ? GOrbitViewsBudgetEdge : GOrbitLegacyDefaultEdge;
    const EResolutionSource DefaultSource =
        bBudgetSize ? EResolutionSource::Budget : EResolutionSource::Default;
    const int32 Width = Ctx.GetInt(TEXT("width"), DefaultEdge);
    const int32 Height = Ctx.GetInt(TEXT("height"), DefaultEdge);

    if (!AreDimensionsValid(Width, Height))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("width and height must be in (0, %d]"), GMaxCaptureDimension));
        return true;
    }
    if (Fov <= 0.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("fov must be greater than zero"));
        return true;
    }

    // ---- Build the shot plan. Precedence: views (exclusive) | angles > count > canonical set. ----
    TArray<FPlannedShot> Plan;
    const TArray<TSharedPtr<FJsonValue>>* Angles = Ctx.GetArray(TEXT("angles"));
    const bool bCountProvided = bHasField && Payload->HasField(TEXT("count"));

    if (bViewsProvided)
    {
        // Rejected rather than silently ranked: `views` and `count`/`angles` are two different
        // answers to "which poses", and picking one for the caller is how a verb ends up with
        // parameters that contradict each other depending on a mode flag.
        if (bCountProvided || (Angles && Angles->Num() > 0))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("views cannot be combined with count or angles; pass one shot plan"));
            return true;
        }
        if (Views != TEXT("sides"))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("views must be 'sides' (the six axis-aligned views); use count/angles for arbitrary poses"));
            return true;
        }

        // The six axis-aligned views, orthographic unless the caller asked otherwise.
        //
        // ONE TABLE, shared. These six pairs used to be written out here, again in
        // AnimationShotsHandler.cpp and again in AnimationPreviewCaptureHandler.cpp - three
        // transcriptions of the same six numbers, where the spellings are load-bearing (`back` is
        // 180 rather than -180 because FRotator::NormalizeAxis(180) returns 180 and the snap would
        // otherwise report a spurious move; `right` is -90 rather than 270 for the same reason).
        // MakeSideViews in CameraShotPlanUtils.h is now the only copy, so "the six sides" cannot
        // come to mean six different things depending on which verb was asked.
        const FString SideProjection = bProjectionProvided ? RequestedProjection : TEXT("orthographic");
        Plan = MakeSideViews(SideProjection);
    }
    else if (Angles && Angles->Num() > 0)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Angles)
        {
            const TSharedPtr<FJsonObject>* AngleObj = nullptr;
            if (Value.IsValid() && Value->TryGetObject(AngleObj) && AngleObj && (*AngleObj).IsValid())
            {
                FPlannedShot Shot;
                Shot.Azimuth = static_cast<float>(GetJsonNumberField(*AngleObj, TEXT("azimuth"), 0.0));
                Shot.Elevation = static_cast<float>(GetJsonNumberField(*AngleObj, TEXT("elevation"), DefaultElevation));
                Shot.ProjectionMode = RequestedProjection;
                Plan.Add(Shot);
            }
        }
        if (Plan.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("angles must be a non-empty array of {azimuth, elevation} objects"));
            return true;
        }
    }
    else if (bCountProvided)
    {
        const int32 Count = Ctx.GetInt(TEXT("count"), 4);
        if (Count < 1)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("count must be at least 1"));
            return true;
        }
        Plan = (Distribution == EShotDistribution::Sphere)
            ? MakeSphereDistribution(Count, AzimuthOffsetDegrees, RequestedProjection)
            : MakeRingDistribution(Count, DefaultElevation, AzimuthOffsetDegrees, RequestedProjection);
    }
    else
    {
        // Canonical set: a 3/4 perspective plus axis-aligned top/front/side orthographic.
        Plan.Add(FPlannedShot{45.0f, 30.0f, TEXT("perspective")});
        Plan.Add(FPlannedShot{0.0f, 0.0f, TEXT("orthographic")});   // front (looking along -X)
        Plan.Add(FPlannedShot{90.0f, 0.0f, TEXT("orthographic")});  // side (looking along -Y)
        Plan.Add(FPlannedShot{0.0f, 90.0f, TEXT("orthographic")});  // top (looking down -Z)
    }

    if (Plan.Num() > GMaxOrbitShots)
    {
        Ctx.SendError(ErrorCodes::ERR_TOO_MANY_SHOTS,
            FString::Printf(TEXT("Requested %d shots exceeds the maximum of %d"), Plan.Num(), GMaxOrbitShots));
        return true;
    }

    // ---- Resolve the target center + radius, and the viewport that shows it. ----
    //
    // Three shapes, in precedence order. `subject` goes through the provider registry and is what
    // reaches an asset-editor preview (static mesh, skeletal mesh, animation, Niagara) - the
    // camera plan above this point is identical either way, because a plan plus a bounding sphere
    // is all this verb ever needed from its target. `actorName` and `point`+`radius` keep their
    // own code and their own refusals verbatim, so no call shape that already shipped moves.
    const FString ActorName = ActorNameParamUtils::ResolveActorName(Ctx);
    const bool bHasPoint = bHasField && Payload->HasField(TEXT("point"));
    const bool bSubjectProvided = PinWrightCameraFrameSubject::HasSubjectField(Payload);
    const bool bRadiusProvided = bHasField && Payload->HasField(TEXT("radius"));
    const float RadiusParam = static_cast<float>(Ctx.GetNumber(TEXT("radius"), 0.0));

    FVector Center = FVector::ZeroVector;
    float BoundsRadius = 1.0f;  // drives orthographic extent
    float BaseDistance = 100.0f; // camera distance for every shot
    // Did an explicit distance win over the bounds fit? Only tracked so a supplied-but-inert
    // `padding` can be named in the response instead of silently dropped; every branch below
    // sets it, so it cannot go stale by a path being added later without a value.
    bool bDistanceOverridden = false;

    FEditorViewportClient* ViewportClient = nullptr;
    TSharedPtr<FSceneViewport> SceneViewport;
    // Carries the viewport state (view mode above all) out of the shot loop for the single
    // top-level `viewport` block; only meaningful once at least one shot succeeded.
    PinWrightRenderCapture::FViewportCaptureOutput LastCapture;
    PinWrightCameraFrameSubject::FSubjectBinding Subject;
    FString ErrCode;
    FString ErrMsg;

    if (bSubjectProvided && (!ActorName.IsEmpty() || bHasPoint))
    {
        // Two answers to "what am I orbiting". Refused naming BOTH keys rather than ranked, for
        // the same reason `views` cannot be combined with `count`/`angles` above: a parameter that
        // silently loses to another is indistinguishable from one that was never read.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(
                TEXT("`subject` and `%s` both name a target; pass one shot target. Drop the legacy "
                     "key to orbit the subject, or drop `subject` to orbit the level."),
                !ActorName.IsEmpty() ? TEXT("actorName") : TEXT("point")));
        return true;
    }

    if (bSubjectProvided)
    {
        // A subject supplies BOTH the bounds and the viewport: an asset preview is not the Level
        // Editor viewport, so acquiring the level one here and then shooting through the preview
        // would be two different viewports in one call.
        if (!PinWrightCameraFrameSubject::BindSubject(Payload, Subject, ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, ErrMsg);
            return true;
        }
        ViewportClient = Subject.Resolved.ViewportClient;
        SceneViewport = Subject.Resolved.SceneViewport;
        Center = Subject.Resolved.BoundsOrigin;
        BoundsRadius = PinWrightCameraFrameSubject::FitRadius(Subject.Resolved);
        // Same precedence as the actor path, extended by one step: the top-level `radius` wins,
        // then `subject.radius`, then a distance solved from the bounds. `subject.radius` is read
        // because a bare world point has no size and the orbit radius IS its framing extent, which
        // is the rule the legacy point path below has always used -- dropping it here would make
        // the same payload mean two different distances depending on which spelling carried it.
        // camera.frame_actor reads the SAME field as a sphere radius instead; ResolveCameraDistance's
        // declaration records both meanings so the divergence is documented rather than discovered.
        BaseDistance = ResolveCameraDistance(bRadiusProvided, RadiusParam,
            Subject.Request.bRadiusProvided, Subject.Request.Radius,
            BoundsRadius, Fov, Padding);
        bDistanceOverridden = (bRadiusProvided && RadiusParam > 0.0f) ||
            (Subject.Request.bRadiusProvided && Subject.Request.Radius > 0.0f);
    }
    else if (!ActorName.IsEmpty())
    {
        AActor* Actor = McpActorUtils::FindActorByName(nullptr, ActorName);
        if (!Actor)
        {
            Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                FString::Printf(TEXT("Actor not found: %s"), *ActorName));
            return true;
        }
        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent);
        Center = Origin;
        BoundsRadius = FMath::Max(static_cast<float>(Extent.Size()), 1.0f);
        // No subject, so no `subject.radius` step: the top-level `radius` wins, otherwise the fit.
        BaseDistance = ResolveCameraDistance(bRadiusProvided, RadiusParam,
            /*bSubjectRadiusProvided=*/false, /*SubjectRadius=*/0.0f,
            BoundsRadius, Fov, Padding);
        bDistanceOverridden = bRadiusProvided && RadiusParam > 0.0f;

        if (!GetActiveLevelViewport(ViewportClient, SceneViewport, ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, ErrMsg);
            return true;
        }
    }
    else if (bHasPoint)
    {
        Center = Ctx.GetVector(TEXT("point"), FVector::ZeroVector);
        if (!bRadiusProvided || RadiusParam <= 0.0f)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("radius (> 0) is required when the target is a bare point with no bounds"));
            return true;
        }
        // A bare point has no size, so the orbit radius doubles as the framing extent. The fit
        // never runs on this path -- there are no bounds to fit -- which is why `radius` is
        // required here and why `padding` cannot move the camera for a bare point.
        BaseDistance = RadiusParam;
        BoundsRadius = RadiusParam;
        bDistanceOverridden = true;

        if (!GetActiveLevelViewport(ViewportClient, SceneViewport, ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, ErrMsg);
            return true;
        }
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("Provide a target: actorName (or objectPath/actorPath), point {x, y, z}, or a subject object"));
        return true;
    }

    // A rig against the Level Editor viewport is refused, not silently dropped -- the same
    // condition and the same code camera.frame_actor and render.capture_annotated use. Checked
    // HERE because "which viewport is this" is only known once the target has resolved: a
    // `subject` can name a world point or a placed actor, both of which orbit the level viewport.
    //
    // The subject is released FIRST, the way the success path releases before building the
    // response, and this function's ViewportClient/SceneViewport copies go with it -- THIS is the
    // call site that crashed once by holding a preview FSceneViewport across its own release.
    if (PreviewSceneRigPin.bRequested && !PinWrightCameraFrameSubject::IsAssetSubject(Subject))
    {
        if (Subject.bResolved)
        {
            PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs(
                Subject.Resolved, ViewportClient, SceneViewport);
        }
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            PinWrightCameraFrameSubject::PreviewSceneRigLevelViewportMessage(TEXT("orbits")));
        return true;
    }

    // One wall-clock stamp shared by every shot in this orbit; the per-shot index (plus
    // az/el) is what makes each filename unique. Left empty, the capture util auto-names
    // from a SECOND-resolution timestamp, so shots taken within the same second (the
    // common case — an orbit set completes well under a second) collide on one filename
    // and overwrite each other on disk, leaving only the last-per-second PNG even though
    // the response still lists every shot.
    const FString OrbitStamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

    // Hoisted for the same reason as the exposure pin, and read through the SAME parser as every
    // other capture verb so the vocabulary cannot drift. Parsed HERE rather than with the other
    // arguments because it is given the resolved viewport client: that is what lets a mode whose
    // sub-visualisation is already selected in the editor through instead of refusing it.
    //
    // This argument used to be documented as "informational label echoed back in the result (does
    // not change rendering)" - it now applies, scoped, and an unrecognised or unrenderable mode is
    // an ERROR rather than an echo. That is the point: a parameter that looks like it works is the
    // defect being cleared, not a compatibility surface to preserve.
    PinWrightRenderCapture::FViewModePin ViewModePin;
    {
        FString ViewModeErrCode;
        FString ViewModeErrMsg;
        if (!PinWrightRenderCapture::ParseViewModePin(Ctx.GetRawPayload(), ViewModePin,
                ViewModeErrCode, ViewModeErrMsg, ViewportClient))
        {
            // The FIRST refusal on this verb that can happen with a subject already open, because
            // this parse is the one that needs the resolved viewport client and so had to move
            // below the bind. Same release as every other post-bind exit: `Subject` is declared
            // after ViewportClient/SceneViewport and is therefore destroyed first, so leaving these
            // copies alive makes ~FResolvedSubject's close refuse itself and strand the editor
            // window until it faults at shutdown.
            if (Subject.bResolved)
            {
                PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs(
                    Subject.Resolved, ViewportClient, SceneViewport);
            }
            Ctx.SendError(ViewModeErrCode, ViewModeErrMsg);
            return true;
        }
    }

    // ---- resolve every pose, then hand the whole list to the base capture primitive ----
    //
    // This verb is now a POSE GENERATOR plus one call. Everything below the generator - applying
    // the camera, the warm-up frame, the exposure pin, the scoped view mode, sprite suppression,
    // orbit suppression, both viewport transform slots and the aim verification - belongs to
    // PinWrightPoseCapture::CaptureCameraPoses and the util it wraps, so a fix there reaches every
    // verb on it instead of one. The orbit-camera aiming defect was diagnosed and fixed
    // independently TWICE in this tree precisely because there was no such base.
    PinWrightPoseCapture::FPoseListCaptureRequest PoseRequest;
    PoseRequest.Width = Width;
    PoseRequest.Height = Height;
    PoseRequest.FilenamePrefix = TEXT("CameraOrbit");
    PoseRequest.Subdirectory = TEXT("CameraOrbit");
    PoseRequest.Exposure = ExposurePin;
    PoseRequest.bHideEditorSprites = bHideEditorSprites;
    PoseRequest.ViewMode = ViewModePin;
    // Set-level like the exposure pin: one rig applied and restored once around the whole orbit,
    // so no two shots in one set can be lit differently from each other or from their own report.
    PoseRequest.PreviewSceneRig = PreviewSceneRigPin;
    // This verb has allowed 24 shots since it shipped and the primitive's own default is 8.
    // Keeping the verb's own bound is what stops the conversion silently shortening sets callers
    // have been taking for months; the value in force is published as poseSet.maxPosesPerCall.
    PoseRequest.MaxPoses = GMaxOrbitShots;
    // The subject's bounds, for the per-shot `framing` verdict. Set-level rather than per-pose
    // because bounds that move between shots make "the subject left the frame" and "the bounds
    // grew" the same reading.
    PoseRequest.BoundsOrigin = Center;
    PoseRequest.BoundsRadius = static_cast<double>(BoundsRadius);
    // No SubjectTimeSetter is bound: this verb crosses a view axis only, never a time axis, so no
    // FCameraPose it builds carries an instant.
    PoseRequest.Poses.Reserve(Plan.Num());

    // Parallel to PoseRequest.Poses: what each pose was ASKED for, before the orthographic axis
    // snap, plus whether the snap moved it. Plan[] is rewritten in place with the RESOLVED angles.
    TArray<FPlannedShot> RequestedShots;
    TArray<bool> OrthoAxisSnapped;
    RequestedShots.Reserve(Plan.Num());
    OrthoAxisSnapped.Reserve(Plan.Num());

    for (int32 ShotIndex = 0; ShotIndex < Plan.Num(); ++ShotIndex)
    {
        const FPlannedShot Shot = Plan[ShotIndex];
        // Orthographic shots must look along a cardinal world axis (see
        // SnapOrbitAnglesToOrthographicAxis). The canonical front/side/top set is already
        // axis-aligned, so this is a no-op there; it guards explicit angle sets.
        float ShotAzimuth = Shot.Azimuth;
        float ShotElevation = Shot.Elevation;
        bool bOrthoAxisSnapped = false;
        if (Shot.ProjectionMode == TEXT("orthographic"))
        {
            bOrthoAxisSnapped = SnapOrbitAnglesToOrthographicAxis(ShotAzimuth, ShotElevation);
        }

        PinWrightPoseCapture::FCameraPose Pose;
        Pose.ProjectionMode = Shot.ProjectionMode;
        Pose.Fov = Fov;
        // Distinct filename per shot so no two shots in the set overwrite each other.
        // The index alone guarantees uniqueness within the call; az/el keep the name
        // readable/self-describing. az/el are rounded to ints so the name carries no
        // '.', '/', or '\' that MakeScreenshotFilename would reject (which would fall
        // back to the colliding auto-name).
        Pose.Filename = FString::Printf(TEXT("CameraOrbit_%s_shot%02d_az%d_el%d.png"),
            *OrbitStamp, ShotIndex,
            FMath::RoundToInt(ShotAzimuth), FMath::RoundToInt(ShotElevation));
        PlaceOrbitCamera(Center, ShotAzimuth, ShotElevation, BaseDistance, Pose.Location, Pose.Rotation);
        if (Shot.ProjectionMode == TEXT("orthographic"))
        {
            Pose.OrthoWidth = ComputeOrthoWorldWidth(BoundsRadius, Padding, Width, Height);
        }

        PoseRequest.Poses.Add(MoveTemp(Pose));
        RequestedShots.Add(Shot);
        OrthoAxisSnapped.Add(bOrthoAxisSnapped);
        Plan[ShotIndex].Azimuth = ShotAzimuth;
        Plan[ShotIndex].Elevation = ShotElevation;
    }

    PinWrightPoseCapture::FPoseListCaptureOutput PoseResult;
    if (!PinWrightPoseCapture::CaptureCameraPoses(*ViewportClient, SceneViewport, PoseRequest,
            PoseResult, ErrCode, ErrMsg))
    {
        // The failure twin of the release below, and it was missing. THIS is the call site that
        // crashed once by holding SceneViewport across its own release; the extra-holder guard in
        // CaptureSubject.cpp turned that abort into a REFUSED close, which is survivable but leaves
        // the asset editor open and reported as not closed -- and an asset editor still open at
        // editor exit faults in ~FStaticMeshEditor (docs/lessons.md:166). A shot that failed must
        // clean up exactly as thoroughly as one that succeeded.
        if (Subject.bResolved)
        {
            PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs(
                Subject.Resolved, ViewportClient, SceneViewport);
        }
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    // Released BEFORE the response is built, not left to the destructor, so the `subject` block's
    // assetEditorClosed is MEASURED rather than a snapshot taken while the shot was still running.
    // Idempotent, so the destructor's own release is a no-op. Nothing below touches the viewport.
    //
    // ...AND THIS FUNCTION'S OWN COPIES GO WITH IT. THIS IS THE CALL SITE THAT CRASHED: an orbit
    // over a staticMesh subject held SceneViewport here while the release closed the Static Mesh
    // editor, and ~SEditorViewport's check(SceneViewport.IsUnique()) aborted the editor mid-suite.
    if (Subject.bResolved)
    {
        PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs(
            Subject.Resolved, ViewportClient, SceneViewport);
    }

    TArray<TSharedPtr<FJsonValue>> ShotsJson;
    ShotsJson.Reserve(PoseResult.Captures.Num());
    for (int32 ShotIndex = 0; ShotIndex < PoseResult.Captures.Num(); ++ShotIndex)
    {
        const PinWrightRenderCapture::FViewportCaptureOutput& Capture = PoseResult.Captures[ShotIndex];
        // The view mode is a property of the viewport, not of a shot, and every shot in the set
        // was taken under the same one - so it is reported once at the top level.
        LastCapture = Capture;

        TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> AngleObj = MakeShared<FJsonObject>();
        // UNCONDITIONAL, all four. `azimuth`/`elevation` are the RESOLVED pose; the two
        // `requested*` fields are what was asked for. They used to appear only when an
        // orthographic snap moved the pose, so a caller who found a defect in shot 5 of a sphere
        // distribution had no way to re-shoot that exact pose with `angles` and had to reverse it
        // out of the filename, which carries only rounded integers.
        AngleObj->SetNumberField(TEXT("azimuth"), Plan[ShotIndex].Azimuth);
        AngleObj->SetNumberField(TEXT("elevation"), Plan[ShotIndex].Elevation);
        AngleObj->SetNumberField(TEXT("requestedAzimuth"), RequestedShots[ShotIndex].Azimuth);
        AngleObj->SetNumberField(TEXT("requestedElevation"), RequestedShots[ShotIndex].Elevation);
        ShotObj->SetObjectField(TEXT("angle"), AngleObj);
        // Report the snap the way camera.frame_actor does. Orbit used to snap silently, so a
        // caller who asked for azimuth 37 orthographic got a 0-degree shot with nothing in the
        // response saying the pose had moved. Kept at the shot level as well as inside `angle`
        // because existing callers read these three keys there.
        if (OrthoAxisSnapped[ShotIndex])
        {
            ShotObj->SetBoolField(TEXT("orthoAxisSnapped"), true);
            ShotObj->SetNumberField(TEXT("requestedAzimuth"), RequestedShots[ShotIndex].Azimuth);
            ShotObj->SetNumberField(TEXT("requestedElevation"), RequestedShots[ShotIndex].Elevation);
        }
        AddShotFields(Capture, PoseResult.Requests[ShotIndex], ShotObj);
        // Did THIS shot contain the subject? Per shot rather than once per set, because a 24-pose
        // orbit in which one pose missed is exactly the case a single top-level verdict hides -
        // and `blank` cannot see it at all, since a lit backdrop is not a black frame. Measured by
        // the primitive against the pose the renderer resolved to, and absent entirely when it
        // could not be measured.
        PinWrightPoseCapture::AddPoseFramingField(PoseResult, ShotIndex, ShotObj);
        MaybeAddBase64(Capture.Path, bInline, ShotObj);
        ShotsJson.Add(MakeShared<FJsonValueObject>(ShotObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("shots"), ShotsJson);
    Result->SetNumberField(TEXT("count"), ShotsJson.Num());
    if (ShotsJson.Num() > 0)
    {
        Result->SetObjectField(TEXT("viewport"),
            PinWrightRenderCapture::MakeViewportInfoObject(LastCapture));
    }
    if (bViewsProvided)
    {
        Result->SetStringField(TEXT("views"), Views);
    }
    // Which rule picked the pixel size: "caller" when width and/or height were passed, "budget"
    // when the multi-image policy applied, "default" for the ordinary omitted-size policy.
    //
    // ONE VOCABULARY, and now DERIVED rather than hand-spelled. The three words used to be written
    // out here as literals; camera.animation_shots and render.capture_animation_preview answered
    // the same question with "burstBudget"/"singleStill" and camera.frame_actor did not answer it
    // at all, so one concept had three word sets across three verbs. Classifying from the
    // DefaultEdge this call actually used means the label cannot disagree with the pixels.
    Result->SetStringField(TEXT("resolutionSource"),
        ResolveResolutionSource(bWidthProvided || bHeightProvided, DefaultSource));
    // A supplied parameter that did nothing, named rather than dropped -- the same rule and the
    // same field shape as `elevationIgnored` below. Present only when it actually happened.
    if (bPaddingProvided && bDistanceOverridden)
    {
        Result->SetBoolField(TEXT("paddingIgnored"), true);
        Result->SetStringField(TEXT("paddingWarning"),
            TEXT("`padding` was supplied and did NOT move the camera: an explicit distance won "
                 "over the bounds fit (`radius`, or `subject.radius`, or a bare `point`, whose "
                 "radius IS its framing extent), and `padding` is only an input to that fit. Drop "
                 "the explicit radius to let padding pull the camera back. On ORTHOGRAPHIC shots "
                 "`padding` still decides orthoWidth, which is the frame extent rather than the "
                 "camera distance, so it is not inert there."));
    }
    if (!ActorName.IsEmpty())
    {
        Result->SetStringField(TEXT("actorName"), ActorName);
    }
    // Present IF AND ONLY IF a subject was actually resolved: an orbit around a placed actor or a
    // bare world point was given no subject, and an empty block would claim otherwise.
    if (Subject.bResolved)
    {
        Result->SetObjectField(TEXT("subject"),
            PinWrightCaptureSubject::MakeSubjectInfoObject(Subject.Resolved));
    }

    // What the base primitive did with the pose list: how many were asked for, how many were
    // captured, how many the bound dropped, and whether a throwaway warm-up frame was taken.
    Result->SetObjectField(TEXT("poseSet"),
        PinWrightPoseCapture::MakePoseSetInfoObject(PoseResult));

    // ---- how the poses were distributed, and the seed that reproduces them ----
    // Published unconditionally so a set can always be retaken: `distribution` names the
    // construction, `seed` is the exact value to pass back, and `seeded` says whether one was in
    // force at all. Without a seed the output is deterministic, so an absent seed is not an
    // absent contract.
    TSharedPtr<FJsonObject> DistributionInfo = MakeShared<FJsonObject>();
    DistributionInfo->SetStringField(TEXT("distribution"),
        Distribution == EShotDistribution::Sphere ? TEXT("sphere") : TEXT("ring"));
    DistributionInfo->SetBoolField(TEXT("seeded"), bSeedProvided);
    if (bSeedProvided)
    {
        DistributionInfo->SetNumberField(TEXT("seed"), Seed);
        DistributionInfo->SetNumberField(TEXT("azimuthOffsetDegrees"), AzimuthOffsetDegrees);
    }
    // Applies only to `count`; `angles` and `views` name their own poses and neither reads
    // `distribution` at all. Said out loud rather than left for the reader to infer from which
    // fields moved.
    DistributionInfo->SetBoolField(TEXT("appliesToPlan"), bCountProvided);
    if (Distribution == EShotDistribution::Sphere && bElevationProvided)
    {
        // The sphere construction derives every elevation from the shot index, so there is
        // nothing for an `elevation` argument to modify. Saying so is the whole difference
        // between a parameter that does not apply and a parameter that was silently dropped -
        // which is the same defect class as this verb's old inert `viewMode`.
        DistributionInfo->SetBoolField(TEXT("elevationIgnored"), true);
        DistributionInfo->SetStringField(TEXT("elevationWarning"),
            TEXT("`elevation` was supplied and IGNORED: distribution:'sphere' derives each shot's "
                 "elevation from its index by the golden-angle construction, so a single fixed "
                 "elevation has nothing to modify. Use distribution:'ring' to pin every shot to "
                 "one elevation, or `angles` to name each pose."));
    }
    Result->SetObjectField(TEXT("shotDistribution"), DistributionInfo);

    if (ShotsJson.Num() > 0)
    {
        // MEASURED, not echoed. The `viewMode` ARGUMENT of this verb used to be a caller-supplied
        // label that changed nothing and was written back under this key, so a caller who passed
        // viewMode:"lit" to a wireframe viewport read its own label back as if it were a fact.
        // It is now the mode the pixels were actually rendered in, taken off the viewport;
        // viewport.viewModeOverride carries the applied/restored verdict.
        Result->SetStringField(TEXT("viewMode"), LastCapture.ViewModeKey);
        // Retained so existing readers of this key keep getting a string, but it is now the
        // RESOLVED key rather than the raw request - there is no longer a free-text label to
        // echo, because an unrecognised viewMode is rejected instead of accepted.
        Result->SetStringField(TEXT("viewModeLabel"), LastCapture.ViewModeKey);
    }
    Ctx.SendSuccess(Result);
    return true;
}
