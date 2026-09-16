// Copyright (c) 2026 Alexander Penkin. MIT License.

// ViewportHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles editor.focus_actor, editor.set_camera, editor.set_view_mode,
// editor.set_viewport_realtime, editor.set_game_view, editor.screenshot

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Editor/EditorHandlerUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
// The shared view-mode vocabulary: this verb's parse chain and the capture response's key table
// are now the same code.
#include "Handlers/Render/ViewModeVocabulary.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
// The readback precondition shared with render.capture_open_level: nothing compiling into the
// renderer when the pixels are asked for.
#include "Utils/CaptureReadinessGate.h"
#include "Utils/CollisionSummaryUtils.h"
#include "Utils/OnScreenMessageSurvey.h"
// The overlay show-flag table shared with the capture verbs, so editor.set_game_view's
// `overlayShowFlags` and every capture's `viewport.overlayShowFlags` name the same bits.
#include "Utils/GameViewOverlayFlags.h"
#include "Utils/JsonUtils.h"
#include "Utils/ScreenshotUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineUtils.h" // TActorIterator - the collision report's level scan
#include "Engine/World.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "IAssetViewport.h"
#include "Modules/ModuleManager.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameViewportClient.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#if __has_include("Subsystems/UnrealEditorSubsystem.h")
#include "Subsystems/UnrealEditorSubsystem.h"
#define MCP_VP_HAS_UNREALEDITOR_SUBSYSTEM 1
#elif __has_include("UnrealEditorSubsystem.h")
#include "UnrealEditorSubsystem.h"
#define MCP_VP_HAS_UNREALEDITOR_SUBSYSTEM 1
#endif
#if __has_include("Subsystems/LevelEditorSubsystem.h")
#include "Subsystems/LevelEditorSubsystem.h"
#define MCP_VP_HAS_LEVELEDITOR_SUBSYSTEM 1
#elif __has_include("LevelEditorSubsystem.h")
#include "LevelEditorSubsystem.h"
#define MCP_VP_HAS_LEVELEDITOR_SUBSYSTEM 1
#endif
#if __has_include("LevelEditor.h")
#include "LevelEditor.h"
#define MCP_VP_HAS_LEVEL_EDITOR_MODULE 1
#else
#define MCP_VP_HAS_LEVEL_EDITOR_MODULE 0
#endif
#include "GameFramework/Actor.h"

// Forces the active level-editor viewport to render synchronously into its
// framebuffer so a subsequent screenshot observes the just-set camera pose
// instead of the stale previous frame. Invalidating alone only marks the
// viewport dirty for the next engine tick; Draw() flushes the frame now.
// The client is resolved through the shared typed level-viewport route rather than
// GetActiveViewport()->GetClient(): under PIE that client is the game client, and calling
// FEditorViewportClient methods on it is undefined behaviour (see EditorHandlerUtils.h).
static void ForceRedrawActiveViewport()
{
  EditorHandlerUtils::ForceRedrawViewportClient(
      EditorHandlerUtils::ResolveActiveLevelViewportClient());
}

// Fallback for editor.screenshot when GEngine->GameViewport is null (normal
// editor, not PIE/game): capture the active level-editor viewport from its
// current camera at its native resolution, reusing the same ReadPixels path as
// render.capture_open_level. Synchronous; OutCapture holds the saved file on
// success.
static bool CaptureActiveLevelViewportToScreenshot(
    const FString &RequestedFilename,
    const FIntPoint &RequestedSize,
    const PinWrightRenderCapture::FExposurePin &Exposure,
    PinWrightRenderCapture::FViewportCaptureOutput &OutCapture,
    FString &OutErrorCode,
    FString &OutErrorMessage,
    const PinWrightRenderCapture::FViewportCaptureHooks *Hooks)
{

  FLevelEditorModule *LevelEditorModule =
      FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
  if (!LevelEditorModule) {
    OutErrorCode = ErrorCodes::ERR_NO_VIEWPORT;
    return false;
  }

  TSharedPtr<IAssetViewport> ActiveViewport =
      LevelEditorModule->GetFirstActiveViewport();
  if (!ActiveViewport.IsValid()) {
    OutErrorCode = ErrorCodes::ERR_NO_VIEWPORT;
    return false;
  }

  FEditorViewportClient &ViewportClient =
      ActiveViewport->GetAssetViewportClient();
  TSharedPtr<FSceneViewport> SceneViewport =
      ActiveViewport->GetSharedActiveViewport();
  if (!SceneViewport.IsValid()) {
    OutErrorCode = ErrorCodes::ERR_NO_VIEWPORT;
    return false;
  }

  // Capture from the viewport's current pose. A supplied size is exact; otherwise preserve
  // the native Slate extent. In both cases keep the camera the user is looking at.
  const FIntPoint ViewportSize = SceneViewport->GetSizeXY();
  PinWrightRenderCapture::FViewportCaptureRequest Request;
  Request.Filename = RequestedFilename;
  Request.Width = RequestedSize.X > 0
      ? RequestedSize.X
      : (ViewportSize.X > 0 ? ViewportSize.X : 1280);
  Request.Height = RequestedSize.Y > 0
      ? RequestedSize.Y
      : (ViewportSize.Y > 0 ? ViewportSize.Y : 960);
  Request.Exposure = Exposure;
  Request.ProjectionMode =
      ViewportClient.IsOrtho() ? TEXT("orthographic") : TEXT("perspective");
  // The EFFECTIVE pose, not the stored one. On a viewport the user has been orbiting
  // (alt-drag, or a camera lock), GetViewLocation / GetViewRotation hold the orbit gizmo's
  // values and the renderer draws from the orbit matrix instead -- 90 degrees of yaw away,
  // from an eye derived from the pivot. Feeding those raw values back through
  // ApplyCaptureCamera, which now turns orbit OFF to aim the camera, would reproduce the
  // numbers rather than the picture and hand the user a screenshot of a different direction
  // than the one on their screen. MeasureEffectiveViewPose returns the pose orbit resolves to,
  // so the free-mode capture renders exactly what the user is looking at -- and the reported
  // cameraRotation becomes one that round-trips into spatial.raycast_screen.
  const PinWrightRenderCapture::FEffectiveViewPose EffectivePose =
      PinWrightRenderCapture::MeasureEffectiveViewPose(ViewportClient);
  Request.Location = EffectivePose.Location;
  Request.Rotation = EffectivePose.Rotation;
  Request.Fov = ViewportClient.ViewFOV;
  // FViewportCaptureRequest::OrthoWidth is world centimetres, not the raw editor zoom.
  Request.OrthoWidth = PinWrightRenderCapture::ComputeOrthoWorldWidthFromZoom(
      ViewportClient, SceneViewport.Get());
  // This verb reproduces whatever the user is looking at, including a freelook orthographic view
  // whose rotation no cardinal ortho viewport type can express. Keep the current viewport type
  // instead of re-deriving (and possibly rejecting) one from the rotation.
  Request.bPreserveViewportType = true;

  return PinWrightRenderCapture::CaptureEditorViewportToPng(
      ViewportClient, SceneViewport, Request, TEXT("Screenshot"), FString(),
      OutCapture, OutErrorCode, OutErrorMessage, Hooks);
}

