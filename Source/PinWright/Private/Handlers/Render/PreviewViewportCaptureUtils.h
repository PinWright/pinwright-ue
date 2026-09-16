// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Editor/UnrealEdTypes.h"
// EViewModeIndex, for GetViewModeKey / IsLitViewMode below.
#include "Engine/EngineBaseTypes.h"
#include "Handlers/Render/CaptureDefaults.h"
// FGrassBuildReport, held BY VALUE on the capture output below. Included rather than
// forward-declared for that reason; it is a plain struct over Core types and pulls in nothing
// from the Landscape module (the .cpp does that).
#include "Handlers/Render/LandscapeGrassSettle.h"
// FPreviewSceneRigPin / FPreviewSceneRigReport, held BY VALUE on the capture request and the
// capture output below. Included rather than forward-declared for that reason; it in turn pulls
// in the AdvancedPreviewScene module's AssetViewerSettings.h, which PinWright.Build.cs gains for
// this wave.
#include "Handlers/Render/PreviewSceneRig.h"
// FSubjectRegionStats, held BY VALUE on the capture output below. Included rather than
// forward-declared for that reason; it is a plain struct over Core types and deliberately does
// NOT include this header back (see the note on its Rec.709 helper).
#include "Handlers/Render/SubjectRegionStats.h"
// FGameViewOverlayShowFlags, held BY VALUE on the capture output below - the overlay show-flag
// table shared with editor.set_game_view, so both responses name the same bits. Deliberately a
// header that forward-declares FEngineShowFlags rather than including ShowFlags.h, for the same
// reason this one does.
#include "Utils/GameViewOverlayFlags.h"

class FEditorViewportClient;
class FSceneViewport;
class FViewport;
class FJsonObject;
class UWorld;
// FViewportCameraTransform -- the editor viewport's camera state, taken by const reference by
// ResolveEffectiveViewPose. Forward-declared for the same reason FEngineShowFlags below is: only
// the two files that touch its members include EditorViewportClient.h.
struct FViewportCameraTransform;
// FEditorViewportClient::EngineShowFlags. Forward-declared rather than pulling ShowFlags.h into
// every translation unit that includes this header -- the sprite helpers below only ever take it
// by reference, and the two files that touch the bits (the capture util and its test) include
// ShowFlags.h themselves.
struct FEngineShowFlags;

// The `hideEditorSprites` parameter's wire description, shared verbatim by every capture verb that
// renders a LEVEL world, for the same one-definition reason as PINWRIGHT_EXPOSURE_PARAM_DESC.
//
// Not offered on the asset/animation preview verbs. The CONCLUSION is right and re-verified
// against UE 5.8 on 2026-08-21; the reason it used to give ("no AActor anywhere") was not, and is
// corrected here because two chunks of the capture-subject wave nearly acted on it.
//
// What the flag reaches: exactly three component classes gate draw relevance on
// EngineShowFlags.BillboardSprites -- UArrowComponent (ArrowComponent.cpp:170), UBillboardComponent
// (BillboardComponent.cpp:214) and UMaterialBillboardComponent (MaterialBillboardComponent.cpp:249).
//
// What the preview scenes contain: FPreviewScene registers a UDirectionalLightComponent,
// USkyLightComponent and ULineBatchComponent (PreviewScene.cpp:38, :84-97) and FAdvancedPreviewScene
// adds a sky mesh, a UPostProcessComponent and a floor mesh (AdvancedPreviewScene.cpp:63-93), with
// no SpawnActor in either. The Static Mesh editor viewport (SStaticMeshEditorViewport.cpp:143, :489)
// and the Niagara system viewport (SNiagaraSystemViewport.cpp:871) both build exactly that scene and
// only AddComponent into it, so neither holds anything the flag can act on -- and Niagara's own
// renderers never read the flag, so a particle capture has nothing to lose to it either.
//
// The exception, and why it does not change the answer: Persona's preview world DOES hold actors --
// AAnimationEditorPreviewActor (PersonaToolkit.cpp:163), which has no sprite components, and, only
// after the artist toggles wind on in the viewport, an AWindDirectionalSource whose editor-only
// UArrowComponent (bTreatAsASprite=true) and UBillboardComponent do gate on the flag
// (AnimationEditorPreviewScene.cpp:1121, :1166-1180). So the only thing this parameter could ever
// hide in a preview is a user-toggled wind gizmo -- never the light bulbs, audio icons or player
// start it documents. Declaring it there would publish a knob whose stated purpose is unreachable,
// which is worse than not offering it.
#define PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC \
    "Hide the editor-only icon sprites -- light bulbs, audio icons, player start, note and camera " \
    "icons, and the directional-light arrow -- for the duration of this capture, then put the show " \
    "flag back. Default FALSE, which leaves the viewport exactly as it is: flipping it would " \
    "silently change the pixels every existing caller already gets. Pass true for an acceptance " \
    "screenshot, where an icon over the geometry is an artefact of the editor rather than " \
    "something in the level. Mechanism: EngineShowFlags.BillboardSprites, the single flag " \
    "UBillboardComponent / UMaterialBillboardComponent / UArrowComponent gate their draw relevance " \
    "on (UE 5.8 Runtime/Engine/Private/Components/BillboardComponent.cpp:214). It does NOT cover " \
    "selection outlines, the grid, editor-mode handles or component visualizers (spline handles on " \
    "a selected water body) -- for those use editor.set_game_view, which this parameter " \
    "deliberately does not call because game view is viewport state a capture cannot round-trip " \
    "safely. The response's viewport.editorSprites block reports what was applied and whether it " \
    "was restored."

// The `exposure` parameter's wire description, shared verbatim by every capture verb that accepts
// it. One definition because eight RPC_PARAMS blocks that each spell the contract slightly
// differently is how two verbs end up documented as accepting different things while running the
// same parser (PinWrightRenderCapture::ParseExposurePin).
#define PINWRIGHT_EXPOSURE_PARAM_DESC \
    "Pin exposure for this capture and restore the viewport's previous exposure afterwards. " \
    "Omit to leave auto-exposure running (unchanged behaviour). Pass a bare number as shorthand " \
    "for {mode:\"fixed\", ev100:<number>}, or the object form: {mode:\"fixed\", ev100:N} pins at " \
    "absolute EV100 N (range -30..30); {mode:\"auto\"} explicitly asks for auto-exposure and takes " \
    "NO ev100 -- the two contradict each other and the pair is refused rather than half-applied. " \
    "Auto-exposure is a live scalar gain that re-balances between shots, so two unpinned captures " \
    "of the same scene are not comparable and a real change can read as a no-op. The response's " \
    "viewport.exposure block reports what actually happened, including pinned:false plus a " \
    "pinWarning when the pin was requested but the render mode ignores it. To make N shots " \
    "comparable at the exposure the scene resolves to: take one {mode:\"auto\"} shot, read " \
    "viewport.exposure.ev100Equivalent from it, and pass THAT as ev100 on the rest. Do NOT pass " \
    "`adapted` -- it is the renderer's linear scene-colour gain, not an EV100, and feeding it back " \
    "as ev100 underexposes by several stops (a gain of 4 is EV100 -2, not EV100 4)."

// The `viewMode` parameter's wire description, shared verbatim by every capture verb, for the same
// one-definition reason as the two macros above.
//
// The mechanism sentence is load-bearing: `editor.set_view_mode` writes the viewport
// PERSISTENTLY and never restores itself, so a diagnostic view taken that way leaks into every
// later capture and into what the user is looking at. This parameter is the scoped alternative -
// it is applied for the duration of ONE capture and put back on every exit path, error paths
// included, and the response reports the MEASURED mode rather than the requested one.
#define PINWRIGHT_VIEW_MODE_PARAM_DESC \
    "Render this capture in a different view mode, then put the viewport's previous mode back. " \
    "Scoped: BOTH the perspective and orthographic view-mode slots are written and both are " \
    "restored on every exit path, so nothing leaks into the next capture or into what the user " \
    "sees - unlike editor.set_view_mode, which writes persistently and deliberately does not " \
    "restore itself. Omit to capture in whatever mode the viewport is already in (unchanged " \
    "behaviour). Spelling is case- and separator-insensitive over the engine's own EViewModeIndex " \
    "names, so 'front_back_face', 'FrontBackFace' and 'VMI_FrontBackFace' are one request; the " \
    "legacy keys (Lit, Unlit, Wireframe, DetailLighting, LightingOnly, LightComplexity, " \
    "ShaderComplexity, LightmapDensity, StationaryLightOverlap, ReflectionOverride, " \
    "collisionSimple, collisionComplex) all still work. The most useful diagnostics: " \
    "'front_back_face' colours front and back faces differently and is the VISUAL counterpart to " \
    "health.signedVolume - an inside-out closed shell reads as entirely back-facing from outside " \
    "and is otherwise pixel-identical to a correct mesh; 'unlit' removes lighting, which kills the " \
    "whole class of shadow-acne / cast-shadow / final-gather artifacts that get misread as " \
    "geometry defects; 'random_color' gives separate static-mesh primitive components different " \
    "colours, which can show a detached island; it does not distinguish sections or islands inside " \
    "one skeletal-mesh component, whose sections render uniformly in this mode; 'zebra' and 'clay' " \
    "show surface continuity and curvature " \
    "on a neutral material; 'lightmap_density' shows the baked lightmap resolution. A mode that " \
    "renders identically to Lit, a sentinel, a mode the engine disables on this build, and a mode " \
    "whose picture is chosen by a separate sub-visualisation are all REFUSED with a specific error " \
    "rather than quietly rendering something else. The response's viewport.viewModeOverride block " \
    "reports what was measured on the viewport and whether it was restored."

// The `previewScene` parameter's wire description, shared verbatim by every capture verb that
// drives an asset-editor preview scene, for the same one-definition reason as the three macros
// above.
//
// The mechanism sentence is load-bearing for a different reason than `viewMode`'s. The state this
// parameter touches is not per-viewport: `UAssetViewerSettings` is a PROCESS-WIDE object every open
// asset editor shares, and any "Preview Scene Settings" tab teardown flushes it to the COMMITTED
// Config/DefaultEditor.ini with no dirty check of any kind
// (UE 5.8 AdvancedPreviewScene/Private/AssetViewerSettings.cpp:118-146,
// SAdvancedPreviewDetailsTab.cpp:46). So the restore is measured at three levels and all three are
// published -- a caller has to be able to see that a capture did not leave a source-controlled
// file rewritten.
#define PINWRIGHT_PREVIEW_SCENE_PARAM_DESC     "Light this capture with an explicit preview-scene rig, then put the scene's previous rig "     "back. Scoped: the light COMPONENTS, the process-wide UAssetViewerSettings profile array and "     "the bytes of the committed Config/DefaultEditor.ini are all restored on every exit path "     "including errors, and all three restores are MEASURED and reported under "     "viewport.previewScene.restore. Omit to capture under whatever rig the editor's preview scene "     "already has (unchanged behaviour). Shape: {key:{azimuth,elevation,intensity,color}, "     "sky:{intensity}, showFloor, showEnvironment}; every field optional, but an empty object is "     "REFUSED rather than read as absent. azimuth and elevation are where the key light ARRIVES "     "from - azimuth is a compass bearing in degrees, elevation is height above the horizon in "     "[-90,90] - and they must be supplied together or not at all, because half an aim silently "     "keeps the other half of a rig nobody measured. The engine's shipped default is arrival "     "azimuth 112.5, elevation 40, which is why an asset lit only from that side reads as a black "     "silhouette from the opposite one and gets misreported as a geometry defect. WHY THIS EXISTS: "     "a preview scene belongs to the editor showing it, so the same call on two machines returns "     "different pixels at the same pinned exposure - measured at mean luminance 0.47 against 0.18 "     "- and nothing in the response used to say so. Pin key and sky explicitly to make two "     "machines agree. There is deliberately NO profile-selection field: every lighting outcome the "     "shipped 'Grey Ambient' profile produces is reachable as key.intensity 4.0 + sky.intensity "     "2.0 without the process-wide broadcast and the silent tone-curve change that switching "     "profiles carries. The response's viewport.previewScene block reports the MEASURED rig the "     "pixels were drawn under, the profile name and index, the previous rig, the rig after the "     "restore, and whether the shared profile array and the config file came back unchanged."

namespace PinWrightRenderCapture
{
    struct FCaptureActorLockState;

    inline float ResolveCaptureProjectionAspect(int32 Width, int32 Height)
    {
        return Height > 0 ? static_cast<float>(Width) / static_cast<float>(Height) : 1.0f;
    }

    // Make CalcSceneView take its projection aspect from the capture dimensions. A level viewport
    // can be locked to a camera actor; in that state the engine otherwise takes FOV, projection and
    // aspect from the actor's FMinimalViewInfo even after the viewport has been resized. Capture
    // requests own those fields, so temporarily disable that independent source and restore it.
    class FScopedCaptureProjectionAspect
    {
    public:
        FScopedCaptureProjectionAspect(FEditorViewportClient& InClient, int32 Width, int32 Height);
        ~FScopedCaptureProjectionAspect();

