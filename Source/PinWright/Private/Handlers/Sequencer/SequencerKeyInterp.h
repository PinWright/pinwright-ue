// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
// ERichCurveInterpMode / ERichCurveTangentMode. Explicit rather than inherited from a
// sibling TU: the -StrictIncludes -DisableUnity Rocket packaging build does not unity.
#include "Curves/RichCurve.h"
// FJsonObject — ParseKeyTangents reads the two optional numbers straight off a payload (or off
// one entry of a batch verb's keys[] array), so the presence test lives here once.
#include "Dom/JsonObject.h"

// Shared parsers for the `interp` / `tangentMode` / `arriveTangent` / `leaveTangent` wire
// vocabulary taken by every keyframe-authoring verb in the Sequencer cluster:
//   - sequencer.add_keyframe   (float property tracks, SequencerHandler.cpp)
//   - sequence.add_keyframe    (transform channels + property tracks, SequenceHandler.cpp)
//   - sequencer.add_keyframes  (batch transform keys, SequenceHandler.cpp)
//
// They live in a header, not per-handler statics, because two copies is exactly how the
// transform path stayed cubic-only after the float path had already gained the parameter.
// A 2-key march authored with the cubic default eases in and out: the subject starts from a
// dead stop, overshoots the mean rate mid-span, and glides to a halt at the last key. That
// reads as "the animation is weird" and is invisible in any single still.
namespace SequencerSectionHelpers
{
    // Parse an optional interp-mode string (constant|linear|cubic) for a channel key. An
    // empty/absent value defaults to cubic — the historical add_keyframe interpolation — so
    // callers that omit the param write byte-for-byte the same key as before. Mirrors the
    // shipped niagara.set_curve_keys ParseInterpMode (NiagaraCurveHandler.cpp) so every
    // authoring surface takes the same vocabulary. Returns false on an unrecognized value
    // (caller emits INVALID_ARGUMENT).
    inline bool ParseKeyInterpMode(const FString& Text, ERichCurveInterpMode& OutMode)
    {
        if (Text.IsEmpty()) { OutMode = RCIM_Cubic; return true; }
        const FString Lower = Text.ToLower();
        if (Lower == TEXT("constant")) { OutMode = RCIM_Constant; return true; }
        if (Lower == TEXT("linear"))   { OutMode = RCIM_Linear;   return true; }
        if (Lower == TEXT("cubic"))    { OutMode = RCIM_Cubic;    return true; }
        return false;
    }

    // Parse an optional cubic tangent-mode string (auto|user|break|none). Empty defaults to
    // auto — AddCubicKey's own default — and the mode only affects cubic keys
    // (constant/linear ignore tangents). The four values are the version-safe
    // ERichCurveTangentMode members present on every UE the plugin builds on (5.3-5.8);
    // RCTM_SmartAuto is intentionally not exposed for that reason.
    inline bool ParseKeyTangentMode(const FString& Text, ERichCurveTangentMode& OutMode)
    {
        if (Text.IsEmpty()) { OutMode = RCTM_Auto; return true; }
        const FString Lower = Text.ToLower();
        if (Lower == TEXT("auto"))  { OutMode = RCTM_Auto;  return true; }
        if (Lower == TEXT("user"))  { OutMode = RCTM_User;  return true; }
        if (Lower == TEXT("break")) { OutMode = RCTM_Break; return true; }
        if (Lower == TEXT("none"))  { OutMode = RCTM_None;  return true; }
        return false;
    }

    // Explicit tangent VALUES for one key, the half `tangentMode: "user"` had no way to receive.
    // Presence-tracked rather than defaulted: an omitted tangent must leave whatever the channel
    // solved (or the caller wrote earlier) alone, and 0 is a legal authored slope, so "absent" and
    // "zero" cannot share a representation.
    //
    // UNITS ARE CURVE VALUE PER TICK — not per second and not per display frame. Evaluation builds
    // the Bezier as P1 = P0 + Tangent * DX / 3 with DX in tick-resolution frames
    // (MovieSceneInterpolation.cpp), and AutoSetTangents divides by the same tick deltas, so a
    // slope of V units per second is stored as V / TickResolution. At the 24000-tick default that
    // is a 24000x error for a caller who passes uu/s, which reads as the curve exploding rather
    // than as a unit mistake — hence the units are repeated in every parameter description.
    struct FKeyTangents
    {
        bool bHasArrive = false;
        bool bHasLeave = false;
        double Arrive = 0.0;
        double Leave = 0.0;

        bool IsSet() const { return bHasArrive || bHasLeave; }
    };

    // Read the optional numeric arriveTangent / leaveTangent off a payload object. Absent fields
    // leave their presence flag false.
    inline FKeyTangents ParseKeyTangents(const TSharedPtr<FJsonObject>& Payload)
    {
        FKeyTangents Out;
        if (!Payload.IsValid())
        {
            return Out;
        }
        Out.bHasArrive = Payload->TryGetNumberField(TEXT("arriveTangent"), Out.Arrive);
        Out.bHasLeave = Payload->TryGetNumberField(TEXT("leaveTangent"), Out.Leave);
        return Out;
    }

    // Whether a written tangent value survives. FMovieScene{Double,Float}Channel::AutoSetTangents
    // recomputes every RCTM_Auto / RCTM_SmartAuto key from its neighbours — and it runs again on
    // load, from PostEditChange — so an explicit value stamped under `auto` is discarded without a
    // trace. Only RCTM_User and RCTM_Break are left alone.
    inline bool TangentModeKeepsExplicitTangents(ERichCurveTangentMode Mode)
    {
        return Mode == RCTM_User || Mode == RCTM_Break;
    }

    // The one INVALID_ARGUMENT message every keyframe verb emits for that mismatch. Rejecting is
    // deliberate: silently accepting the numbers and letting AutoSetTangents erase them is the
    // exact failure the readback exists to make visible, and it would look like the write worked.
    inline const TCHAR* ExplicitTangentModeError()
    {
        return TEXT("arriveTangent/leaveTangent require tangentMode 'user' or 'break'. "
                    "'auto' (the default) re-solves every key from its neighbours and would "
                    "discard the values, including on the next asset load.");
    }

    // Stamp explicit tangents onto a channel key value (FMovieSceneDoubleValue /
    // FMovieSceneFloatValue — both carry an FMovieSceneTangentData `Tangent`). Only the supplied
    // side is touched, so `arriveTangent` alone leaves the leave tangent as authored.
    template <typename KeyValueType>
    inline void ApplyKeyTangents(KeyValueType& KeyValue, const FKeyTangents& Tangents)
    {
        if (Tangents.bHasArrive)
        {
            KeyValue.Tangent.ArriveTangent = static_cast<float>(Tangents.Arrive);
        }
        if (Tangents.bHasLeave)
        {
            KeyValue.Tangent.LeaveTangent = static_cast<float>(Tangents.Leave);
        }
    }
}