// ---- editor.focus_actor ----
REGISTER_RPC_HANDLER("editor.focus_actor", "editor", "Frame the named actor in the active viewport (equivalent to selecting it and pressing F). Selects the actor and moves the camera to its bounds.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Actor to focus on; case-insensitively matches the display label, the internal object name (the actor.list / actor.find_by_class 'name' field), or the full object path — the same identifiers every actor.* verb accepts.")
    ))
{
  FString ActorName = Ctx.GetString(TEXT("actorName"));
  if (ActorName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
    return true;
  }

  // Resolve through the shared resolver every actor.* verb uses (label OR internal
  // name OR object path), instead of an inline label-only loop. This makes
  // focus_actor accept the exact identifier actor.list / actor.find_by_class report
  // in their `name` field, removing the inverse-of-actor.* surprise where focusing
  // the internal name (e.g. "BP_Gears_146") that just enumerated returned
  // ACTOR_NOT_FOUND while every other per-actor verb accepted it.
  if (AActor *Actor = McpActorUtils::FindActorByName(nullptr, ActorName)) {
    GEditor->SelectNone(true, true, false);
    GEditor->SelectActor(Actor, true, true, true);
    GEditor->Exec(nullptr, TEXT("EDITORTEMPVIEWPORT"));
    GEditor->MoveViewportCamerasToActor(*Actor, false);
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
  }
  Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                FString::Printf(TEXT("No actor matched '%s' by display label, "
                                     "internal object name, or object path"),
                                *ActorName));
  return true;
}