        FScopedCaptureProjectionAspect(const FScopedCaptureProjectionAspect&) = delete;
        FScopedCaptureProjectionAspect& operator=(const FScopedCaptureProjectionAspect&) = delete;

    private:
        FEditorViewportClient& Client;
        float PreviousAspectRatio = 1.0f;
        TUniquePtr<FCaptureActorLockState> ActorLockState;
    };

    // ---- the two caps on an automatically derived r.ViewDistanceScale ----
    //
    // The first cap is the ENGINE's, not a taste judgement, and it is the reason a cap has to
    // exist at all. UE 5.8 truncates the scaled foliage cull distance into an int32:
    //   Runtime/Engine/Private/HierarchicalInstancedStaticMesh.cpp:1675
    //     int32 EndCullDistance = UserData_AllInstances.EndCullDistance * MaxDrawDistanceScale;
    // Push that product past INT32_MAX and the conversion is undefined; the observable failure is
    // a negative EndCullDistance, i.e. "cull everything" -- the exact defect this override exists
    // to fix, reached by trying too hard to fix it. MaxScaledCullDistance keeps the product at
    // half of INT32_MAX so the derived scale can never approach the truncation, and it is the
    // cap that binds for any cull distance above ~1 km (see MaxAutoViewDistanceScaleFor).
    //
    // The second is an absolute ceiling for degenerate scenes (a sub-metre cull distance in a
    // kilometre-wide frame), where the derived scale would be large enough that the capture is
    // drawing essentially every primitive in the level at full detail. 1048576 covers every cull
    // distance down to 2 cm against the ~2.1e6 cm reach an orthographic frame actually has to
    // clear (see FViewDistanceSurvey), so in practice it binds only on authoring mistakes.
    //
    // NEITHER cap is load-bearing for correctness on a normal map: the derived scale is
    // computed from the measured culling origin, so it lands far below both. When a cap does
    // bind, the capture says so and says what is still culled -- it never reports a clean
    // override it did not achieve.
    constexpr float MaxAutoViewDistanceScale = 1048576.0f;
    constexpr double MaxScaledCullDistance = 1073741823.0;

    // Safety factor on the derived scale. The renderer compares squared floats and fades
    // primitives within GDistanceFadeMaxTravel of their cull radius, so landing exactly on the
    // inequality is landing on the boundary. 5% costs nothing and moves off it.
    constexpr double AutoViewDistanceScaleMargin = 1.05;

    // Largest scale that is safe for a scene whose smallest finite cull distance is
    // MinCullDistance -- min(absolute ceiling, the int32 truncation bound above). Pure, so the
    // relationship between the two caps is assertable without a level.
    float MaxAutoViewDistanceScaleFor(double MinCullDistance);
    // Default orthographic frame width in WORLD CENTIMETRES. Shared by every verb that takes
    // `orthoWidth` (render.capture_asset_preview / capture_open_level / capture_annotated,
    // spatial.raycast_screen, spatial.place_on_surface) so an omitted value always reconstructs
    // the same view -- a per-handler literal would silently desync the pose-coherence contract.
    constexpr float DefaultOrthoWorldWidth = 2000.0f;

    // Half-angle tolerance (degrees) within which a requested orthographic camera direction still
    // counts as pointing down a cardinal world axis. See ResolveOrthographicView.
    constexpr float OrthoAxisToleranceDegrees = 1.0f;

    // The engine's legacy CAMERA_ZOOM_DIV. Only used as the fallback zoom scale when there is no
    // FViewport to calibrate against; the live path measures the engine instead (see
    // ComputeOrthoZoomForWorldWidth).
    constexpr float EditorOrthoZoomDivisor = 15.0f;

    // How far the MEASURED camera pose may sit from the requested one before a capture reports
    // bCameraAimApplied=false. Loose enough to absorb the rotator round trip through a 4x4 matrix
    // (MeasureEffectiveViewPose inverts one) and the engine's own location clamp
    // (FViewportCameraTransform::SetLocation bounds the position to a cube), tight enough that the
    // orbit-camera defect -- a full 90 degrees of yaw, and an eye placed on a different side of
    // the subject entirely -- can never pass.
    constexpr double AimToleranceDegrees = 0.5;
    constexpr double AimToleranceCentimetres = 1.0;

    // ---- capture-time exposure pin ----
    //
    // WHY A CAPTURE PARAMETER AND NOT A PRIOR CALL. Auto-exposure is a live scalar gain over the
    // whole frame that re-balances between shots, so an edit that DID work is partly cancelled by
    // the camera re-exposing and gets recorded as a no-op. Pinning used to require a separate
    // `lighting.set_exposure` call first, which (a) nothing enforces, (b) mutates the level -- it
    // find-or-spawns an unbound APostProcessVolume -- and (c) reaches only the level world, so it
    // cannot pin an asset-preview viewport at all. This parameter pins the viewport the capture is
    // about to draw, for the duration of that capture only.
    //
    // WHAT IS WRITTEN. `FEditorViewportClient::ExposureSettings`
    // (UE 5.8 Editor/UnrealEd/Public/EditorViewportClient.h:1986) -- the editor's own
    // override-the-automatic-expose slot, the one the viewport toolbar's EV100 control drives.
    // FEditorViewportClient::Draw copies it into the view family
    // (Editor/UnrealEd/Private/EditorViewportClient.cpp:4876) and the renderer reads
    // `View.Family->ExposureSettings.bFixed`
    // (Runtime/Renderer/Private/PostProcess/PostProcessEyeAdaptation.cpp:641), where it clamps
    // min and max white-point luminance to one value derived from FixedEV100 (:516). That branch
    // sits ABOVE the PostProcessVolume path in the same if/else chain, so a pinned capture is
    // immune to whatever exposure settings the level's volumes carry -- which is exactly the
    // property "these two frames are comparable" needs.
    //
    // WHERE IT IS IGNORED, WHICH IS WHY THE RESPONSE REPORTS A MEASURED `pinned`. The whole chain
    // is the `else` of IsAutoExposureDebugMode (PostProcessEyeAdaptation.cpp:493-511): a frame
    // with `EngineShowFlags.PostProcessing` or `EngineShowFlags.Lighting` off, a collision or
    // buffer-visualization view, or an unlit/debug view mode never applies the override at all.
    // The pin is written and the pixels ignore it, which is a silent no-op of exactly the kind
    // this parameter exists to remove -- so the capture measures whether it governs and says so.
    //
    // AUTO ONCE, THEN PIN AT WHAT IT REPORTED, IS A SUPPORTED FLOW -- and it is the flow the
    // rationale above invites, so it has to work rather than merely not be forbidden.
    //
    // It broke once on a units error worth naming, because the shape of it recurs. The response
    // reported (and still reports) `adapted`: FSceneViewStateInterface::GetLastEyeAdaptationExposure
    // (SceneManagement.h:198), which is a LINEAR GAIN the tonemapper multiplies scene colour by --
    // NOT an EV100. The two run in opposite directions and differ by a log: a scene resolving at
    // EV100 -1.90 reports adapted 3.74. A caller doing the obvious thing and passing 3.74 back as
    // `ev100` therefore shot 5.6 stops under and got a black frame while every field in the
    // response said the pin had succeeded -- which it had. Nothing was broken except the units.
    //
    // The fix is the conversion, published as `ev100Equivalent` beside the gain. It is not a
    // guessed re-derivation: EV100ToLuminance / LuminanceToEV100 are PUBLIC inline helpers in
    // Runtime/RenderCore/Public/RenderUtils.h:744-763 (an earlier version of this comment claimed
    // they were private to the Renderer module and used that to justify shipping the raw gain --
    // they are not, and that claim is what cost the caller the black frame). Only
    // LuminanceMaxFromLensAttenuation is private, and it is a pure function of two cvars this
    // module can read; see ExposureLuminanceMax.
    enum class EExposureRequestMode : uint8
    {
        // The caller said nothing. Byte-for-byte today's behaviour: the viewport's exposure
        // settings are not touched, read or written.
        Unset,
        // The caller explicitly asked for auto-exposure. Same rendering as Unset; recorded in the
        // response so an explicit "do not pin" is distinguishable from an omitted parameter.
        Auto,
        // Pin at an absolute EV100 for the duration of this capture.
        Fixed,
    };

    // Accepted range for a caller-supplied EV100. Wide enough to cover anything physically
    // meaningful (EV100 -30 is far below moonlight, +30 far above direct sun) and narrow enough
    // that a mistyped magnitude is refused instead of rendering a black or blown frame that
    // reports a successful pin.
    constexpr float MinExposureEv100 = -30.0f;
    constexpr float MaxExposureEv100 = 30.0f;

    // --- warm-up settle budget (see FViewportCaptureOutput::WarmupSettleRounds) ---
    //
    // The frame is considered settled when its mean luminance stops moving between two consecutive
    // pumped reads. Both tolerances measured 2026-08-19 on /Engine/BasicShapes/Cube at 512x512,
    // pinned at ev100 0: two back-to-back captures into a warm viewport moved the frame mean by
    // 1.5e-5, two into a freshly reopened one by 1.1e-4, and the warm-up gap this exists to close
    // moved it by 0.19. The absolute tolerance sits ~9x above the worst settled pair and ~190x
    // below the unsettled one; the relative term takes over on bright frames.
    //
    // A settled viewport therefore costs exactly ONE extra PumpViewport + ReadPixels: the loop has
    // to draw a second frame to observe that the first one did not move, and there is no cheaper
    // way to establish that -- an unmeasured "it is probably fine" is what shipped the 2.26-stop
    // frame in the first place. The cost is published as `warmup.settleMs` on every capture rather
    // than asserted here, because it is per-scene and this comment cannot measure the caller's.
    constexpr double WarmupSettleAbsTolerance = 0.001;
    constexpr double WarmupSettleRelTolerance = 0.005;
    // Bounds. Both are hit only by a viewport that genuinely will not settle, and the capture then
    // returns the frame it has with bWarmupSettled false rather than blocking or failing -- a
    // capture verb that hangs is worse than one that reports an unsettled frame.
    constexpr int32 MaxWarmupSettleRounds = 8;
    constexpr double MaxWarmupSettleMs = 1500.0;

    struct FExposurePin
    {
        EExposureRequestMode Mode = EExposureRequestMode::Unset;
        float Ev100 = 0.0f;
        // The only mode that writes anything. Everything else leaves the viewport alone.
        bool WantsPin() const { return Mode == EExposureRequestMode::Fixed; }
    };

    // Wire spelling of a mode, for the response. "unset" | "auto" | "fixed".
    const TCHAR* ExposureModeKey(EExposureRequestMode Mode);

    // A scoped view-mode override for one capture. Default-constructed (bRequested false) writes
    // nothing at all, so the omitted-parameter path is byte-for-byte what it was before this
    // field existed -- the same contract FExposurePin::Unset and bHideEditorSprites:false carry.
    struct FViewModePin
    {
        bool bRequested = false;
        EViewModeIndex ViewMode = VMI_Lit;
        // The canonical key the request resolved to, which is also what a capture reports, so a
        // response feeds straight back into a request.
        FString Key;
        // EngineShowFlags names ApplyViewMode writes differently for this mode than for VMI_Lit.
        // Carried on the request so the capture can MEASURE them off the live client after the
        // write -- the check that separates "the mode was applied" from "the call succeeded".
        TArray<FString> DistinguishingShowFlags;
        // Non-empty when the mode renders a sub-visualisation that was already selected on the
        // viewport (see PinWrightViewModes::EViewModeStatus::NeedsCompanion).
        FString CompanionMode;
        bool WantsOverride() const { return bRequested; }
    };

    // Read the `viewMode` wire parameter off a payload root. Absent -> an unset pin with no error.
    // Present but unusable -> false plus a registered error code and a message naming what is
    // wrong, because a view mode that silently falls back to Lit produces a plausible picture of
    // the wrong thing. Exported for the same reason ParseExposurePin is: the capture verbs that
    // hand-build their FViewportCaptureRequest must read the SAME vocabulary as the ones that go
    // through ParseViewportCaptureRequest.
    //
    // Client is optional. Passing the viewport that will render lets a mode whose companion
    // sub-visualisation is already selected through; without it such a mode is always refused.
    bool ParseViewModePin(const TSharedPtr<FJsonObject>& Payload, FViewModePin& OutPin,
        FString& OutErrorCode, FString& OutErrorMessage,
        const FEditorViewportClient* Client = nullptr);

    // ---- exposure gain <-> EV100 ----
    //
    // TWO QUANTITIES, ONE OF WHICH LOOKS LIKE THE OTHER. The renderer carries exposure as a
    // linear GAIN: the scalar the tonemapper multiplies scene colour by. `ev100` is a log stop
    // scale running the other way -- higher EV100 is a DARKER image. A gain of 3.74 is EV100
    // -1.90; passing 3.74 where an EV100 belongs is 5.6 stops of error that no field in the
    // response can catch, because both numbers are valid in their own unit. These two functions
    // are the only sanctioned crossing.
    //
    // The engine's own LuminanceMaxFromLensAttenuation (Runtime/Renderer/Private/PostProcess/
    // PostProcessEyeAdaptation.cpp:376-389) is declared in that module's PRIVATE header and
    // cannot be linked from here. It is a pure function of two cvars, so this reads THOSE rather
    // than hardcoding a result: r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange gates
    // it entirely (0 -> 1.0, the unitless-lighting case), and r.EyeAdaptation.LensAttenuation
    // divides the ISO 12232:2006 saturation constant 0.78. A project that changes either gets the
    // changed value here on the next call, which a baked constant would silently not.
    float ExposureLuminanceMax();

