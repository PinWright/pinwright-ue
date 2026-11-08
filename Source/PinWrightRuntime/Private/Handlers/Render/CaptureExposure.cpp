// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/CaptureExposure.h"

#include "Handlers/ErrorCodes.h"
#include "HAL/IConsoleManager.h"
// EV100ToLuminance / LuminanceToEV100 -- the engine's own exposure<->EV100 conversion (public, inline).
#include "RenderUtils.h"

namespace PinWrightRenderCapture
{

const TCHAR* ExposureModeKey(EExposureRequestMode Mode)
{
    switch (Mode)
    {
    case EExposureRequestMode::Auto:  return TEXT("auto");
    case EExposureRequestMode::Fixed: return TEXT("fixed");
    case EExposureRequestMode::Unset: return TEXT("unset");
    }
    return TEXT("unset");
}

float ExposureLuminanceMax()
{
    // A transcription of LuminanceMaxFromLensAttenuation (UE 5.8
    // Runtime/Renderer/Private/PostProcess/PostProcessEyeAdaptation.cpp:376-389) reading the same
    // two cvars it reads, because that function lives in the Renderer module's private header. The
    // cvars are looked up on every call rather than cached in a static: both are runtime-settable
    // and a capture session that changes one must not keep converting against the old value.
    //
    // 0.78 is the ISO 12232:2006 saturation-speed constant, spelled exactly as the engine spells
    // it at :383. The 0.01 floor on the attenuation is the engine's too (:385) and is what keeps
    // a zeroed cvar from dividing by zero.
    constexpr float ISOSaturationSpeedConstant = 0.78f;
    constexpr float MinLensAttenuation = 0.01f;

    IConsoleManager& Console = IConsoleManager::Get();
    const IConsoleVariable* ExtendedRange = Console.FindConsoleVariable(
        TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"), /*bTrackFrequentCalls=*/false);
    if (ExtendedRange == nullptr || ExtendedRange->GetInt() != 1)
    {
        // IsExtendLuminanceRangeEnabled() false: the engine hardcodes the scale to 1.0 so that
        // lighting is unitless (1.0 cd/m^2 becomes 1.0 at EV100 0).
        return 1.0f;
    }
    const IConsoleVariable* LensAttenuation =
        Console.FindConsoleVariable(TEXT("r.EyeAdaptation.LensAttenuation"), /*bTrackFrequentCalls=*/false);
    const float Attenuation = LensAttenuation != nullptr
        ? LensAttenuation->GetFloat()
        : ISOSaturationSpeedConstant;
    return ISOSaturationSpeedConstant / FMath::Max(Attenuation, MinLensAttenuation);
}

float Ev100ToExposureGain(float Ev100)
{
    return 1.0f / EV100ToLuminance(ExposureLuminanceMax(), Ev100);
}

float ExposureGainToEv100(float ExposureGain)
{
    if (!(ExposureGain > 0.0f))
    {
        // No gain was measured. Returning 0 rather than -inf/NaN keeps the JSON encodable; the
        // caller must gate on `adaptedMeasured`, which is why this is not reported on its own.
        return 0.0f;
    }
    return LuminanceToEV100(ExposureLuminanceMax(), 1.0f / ExposureGain);
}

bool ParseExposurePin(const TSharedPtr<FJsonObject>& Payload, FExposurePin& OutPin,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    OutPin = FExposurePin();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Payload.IsValid() || !Payload->HasField(TEXT("exposure")))
    {
        return true;
    }

    const TSharedPtr<FJsonValue> Value = Payload->TryGetField(TEXT("exposure"));
    if (!Value.IsValid() || Value->Type == EJson::Null)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("exposure must be a number (EV100) or an object "
            "{mode:\"fixed\", ev100:<number>} or {mode:\"auto\"}; omit the field to leave auto-exposure alone");
        return false;
    }

    // Read the EV100 out of whichever shape carried it, then validate once. `bHasEv100` and not
    // "Ev100 != 0" because 0 is a perfectly ordinary EV100.
    bool bHasEv100 = false;
    double Ev100 = 0.0;
    FString Mode;