// ---- editor.set_camera ----
REGISTER_RPC_HANDLER("editor.set_camera", "editor", "Teleport the active level-editor viewport camera to a world-space location and orientation. Either field may be omitted; missing fields default to zero.",
    RPC_PARAMS(
        RPC_PARAM_OPT("location", "object", "World-space camera location {x, y, z} in centimeters."),
        RPC_PARAM_OPT("rotation", "object", "Camera rotation {pitch, yaw, roll} in degrees.")
    ))
{
  const auto& Payload = Ctx.GetRawPayload();
  FVector Location(0, 0, 0);
  FRotator Rotation(0, 0, 0);
  // ReadVectorField/ReadRotatorField unwrap the named sub-object themselves, so
  // pass the whole payload plus the field name (not a pre-unwrapped object with
  // an empty name, which never matches and leaves the pose at the default).
  ReadVectorField(Payload, TEXT("location"), Location, Location);
  ReadRotatorField(Payload, TEXT("rotation"), Rotation, Rotation);

#if defined(MCP_VP_HAS_UNREALEDITOR_SUBSYSTEM)
  if (UUnrealEditorSubsystem *UES =
          GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>()) {
    UES->SetLevelViewportCameraInfo(Location, Rotation);
#if defined(MCP_VP_HAS_LEVELEDITOR_SUBSYSTEM)
    if (ULevelEditorSubsystem *LES =
            GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
      LES->EditorInvalidateViewports();
#endif
    ForceRedrawActiveViewport();
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
  }
#endif
  if (FEditorViewportClient *ViewportClient =
          EditorHandlerUtils::ResolveActiveLevelViewportClient()) {
    ViewportClient->SetViewLocation(Location);
    ViewportClient->SetViewRotation(Rotation);
    ViewportClient->Invalidate(true, true);
    if (FViewport *Viewport = ViewportClient->Viewport)
      Viewport->Draw();
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
  }
  Ctx.SendError(ErrorCodes::ERR_VIEWPORT_NOT_AVAILABLE, TEXT("No active viewport"));
  return true;
}

// Cap on how many names any one list in the collision report carries. The COUNTS are always
// exact; only the enumerations are clipped, so a 10k-actor level cannot turn one view-mode
// switch into a megabyte of JSON. A caller that hits the cap has already learned the thing
// worth learning (that the level has a systemic collision gap), and can drill in per actor.
static constexpr int32 GMaxCollisionReportNames = 100;

// Build the collision data half of a collision-view response.
//
// This exists because the PICTURE CANNOT ANSWER THE QUESTION. A mesh draws nothing at all in a
// collision view when its collision is disabled, when it holds no simple primitives, or when it
// ignores the channel that view queries — and an empty collision view is pixel-identical to a
// perfectly collided scene. Absence of signal reads as absence of problem, the same failure
// shape as the transparent-PNG defect. So the data is the primary output and the image is the
// evidence, not the other way round.
//
// Scope is the whole editor world, stated as scope:"level" in the payload rather than implied:
// answering "which of these are in frame" needs a rebuilt scene view (Render/ViewProjectionUtils),
// which re-poses the live camera and costs far more than this verb — which carries no framing
// parameters at all — can justify. Pair with a capture verb for the picture.
static void PinWrightAddCollisionReport(TSharedPtr<FJsonObject>& Resp, const TCHAR* ChannelName)
{
  using namespace PinWrightCollisionSummary;

  UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
  if (!World) {
    return;
  }

  const bool bSimple = FCString::Stricmp(ChannelName, TEXT("collisionSimple")) == 0;
  const EDrawChannel Channel = bSimple ? EDrawChannel::Simple : EDrawChannel::Complex;

  int32 Inspected = 0;
  int32 NoCollisionCount = 0;
  int32 ComplexOnlyCount = 0;
  int32 ComplexAsSimpleCount = 0;
  int32 UnknownCount = 0;
  int32 DrawnCount = 0;

  TArray<TSharedPtr<FJsonValue>> NoCollisionNames;
  TArray<TSharedPtr<FJsonValue>> ComplexOnlyNames;
  TArray<TSharedPtr<FJsonValue>> ComplexAsSimpleNames;
  TArray<TSharedPtr<FJsonValue>> NotDrawn;

  for (TActorIterator<AActor> It(World); It; ++It) {
    AActor* Actor = *It;
    const FActorCollisionReport Report = BuildActorReport(Actor, Channel);
    if (Report.PrimitiveComponents == 0) {
      // Lights, logic actors, anything with nothing to collide. Reporting these as
      // collisionless would bury the real findings in noise.
      continue;
    }
    ++Inspected;

    if (Report.bAnyDrawnInThisChannel) {
      ++DrawnCount;
    }
    if (Report.bNoCollision) {
      ++NoCollisionCount;
      if (NoCollisionNames.Num() < GMaxCollisionReportNames) {
        NoCollisionNames.Add(MakeShared<FJsonValueString>(Report.Name));
      }
    }
    if (Report.bComplexOnly) {
      ++ComplexOnlyCount;
      if (ComplexOnlyNames.Num() < GMaxCollisionReportNames) {
        ComplexOnlyNames.Add(MakeShared<FJsonValueString>(Report.Name));
      }
    }
    if (Report.bComplexAsSimple) {
      ++ComplexAsSimpleCount;
      if (ComplexAsSimpleNames.Num() < GMaxCollisionReportNames) {
        ComplexAsSimpleNames.Add(MakeShared<FJsonValueString>(Report.Name));
      }
    }
    if (Report.bUnknown) {
      ++UnknownCount;
    }

    // The set the picture cannot show, with WHY — the three invisibility causes are
    // independent and a caller must be able to tell them apart to act.
    if (!Report.bAnyDrawnInThisChannel && NotDrawn.Num() < GMaxCollisionReportNames) {
      TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
      Entry->SetStringField(TEXT("name"), Report.Name);
      Entry->SetStringField(TEXT("class"), Report.Class);
      Entry->SetNumberField(TEXT("simpleShapes"), Report.SimpleShapeTotal);
      Entry->SetBoolField(TEXT("collisionEnabled"), Report.bAnyCollisionEnabled);
      Entry->SetBoolField(TEXT("respondsToChannel"), Report.bAnyRespondsToChannel);
      Entry->SetBoolField(TEXT("drawnInThisChannel"), false);
      const TCHAR* Reason =
          !Report.bAnyCollisionEnabled   ? TEXT("collisionDisabled")
          : !Report.bAnyRespondsToChannel ? TEXT("channelIgnored")
                                          : TEXT("noGeometryInThisChannel");
      Entry->SetStringField(TEXT("reason"), Reason);
      NotDrawn.Add(MakeShared<FJsonValueObject>(Entry));
    }
  }

  TSharedPtr<FJsonObject> Collision = MakeShared<FJsonObject>();
  Collision->SetStringField(TEXT("channel"), ChannelName);
  Collision->SetStringField(TEXT("draws"), bSimple ? TEXT("simple") : TEXT("complex"));
  Collision->SetStringField(TEXT("scope"), TEXT("level"));
  Collision->SetNumberField(TEXT("actorsInspected"), Inspected);

  TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
  Counts->SetNumberField(TEXT("noCollision"), NoCollisionCount);
  Counts->SetNumberField(TEXT("complexOnly"), ComplexOnlyCount);
  Counts->SetNumberField(TEXT("complexAsSimple"), ComplexAsSimpleCount);
  Counts->SetNumberField(TEXT("unknown"), UnknownCount);
  Counts->SetNumberField(TEXT("drawn"), DrawnCount);
  Counts->SetNumberField(TEXT("notDrawn"), Inspected - DrawnCount);
  Collision->SetObjectField(TEXT("counts"), Counts);

  Collision->SetArrayField(TEXT("noCollision"), NoCollisionNames);
  Collision->SetArrayField(TEXT("complexOnly"), ComplexOnlyNames);
  Collision->SetArrayField(TEXT("complexAsSimple"), ComplexAsSimpleNames);
  Collision->SetArrayField(TEXT("notDrawn"), NotDrawn);
  Collision->SetBoolField(TEXT("truncated"),
      NoCollisionCount > NoCollisionNames.Num() || ComplexOnlyCount > ComplexOnlyNames.Num() ||
      ComplexAsSimpleCount > ComplexAsSimpleNames.Num() ||
      (Inspected - DrawnCount) > NotDrawn.Num());
  Collision->SetNumberField(TEXT("nameLimit"), GMaxCollisionReportNames);

  Resp->SetObjectField(TEXT("collision"), Collision);
}

// ---- editor.set_view_mode ----
REGISTER_RPC_HANDLER("editor.set_view_mode", "editor", "Switch the level viewport's rendering view mode (Lit, Unlit, Wireframe, ShaderComplexity, collisionSimple/collisionComplex, etc.) by setting it directly on the viewport client. Recognises common aliases case-insensitively. A viewport client keeps SEPARATE perspective and orthographic view modes, so this verb writes both by default and reports the measured mode of each slot - a mode set only for perspective would leave every orthographic capture rendering the old one. Collision modes also return a per-actor collision report, because a mesh with no collision draws NOTHING and an empty collision view is pixel-identical to a fully collided scene.",
    RPC_PARAMS(
        RPC_PARAM_REQ("viewMode", "string", "View mode name, case- and separator-insensitive over the engine's own EViewModeIndex names - 'front_back_face', 'FrontBackFace' and 'VMI_FrontBackFace' are one request. The legacy keys all still work: Lit, Unlit, Wireframe, DetailLighting, LightingOnly, LightComplexity, ShaderComplexity, LightmapDensity, StationaryLightOverlap, ReflectionOverride, collisionSimple (world/player collision - the simple shapes gameplay traces hit), collisionComplex (precise/visibility collision - per-triangle). A sentinel, a mode that renders identically to Lit, a mode the engine disables on this build, and a mode whose picture is chosen by a separate sub-visualisation are each refused with their own error code rather than quietly rendering something else. PREFER the `viewMode` parameter on the capture verbs for a diagnostic look: it applies the mode for one capture and restores it, where this verb writes it persistently and never restores itself."),
        RPC_PARAM_DEF("projection", "string", "Which of the viewport client's two view-mode slots to write: 'both' (default - the mode applies whether the next capture is perspective or orthographic), 'perspective', 'orthographic' (aliases 'ortho'/'persp'), or 'active' (only the slot the viewport is currently rendering with, the pre-fix behaviour). An unrecognised value is rejected; nothing is written. The response reports the measured mode of BOTH slots either way.", "both"),
        RPC_PARAM_OPT("collisionReport", "boolean", "For the collision view modes only: also return the per-actor collision report (default true). Set false to skip the level scan when only the picture is wanted.")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  // ONE vocabulary, shared with the capture verbs' `viewMode` parameter
  // (Handlers/Render/ViewModeVocabulary.h). This used to be a hand-written if/else chain over
  // twelve modes, kept in step with the capture response's key table by a test; the two are now
  // the same code, so a mode the engine adds is settable here and reportable there with no edit
  // to either. The chain's own quirks are preserved by that vocabulary's override and alias
  // tables, including "Wireframe" selecting VMI_BrushWireframe.
  //
  // NO CLIENT is passed even though one is resolved below: this verb writes the mode
  // PERSISTENTLY, and the pre-selected-sub-visualisation allowance exists for a scoped capture
  // that puts the viewport back. Refusing those modes here keeps the persistent path narrower
  // than the scoped one, which is the safe direction.
  const FString Mode = Ctx.GetString(TEXT("viewMode"));
  const PinWrightViewModes::FViewModeResolution Resolution = PinWrightViewModes::Resolve(Mode);
  if (!Resolution.IsValid()) {
    Ctx.SendError(Resolution.ErrorCode, Resolution.ErrorMessage);
    return true;
  }
  const FString Chosen = Resolution.Key;
  // The token the engine's `viewmode` console command parses, empty when it equals the key.
  const FString ExecName = Resolution.ExecName;
  const EViewModeIndex ViewModeIndex = Resolution.ViewMode;
  // Which collision geometry this mode draws, null for every non-collision mode.
  const TCHAR* CollisionChannelName = Resolution.CollisionChannelName;

  const bool bWantCollisionReport =
      CollisionChannelName != nullptr && Ctx.GetBool(TEXT("collisionReport"), true);

  // Which of the client's two slots to write. Unknown values are rejected rather than falling
  // back to a default, so a typo cannot quietly become "active only" - the shape of the defect
  // this parameter exists to close.
  const FString ProjectionArg = Ctx.GetString(TEXT("projection"), TEXT("both")).ToLower();
  bool bWritePersp = true;
  bool bWriteOrtho = true;
  bool bWriteActiveOnly = false;
  if (ProjectionArg.IsEmpty() || ProjectionArg == TEXT("both")) {
    // defaults above
  } else if (ProjectionArg == TEXT("perspective") || ProjectionArg == TEXT("persp")) {
    bWriteOrtho = false;
  } else if (ProjectionArg == TEXT("orthographic") || ProjectionArg == TEXT("ortho")) {
    bWritePersp = false;
  } else if (ProjectionArg == TEXT("active")) {
    bWriteActiveOnly = true;
  } else {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                  FString::Printf(TEXT("Unrecognised projection '%s'. Valid: both (default), "
                                       "perspective, orthographic, active."),
                                  *ProjectionArg));
    return true;
  }

  // One resolution walk for both the module path and the no-module path, so the readback below
  // is reachable in every case a client exists at all. Mirrors editor.set_game_view.
  FEditorViewportClient* ViewportClient = nullptr;
#if MCP_VP_HAS_LEVEL_EDITOR_MODULE
  FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
  TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();
  if (ActiveViewport.IsValid())
    ViewportClient = &ActiveViewport->GetAssetViewportClient();
#endif
  if (!ViewportClient)
    ViewportClient = EditorHandlerUtils::ResolveActiveLevelViewportClient();

  if (ViewportClient) {
    // FEditorViewportClient keeps PerspViewModeIndex and OrthoViewModeIndex as separate fields
    // (UE 5.8 Editor/UnrealEd/Public/EditorViewportClient.h:2302, :2305) and SetViewMode writes
    // only the one matching the CURRENT projection (EditorViewportClient.cpp:6460-6485). That is
    // why a mode set from a perspective viewport used to leave every orthographic capture
    // rendering the previous mode while the response said the new one had been applied.
    const EViewModeIndex PrevPersp = ViewportClient->GetPerspViewMode();
    const EViewModeIndex PrevOrtho = ViewportClient->GetOrthoViewMode();
    const bool bActiveIsOrtho = ViewportClient->IsOrtho();

    if (bWriteActiveOnly) {
      bWritePersp = !bActiveIsOrtho;
      bWriteOrtho = bActiveIsOrtho;
    }

    const EViewModeIndex TargetPersp = bWritePersp ? ViewModeIndex : PrevPersp;
    const EViewModeIndex TargetOrtho = bWriteOrtho ? ViewModeIndex : PrevOrtho;

    // SetViewMode first when the active slot is one of the targets: it is the only entry point
    // that also clears ViewModeParam / ViewModeParamName / ViewModeParamNameMap
    // (EditorViewportClient.cpp:6462-6464), which SetViewModes does not, so skipping it would
    // leave a stale LODColoration-style index attached to the new mode. SetViewModes then writes
    // BOTH slots (EditorViewportClient.cpp:6576-6592) and re-applies the show flags for whichever
    // one is active. Calling both is one redundant ApplyViewMode and no behavioural overlap.
    const bool bWritingActiveSlot = bActiveIsOrtho ? bWriteOrtho : bWritePersp;
    if (bWritingActiveSlot) {
      ViewportClient->SetViewMode(ViewModeIndex);
    }
    ViewportClient->SetViewModes(TargetPersp, TargetOrtho);
    ViewportClient->Invalidate();

    // Everything reported below is READ BACK off the client after the write. Nothing here is the
    // requested value echoed: the whole defect was a response that described the request.
    const EViewModeIndex MeasuredPersp = ViewportClient->GetPerspViewMode();
    const EViewModeIndex MeasuredOrtho = ViewportClient->GetOrthoViewMode();
    const EViewModeIndex MeasuredActive = bActiveIsOrtho ? MeasuredOrtho : MeasuredPersp;

    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    Applied->SetStringField(TEXT("perspective"),
                            PinWrightRenderCapture::GetViewModeKey(MeasuredPersp));
    Applied->SetStringField(TEXT("orthographic"),
                            PinWrightRenderCapture::GetViewModeKey(MeasuredOrtho));
    Applied->SetStringField(TEXT("activeProjection"),
                            bActiveIsOrtho ? TEXT("orthographic") : TEXT("perspective"));

    TSharedPtr<FJsonObject> Previous = MakeShared<FJsonObject>();
    Previous->SetStringField(TEXT("perspective"), PinWrightRenderCapture::GetViewModeKey(PrevPersp));
    Previous->SetStringField(TEXT("orthographic"), PinWrightRenderCapture::GetViewModeKey(PrevOrtho));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    // The active slot's measured mode, so `viewMode` keeps meaning "what this viewport renders
    // with right now" - it is just no longer a copy of the request.
    Resp->SetStringField(TEXT("viewMode"), PinWrightRenderCapture::GetViewModeKey(MeasuredActive));
    Resp->SetStringField(TEXT("requestedViewMode"), Chosen);
    Resp->SetStringField(TEXT("projection"), ProjectionArg.IsEmpty() ? TEXT("both") : *ProjectionArg);
    Resp->SetObjectField(TEXT("applied"), Applied);
    Resp->SetObjectField(TEXT("previous"), Previous);
    // True only on this branch, where both slots were read back off the client.
    Resp->SetBoolField(TEXT("verified"), true);
    if (bWantCollisionReport) {
      PinWrightAddCollisionReport(Resp, CollisionChannelName);
    }
    Ctx.SendSuccess(Resp);
    return true;
  }

  // No viewport client resolvable at all. Route the per-viewport console command through the
  // editor world so older builds still reach one, but do NOT claim a slot was written: with no
  // client there is nothing to read back, and `viewMode` is omitted rather than filled in with
  // the request. `verified:false` is the field a caller must branch on.
  UWorld* World = GEditor->GetEditorWorldContext().World();
  const FString Cmd = FString::Printf(TEXT("viewmode %s"), ExecName.IsEmpty() ? *Chosen : *ExecName);
  if (GEditor->Exec(World, *Cmd)) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("requestedViewMode"), Chosen);
    Resp->SetStringField(TEXT("projection"), ProjectionArg.IsEmpty() ? TEXT("both") : *ProjectionArg);
    Resp->SetBoolField(TEXT("verified"), false);
    Resp->SetStringField(TEXT("unverifiedReason"),
        TEXT("No level-editor viewport client was resolvable, so the mode was issued as the "
             "'viewmode' console command and neither view-mode slot could be read back. Which "
             "projection it reached, if any, is unknown - re-issue this call once a level "
             "viewport exists if the mode matters."));
    if (bWantCollisionReport) {
      PinWrightAddCollisionReport(Resp, CollisionChannelName);
    }
    Ctx.SendSuccess(Resp);
    return true;
  }
  Ctx.SendError(ErrorCodes::ERR_EXEC_FAILED, TEXT("View mode command failed"));
  return true;
}