    // The gain the renderer applies to a frame pinned at this EV100. Closed form, not a
    // measurement: on the fixed branch GetEyeAdaptationFixedExposure
    // (PostProcessEyeAdaptation.cpp:810-825) reduces to 1 / EV100ToLuminance(LuminanceMax, EV100)
    // -- the middle-grey factors cancel and ExposureCompensation is forced to 1 at :644-646.
    float Ev100ToExposureGain(float Ev100);

    // The inverse, and the one the response publishes as `ev100Equivalent`: the EV100 a caller
    // pins at to reproduce a gain the renderer resolved on its own. Exact round trip with
    // Ev100ToExposureGain. Returns 0 for a non-positive gain, which callers must gate on
    // `adaptedMeasured` rather than read as an exposure.
    float ExposureGainToEv100(float ExposureGain);

    // Read the `exposure` wire parameter off a payload root. Accepts a bare number as shorthand
    // for {mode:"fixed", ev100:N}, the object form {mode:"fixed", ev100:N}, and {mode:"auto"} on
    // its own -- "auto" with an ev100 is a contradiction and is an error, not a half. Absent ->
    // Unset with no error. Exported so the capture verbs that hand-build their
    // FViewportCaptureRequest (camera.frame_actor, camera.orbit_shots, camera.animation_shots,
    // render.capture_animation_preview) read the SAME wire vocabulary as the ones that go through
    // ParseViewportCaptureRequest -- a second parse is how two verbs end up disagreeing about
    // what `exposure` means.
    bool ParseExposurePin(const TSharedPtr<FJsonObject>& Payload, FExposurePin& OutPin,
        FString& OutErrorCode, FString& OutErrorMessage);

    // Force a view mode for the life of one capture and put the previous one back.
    //
    // BOTH SLOTS, NOT ONE. FEditorViewportClient keeps PerspViewModeIndex and OrthoViewModeIndex
    // as separate fields (UE 5.8 Editor/UnrealEd/Public/EditorViewportClient.h:2302, :2305) and
    // GetViewMode() returns whichever matches the CURRENT projection
    // (EditorViewportClient.cpp:6617-6620). A scope that wrote only the active slot would leave
    // every capture through the other projection rendering the old mode while reporting the new
    // one - the defect editor.set_view_mode's `projection` parameter already exists to close -
    // and a scope that RESTORED only one would leave the other permanently moved. So both are
    // saved, both are written, and both are restored.
    //
    // SetViewModes, NEVER SetViewMode. SetViewMode additionally clears ViewModeParam,
    // ViewModeParamName and ViewModeParamNameMap (EditorViewportClient.cpp:6462-6464) and the
    // engine exposes no getter for ViewModeParam, so a scope that called it could not put that
    // state back - which is the whole contract here. SetViewModes writes the two slots and
    // re-applies the show flags for whichever is active (EditorViewportClient.cpp:6576-6592) and
    // touches nothing else.
    //
    // NO ANALOGUE OF THE POSE-RESTORE BUG. The known defect in the pose restore is that
    // FViewportCameraTransform state read before a projection-type change can be written back
    // into the OTHER transform, because SetViewLocation / SetLookAtLocation route through
    // GetViewTransform(). The two view-mode slots are plain fields reached by GetPerspViewMode /
    // GetOrthoViewMode / SetViewModes and are not routed through any transform, so nothing here
    // can cross them. Ordering inside CaptureEditorViewportToPng is still deliberate: this
    // scope's destructor runs BEFORE the pose guard restores the viewport type, and
    // SetViewportType then re-applies ApplyViewMode(GetViewMode(), IsPerspective(),
    // EngineShowFlags) (EditorViewportClient.cpp:2054-2062) for the RESTORED slot, so the show
    // flags land on the original mode without a second write.
    //
    // Exported rather than file-local so the restore contract is assertable directly on a
    // viewport client, with no SceneViewport, no GPU and no readback - the capture-level test
    // that needs pixels is the one that goes quiet when another process holds the GPU.
    class FScopedViewModeOverride
    {
    public:
        FScopedViewModeOverride(FEditorViewportClient& InClient, const FViewModePin& Pin);
        ~FScopedViewModeOverride();

        FScopedViewModeOverride(const FScopedViewModeOverride&) = delete;
        FScopedViewModeOverride& operator=(const FScopedViewModeOverride&) = delete;

        bool WasApplied() const { return bApplied; }
        EViewModeIndex GetPreviousPersp() const { return PreviousPersp; }
        EViewModeIndex GetPreviousOrtho() const { return PreviousOrtho; }

    private:
        FEditorViewportClient& Client;
        EViewModeIndex PreviousPersp = VMI_Lit;
        EViewModeIndex PreviousOrtho = VMI_Lit;
        bool bApplied = false;
    };

    struct FViewportCaptureOutput;

    struct FViewportCaptureRestoreReceipt
    {
        bool bExposureRestored = true;
        bool bTemporalAntiAliasingRestored = true;
        bool bViewportSizeAndFixedStateRestored = true;
    };

    // Owns the capture state that must precede a positive viewport resize. Pose sets share one
    // instance across every frame; direct captures create a local one-call instance.
    class FViewportCaptureSetContext final
    {
    public:
        FViewportCaptureSetContext(
            FEditorViewportClient& ViewportClient,
            const TSharedPtr<FSceneViewport>& SceneViewport,
            const FExposurePin& Exposure,
            FViewportCaptureRestoreReceipt& OutRestoreReceipt);
        ~FViewportCaptureSetContext();

        FViewportCaptureSetContext(const FViewportCaptureSetContext&) = delete;
        FViewportCaptureSetContext& operator=(const FViewportCaptureSetContext&) = delete;

        bool IsValid() const;
        void PrepareForDraw(uint32 Width, uint32 Height);
        void ReassertPins();
        bool PopulateFrameEvidence(FViewportCaptureOutput& OutCapture) const;

    private:
        struct FImpl;
        TUniquePtr<FImpl> Impl;
    };

    struct FViewportCaptureRequest
    {
        FString Filename;
        int32 Width = DefaultCaptureEdge;
        int32 Height = DefaultCaptureEdge;
        FString ProjectionMode = TEXT("perspective");
        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        float Fov = 50.0f;
        // Orthographic frame width in WORLD CENTIMETRES (the world span the image covers left to
        // right) -- NOT the editor's raw ortho zoom. ApplyCaptureCamera converts it to a zoom with
        // the engine's own GetOrthoUnitsPerPixel scale, which folds in CAMERA_ZOOM_DIV and the
        // width-dependent r.Editor.AlignedOrthoZoom factor.
        float OrthoWidth = DefaultOrthoWorldWidth;
        // Whether the caller actually supplied location/rotation (vs. the parser
        // defaulting them to origin/zero). Lets a handler frame the no-args path
        // without re-reaching into the raw payload and re-doing HasField checks --
        // all wire-field knowledge (names + aliases + defaults) stays in the parser.
        bool bLocationProvided = false;
        bool bRotationProvided = false;
        // Keep the viewport client's CURRENT ELevelViewportType instead of deriving one from
        // Rotation. Set only by editor.screenshot, which mirrors whatever the user is looking at
        // (including an LVT_OrthoFreelook pose that no cardinal ortho type can express); every
        // other caller supplies a pose and must get the viewport type that renders it.
        bool bPreserveViewportType = false;
        // Blank-frame rejection is opt-in at the shared utility layer because some preview and
        // screenshot callers intentionally capture black content. render.capture_open_level sets
        // this true; allowBlank is the caller's explicit escape hatch for a deliberately black
        // level or camera pose.
        bool bRejectBlankCapture = false;
        bool bAllowBlank = false;
        // r.ViewDistanceScale forced for the duration of this capture and restored afterwards.
        // <= 0 means "leave the cvar alone". See bAutoViewDistanceScale for how an omitted value
        // is resolved, and FViewDistanceSurvey for why an override is needed at all.
        float ViewDistanceScale = 0.0f;
        bool bViewDistanceScaleProvided = false;
        // Derive ViewDistanceScale from the scene when the caller omitted it. Set only by
        // render.capture_open_level, and only for orthographic captures: an asset-preview
        // viewport has no level world to survey, and changing the perspective path would move
        // every existing measurement in every project that already captures that way.
        bool bAutoViewDistanceScale = false;
        // Exposure pin for the duration of this capture, restored afterwards. Default-constructed
        // (Unset) means the viewport's exposure settings are never touched -- see the
        // EExposureRequestMode comment above for the mechanism and its limits.
        FExposurePin Exposure;
        // Clear EngineShowFlags.BillboardSprites for the duration of this capture and restore it
        // afterwards. False -- the default -- writes nothing at all, so the omitted-parameter path
        // is byte-for-byte what it was before this field existed. See
        // PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC for what the flag does and does not cover.
        bool bHideEditorSprites = false;
        // View mode forced for the duration of this capture and restored afterwards, BOTH slots.
        // Default-constructed (bRequested false) writes nothing -- see FViewModePin.
        FViewModePin ViewMode;
        // Preview-scene rig applied for the duration of this capture and restored afterwards, at
        // all three levels (components, shared profile array, committed config file).
        // Default-constructed (bRequested false) applies nothing -- but note that the guard still
        // SNAPSHOTS AND RESTORES the shared profile array on that path, because the engine mutates
        // it without being asked (SNiagaraSystemViewport.cpp:872, AdvancedPreviewScene.cpp:188).
        // See FPreviewSceneRigPin and FScopedPreviewSceneRig.
        PinWrightPreviewSceneRig::FPreviewSceneRigPin PreviewSceneRig;
        // Internal pose-set handoff. When true, CaptureCameraPoses owns one rig guard around the
        // whole set and supplies the entry report; individual frames must only measure the rig in
        // force. Wire parsers never set either field.
        bool bPreviewSceneRigAlreadyScoped = false;
        PinWrightPreviewSceneRig::FPreviewSceneRigReport PreviewSceneRigAtSetEntry;
        // Same handoff, for the sky/reflection capture drain the set-owning guard ran once. Every
        // shot in the set is lit by that one drain, so every shot reports both halves of it.
        bool bPreviewSceneCaptureUpdatedAtSetEntry = false;
        bool bPreviewSceneCaptureIncompleteAtSetEntry = false;
        // Internal pose-set handoff. The pointed-to context outlives the synchronous frame loop.
        // Wire parsers never set it; null preserves direct one-call capture behavior.
        FViewportCaptureSetContext* ViewportCaptureSetContext = nullptr;

        // Keep the frame's pixels on the output instead of throwing them away after the PNG is
        // encoded.
        //
        // OFF BY DEFAULT AND IT MUST STAY THAT WAY: a retained 1024x1024 frame is 4 MB, and every
        // multi-shot verb holds one FViewportCaptureOutput per pose for the life of the call, so a
        // 24-shot set would carry 96 MB for no reason. It exists for measurements that need TWO
        // buffers of the same pose or a retained burst for stale-pose validation. The subject-
        // coverage differential and render.capture_animation_preview are the callers.
        //
        // Retained BEFORE the PNG encode, so what is measured is what ReadPixels returned rather
        // than a decoded PNG in a different gamma space (the mismatch recorded in
        // Tests/Render/TestCaptureExposurePin.cpp:1275).
        bool bRetainPixels = false;

        // The subject's world-space bounding sphere, for the subject-region measurement.
        //
        // A RADIUS OF 0 -- the default -- means "this caller has no bounds", and the capture then
        // reports `subjectRegion.measured: false` with that as the reason rather than inventing a
        // region. Supplied here rather than measured after the fact because the pixels are free
        // only while ReadPixels' buffer is still alive inside the capture; reading them again
        // afterwards would mean retaining the whole frame (see bRetainPixels above, and why it is
        // off by default).
        FVector BoundsOrigin = FVector::ZeroVector;
        double BoundsRadius = 0.0;
    };