    if (Value->Type == EJson::Number)
    {
        // Bare-number shorthand: the one-token spelling of the only mode that does anything.
        Mode = TEXT("fixed");
        Ev100 = Value->AsNumber();
        bHasEv100 = true;
    }
    else if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject>& Object = Value->AsObject();
        if (!Object.IsValid())
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = TEXT("exposure object could not be read");
            return false;
        }
        Object->TryGetStringField(TEXT("mode"), Mode);
        bHasEv100 = Object->TryGetNumberField(TEXT("ev100"), Ev100);
        if (Mode.IsEmpty())
        {
            // An object carrying only an ev100 means "fixed" and nothing else; requiring the
            // caller to repeat it would be ceremony. An object carrying neither is a typo.
            if (!bHasEv100)
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrorMessage = TEXT("exposure object needs a mode (\"auto\" or \"fixed\") or an "
                    "ev100 number; omit the field entirely to leave auto-exposure alone");
                return false;
            }
            Mode = TEXT("fixed");
        }
    }
    else
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("exposure must be a number (EV100) or an object "
            "{mode:\"fixed\", ev100:<number>} or {mode:\"auto\"}");
        return false;
    }

    const FString ModeKey = Mode.ToLower();
    if (ModeKey == TEXT("auto"))
    {
        if (bHasEv100)
        {
            // The two halves of this request cancel: "auto" says leave auto-exposure running,
            // `ev100` says pin at a number. Accepting it and dropping the ev100 silently -- which
            // is what this did -- hands back an auto-exposed frame under a response that names the
            // ev100 the caller asked for, and the pin exists precisely so two frames can be
            // compared. Erroring on both halves at once is rpc-design.md section 3: there is no
            // safe half to keep, so the verb refuses instead of choosing one.
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(
                TEXT("exposure mode \"auto\" and ev100 %g contradict each other: \"auto\" leaves ")
                TEXT("auto-exposure running and ev100 pins the exposure, and no frame can be both. ")
                TEXT("Pass {mode:\"fixed\", ev100:%g} to pin at that EV100, or {mode:\"auto\"} with ")
                TEXT("no ev100 to leave auto-exposure alone."),
                Ev100, Ev100);
            return false;
        }
        // Explicit "do not pin". Renders exactly like an omitted parameter; the difference is
        // that the response can say the caller meant it.
        OutPin.Mode = EExposureRequestMode::Auto;
        return true;
    }
    if (ModeKey != TEXT("fixed"))
    {
        // An unrecognized mode errors rather than degrading to auto: a typo that silently means
        // "no pin" produces exactly the uncomparable pair the parameter exists to prevent
        // (rpc-design.md section 3).
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = FString::Printf(
            TEXT("Unknown exposure mode '%s'. Valid modes are \"fixed\" (with ev100) and \"auto\"."),
            *Mode);
        return false;
    }

    if (!bHasEv100)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("exposure mode \"fixed\" needs an ev100 number. There is no safe "
            "default: any value this verb picked would be wrong for some scene and would be "
            "indistinguishable, in the response, from a value the caller chose.");
        return false;
    }

    if (!FMath::IsFinite(Ev100) ||
        Ev100 < static_cast<double>(MinExposureEv100) ||
        Ev100 > static_cast<double>(MaxExposureEv100))
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = FString::Printf(
            TEXT("exposure ev100 must be a finite value in [%.0f, %.0f]; got %g. The bound is wide "
                 "enough for anything physically meaningful, so a value outside it is a mistyped "
                 "magnitude that would render a black or blown frame while reporting a successful pin."),
            MinExposureEv100, MaxExposureEv100, Ev100);
        return false;
    }

    OutPin.Mode = EExposureRequestMode::Fixed;
    OutPin.Ev100 = static_cast<float>(Ev100);
    return true;
}

TSharedPtr<FJsonObject> MakeVectorObject(const FVector& Value)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("x"), Value.X);
    Object->SetNumberField(TEXT("y"), Value.Y);
    Object->SetNumberField(TEXT("z"), Value.Z);
    return Object;
}

TSharedPtr<FJsonObject> MakeRotatorObject(const FRotator& Value)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("pitch"), Value.Pitch);
    Object->SetNumberField(TEXT("yaw"), Value.Yaw);
    Object->SetNumberField(TEXT("roll"), Value.Roll);
    return Object;
}

}  // namespace PinWrightRenderCapture