// ---- editor.set_viewport_realtime ----
REGISTER_RPC_HANDLER("editor.set_viewport_realtime", "editor", "Enable or disable continuous (realtime) rendering of the active level-editor viewport. Disabling saves GPU when the editor is idle but freezes animated previews.",
    RPC_PARAMS(
        EditorHandlerUtils::EditorToggleParamOpt(TEXT("realtime"), TEXT("True to render every frame, false to render on demand only. Defaults to true."))
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  bool bRealtime = Ctx.GetBoolFirstOf(EditorHandlerUtils::ToggleKeys(), true);

#if MCP_VP_HAS_LEVEL_EDITOR_MODULE
  FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
  TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();

  if (ActiveViewport.IsValid()) {
    FEditorViewportClient& ViewportClient = ActiveViewport->GetAssetViewportClient();
    ViewportClient.SetRealtime(bRealtime);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("realtime"), bRealtime);
    Resp->SetStringField(TEXT("message"), bRealtime ? TEXT("Viewport realtime enabled") : TEXT("Viewport realtime disabled"));
    Ctx.SendSuccess(Resp);
    return true;
  }
#endif

  // Fallback: use console command
  FString Command = bRealtime ? TEXT("Viewport Realtime") : TEXT("Viewport Realtime 0");
  UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
  GEditor->Exec(World, *Command);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetBoolField(TEXT("realtime"), bRealtime);
  Resp->SetStringField(TEXT("message"), bRealtime ? TEXT("Viewport realtime enabled") : TEXT("Viewport realtime disabled"));
  Ctx.SendSuccess(Resp);
  return true;
}