    // What a level capture needs to know to size its view-distance override.
    //
    // WHY THIS EXISTS. Distance culling is radial from ONE point, and for an orthographic editor
    // view that point IS NOT THE CAMERA. Two separate effects stack:
    //
    //  1. An orthographic frame's world coverage comes from orthoWidth and is independent of how
    //     far the camera is, so a whole-map top-down has frame corners tens of thousands of
    //     centimetres from the camera while its centre is a few thousand.
    //
    //  2. The renderer measures from `View.CullingOrigin`
    //     (Runtime/Renderer/Private/SceneVisibility.cpp:867), which is
    //     `ViewMatrices.GetViewOrigin()` (Runtime/Engine/Private/SceneView.cpp:844). For a LIT
    //     orthographic editor view that origin is pushed BACKWARDS along the view direction by
    //     the near-plane correction: FEditorViewportClient sets
    //     `OrthoNearClipPlane = -UE_OLD_WORLD_MAX` (= -2097152, EngineDefines.h:37) at
    //     Editor/UnrealEd/Private/EditorViewportClient.cpp:1420, and
    //     `FSceneViewProjectionData::UpdateOrthoPlanes` applies
    //     `ViewOrigin += ViewForward * (0 + NearPlane)` at SceneView.cpp:609. So EVERY primitive
    //     in the frame sits ~2.1e6 cm from the point the cull measures from, however close the
    //     camera is. The HISM path eats the same offset -- its per-cluster distance is measured
    //     from `View->GetTemporalLODOrigin(...)`
    //     (HierarchicalInstancedStaticMesh.cpp:1652), which returns the same view origin
    //     (SceneView.cpp:1217).
    //
    //     This offset is CONDITIONAL and must not be hardcoded: it needs
    //     `ViewFamily->ViewMode > VMI_Unlit` (EditorViewportClient.cpp:1407 -> :1417 ->
    //     SceneView.cpp:619) and `r.Ortho.AllowNearPlaneCorrection != 0` (SceneView.cpp:579), so
    //     a Wireframe or Unlit capture has exactly zero pushback. That is why CullingOrigin below
    //     is MEASURED off the FSceneView the capture is about to render, not computed from a
    //     constant. A derivation that measures from the camera under-scales by whatever the
    //     pushback happens to be, reports `overridden: true`, and recovers nothing.
    //
    // The mechanism the scale then defeats is the same for both kinds of geometry the shot cares
    // about (UE 5.8):
    //   - foliage / instanced meshes: Runtime/Engine/Private/HierarchicalInstancedStaticMesh.cpp
    //     computes `EndCullDistance = UserData_AllInstances.EndCullDistance * MaxDrawDistanceScale`
    //     and folds it into FinalCull, which CalcLOD then uses to drop whole cluster nodes.
    //   - ordinary primitives: Runtime/Renderer/Private/SceneVisibility.cpp FrustumCull scales
    //     `Bounds.MaxCullDistance` by the same MaxDrawDistanceScale.
    // In both cases MaxDrawDistanceScale is `GetCachedScalabilityCVars().ViewDistanceScale`, i.e.
    // r.ViewDistanceScale -- one lever that covers both.
    //
    // Note what is NOT the cause, because it was the first hypothesis and it is wrong: the
    // screen-size cull is already disabled for orthographic views
    // (`MinSize = bIsOrtho ? 0.0f : CVarFoliageMinimumScreenSize`), so foliage with NO cull
    // distance set is not culled in ortho at all. Only a finite cull distance culls.
    struct FViewDistanceSurvey
    {
        // The point the renderer will measure cull distances from, and the point every distance
        // in this struct is measured from. Read off the FSceneView the capture is about to draw
        // (MeasureCaptureCullingOrigin); falls back to the camera location when no view could be
        // built, which bCullingOriginMeasured reports rather than hides.
        FVector CullingOrigin = FVector::ZeroVector;
        bool bCullingOriginMeasured = false;
        // How far the culling origin sits behind the camera. ~2097152 for a lit orthographic
        // editor view, 0 for perspective and for unlit/wireframe ortho. Reported because it is
        // the whole reason a wide ortho needs a scale two orders of magnitude larger than its
        // own frame size would suggest.
        double CullingOriginPushback = 0.0;
        // Smallest finite cull distance found on any primitive in the world, in centimetres.
        // 0 means nothing in the world is distance-culled and no override is needed.
        double MinCullDistance = 0.0;
        // Farthest any surveyed primitive's bounding sphere reaches from the CULLING ORIGIN.
        double MaxPrimitiveDistance = 0.0;
        // The scale the scene actually requires, BEFORE margin and caps: the largest
        // reach/cullDistance ratio over the primitives that can be culled, each measured against
        // ITS OWN cull distance.
        //
        // This is deliberately not MaxPrimitiveDistance/MinCullDistance. That form pairs the
        // reach of one primitive with the cull distance of a different one, so a single actor
        // with a huge bounding sphere (the host map has one at 7.6e12 cm) pins the derived scale
        // to the cap regardless of what is actually at risk -- which is how a cap becomes
        // load-bearing by accident. Only a primitive with a finite cull distance can be lost, so
        // only such a primitive gets to set the requirement.
        double RequiredScale = 0.0;
        // Largest non-zero MinDrawDistance found, and the closest any primitive comes to the
        // culling origin. A scale multiplies NEAR culling too
        // (SceneVisibility.cpp:999 MinDrawDistanceSq = Square(MinDrawDistance * Scale)), so
        // these two bound how far the scale can be pushed before near geometry starts vanishing.
        // 0 MaxMinDrawDistance means nothing in the world near-culls and the bound does not apply.
        double MaxMinDrawDistance = 0.0;
        double MinPrimitiveDistance = 0.0;
        int32 NumPrimitives = 0;
        // How many of them carry a finite cull distance -- the population at risk.
        int32 NumCulledPrimitives = 0;
        // How many of those are instanced-mesh components (foliage), reported separately because
        // forest paths and clearings are made of exactly these and nothing else.
        int32 NumInstancedComponents = 0;
        // `foliage.MaxEndCullDistance` as it read at survey time, in centimetres (0 = disabled,
        // which is the engine default). This is the ONE term in the instanced path that
        // r.ViewDistanceScale cannot lift, because the engine applies it AFTER the multiply:
        //   HierarchicalInstancedStaticMesh.cpp:1675-1686
        //     int32 EndCullDistance = UserData_AllInstances.EndCullDistance * MaxDrawDistanceScale;
        //     if (MaxEndCullDistance > 0) EndCullDistance = (EndCullDistance > 0)
        //         ? FMath::Min(MaxEndCullDistance, EndCullDistance) : MaxEndCullDistance;
        // What follows from that, and it is ONE consequence rather than the two an earlier version
        // of this comment claimed: a non-zero ceiling bounds what any scale can reach, so a frame
        // whose foliage sits beyond it cannot be recovered by this lever at all. That is disclosed
        // (NumFoliageCeilingLimited plus a warning) rather than scaled at.
        //
        // The claim that was WRONG, recorded because it shipped and cost a red test: that the
        // ceiling should also be SUBSTITUTED as the cull distance of an instanced component which
        // sets none of its own, on the reasoning that the engine culls such a component at the
        // ceiling and the survey would otherwise read it as un-cullable. The engine does cull it
        // there -- and no r.ViewDistanceScale ever moves it, because the multiply above is
        // `EndCullDistance * MaxDrawDistanceScale` and the component's EndCullDistance is 0. So
        // such a component has no derivable requirement in either direction, and substituting the
        // ceiling only invented a reach/ceiling ratio (always <= 1, since a larger reach is the
        // ceiling-limited case) and pulled the ceiling into MinCullDistance.
        //
        // 0 -- the engine default, and this project's setting -- means NO CEILING, not a ceiling
        // of zero: the engine's whole clamp is inside `if (MaxEndCullDistance > 0)`. Nothing is
        // ceiling-limited at 0 and the survey is bit-identical to its pre-ceiling behaviour.
        double FoliageMaxEndCullDistance = 0.0;
        // HIERARCHICAL instanced components whose reach from the culling origin is past that
        // ceiling. No r.ViewDistanceScale recovers them, so they are counted and reported instead
        // of being allowed to drive the derived scale to a cap it cannot cash in.
        //
        // HISM, not "instanced": both reads of the cvar are inside FHierarchicalStaticMeshSceneProxy
        // (HierarchicalInstancedStaticMesh.cpp:1658, :1870) and only
        // UHierarchicalInstancedStaticMeshComponent builds that proxy (:3004). A plain
        // UInstancedStaticMeshComponent gets FInstancedStaticMeshSceneProxy
        // (InstancedStaticMesh.cpp:2600) and is never clamped, so counting one here would exclude
        // from RequiredScale a component the scale genuinely recovers. (A Nanite-rendered HISM
        // takes Nanite::FSceneProxy and is likewise never clamped; that is NOT modelled, because
        // whether a component renders as Nanite depends on a material audit this survey does not
        // run, and guessing wrong the other way would under-report the limit.)
        int32 NumFoliageCeilingLimited = 0;
        bool bValid = false;
    };

    // The point the renderer will measure cull distances from for the pose already applied to
    // this viewport client. Builds the same FSceneView the capture is about to draw and reads
    // `FSceneView::CullingOrigin` -- the exact value FrustumCull uses -- so no engine constant is
    // duplicated here and the orthographic pushback is picked up whether or not it applies.
    //
    // MUST be called AFTER ApplyCaptureCamera: it reads the client's live pose. Sets
    // bOutMeasured false and returns the client's view location when no view can be built
    // (headless host, no scene), which is the honest fallback rather than a guessed offset.
    FVector MeasureCaptureCullingOrigin(FEditorViewportClient& ViewportClient,
        const TSharedPtr<FSceneViewport>& SceneViewport, bool& bOutMeasured);

    // Walk the world's primitive components once and report the numbers that decide the override,
    // every distance measured from CullingOrigin (NOT from the camera -- see the struct comment).
    // Game thread only. Returns an invalid survey for a null world.
    FViewDistanceSurvey SurveyViewDistances(UWorld* World, const FVector& CullingOrigin);

    // The r.ViewDistanceScale that keeps every culled primitive inside its own cull radius:
    // for every primitive p with a finite cull distance,
    //     p.CullDistance * Scale >= Dist(p, CullingOrigin) + p.SphereRadius.
    // Returns 1.0 when nothing needs help (no finite cull distances, or they already reach far
    // enough), applies AutoViewDistanceScaleMargin, and never exceeds
    // MaxAutoViewDistanceScaleFor(MinCullDistance). Pure so the property can be asserted without
    // a level.
    float ComputeAutoViewDistanceScale(const FViewDistanceSurvey& Survey);

    // The scale the renderer will ACTUALLY use, read out of the renderer's own scalability cache
    // rather than out of the cvar. These differ: `r.ViewDistanceScale.ApplySecondaryScale` folds
    // `r.ViewDistanceScale.SecondaryScale` into the cached value
    // (Runtime/Engine/Private/UnrealEngine.cpp:905-906), and the cache only refreshes when a
    // console-variable sink runs. Reading it back is how a capture can tell that its override
    // landed instead of asserting that it did.
    float ReadEffectiveViewDistanceScale();

    // --- blank-frame criterion (see CalculateCaptureImageStats) ---
    //
    // The criterion has to give the SAME verdict for the same content at every capture size, and
    // the previous one (mean <= 0.01 && variance <= 0.0001) did not, because variance measures the
    // SHARE of the frame that carries content. Editor overlays -- the world-axis gizmo, the stats
    // text -- are drawn at a fixed pixel size, so their share falls as 1/pixels while the scene's
    // share does not. Measured 2026-08-19, one camera in empty space, ev100 9, four sizes:
    //
    //     size   mean       variance     lit px (>0.02)   old verdict
    //     256    0.0016148  0.00050337   455              blank:false
    //     512    0.0004346  0.00015178   456              blank:false
    //     1024   0.0000937  0.00003047   419              blank:TRUE
    //     2048   0.0000233  0.00000756   421              blank:TRUE
    //
    // render.capture_open_level sets bRejectBlankCapture, so raising `width` to get more detail
    // turned a working capture into a hard BLANK_CAPTURE with no other change.
    //
    // Note the last column: the ABSOLUTE count of lit pixels is what stays put for a fixed-size
    // overlay (455 -> 421 over a 16x change in pixel count), while for real scene content it is the
    // FRACTION that stays put (the same asset at those four sizes: 4.76%, 4.17%, 4.33%, 3.86%).
    // Neither statistic alone is stable across both regimes, so the criterion admits either one as
    // evidence that something was drawn.

    // Luminance above which a pixel counts as "lit". Above 8-bit quantisation (1/255 = 0.0039) and
    // above tonemapper dither; the same 0.02 the ortho-tile burst already warns below.
    constexpr double BlankLitLuminanceThreshold = 0.02;
    // A frame is not blank once this many pixels are lit, however large the frame. Sized well below
    // the ~420-460 pixels a bare editor overlay set contributes at any resolution, and well above
    // the handful a stuck pixel or an encoder artefact could produce.
    constexpr int64 BlankMinLitPixels = 64;
    // ...or once this fraction of them is lit, which is the term that carries small frames (below
    // ~128k pixels the fraction is the more permissive of the two) and real scene content.
    constexpr double BlankMinLitFraction = 0.0005;
    // A uniformly dark but genuinely drawn frame (a flat dark material, a night scene) has no lit
    // pixels at all and must still not be called blank. Unchanged from the previous criterion, and
    // it can only ever move the verdict toward "not blank".
    constexpr double BlankMeanLuminance = 0.01;

