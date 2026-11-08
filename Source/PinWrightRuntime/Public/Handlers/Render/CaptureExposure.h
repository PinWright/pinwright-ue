// Copyright (c) 2026 Alexander Penkin. MIT License.

// The capture `exposure` pin: wire description, parse, and the gain <-> EV100 conversions. Split out
// of the editor module's Handlers/Render/PreviewViewportCaptureUtils.h (which includes this header)
// so the runtime editor.screenshot reads the same vocabulary as every editor capture verb.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

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

namespace PinWrightRenderCapture
{
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

    struct FExposurePin
    {
        EExposureRequestMode Mode = EExposureRequestMode::Unset;
        float Ev100 = 0.0f;
        // The only mode that writes anything. Everything else leaves the viewport alone.
        bool WantsPin() const { return Mode == EExposureRequestMode::Fixed; }
    };

    // Wire spelling of a mode, for the response. "unset" | "auto" | "fixed".
    PINWRIGHTRUNTIME_API const TCHAR* ExposureModeKey(EExposureRequestMode Mode);

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
    PINWRIGHTRUNTIME_API float ExposureLuminanceMax();

    // The gain the renderer applies to a frame pinned at this EV100. Closed form, not a
    // measurement: on the fixed branch GetEyeAdaptationFixedExposure
    // (PostProcessEyeAdaptation.cpp:810-825) reduces to 1 / EV100ToLuminance(LuminanceMax, EV100)
    // -- the middle-grey factors cancel and ExposureCompensation is forced to 1 at :644-646.
    PINWRIGHTRUNTIME_API float Ev100ToExposureGain(float Ev100);

    // The inverse, and the one the response publishes as `ev100Equivalent`: the EV100 a caller
    // pins at to reproduce a gain the renderer resolved on its own. Exact round trip with
    // Ev100ToExposureGain. Returns 0 for a non-positive gain, which callers must gate on
    // `adaptedMeasured` rather than read as an exposure.
    PINWRIGHTRUNTIME_API float ExposureGainToEv100(float ExposureGain);

    // Read the `exposure` wire parameter off a payload root. Accepts a bare number as shorthand
    // for {mode:"fixed", ev100:N}, the object form {mode:"fixed", ev100:N}, and {mode:"auto"} on
    // its own -- "auto" with an ev100 is a contradiction and is an error, not a half. Absent ->
    // Unset with no error. Exported so the capture verbs that hand-build their
    // FViewportCaptureRequest (camera.frame_actor, camera.orbit_shots, camera.animation_shots,
    // render.capture_animation_preview) read the SAME wire vocabulary as the ones that go through
    // ParseViewportCaptureRequest -- a second parse is how two verbs end up disagreeing about
    // what `exposure` means.
    PINWRIGHTRUNTIME_API bool ParseExposurePin(const TSharedPtr<FJsonObject>& Payload, FExposurePin& OutPin,
        FString& OutErrorCode, FString& OutErrorMessage);

    PINWRIGHTRUNTIME_API TSharedPtr<FJsonObject> MakeVectorObject(const FVector& Value);
    PINWRIGHTRUNTIME_API TSharedPtr<FJsonObject> MakeRotatorObject(const FRotator& Value);
}