// The overlay flag table this verb reports now lives in Utils/GameViewOverlayFlags.h, because the
// capture verbs report the same block: the frame that carried the un-suppressed spline line came
// out of a capture, not out of this verb, and a reviewer reading that PNG never has to call this
// one. See the header for which flags are in the set and why `splines` is among them.

// ---- editor.set_game_view ----
REGISTER_RPC_HANDLER("editor.set_game_view", "editor", "Toggle 'Game View' in the active viewport — hides editor-only sprites/icons/billboards so the viewport renders like the shipped game (G key in the editor). Returns the PREVIOUS value so a caller can put the viewport back, and the MEASURED overlay show-flags afterwards, because game view does not cover every overlay: component visualizers and the editor-mode render passes are separate mechanisms, and the spline flag in particular has been observed still on after a confirmed game-view enable.",
    RPC_PARAMS(
        EditorHandlerUtils::EditorToggleParamOpt(TEXT("enabled"), TEXT("True to enable game view, false to disable. Defaults to true."))
    ))
{
  bool bEnabled = Ctx.GetBoolFirstOf(EditorHandlerUtils::ToggleKeys(), true);

  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  // Resolve the active level-editor viewport client and toggle Game View on it
  // directly. "ToggleGameView" is NOT an engine exec/console command — it exists
  // only as the G-key FUICommandInfo (LevelViewportActions.cpp) bound to
  // SLevelViewport::ToggleGameView -> FEditorViewportClient::SetGameView. The old
  // GEditor->Exec(World, "ToggleGameView 1") therefore matched no exec handler,
  // returned false, and silently no-op'd while this handler still reported
  // gameViewEnabled:true. SetGameView(bool) swaps EngineShowFlags between the Game
  // and Editor sets, which is what hides the editor-only billboards/sprites. This
  // mirrors set_view_mode/set_viewport_realtime's viewport resolution.
  FEditorViewportClient *ViewportClient = nullptr;

#if MCP_VP_HAS_LEVEL_EDITOR_MODULE
  FLevelEditorModule &LevelEditorModule =
      FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
  TSharedPtr<IAssetViewport> ActiveViewport =
      LevelEditorModule.GetFirstActiveViewport();
  if (ActiveViewport.IsValid())
    ViewportClient = &ActiveViewport->GetAssetViewportClient();
#endif

  // Fallback to the active level-editor viewport client (the set_camera pattern)
  // so a build without the LevelEditor module, or a moment with no active level
  // viewport, still reaches a real client instead of the former phantom Exec.
  // ResolveActiveLevelViewportClient() is the shared GEditor->GetActiveViewport()
  // walk, reused here instead of re-cloning the cast (it also null-guards the
  // resolved client, which the VIEWPORT_NOT_AVAILABLE check below already covered).
  if (!ViewportClient)
    ViewportClient = EditorHandlerUtils::ResolveActiveLevelViewportClient();

  if (!ViewportClient) {
    Ctx.SendError(ErrorCodes::ERR_VIEWPORT_NOT_AVAILABLE, TEXT("No active viewport"));
    return true;
  }

  // Captured BEFORE the toggle. Game view is per-viewport state this verb does not restore
  // itself (a capture burst wants it to persist), so the only way an agent can leave the editor
  // as it found it is to be told what it was - and until now the value was unreadable from here.
  const bool bPreviousGameView = ViewportClient->IsInGameView();
  TSharedPtr<FJsonObject> PreviousOverlays = MakeShared<FJsonObject>();
  PinWrightAddGameViewOverlayFlags(PreviousOverlays,
      PinWrightReadGameViewOverlayFlags(ViewportClient->EngineShowFlags));

  ViewportClient->SetGameView(bEnabled);
  // SetGameView invalidates for the next tick; force a synchronous frame now so a
  // screenshot taken on this call stack observes the icon-free frame. Reuses the
  // shared force-a-frame-now idiom (Invalidate + Draw) instead of open-coding it.
  EditorHandlerUtils::ForceRedrawViewportClient(ViewportClient);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  // Report the REAL post-toggle state, never echo the unverified request value.
  const bool bNowGameView = ViewportClient->IsInGameView();
  Resp->SetBoolField(TEXT("gameViewEnabled"), bNowGameView);

  TSharedPtr<FJsonObject> Previous = MakeShared<FJsonObject>();
  Previous->SetBoolField(TEXT("gameViewEnabled"), bPreviousGameView);
  Previous->SetObjectField(TEXT("overlayShowFlags"), PreviousOverlays);
  Resp->SetObjectField(TEXT("previous"), Previous);

  // Measured after the toggle, off the same client. This is the block that says what game view
  // did and did not cover; every value is read from EngineShowFlags, none is a literal.
  TSharedPtr<FJsonObject> Overlays = MakeShared<FJsonObject>();
  PinWrightAddGameViewOverlayFlags(Overlays,
      PinWrightReadGameViewOverlayFlags(ViewportClient->EngineShowFlags));
  Resp->SetObjectField(TEXT("overlayShowFlags"), Overlays);

  // Component visualizers are drawn from FLevelEditorViewportClient::Draw under an explicit
  // !IsInGameView() guard (UE 5.8 Editor/UnrealEd/Private/LevelEditorViewport.cpp:5088-5091), so
  // this one IS derived from game view - stated rather than left implicit, because it is the
  // mechanism behind the spline handles and tangent arrows on a selected water body.
  Resp->SetBoolField(TEXT("componentVisualizersSuppressed"), bNowGameView);

  // What game view does NOT govern. Listed unconditionally so the response never reads as a
  // blanket "the viewport is now clean" claim.
  TArray<TSharedPtr<FJsonValue>> NotGoverned;
  NotGoverned.Add(MakeShared<FJsonValueString>(
      TEXT("editorModeRender: active editor modes draw through FEditorModeTools::Render, which "
           "carries no game-view guard - a landscape/spline/geometry tool keeps drawing its "
           "handles and dashed helper lines in game view. Deactivate the mode instead.")));
  NotGoverned.Add(MakeShared<FJsonValueString>(
      TEXT("debugDrawn: anything issued through DrawDebug* or a persistent line batcher is world "
           "geometry to the renderer and is unaffected by any show flag listed here.")));
  NotGoverned.Add(MakeShared<FJsonValueString>(
      TEXT("showFlagCVarOverride: every show flag also has a ShowFlag.<Name> console variable "
           "(0 = force it off, 1 = force it on, 2 = do not override, the default). A non-default "
           "value is ORed over the view's flags AFTER this viewport's own are copied, so it is "
           "process-global, it survives a game-view toggle, and none of the overlayShowFlags "
           "reported here shows it - a Visualize* flag forced on draws a full-screen debug pass "
           "over the frame. The capture verbs survey these and publish "
           "viewport.showFlagOverrides; restore one with system.console_command "
           "\"ShowFlag.<Name> 2\".")));
  Resp->SetArrayField(TEXT("notGovernedByGameView"), NotGoverned);

  if (bNowGameView && ViewportClient->EngineShowFlags.Splines != 0)
  {
    // The measured contradiction, spelled out. SetGameView only installs a fresh game flag set
    // when neither the current nor the saved flags already claim to be the game set
    // (EditorViewportClient.cpp:7229-7240); otherwise it reuses a saved set that can carry
    // Splines on. A caller reading `gameViewEnabled: true` alone would have no way to know.
    Resp->SetStringField(TEXT("overlayWarning"),
        TEXT("Game view is on but EngineShowFlags.Splines is still set, so spline components "
             "(water bodies, landscape splines, any USplineComponent with bDrawDebug) still draw "
             "into this viewport. A line running along a river in a capture is that overlay, not "
             "foam. Toggle game view off and on again to force a fresh game flag set, or clear "
             "the Splines show flag on the viewport, before treating the frame as shipped "
             "geometry."));
  }

  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.screenshot ----
REGISTER_RPC_HANDLER("editor.screenshot", "editor", "Capture a PNG screenshot into Saved/Screenshots/. Uses the game/PIE viewport when one exists; otherwise falls back to the active level-editor viewport. Optional paired width/height render an exact-size frame; on the game viewport this includes Slate/UMG at the requested UIScaleCurve value. Runs as a synchronously-completed tracked job. Filename is sanitised against path traversal.",
    RPC_PARAMS(
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/. Auto-generated as 'Screenshot_<timestamp>.png' when empty. The .png extension is appended if missing."),
        RPC_PARAM_OPT("width", "number", "Exact output width in pixels. Must be supplied together with height; omit both for the live viewport size."),
        RPC_PARAM_OPT("height", "number", "Exact output height in pixels. Must be supplied together with width; omit both for the live viewport size."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC)
    ))
{
  const FString RequestedFilename = Ctx.GetString(TEXT("filename"));
  const TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
  const bool bHasWidth = Payload.IsValid() && Payload->HasField(TEXT("width"));
  const bool bHasHeight = Payload.IsValid() && Payload->HasField(TEXT("height"));
  if (bHasWidth != bHasHeight)
  {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          TEXT("width and height must be supplied together, or both omitted"));
      return true;
  }

  FIntPoint RequestedSize = FIntPoint::ZeroValue;
  if (bHasWidth)
  {
      RequestedSize.X = Ctx.GetInt(TEXT("width"));
      RequestedSize.Y = Ctx.GetInt(TEXT("height"));
      const int64 RequestedPixels = static_cast<int64>(RequestedSize.X)
          * static_cast<int64>(RequestedSize.Y);
      if (RequestedSize.X <= 0 || RequestedSize.Y <= 0
          || RequestedSize.X > PinWrightScreenshotUtils::MaxGameViewportCaptureDimension
          || RequestedSize.Y > PinWrightScreenshotUtils::MaxGameViewportCaptureDimension
          || RequestedPixels > PinWrightScreenshotUtils::MaxGameViewportCapturePixels)
      {
          Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
              FString::Printf(TEXT("width and height must be in (0, %d] and total at most %lld pixels"),
                  PinWrightScreenshotUtils::MaxGameViewportCaptureDimension,
                  static_cast<long long>(PinWrightScreenshotUtils::MaxGameViewportCapturePixels)));
          return true;
      }
  }

  PinWrightRenderCapture::FExposurePin Exposure;
  FString ParseErrorCode;
  FString ParseErrorMessage;
  if (!PinWrightRenderCapture::ParseExposurePin(
          Payload, Exposure, ParseErrorCode, ParseErrorMessage))
  {
      Ctx.SendError(ParseErrorCode, ParseErrorMessage);
      return true;
  }

  FString Filename;
  const FString OutPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
      RequestedFilename, TEXT("Screenshot"), FString(), Filename);

  FJobBindArgs Args;
  Args.Method = TEXT("editor.screenshot");
  Args.StartedPayload = MakeShared<FJsonObject>();
  Args.StartedPayload->SetStringField(TEXT("requested_path"), OutPath);

  Args.BindNativeDelegate =
      [OutPath, RequestedFilename, RequestedSize, Exposure](FJobOnComplete OnComplete)
  {
      // The readback preamble's shared state. Both branches fill it immediately before their own
      // readback -- never at handler entry, where the pose that queues the compiles has not been
      // applied yet.
      const TSharedRef<PinWrightCaptureReadiness::FReadinessResult> Readiness =
          MakeShared<PinWrightCaptureReadiness::FReadinessResult>();
      const auto RefuseNotReady = [&OnComplete, Readiness](const FString& Message)
      {
          // The job registry keeps the payload on a failed ticket, so the evidence and the reason
          // stay readable through system.job_status while the error code stays greppable.
          TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
          PinWrightCaptureReadiness::AddReadinessFields(*Readiness, Details);
          Details->SetStringField(TEXT("message"), Message);
          OnComplete(false, Details, ErrorCodes::ERR_CAPTURE_NOT_READY);
      };

      if (!GEngine || !GEngine->GameViewport)
      {
          // No game/PIE viewport: capture the level-editor viewport directly.
          // CaptureEditorViewportToPng is synchronous, so complete the job now. The gate and the
          // flush ride the BeforeFinalFrame hook, which is the point after the pose and the settle
          // loop and immediately before the final draw + readback. Binding the hook costs one
          // extra draw + readback, and that is the point: unhooked, the returned pixels would be
          // the settle loop's last read, taken before the flush.
          PinWrightRenderCapture::FViewportCaptureHooks GatedHooks;
          GatedHooks.BeforeFinalFrame =
              [Readiness](const PinWrightRenderCapture::FViewportCaptureOutput& /*SettledCapture*/,
                  FString& OutErrorCode, FString& OutErrorMessage)
          {
              *Readiness = PinWrightCaptureReadiness::DrainBeforeReadback();
              if (Readiness->ShouldRefuseReadback())
              {
                  OutErrorCode = ErrorCodes::ERR_CAPTURE_NOT_READY;
                  OutErrorMessage = PinWrightCaptureReadiness::MakeRefusalMessage(*Readiness);
                  return false;
              }
              Readiness->bReadbackFlushed = PinWrightScreenshotUtils::FlushBeforeReadback();
              return true;
          };

          PinWrightRenderCapture::FViewportCaptureOutput Capture;
          FString ErrorCode;
          FString ErrorMessage;
          if (CaptureActiveLevelViewportToScreenshot(
                  RequestedFilename, RequestedSize, Exposure, Capture, ErrorCode, ErrorMessage,
                  &GatedHooks))
          {
              auto R = MakeShared<FJsonObject>();
              R->SetNumberField(TEXT("width"),  Capture.Width);
              R->SetNumberField(TEXT("height"), Capture.Height);
              R->SetStringField(TEXT("path"),   Capture.Path);
              R->SetStringField(TEXT("captureSource"), TEXT("levelEditorViewport"));
              R->SetBoolField(TEXT("fixedSize"), RequestedSize != FIntPoint::ZeroValue);
              const TSharedPtr<FJsonObject> ViewportBlock =
                  PinWrightRenderCapture::MakeViewportInfoObject(Capture);
              PinWrightCaptureReadiness::AddReadinessFields(*Readiness, ViewportBlock);
              PinWrightOnScreenMessages::AddOnScreenMessageFields(
                  PinWrightOnScreenMessages::Survey(), ViewportBlock);
              R->SetObjectField(TEXT("viewport"), ViewportBlock);
              OnComplete(true, R, FString());
          }
          else if (ErrorCode == ErrorCodes::ERR_CAPTURE_NOT_READY)
          {
              RefuseNotReady(ErrorMessage);
          }
          else
          {
              OnComplete(false, nullptr, ErrorCode);
          }
          return;
      }
      // Game/PIE viewport active. The async FScreenshotRequest +
      // OnScreenshotCaptured delegate does not reliably fire under PIE-in-editor,
      // which left this job hung in "running" forever. Capture synchronously via
      // the shared game-viewport->PNG path (the same mechanism ui.screenshot uses
      // on this exact viewport) and complete the job in-line — no async wait, no
      // watchdog.
      int32 Width = 0, Height = 0;
      FString ErrorCode;
      PinWrightScreenshotUtils::FGameViewportCaptureOptions CaptureOptions;
      CaptureOptions.OutputSize = RequestedSize;
      CaptureOptions.bPinExposure = Exposure.WantsPin();
      CaptureOptions.ExposureEv100 = Exposure.Ev100;

      // Immediately before the readback: nothing on this branch draws or moves a camera between
      // here and CaptureGameViewportToPngFile, so this IS the last point before the pixels are
      // asked for. The flush half runs inside that call, at the last point before its three
      // readback branches, and comes back on the metadata.
      *Readiness = PinWrightCaptureReadiness::DrainBeforeReadback();
      if (Readiness->ShouldRefuseReadback())
      {
          RefuseNotReady(PinWrightCaptureReadiness::MakeRefusalMessage(*Readiness));
          return;
      }

      PinWrightScreenshotUtils::FGameViewportCaptureMetadata Metadata;
      if (PinWrightScreenshotUtils::CaptureGameViewportToPngFile(
              OutPath, Width, Height, ErrorCode, nullptr, &CaptureOptions, &Metadata))
      {
          auto R = MakeShared<FJsonObject>();
          R->SetNumberField(TEXT("width"),  Width);
          R->SetNumberField(TEXT("height"), Height);
          R->SetStringField(TEXT("path"),   OutPath);
          R->SetStringField(TEXT("captureSource"), TEXT("gameViewport"));
          R->SetBoolField(TEXT("fixedSize"), RequestedSize != FIntPoint::ZeroValue);
          R->SetStringField(TEXT("captureMode"), Metadata.bUsedOffscreenComposite
              ? TEXT("fixedSizeScenePlusUmg")
              : (Metadata.bUsedNativeBackBuffer
                  ? TEXT("nativeBackBuffer")
                  : TEXT("sceneOnlyFallback")));
          R->SetNumberField(TEXT("dpiScale"), Metadata.DpiScale);
          R->SetBoolField(TEXT("viewportRestored"), Metadata.bViewportRestored);

          TSharedPtr<FJsonObject> ExposureInfo = MakeShared<FJsonObject>();
          ExposureInfo->SetStringField(TEXT("mode"),
              PinWrightRenderCapture::ExposureModeKey(Exposure.Mode));
          ExposureInfo->SetBoolField(TEXT("pinRequested"), Exposure.WantsPin());
          ExposureInfo->SetBoolField(TEXT("pinned"), Metadata.bExposureApplied);
          ExposureInfo->SetBoolField(TEXT("restored"), Metadata.bExposureRestored);
          ExposureInfo->SetNumberField(TEXT("viewCount"), Metadata.ExposureViewCount);
          if (Exposure.WantsPin())
          {
              ExposureInfo->SetNumberField(TEXT("ev100"), Exposure.Ev100);
          }
          R->SetObjectField(TEXT("exposure"), ExposureInfo);

          // The game/PIE path publishes no FViewportCaptureOutput, so it carries the two
          // frame-state blocks on a `viewport` object of its own: what was still compiling when
          // this readback went out, and what text the engine drew into the pixels.
          Readiness->bReadbackFlushed = Metadata.bReadbackFlushed;
          const TSharedPtr<FJsonObject> ViewportBlock = MakeShared<FJsonObject>();
          PinWrightCaptureReadiness::AddReadinessFields(*Readiness, ViewportBlock);
          PinWrightOnScreenMessages::AddOnScreenMessageFields(
              PinWrightOnScreenMessages::Survey(), ViewportBlock);
          R->SetObjectField(TEXT("viewport"), ViewportBlock);
          OnComplete(true, R, FString());
      }
      else
      {
          OnComplete(false, nullptr, ErrorCode);
      }
  };
  Ctx.StartJob(Args);
  return true;
}