    // --- tone-range criterion: "this frame carries no usable dynamic range" ---
    //
    // A DIFFERENT QUANTITY FROM `blank`, deliberately, and it must stay one. `blank` asks "was
    // anything drawn into this frame at all" and its two terms are load-bearing for the dead
    // readback it was written for. The failure recorded in board ticket
    // B-exposure-pin-black-frame is the opposite situation: the pin was applied, the renderer
    // honoured it, correct pixels were drawn -- and the exposure was so far past the end of the
    // scene's range that the 8-bit image the caller receives cannot represent anything.
    // Measured on the FAdvancedPreviewScene fixture, meanLuminance by EV100: 0.3644 (-1),
    // 0.2466 (0), 0.0162 (5), 0.0068 (10), 0.0068 (16) -- pinned at the floor and no longer
    // responding to the parameter from EV100 10 up, while the response read `pinned: true`,
    // `blank: false` and carried no warning at all.
    //
    // Widening `blank` to cover it was considered and rejected: loosening either of its terms
    // makes it reject legitimately dark frames, and a crushed-but-textured frame is a different
    // classification that should carry its own name (rpc-design.md section 16 -- a metric written
    // for one defect class does not cover the next one that looks like it).
    //
    // WHAT IS MEASURED. How many distinct 8-bit luminance levels the frame actually resolves,
    // counting only levels that carry real mass. That is dynamic range in the units of the defect:
    // a frame collapsed onto a handful of levels cannot show a gradient, a shadow terminator or a
    // material response whatever was rendered into it, and one that spreads over dozens can --
    // regardless of how dark it is overall. A deliberately dark night scene with real shading
    // therefore passes, which a darkness threshold could not do.
    //
    // Resolution-invariant by the same construction the blank criterion uses, and for the same
    // two regimes: a level carried by real scene content keeps its SHARE of the frame as the frame
    // grows, while a level carried by a fixed-pixel editor overlay (the world-axis gizmo, the
    // stats text -- ~420-460 px at any size) keeps its COUNT. Admitting either as evidence that a
    // level is populated leaves no content whose verdict moves with the capture size.
    //
    // The floor is min(64, ceil(0.0005 * pixels)) and both terms are at least one pixel, exactly
    // as for BlankMinLitPixels / BlankMinLitFraction; the two criteria share the argument because
    // they share the problem, not the question.
    constexpr int64 ToneLevelMinPixels = 64;
    constexpr double ToneLevelMinFraction = 0.0005;
    // Below this many populated levels the frame carries no usable dynamic range. Eight of 256 is
    // three bits of tone over the whole image.
    //
    // DERIVED, NOT YET MEASURED against a live capture (no editor was available in the wave that
    // landed it): the EV100-11 frame in the ticket sits at meanLuminance 0.0068, i.e. an average
    // 8-bit level of ~1.7, so its populated levels are 0..3 plus whatever the editor overlay
    // contributes -- five or six. A well-exposed preview backdrop spreads over dozens. The gap is
    // wide, which is why a derived constant is defensible here, but `toneLevelsUsed` is published
    // beside the verdict so no caller has to inherit this number, and the warning text quotes the
    // measured count rather than the threshold.
    constexpr int32 MinUsableToneLevels = 8;
    // Which END a collapsed frame collapsed toward, and therefore which way `ev100` has to move.
    // A LABEL ON THE REMEDY, NOT THE VERDICT: the verdict is the level count above, and a frame
    // that lands either side of this split is equally unusable. Split at the middle of the range
    // so the two labels are mutually exclusive and jointly exhaustive over a collapse -- a flat
    // frame can never come back with neither.
    constexpr double CollapsedFrameDarkHalfMean = 0.5;

    struct FCaptureImageStats
    {
        double MeanLuminance = 0.0;
        double LuminanceVariance = 0.0;
        double MinLuminance = 0.0;
        double MaxLuminance = 0.0;
        // Pixels whose luminance exceeds BlankLitLuminanceThreshold, as a count and as a share of
        // the frame. Published beside the verdict because they are the two numbers the verdict is
        // made of, and because they answer "how much of this frame is anything" directly -- which
        // `blank` deliberately does not (rpc-design.md section 4).
        int64 LitPixelCount = 0;
        double LitPixelFraction = 0.0;
        bool bBlank = false;
        // These pixels were actually looked at. FALSE IS THE DEFAULT AND IT MATTERS: a
        // default-constructed FCaptureImageStats has ToneLevelsUsed 0, which would read as the
        // most collapsed frame possible, so every tone-range field and every tone-range warning is
        // gated on this flag. A capture whose readback returned no pixels at all leaves it false
        // and reports `blank` alone -- there was nothing to measure a range over.
        bool bStatsMeasured = false;
        // Distinct 8-bit luminance levels (0..255) carrying at least ToneLevelMinPixelsUsed
        // pixels. The dynamic-range number, and the one the two verdicts below are made of.
        int32 ToneLevelsUsed = 0;
        // The per-level floor this frame's size resolved to, published for the same reason
        // `litLuminanceThreshold` is: so a caller can reproduce the verdict.
        int64 ToneLevelMinPixelsUsed = 0;
        // The frame resolves fewer than MinUsableToneLevels levels AND sits in the dark half.
        // The EV100-past-the-end case: raise brightness, i.e. LOWER `ev100`.
        bool bCrushed = false;
        // Same collapse, bright half. Lower brightness, i.e. RAISE `ev100`. Exactly one of the two
        // is true for a collapsed frame and neither is true otherwise.
        bool bBlownOut = false;
    };

    // How a requested orthographic camera rotation maps onto the engine's fixed orthographic
    // viewport types. FEditorViewportClient::CalcSceneView builds the orthographic view matrix
    // from the VIEWPORT TYPE and ignores the camera rotation entirely (UE 5.8
    // Engine/Source/Editor/UnrealEd/Private/EditorViewportClient.cpp:1341-1401), so only the six
    // cardinal world axes are renderable and the in-plane (roll/yaw) orientation is fixed by the
    // engine. EffectiveRotation is the pose the rendered pixels actually show.
    struct FOrthographicViewResolution
    {
        ELevelViewportType ViewportType = LVT_OrthoFreelook;
        FRotator EffectiveRotation = FRotator::ZeroRotator;
        // "top" | "bottom" | "front" | "back" | "left" | "right" (UE's ELevelViewportType aliases).
        FString ViewName;
        // The requested forward direction matched a cardinal world axis within the tolerance.
        bool bAxisAligned = false;
        // EffectiveRotation differs from the requested rotation (the engine quantised the in-plane
        // orientation, or the request was not axis-aligned at all).
        bool bRotationSnapped = false;
    };

    // ---- one `ShowFlag.<Name>` console variable found away from its default ----
    //
    // A THIRD suppression channel, separate from the viewport's own EngineShowFlags and from the
    // editor billboards, and the only one nothing measured. FSystemSettings registers one bit-ref
    // console variable per show flag (UE 5.8 Runtime/Engine/Private/SystemSettings.cpp:137-148)
    // whose values are 0 = force the flag OFF, 1 = force it ON, 2 = do not override (the default).
    // EngineShowFlagOverride then ORs the resulting force masks over the flags of the view being
    // rendered (Runtime/Engine/Private/ShowFlags.cpp:767-783) -- AFTER the viewport client's own
    // flags have been copied. Three consequences, and each is why this is measured off the cvar
    // rather than off the client:
    //   * the client keeps reporting the flag it was given, so ViewportClient.EngineShowFlags and
    //     the flags the renderer used are different values;
    //   * the override is PROCESS-GLOBAL, so one caller's `ShowFlag.X 1` lands in every other
    //     caller's frame, in every viewport, until somebody puts it back;
    //   * game view does not clear it -- a Visualize* pass is a render pass, not an editor overlay.
    //
    // Not read through FEngineShowFlags::IsForceFlagSet: that answers only "is it forced" (and its
    // doc comment states the inverse of what its body returns), cannot say in which DIRECTION, and
    // cannot name the priority that set it. The cvar carries all three and is also what the remedy
    // names.
    struct FForcedShowFlagOverride
    {
        // The show-flag name, e.g. "VisualizeLightFunctionAtlas".
        FString Name;
        // The console variable forcing it, e.g. "ShowFlag.VisualizeLightFunctionAtlas". Spelled
        // out rather than left for the reader to assemble, because the remedy is `<cvar> 2` and
        // finding the name from the picture alone cost a full console-registry sweep.
        FString CVar;
        // 0 = forced OFF, 1 = forced ON. 2 is the default and never lands in this list.
        int32 Value = 2;
        // Which priority last wrote it, from the engine's own ECVF_SetByMask vocabulary
        // ("Console", "Code", "SystemSettingsIni", ...). In a shared editor this is the difference
        // between "the project ships this" and "another agent left it on".
        FString SetBy;
    };

    // Survey every registered ShowFlag.* console variable and return the ones NOT at 2.
    //
    // EMPTY is the expected reading: neither the engine's own config nor a stock project ini ships
    // a non-default value for any of them, so the survey reads exactly zero on the input every
    // capture is meant to run on, and any entry is a real difference between the frame and the
    // scene. Costs one console lookup per show flag (a few hundred hash probes) once per capture.
    TArray<FForcedShowFlagOverride> SurveyForcedShowFlagOverrides();

    struct FViewportCaptureOutput
    {
        FString Path;
        FString Filename;
        int32 Width = 0;
        int32 Height = 0;
        int64 SizeBytes = 0;
        FString Renderer;
        // The editor's normal resolution heuristic can rasterise below Width x Height and upscale
        // to the readback. Still capture pins both resolution stages at draw time; these fields
        // publish the measured view-family raster beside the PNG dimensions.
        int32 RenderWidth = 0;
        int32 RenderHeight = 0;
        double RenderPrimaryResolutionFraction = 0.0;
        double RenderSecondaryResolutionFraction = 0.0;
        double RenderResolutionFraction = 0.0;
        int32 RenderScreenPercentage = 0;
        FString RenderAntiAliasingMethod;
        int32 RenderAntiAliasingMethodValue = 0;
        bool bRenderResolutionPinned = false;
        bool bRenderUpscaled = false;
        bool bTemporalAntiAliasingSuppressed = false;
        FCaptureImageStats ImageStats;
        // The SUBJECT's own luminance, as opposed to the frame's. Unmeasured unless the request
        // carried bounds; see SubjectRegionStats.h for why a frame mean cannot answer the question
        // this one answers.
        PinWrightSubjectRegion::FSubjectRegionStats SubjectRegion;
        // BGRA8, Width*Height entries, and EMPTY unless the request asked for it. Same buffer
        // CalculateCaptureImageStats ran on, so a measurement taken here and the published
        // imageStats describe the same pixels.
        TArray<FColor> Pixels;
        int32 RedrawRetries = 0;
        FString ViewportType;
        int32 ViewportTypeValue = 0;
        // Human-readable view mode, from PinWrightViewModes::GetDisplayName. LOCALIZED and not
        // one-to-one with the editor.set_view_mode vocabulary ("Wireframe only" for the mode
        // set_view_mode calls "Wireframe"), so it is for reading, never for comparing.
        FString ViewMode;
        // Stable machine key for the same mode, spelled exactly as editor.set_view_mode accepts it
        // (see GetViewModeKey). This is the field to compare against and the one to feed back to
        // set_view_mode to restore the mode after a review.
        FString ViewModeKey;
        int32 ViewModeValue = 0;

        // ---- the scoped `viewMode` override (FViewModePin), all MEASURED ----
        //
        // Every field below is read off the viewport client, never echoed from the request. The
        // defect class this closes is a parameter that looks like it works: camera.orbit_shots
        // shipped a `viewMode` argument documented as "informational label echoed back in the
        // result (does not change rendering)", and a caller reading the response could not tell
        // it from one that had applied.
        bool bViewModeOverrideRequested = false;
        FString ViewModeRequestedKey;
        int32 ViewModeRequestedValue = 0;
        // Both slots as they stood when the capture started, and both as they read back after the
        // scope closed. A viewport client keeps SEPARATE perspective and orthographic view modes
        // (EditorViewportClient.h:2302, :2305), so a scope that saved and restored only one would
        // leave the other moved -- silently, for every later capture through the other projection.
        FString ViewModePerspBeforeKey;
        FString ViewModeOrthoBeforeKey;
        FString ViewModePerspAfterKey;
        FString ViewModeOrthoAfterKey;
        bool bViewModeRestored = true;
        // Both slots read back as the requested mode while the pixels were drawn.
        bool bViewModeApplied = false;
        // EngineShowFlags names ApplyViewMode writes differently for the requested mode than for
        // Lit, and the subset of them that did NOT read back off the client. The second check is
        // the one that cannot be faked by assigning a field: the show flags are what the renderer
        // reads, so an empty mismatch list is evidence the mode reached the frame.
        TArray<FString> ViewModeShowFlags;
        TArray<FString> ViewModeShowFlagMismatches;
        // Set when the applied mode renders a sub-visualisation that was already selected on the
        // viewport; names it, because the same mode with a different target is a different image.
        FString ViewModeCompanion;
        // The captured pixels show materials AND lighting (see IsLitViewMode). False means the
        // frame cannot be used to judge material, lighting or colour work.
        bool bLitViewMode = false;
        bool bGameView = false;
        bool bRealtime = false;
        // ---- forced ShowFlag.* console overrides, surveyed while these pixels were drawn ----
        // `bShowFlagOverridesMeasured` separates "surveyed and nothing was forced" from "never
        // surveyed", which the empty array alone cannot: an output built on a path that does not
        // reach the survey would otherwise publish an absence of contamination it never checked.
        bool bShowFlagOverridesMeasured = false;
        TArray<FForcedShowFlagOverride> ForcedShowFlags;
        // ---- the viewport's OWN overlay show flags, read while these pixels were drawn ----
        // A different mechanism from the forced cvars above, and the one that produced a frame
        // twice on the verge of being written up as authored map content: a water body's spline
        // drew a white dashed line down each bank while the capture reported `gameView: true`.
        // Game view does not clear EngineShowFlags.Splines on every path (SetGameView reuses the
        // current flags as the game set when EngineShowFlags.Game is already true,
        // EditorViewportClient.cpp:7229-7240), and editor.set_game_view - the verb that reports
        // these flags - is a persistent per-viewport toggle another agent can move between its
        // confirmation and this shutter. So they are read HERE, off the client that is about to
        // draw. `bOverlayShowFlagsMeasured` separates that from an output built on a path that
        // never reached the read, exactly as `bShowFlagOverridesMeasured` does above.
        bool bOverlayShowFlagsMeasured = false;
        FGameViewOverlayShowFlags OverlayShowFlags;
        // The pose the captured pixels actually show. MEASURED off the client after the camera was
        // applied (MeasureEffectiveViewPose) on the perspective path, so an aim the viewport
        // discarded shows up here instead of being echoed back as if it had been honoured; for an
        // orthographic capture it is the engine's fixed orientation for the resolved viewport type
        // (see FOrthographicViewResolution), which the engine derives from the viewport type and
        // not from any rotation on the client.
        FRotator EffectiveRotation = FRotator::ZeroRotator;
        // Where the camera actually was, same measurement. It differed from the request on every
        // orbit-mode perspective capture: orbit derives the eye from the pivot and keeps only the
        // requested location's DISTANCE from it.
        FVector EffectiveLocation = FVector::ZeroVector;
        // The client was in orbit mode when the capture started, and this capture turned it off to
        // aim the camera (perspective only). Reported because it is the difference between a
        // response whose pose is the request and one whose pose had to be recovered.
        bool bOrbitCameraAtEntry = false;
        bool bOrbitCameraSuppressed = false;
        // The measured pose matches what was asked for, within AimTolerance*. False means these
        // pixels show a different part of the world than the request describes -- the one thing an
        // opposed-camera comparison cannot survive. True on an orthographic capture whose in-plane
        // orientation was snapped, because the snap is already reported by bOrthoRotationSnapped.
        bool bCameraAimApplied = true;
        double CameraAimErrorDegrees = 0.0;
        double CameraAimLocationErrorCm = 0.0;
        // Resolved orthographic view name; empty for perspective captures.
        FString OrthoView;
        bool bOrthoRotationSnapped = false;
        // --- view-distance override, reported unconditionally on every capture ---
        // What r.ViewDistanceScale was while the pixels were rendered. Equals
        // ViewDistanceScaleBefore when no override was applied.
        float ViewDistanceScaleApplied = 1.0f;
        float ViewDistanceScaleBefore = 1.0f;
        bool bViewDistanceScaleOverridden = false;
        // The cvar read back AFTER the restore matched the value read before it. False is the
        // signal that this capture left a global rendering setting moved, which would make every
        // later capture in the session incomparable -- so it is reported, not assumed.
        bool bViewDistanceScaleRestored = true;
        // "caller" | "auto" | "none" -- how ViewDistanceScaleApplied was chosen.
        FString ViewDistanceScaleSource = TEXT("none");
        // The auto scale was clipped by a cap, so some distant primitives may still be culled.
        // Present so a wide shot cannot quietly under-report its own coverage.
        bool bViewDistanceScaleClamped = false;
        // Which cap bound, for the response: "" | "absolute" | "int32" | "nearCull".
        FString ViewDistanceClampReason;
        // What the RENDERER read while the pixels were drawn, from its scalability cache --
        // ViewDistanceScaleApplied is only what was written to the cvar. A mismatch means a
        // secondary scale or a sink that did not run, and it is the difference between a capture
        // that recovered its content and one that reports having done so.
        float ViewDistanceScaleEffective = 1.0f;
        // The effective scale actually satisfies the survey's coverage inequality. False on an
        // orthographic auto capture means distant geometry is STILL missing from these pixels --
        // the one thing this whole mechanism exists to stop being silent.
        bool bViewDistanceCoverageSufficient = true;
        FViewDistanceSurvey ViewDistanceSurvey;

        // ---- exposure pin, reported unconditionally on every capture ----
        // What the caller asked for. Unset is the omitted-parameter case and is the only value
        // that guarantees nothing was written.
        EExposureRequestMode ExposureMode = EExposureRequestMode::Unset;
        // A pin was requested (mode fixed). Distinguishes "not pinned because nobody asked" from
        // "not pinned although somebody did", which are opposite facts about the same frame.
        bool bExposurePinRequested = false;
        // MEASURED, not assumed: the override was written onto the viewport client AND the frame
        // was rendered in a state where the renderer applies it. False with
        // bExposurePinRequested true is the one condition that makes a returned frame
        // non-comparable, and MakeViewportInfoObject attaches an explicit warning for it.
        //
        // A PREDICATE OVER VIEWPORT STATE, AND ONLY THAT. It answers "was the pin applied", never
        // "did the pin produce a usable image" -- a pin driven past the end of the scene's range
        // is applied exactly as asked and returns a frame crushed to black
        // (B-exposure-pin-black-frame). Its meaning is unchanged and must stay unchanged, because
        // callers already branch on it; the frame question is answered separately by the tone-range
        // criterion above and published as `pinnedFrameUsable` beside this field.
        bool bExposurePinned = false;
        float Ev100Requested = 0.0f;
        // The client's exposure state WHILE THE PIXELS WERE DRAWN, read back off the client
        // rather than echoed from the request. bExposureFixedApplied can be true without this
        // capture having asked for anything: a viewport left pinned through the editor's own
        // EV100 control renders fixed too, and a caller comparing frames needs to know that.
        bool bExposureFixedApplied = false;
        float Ev100Applied = 0.0f;
        // The client's exposure state before the capture touched it, for the restore verdict.
        bool bExposureFixedBefore = false;
        float Ev100Before = 0.0f;
        // The client's exposure settings read back AFTER the restore matched what they were
        // before it. False means this capture left the viewport's exposure moved, which would
        // silently re-expose every later capture in the session -- reported, not assumed.
        bool bExposureRestored = true;
        // Why a requested pin did not govern the pixels. Empty when it did, or when none was
        // requested. Never a bare "failed": it names the show flag or view mode that ignored it.
        FString ExposurePinBlockedReason;
        // The exposure GAIN these pixels were drawn with -- the linear scalar the tonemapper
        // multiplies scene colour by, NOT an EV100 (see ExposureGainToEv100, and `ev100Equivalent`
        // in the response, which is the reusable form).
        //
        // It comes from one of two sources, and which one matters:
        //
        //  * UNPINNED: FSceneViewStateInterface::GetLastEyeAdaptationExposure (SceneManagement.h:198)
        //    reached through FEditorViewportClient::ViewState. A DRIFT SIGNAL rather than an exact
        //    gain -- a GPU->CPU readback lagging the drawn frame by a few frames. Two unpinned
        //    captures reporting different values are provably incomparable, which is what it is for.
        //
        //  * PINNED (bAdaptedExposureFromPin): computed from the EV100 in force, because the
        //    readback is WRONG for a pinned frame and not merely imprecise. Clearing the
        //    EyeAdaptation show flag stops the pass that refreshes it, so it holds whatever the
        //    last auto-exposed frame left behind -- measured 2026-08-19 at ~3.12 on a viewport
        //    pinned in turn at EV100 0, 4.34 and -1.90, i.e. one stale number reported for three
        //    frames six stops apart. On this branch the gain is not a measurement at all: the
        //    renderer derives it in closed form from FixedEV100, so Ev100ToExposureGain reproduces
        //    it exactly and `ev100Equivalent` comes back equal to `ev100`.
        float AdaptedExposure = 0.0f;
        bool bAdaptedExposureMeasured = false;
        // Which of the two above. Published as `adaptedSource` so a caller can tell an exact
        // value from a lagging one without inferring it from the other fields.
        bool bAdaptedExposureFromPin = false;
        // The renderer has no completed eye-adaptation readback for this viewport AT ALL. A fact
        // about the viewport rather than about the exposure, and the only warm-up signal a capture
        // has, so it is recorded even on the pinned branch where the readback is not used.
        //
        // It was once published as a warm-up warning, on the strength of correlating with the
        // unfinished frame in four trials. It does NOT identify one, and the response no longer
        // claims it does: measured 2026-08-19 on /Engine/BasicShapes/Cube at ev100 0, closing the
        // asset editor and reopening it in a SEPARATE call before capturing gives a fully warmed
        // frame (meanLuminance 0.2437 and 0.2436, against 0.0506 for open-and-capture in one call)
        // with this flag still true on both. Two false positives out of two, on the exact frame the
        // warning told the caller to discard. Warm-up is now measured on the pixels instead -- see
        // the settle block below -- and this stays as the plain fact it is.
        bool bAdaptedExposureReadbackPending = false;

        // --- warm-up settle, measured on the frame rather than inferred ---
        // A viewport that has just been created draws a dark frame first. Measured 2026-08-19,
        // /Engine/BasicShapes/Cube pinned at ev100 0, 512x512: the capture that OPENED the asset
        // editor read meanLuminance 0.0506, and the identical capture into the same editor a call
        // later read 0.2437 -- 2.26 stops, at a pinned exposure that is supposed to make two
        // captures comparable. So the capture pumps extra frames until the frame mean stops
        // moving, and reports what that cost. `bSettled` false means the budget ran out with the
        // frame still moving: those pixels are mid-warm-up and are not comparable with anything.
        // False only for capture backends which do not execute the viewport settle loop; their
        // serializer omits this viewport-only evidence instead of publishing a false warning.
        bool bWarmupMeasured = true;
        int32 WarmupSettleRounds = 0;
        bool bWarmupSettled = false;
        // |mean(final) - mean(previous)| over the last settle round. Zero rounds means unmeasured,
        // which is why the round count is published beside it.
        double WarmupMeanLuminanceDelta = 0.0;
        bool bWarmupPixelChangeMeasured = false;
        double WarmupMeanAbsDelta = 0.0;
        int32 WarmupMaxDelta = 0;
        double WarmupChangedPixelFraction = 0.0;
        int32 WarmupChannelThreshold = 1;
        double WarmupSettleMs = 0.0;

        // ---- editor sprite suppression, reported unconditionally on every capture ----
        // What the caller asked for, kept apart from what happened: "sprites are visible because
        // nobody asked to hide them" and "sprites are visible although somebody did" are opposite
        // facts about the same frame, exactly as with the exposure pin.
        bool bHideEditorSpritesRequested = false;
        // The flag as it stood BEFORE this capture touched it, and as it stood WHILE the pixels
        // were drawn -- both read off the client rather than echoed from the request. They differ
        // from the request in the case that matters: a viewport a human already had in game view,
        // or with the flag cleared by hand, renders sprite-free whether or not this call asked.
        bool bBillboardSpritesBefore = true;
        bool bBillboardSpritesApplied = true;
        // The flag read back AFTER the restore matched what it was before. False means this
        // capture left the viewport's show flags moved, which changes every later frame the user
        // sees in that viewport -- reported, not assumed.
        bool bEditorSpritesRestored = true;

        // ---- the preview-scene rig, reported unconditionally on every capture ----
        //
        // UNCONDITIONAL FOR THE SAME REASON `exposure` AND `viewModeOverride` ARE, and the reason
        // is sharper here: two machines returned mean luminance 0.47 and 0.18 for the same call at
        // the same pinned ev100, because a preview scene belongs to the editor showing it. A
        // response that omitted this block when nobody asked for a rig would make "the default rig
        // drew these pixels" and "a rig was asked for and did nothing" the same reading.
        //
        // THREE MEASUREMENTS, NOT TWO. `Before` is the scene at guard entry, `Drawn` is the scene
        // WHILE THE PIXELS WERE DRAWN (the rig in force, whether this call chose it or not), and
        // `After` is the scene once the guard's destructor has run. Every field of all three is
        // read off the live scene; nothing is echoed from the request.
        PinWrightPreviewSceneRig::FPreviewSceneRigReport PreviewSceneRigBefore;
        PinWrightPreviewSceneRig::FPreviewSceneRigReport PreviewSceneRigDrawn;
        PinWrightPreviewSceneRig::FPreviewSceneRigReport PreviewSceneRigAfter;
        bool bPreviewSceneRigRequested = false;
        // A private transient scene is destroyed at RPC exit, so it has no external rig state to
        // restore and no honest after-restore measurement. Its serializer reports disposal.
        bool bPreviewSceneRigDisposable = false;
        // The scene read back carrying what the request asked for, field by field, immediately
        // after the write. False with bPreviewSceneRigRequested true is the condition that makes
        // these pixels non-comparable, and the serializer attaches an explicit warning for it.
        bool bPreviewSceneRigApplied = false;
        // The sky-light and reflection-capture drain was DRIVEN for the world these pixels were
        // drawn in. SEPARATE FROM bPreviewSceneRigApplied on purpose: `applied` is a true
        // statement about the WRITES only, because the queue those writes feed is emptied by an
        // editor tick that no capture runs
        // (PinWrightPreviewSceneRig::UpdatePreviewSceneCaptures). False means these pixels may be
        // lit by whatever the last editor frame captured, and the serializer attaches a warning
        // for it -- the warm-up settle loop cannot detect that state, because its own pump cannot
        // drive the work it would be waiting for.
        bool bPreviewSceneCaptureUpdated = false;
        // A sky capture was still queued when that drain returned. NECESSARY BECAUSE DRIVEN IS
        // NOT COMPLETED: the engine re-queues a capture it could not finish while assets compile
        // and will not retry it for 5 s (SkyLightComponent.cpp:771-787, :837-860), so a drain can
        // run in full and change nothing. Kept out of bPreviewSceneCaptureUpdated because the
        // engine predicate behind it is process-wide -- folding it in would let another window's
        // pending capture decide this capture's verdict. Only meaningful when
        // bPreviewSceneCaptureUpdated is true, and only reported then.
        bool bPreviewSceneCaptureIncomplete = false;
        // The light components read back as they were before the capture. Separate from
        // bSharedProfilesRestored below because the two fail independently and mean different
        // things: a moved component affects the next capture from this viewport, a moved profile
        // affects every open asset editor in the process AND a source-controlled file.
        bool bPreviewSceneRigRestored = true;
        // The whole UAssetViewerSettings::Profiles array compared field-wise against its value at
        // capture entry. NOT `operator==` -- FPreviewSceneProfile's compares only ProfileName, so
        // it cannot see any mutation this block exists to catch (see SharedProfilesMatch).
        bool bSharedProfilesRestored = true;
        // The committed Config/DefaultEditor.ini digest at exit equals the digest at entry. This
        // is the criterion Defect 2 is actually judged by, rather than "we did not call Save()" --
        // which would be untestable, because this plugin never calls it.
        bool bConfigFileUnchanged = true;
        FString ConfigFileDigest;
        FString PreviewSceneRigWarning;
        FString PreviewSceneRigRestoreWarning;

        // ---- landscape grass, built against THIS capture's pose and measured afterwards ----
        //
        // Unconditional, for the same reason `previewScene` is, and the reason is the same shape:
        // a pose-driven capture used to photograph grass built around the persistent viewport
        // camera and report `success`, non-blank and settled while doing it
        // (B-capture-open-level-pose-params-photograph-stale-grass). The block says which camera
        // the grass in these pixels was built around, whether that build finished, and how many
        // instances came out of it -- so bare ground and an unfinished build stop being the same
        // picture. See LandscapeGrassSettle.h for the mechanism.
        PinWrightCaptureGrass::FGrassBuildReport GrassBuild;
    };

    // ---- the editor-sprite show flags, as a value ----
    //
    // ONE flag, verified rather than assumed. `EngineShowFlags.BillboardSprites` is what
    // UBillboardComponent (Runtime/Engine/Private/Components/BillboardComponent.cpp:214 and :321),
    // UMaterialBillboardComponent (:249) and UArrowComponent (ArrowComponent.cpp:170) gate their
    // draw relevance on, and it is the only flag any of the editor icon primitives read.
    //
    // The neighbouring candidates are NOT icons and are deliberately not in here:
    // `LightRadius` and `AudioRadius` are read only by component visualizers
    // (Editor/ComponentVisualizers/Private/PointLightComponentVisualizer.cpp:29,
    // Public/AudioComponentVisualizer.h:13), which draw for SELECTED components and are already
    // suppressed by FLevelEditorViewportClient's own !IsInGameView() guard; `Grid`, `ModeWidgets`,
    // `Snap`, `SelectionOutline` and `Selection` are separate decoration families with separate
    // levers. Note also that the game flag set does NOT clear BillboardSprites -- the flag is
    // absent from FEngineShowFlags::Init entirely (Runtime/Engine/Public/ShowFlags.h:394-520), so
    // it is left at the memset default of ON for ESFIM_Game and ESFIM_Editor alike, and game view
    // hides icons through a different mechanism (FPrimitiveSceneProxy::IsShown,
    // Runtime/Engine/Private/PrimitiveSceneProxy.cpp:1515-1545).
    //
    // Split out as a value type with pure apply/restore functions so the applied-and-restored
    // property is assertable on a bare FEngineShowFlags -- no viewport, no world, no GPU. Capture
    // tests that need pixels are the ones that silently skip their assertions when another process
    // holds the GPU (board ticket B-test-skips-assertions-silently).
    struct FEditorSpriteShowFlags
    {
        bool bBillboardSprites = true;
    };

    FEditorSpriteShowFlags ReadEditorSpriteShowFlags(const FEngineShowFlags& Flags);
    // Clears the sprite flags on `Flags` and returns what they were, for the paired restore.
    FEditorSpriteShowFlags SuppressEditorSpriteShowFlags(FEngineShowFlags& Flags);
    void RestoreEditorSpriteShowFlags(FEngineShowFlags& Flags, const FEditorSpriteShowFlags& Previous);

    // Read the `hideEditorSprites` wire parameter off a payload root. Absent, non-boolean or null
    // -> false, which is the do-nothing path. Exported for the same reason ParseExposurePin is:
    // camera.frame_actor and camera.orbit_shots hand-build their FViewportCaptureRequest and must
    // read the SAME wire spelling as the verbs that go through ParseViewportCaptureRequest.
    bool ParseHideEditorSprites(const TSharedPtr<FJsonObject>& Payload);

    // Common capture fields that apply to both editor viewports and ownerless scene captures.
    // Deliberately excludes allowBlank, viewDistanceScale and hideEditorSprites: those fields
    // control editor-viewport mechanisms and would be inert on a transient scene capture.
    bool ParseOffscreenCaptureRequest(
        const TSharedPtr<FJsonObject>& Payload,
        FViewportCaptureRequest& OutRequest,
        FString& OutErrorCode,
        FString& OutErrorMessage);

    bool ParseViewportCaptureRequest(
        const TSharedPtr<FJsonObject>& Payload,
        FViewportCaptureRequest& OutRequest,
        FString& OutErrorCode,
        FString& OutErrorMessage);

    // Map a requested orthographic camera rotation onto the engine viewport type that renders it.
    // Returns false when the direction is more than OrthoAxisToleranceDegrees off every world axis
    // (no editor orthographic viewport type can render such a pose); OutResolution is still filled
    // with the LVT_OrthoFreelook fallback so a caller that tolerates it can proceed.
    bool ResolveOrthographicView(const FRotator& RequestedRotation, FOrthographicViewResolution& OutResolution);

    // Editor ortho zoom that makes the viewport span WorldWidth centimetres horizontally.
    // Calibrated against the engine's own GetOrthoUnitsPerPixel so it stays correct across engine
    // versions and across r.Editor.AlignedOrthoZoom (which makes the zoom->world scale depend on
    // the viewport's pixel width). Viewport may be null (falls back to CAMERA_ZOOM_DIV only).
    float ComputeOrthoZoomForWorldWidth(const FEditorViewportClient& ViewportClient,
        const FViewport* Viewport, float WorldWidth);

    // Inverse of the above: the world width in centimetres the viewport currently spans.
    // Mirrors the engine's own definition of ortho world width
    // (UE 5.8 Editor/UnrealEd/Private/Tools/EdModeInteractiveToolsContext.cpp:148).
    float ComputeOrthoWorldWidthFromZoom(const FEditorViewportClient& ViewportClient,
        const FViewport* Viewport);

    // Apply a capture pose to a viewport client (projection type, FOV / ortho zoom, location,
    // rotation). Exported because PinWrightViewProjection MUST rebuild the FSceneView through the
    // exact same field sequence -- a second copy of this logic is how deprojected rays silently
    // drift off the captured pixels, so there is deliberately only one.
    //
    // ORBIT. On the PERSPECTIVE path this also turns the client's orbit camera OFF, because in
    // orbit mode SetViewLocation and SetViewRotation do not aim anything: they feed the orbit
    // gizmo, and FEditorViewportClient::CalcViewRotationMatrix builds the view from
    // FViewportCameraTransform::ComputeOrbitMatrix instead (UE 5.8
    // Editor/UnrealEd/Private/EditorViewportClient.cpp:7392-7400).
    //
    // What that matrix does to a requested pose, worked out from :395-405 -- a translation by
    // -LookAt, a yaw, a roll by the requested PITCH, a translation of |ViewLocation - LookAt|
    // along +Y, and a trailing FInverseRotationMatrix(FRotator(0,90,0)):
    //
    //   * the eye lands on a sphere around the orbit LookAt, at radius |ViewLocation - LookAt|.
    //     The requested location's DIRECTION is discarded outright; only its distance from the
    //     pivot survives, and the pivot is wherever the user last orbited -- so a camera placed to
    //     one side of a mesh routinely ends up on the other, or off the subject entirely.
    //   * the rendered yaw is 90 - requested yaw. That is a MIRROR about the 45-degree diagonal,
    //     not a rotation, so it is not fixable by adding a constant: yaw 0 renders as +90, yaw -90
    //     renders as 180, yaw 90 renders as 0.
    //
    // Asset-editor preview viewports run in orbit mode by default (SetCameraFollowMode's default
    // enables it), so every perspective capture through one was mis-aimed while returning a
    // perfectly valid PNG and echoing the requested pose back as the pose it had used.
    //
    // ToggleOrbitCamera(false) converts the orbit pose into the equivalent free pose before the
    // requested pose is written over it, so nothing here depends on what the pivot was. The
    // ORTHOGRAPHIC path is deliberately left alone: an orthographic view matrix is built from the
    // ELevelViewportType and never consults the orbit transform, so it already aims where it was
    // asked and toggling would mutate the orthographic camera transform for nothing.
    //
    // The caller owns the restore. CaptureEditorViewportToPng and
    // PinWrightViewProjection::FScopedRebuiltView both re-enable orbit and put the pose back.
    void ApplyCaptureCamera(FEditorViewportClient& ViewportClient, const FViewport* Viewport,
        const FViewportCaptureRequest& Request);

    // The pose the RENDERER will draw from, which in orbit mode is NOT the pose stored on the
    // client. Measured rather than echoed, so a capture response can state where the camera
    // actually was instead of repeating the request back (which is exactly how a mis-aimed capture
    // passed for a correctly aimed one).
    struct FEffectiveViewPose
    {
        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        // The pose above was derived from the orbit transform rather than read off
        // GetViewLocation / GetViewRotation.
        bool bFromOrbitCamera = false;
    };

    // The pure core: the pose a camera transform resolves to, given whether the orbit camera is in
    // force for it. With orbit off it is the stored pose verbatim; with orbit on it is the engine's
    // own FViewportCameraTransform::ComputeOrbitMatrix inverted.
    //
    // Split out from MeasureEffectiveViewPose so the aiming contract is assertable on a bare
    // FViewportCameraTransform -- no viewport, no world, no GPU, nothing that can make a test skip
    // its assertions and still report success (board ticket B-test-skips-assertions-silently).
    FEffectiveViewPose ResolveEffectiveViewPose(bool bOrbitCameraInForce,
        const FViewportCameraTransform& Transform);

    // Read the orbit flag and the camera transform off a live client and resolve them.
    //
    // ORTHOGRAPHIC clients report their stored pose unchanged: CalcSceneView's orbit branch is
    // inside its LVT_Perspective arm only, so orbit does not move an orthographic frame and
    // running the orbit matrix over it would invent a pose the pixels never had.
    FEffectiveViewPose MeasureEffectiveViewPose(const FEditorViewportClient& ViewportClient);

    // ---- "did this frame contain the subject at all?" ----
    //
    // A conservative bounds-vs-frustum test, and the answer to a failure `blank` cannot see: two
    // captures of a badly aimed camera came back byte-identical, pure backdrop, with blank=false
    // and litPixelFraction=1, because the backdrop is brightly lit and `blank` only catches BLACK
    // frames. Nothing in the response said the mesh was not in either picture.
    //
    // Conservative in one direction on purpose: bBoundsInFrame is false only when the subject
    // PROVABLY cannot project into the frame. The frustum is approximated by the cone (perspective)
    // or cylinder (orthographic) that CIRCUMSCRIBES it, and the subject by its bounding SPHERE, so
    // a false verdict is a fact and a true verdict is only "not provably absent". That is the right
    // asymmetry: a spurious "not framed" would send a caller chasing a working capture.
    struct FBoundsFramingCheck
    {
        // False when the caller supplied no usable bounds; every other field is then meaningless.
        bool bEvaluated = false;
        bool bBoundsInFrame = true;
        // The whole bounding sphere is behind the camera plane (perspective only).
        bool bBehindCamera = false;
        // The two numbers the verdict is made of, published so a caller can judge a near-miss
        // without inheriting this function's approximation. OffAxis is the nearest approach of the
        // bounding sphere to the view axis; FrameLimit is the circumscribing half-extent.
        double OffAxis = 0.0;
        double FrameLimit = 0.0;
        // "degrees" for a perspective frame, "centimetres" for an orthographic one.
        FString Units;
    };

    // BoundsRadius <= 0 (or a degenerate camera rotation) yields bEvaluated=false rather than a
    // guess. Takes the EFFECTIVE camera pose -- see MeasureEffectiveViewPose -- not the requested
    // one, because the whole point is to catch a camera that did not go where it was asked.
    FBoundsFramingCheck EvaluateBoundsFraming(
        const FViewportCaptureRequest& Request,
        const FVector& CameraLocation,
        const FRotator& CameraRotation,
        const FVector& BoundsOrigin,
        double BoundsRadius);

    // The `framing` block: the verdict, the two numbers behind it, and -- only when the subject is
    // provably out of frame -- a `framingWarning` naming the consequence. Exported so the contract
    // is unit-testable without a viewport, matching MakeExposureInfoObject.
    TSharedPtr<FJsonObject> MakeBoundsFramingObject(const FBoundsFramingCheck& Check);

    // Calculate normalized Rec.709 luminance statistics for a viewport readback. Kept in the
    // production capture utility so regression tests exercise the same classifier used by the
    // handler rather than an inline test copy.
    FCaptureImageStats CalculateCaptureImageStats(TConstArrayView<FColor> ColorData);

    // Write the tone-range numbers into a response's `imageStats` object: `toneLevelsUsed` and the
    // `toneLevelMinPixels` floor it was counted against. Writes NOTHING when the stats were never
    // measured, so an unmeasured frame reports no range rather than the most collapsed one.
    //
    // Exported as a pair with MakeToneRangeWarning so a capture verb adds the whole signal in two
    // lines. Adopted by render.capture_asset_preview / render.capture_open_level
    // (RenderHandler.cpp AddCaptureFields); the other emitters that hand-build an `imageStats`
    // block -- CameraShotPlanUtils.h AddShotFields and OrthoTileCaptureHandler.cpp -- still do not
    // carry it, which is the drift AddShotFields' own header comment warns about.
    void AddToneRangeStatsFields(const FCaptureImageStats& Stats,
        const TSharedPtr<FJsonObject>& ImageStats);

    // The frame-level `rangeWarning`: what was measured, which way `ev100` has to move, and why
    // `blank` is false on a frame nobody can read. EMPTY when the frame is fine or was never
    // measured, so the caller's test is "is this string present", the same shape as `pinWarning`.
    FString MakeToneRangeWarning(const FCaptureImageStats& Stats);

    // Write the top-level tone-range VERDICT into a capture response: `crushed`, `blownOut` and
    // `rangeWarning` on a lit frame; `toneRangeApplicable: false` plus a `toneRangeNotApplicable`
    // explanation on any other. Writes nothing at all when the pixels were never measured, the
    // same rule AddToneRangeStatsFields follows.
    //
    // ONE gate, one call site shape. The verdict was published inline at two handlers and the
    // lit-ness gate would have had to be copied into both; it lives here instead so
    // render.capture_* and render.capture_annotated cannot disagree about when the classifier is
    // allowed to speak. Takes the whole capture rather than the stats because the not-applicable
    // text names the mode in both spellings, exactly as `viewModeWarning` does.
    void AddToneRangeVerdictFields(const FViewportCaptureOutput& Capture,
        const TSharedPtr<FJsonObject>& Result);

    // Optional synchronous stages for callers that must mutate time-dependent content inside one
    // capture. BeforeWarmup runs after the capture pose and exposure request are installed but
    // before the settling draws. BeforeFinalFrame runs only after exposure has settled and its
    // renderer readback was recorded. Mutations made by either hook are submitted before the
    // following draw. Once the final stage starts, AfterFinalFrame runs on every exit path,
    // including callback, readback, encoding, and save failures.
    struct FViewportCaptureHooks
    {
        TFunction<bool(FString& OutErrorCode, FString& OutErrorMessage)> BeforeWarmup;
        TFunction<bool(const FViewportCaptureOutput& SettledCapture,
            FString& OutErrorCode, FString& OutErrorMessage)> BeforeFinalFrame;
        TFunction<void()> AfterFinalFrame;
    };

    bool CaptureEditorViewportToPng(
        FEditorViewportClient& ViewportClient,
        const TSharedPtr<FSceneViewport>& SceneViewport,
        const FViewportCaptureRequest& Request,
        const FString& DefaultPrefix,
        const FString& Subdirectory,
        FViewportCaptureOutput& OutCapture,
        FString& OutErrorCode,
        FString& OutErrorMessage,
        const FViewportCaptureHooks* Hooks = nullptr);

    TSharedPtr<FJsonObject> MakeVectorObject(const FVector& Value);
    TSharedPtr<FJsonObject> MakeRotatorObject(const FRotator& Value);

    // The `viewDistance` block reported by every capture response: what the scale was, what it
    // became, why, whether it was restored, and the survey behind an auto value. Unconditional
    // for the same reason `viewModeWarning` is -- a frame whose distant content was culled looks
    // exactly like a frame whose distant content is missing from the level.
    TSharedPtr<FJsonObject> MakeViewDistanceInfoObject(const FViewportCaptureOutput& Capture);

    // The `exposure` sub-block of the `viewport` block: the mode requested, whether a pin was
    // asked for, whether it actually governed the pixels (measured), the fixed EV100 in force
    // while they were drawn, the restore verdict, and the renderer's own adapted-exposure
    // readback. Exported (rather than file-local) so the reporting contract is unit-testable
    // without a viewport, and so Unity cannot merge it with a same-named helper elsewhere.
    TSharedPtr<FJsonObject> MakeExposureInfoObject(const FViewportCaptureOutput& Capture);

    // The `editorSprites` sub-block of the `viewport` block: whether suppression was asked for,
    // the BillboardSprites flag as it stood before the capture and while the pixels were drawn,
    // and the restore verdict. Unconditional, and for the same reason `lit` is: a frame with a
    // light-bulb icon over the geometry is indistinguishable from a frame with something in the
    // level there, so the response has to say which viewport state produced these pixels.
    // Exported alongside MakeExposureInfoObject so the contract is unit-testable without a GPU.
    TSharedPtr<FJsonObject> MakeEditorSpriteInfoObject(const FViewportCaptureOutput& Capture);

    // The `showFlagOverrides` sub-block of the `viewport` block: whether the survey ran, every
    // ShowFlag.* console variable found away from its default while the pixels were drawn, and --
    // only when at least one was -- an `overrideWarning` naming them and the remedy.
    // Unconditional, and for the same reason `editorSprites` is: a forced show flag either paints
    // a full-screen debug pass over the frame or removes a whole rendering feature from it, and
    // either way the result is a plausible picture of a scene that is not the one under review.
    // Exported alongside MakeEditorSpriteInfoObject so the contract is unit-testable without a GPU.
    TSharedPtr<FJsonObject> MakeShowFlagOverrideInfoObject(const FViewportCaptureOutput& Capture);

    // The `viewModeOverride` sub-block of the `viewport` block: whether a scoped mode was asked
    // for, what BOTH view-mode slots held before, what they held while the pixels were drawn, the
    // show flags that were measured to prove the mode reached the renderer's inputs, and the
    // restore verdict for both slots. Unconditional, so a response says "no override was asked
    // for" rather than being silent about it -- an absent block and an override that did nothing
    // read identically. Exported alongside the other two so the contract is unit-testable without
    // a viewport.
    TSharedPtr<FJsonObject> MakeViewModeOverrideInfoObject(const FViewportCaptureOutput& Capture);

    // Stable machine key for a view mode, spelled exactly as the `viewMode` parameter and
    // editor.set_view_mode accept it, so a caller can feed the reported mode straight back.
    //
    // ONE definition, shared: this delegates to PinWrightViewModes::GetKey, which derives the key
    // from StaticEnum<EViewModeIndex>() plus five published overrides, and which is also what the
    // request parser resolves against. The key table and the parse chain used to live in two
    // files and be pinned together by a test; they are now the same code, so they cannot drift at
    // all. Values with no enumerator return "Unknown".
    FString GetViewModeKey(EViewModeIndex ViewMode);

    // Does a frame captured in this mode show BOTH materials and lighting?
    //
    // Deliberately narrow: VMI_Lit and VMI_PathTracing only. Every other mode either removes
    // materials (VMI_LightingOnly, VMI_Lit_DetailLighting), removes lighting (VMI_Unlit), or
    // replaces the image with a debug visualization (wireframe, complexity, collision, LOD
    // coloration, buffer/Lumen/Nanite visualizers). A frame in any of those cannot support a
    // material/lighting/colour acceptance decision, which is what a capture is normally requested
    // for. Being narrow is the safe direction: a spurious "not lit" costs one line of noise, a
    // spurious "lit" is the exact defect this reporting exists to prevent.
    bool IsLitViewMode(EViewModeIndex ViewMode);

    // The four "geometry inspection" modes whose picture is produced ONLY by swapping each mesh
    // batch's material for a GEngine debug material inside the engine's ApplyViewModeOverrides
    // (PrimitiveDrawingUtils.cpp:1750-1805): front_back_face, clay, zebra, random_color.
    //
    // WHY A CAPTURE VERB CARES. That substitution is reached only from
    // FMeshElementCollector::AddMesh, i.e. only for geometry gathered through the DYNAMIC
    // mesh-element path. An ordinary static mesh gets there only because these modes force
    // Lighting off (ShowFlags.cpp:592-604), which makes IsRichView() true and flips the proxy to
    // dynamic relevance (StaticMeshSceneProxy.cpp:2456). A NANITE proxy does not honour
    // IsRichView -- the term is commented out at NaniteResources.cpp:1310-1311 under the engine's
    // own note "Nanite doesn't respect rich view enabling dynamic relevancy" -- so it keeps static
    // relevance, rasterises through Nanite::DispatchBasePass with its real materials, and is never
    // substituted. Nanite's own debug-view selector (BasePassRendering.cpp:1324-1357) has no case
    // for any of these four.
    //
    // The frame therefore comes back UNTINTED, and for front_back_face untinted is pixel-identical
    // to the "correct winding" answer -- a false PASS, not a visible failure. Measured on one
    // inverted 2536-triangle mesh, nothing changed but the Nanite flag: mean RGB (8.7, 128.0, 8.7)
    // with Nanite off against (105.4, 89.4, 105.4) with it on, while the mesh's signed volume read
    // -1.5e8 in both cases. `viewModeOverride.applied` reads true either way and cannot separate
    // them, which is why the pairing is reported rather than left to the caller to notice.
    bool IsDebugMaterialSubstitutionMode(EViewModeIndex ViewMode);

    // Build the `viewport` block shared by every capture response: type, view mode (display string,
    // stable key and raw enum value), lit-ness, game-view and realtime flags, an `exposure`
    // sub-block (what was requested, what the pixels actually used, whether it was restored), plus
    // a `viewModeWarning` string that is present IF AND ONLY IF the capture was not lit.
    //
    // Every capture verb routes its response through this one function, so putting the exposure
    // report here is what makes `camera.orbit_shots` and `render.capture_asset_preview` report it
    // without either handler knowing exposure exists.
    //
    // WHY THIS IS A WARNING AND NOT A HARD FAILURE. Rejecting non-Lit captures would break the
    // documented collision-review workflow, which is editor.set_view_mode -> capture ->
    // set_view_mode back (docs/wiki-src/editor.collision-review.md), goes through these same
    // ordinary capture verbs, and has no separate "capture in collision mode" verb to exempt.
    // set_view_mode also deliberately does not self-restore, so any viewport left in a debug mode
    // by earlier work would start hard-failing every subsequent capture -- turning a reporting gap
    // into an outage. The defect being fixed is that the response did not SAY what it showed; the
    // fix for that is to say it. `lit` is the machine-checkable field, `viewModeWarning` the
    // human-readable one, and both are unconditional, so a wireframe frame can no longer arrive
    // looking like a clean lit capture.
    // The `previewScene` sub-block of every capture's `viewport` block. Unconditional, per the
    // rule render.md states for that block: "a verb-by-verb difference in this block is a bug, not
    // a design". On a level-viewport verb it carries `sceneAvailable: false` and nothing else --
    // the ABSENCE of profileName is the assertion, not an empty string.
    //
    // Exported alongside MakeExposureInfoObject so the contract is unit-testable without a GPU.
    TSharedPtr<FJsonObject> MakePreviewSceneRigInfoObject(const FViewportCaptureOutput& Capture);

    TSharedPtr<FJsonObject> MakeViewportInfoObject(const FViewportCaptureOutput& Capture);
}
