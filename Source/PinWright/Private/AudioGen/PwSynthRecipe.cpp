// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSynthRecipe.cpp - kind schema tables + strict parse / lossless serialize.
//
// Every table row below is simultaneously a validation rule and a documentation
// entry: ParseSynthRecipe enforces it and audio.synth.describe_schema publishes
// it. Adding a generator/effect parameter is a one-line table edit that updates
// both, which is the reason the per-kind storage is a schema-validated bag
// rather than 21 hand-written structs.
//
// Error-code split (published verbatim by describe_schema so the LLM can route
// on the code without parsing prose):
//   UNKNOWN_GENERATOR - generator.kind is not one of the six
//   UNKNOWN_EFFECT    - an fx[].kind is not one of the fifteen
//   INVALID_PARAMS    - a VALUE problem: missing required value, wrong JSON
//                       type, out of range, outside a closed vocabulary
//   INVALID_RECIPE    - a SHAPE problem: unknown key, cap exceeded, missing
//                       container, ordering violation, cross-field contradiction

#include "AudioGen/PwSynthRecipe.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/JsonUtils.h"

// ===========================================================================
// Spec-table builders. Named (not anonymous) namespace with a PwSynth prefix so
// a Unity merge cannot collide these with another translation unit's helpers.
// ===========================================================================
namespace PwSynthSpecInternal
{
    FPwSynthParamSpec MakeBase(const TCHAR* Name, EPwSynthParamType Type, const TCHAR* Unit, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec;
        Spec.Name = FName(Name);
        Spec.DisplayName = Name;
        Spec.Type = Type;
        Spec.Unit = Unit;
        Spec.Meaning = Meaning;
        return Spec;
    }

    FPwSynthParamSpec ReqNum(const TCHAR* Name, const TCHAR* Unit, double Min, double Max, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = MakeBase(Name, EPwSynthParamType::Number, Unit, Meaning);
        Spec.bRequired = true;
        Spec.bHasMin = true;
        Spec.bHasMax = true;
        Spec.Min = Min;
        Spec.Max = Max;
        return Spec;
    }

    FPwSynthParamSpec DefNum(const TCHAR* Name, const TCHAR* Unit, double Min, double Max, double Default, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = ReqNum(Name, Unit, Min, Max, Meaning);
        Spec.bRequired = false;
        Spec.DefaultNumber = Default;
        return Spec;
    }

    FPwSynthParamSpec ReqInt(const TCHAR* Name, const TCHAR* Unit, double Min, double Max, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = ReqNum(Name, Unit, Min, Max, Meaning);
        Spec.Type = EPwSynthParamType::Integer;
        return Spec;
    }

    FPwSynthParamSpec DefInt(const TCHAR* Name, const TCHAR* Unit, double Min, double Max, double Default, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = DefNum(Name, Unit, Min, Max, Default, Meaning);
        Spec.Type = EPwSynthParamType::Integer;
        return Spec;
    }

    FPwSynthParamSpec DefBool(const TCHAR* Name, bool bDefault, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = MakeBase(Name, EPwSynthParamType::Boolean, nullptr, Meaning);
        Spec.DefaultNumber = bDefault ? 1.0 : 0.0;
        return Spec;
    }

    FPwSynthParamSpec ReqStr(const TCHAR* Name, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = MakeBase(Name, EPwSynthParamType::String, nullptr, Meaning);
        Spec.bRequired = true;
        return Spec;
    }

    FPwSynthParamSpec ReqEnum(const TCHAR* Name, const TCHAR* Values, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = MakeBase(Name, EPwSynthParamType::Enum, nullptr, Meaning);
        Spec.bRequired = true;
        Spec.EnumValues = Values;
        return Spec;
    }

    FPwSynthParamSpec DefEnum(const TCHAR* Name, const TCHAR* Values, const TCHAR* Default, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = ReqEnum(Name, Values, Meaning);
        Spec.bRequired = false;
        Spec.DefaultString = Default;
        return Spec;
    }

    FPwSynthParamSpec ReqNumArray(const TCHAR* Name, const TCHAR* Unit, double Min, double Max,
        int32 MinNum, int32 MaxNum, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = ReqNum(Name, Unit, Min, Max, Meaning);
        Spec.Type = EPwSynthParamType::NumberArray;
        Spec.MinArrayNum = MinNum;
        Spec.MaxArrayNum = MaxNum;
        return Spec;
    }

    // modal's three per-mode arrays describe the same modes, so they must agree
    // in length. A single-row spec cannot say that; this is the hook for it.
    bool ValidateModalModes(const FPwSynthParams& Params, FString& OutParamName, FString& OutMessage)
    {
        const TArray<double>* Freqs = Params.GetNumbers(FName(TEXT("modeFreqsHz")));
        const TArray<double>* Decays = Params.GetNumbers(FName(TEXT("modeDecaysMs")));
        const TArray<double>* Gains = Params.GetNumbers(FName(TEXT("modeGainsDb")));
        if (!Freqs || !Decays || !Gains)
        {
            return true;    // a missing array already failed the per-row check
        }
        if (Decays->Num() != Freqs->Num())
        {
            OutParamName = TEXT("modeDecaysMs");
            OutMessage = FString::Printf(
                TEXT("has %d entries but modeFreqsHz has %d; the three mode arrays describe the same modes and must be the same length"),
                Decays->Num(), Freqs->Num());
            return false;
        }
        if (Gains->Num() != Freqs->Num())
        {
            OutParamName = TEXT("modeGainsDb");
            OutMessage = FString::Printf(
                TEXT("has %d entries but modeFreqsHz has %d; the three mode arrays describe the same modes and must be the same length"),
                Gains->Num(), Freqs->Num());
            return false;
        }
        return true;
    }

    const TArray<FPwSynthParamSpec>& OscParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqEnum(TEXT("waveform"), TEXT("sine|triangle|saw|square|pulse"), TEXT("Oscillator wave shape.")),
            ReqNum(TEXT("frequencyHz"), TEXT("hz"), 0.01, 20000.0, TEXT("Base pitch before the pitch envelope.")),
            DefNum(TEXT("pulseWidth"), TEXT("ratio"), 0.01, 0.99, 0.5, TEXT("Duty cycle; pulse waveform only.")),
            DefNum(TEXT("phase"), TEXT("turns"), 0.0, 1.0, 0.0, TEXT("Start phase in turns.")),
            DefNum(TEXT("detuneCents"), TEXT("cents"), -1200.0, 1200.0, 0.0, TEXT("Constant pitch offset.")),
            DefInt(TEXT("unison"), nullptr, 1.0, 8.0, 1.0, TEXT("Number of detuned copies summed.")),
            DefNum(TEXT("unisonSpreadCents"), TEXT("cents"), 0.0, 100.0, 0.0, TEXT("Total detune spread across the unison voices."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& NoiseParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqEnum(TEXT("color"), TEXT("white|pink|brown|blue|violet"), TEXT("Spectral tilt of the noise.")),
            DefNum(TEXT("lowCutHz"), TEXT("hz"), 20.0, 20000.0, 20.0, TEXT("High-pass corner applied to the noise.")),
            DefNum(TEXT("highCutHz"), TEXT("hz"), 20.0, 20000.0, 20000.0, TEXT("Low-pass corner applied to the noise."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& ModalParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNumArray(TEXT("modeFreqsHz"), TEXT("hz"), 0.01, 20000.0, 1, 32, TEXT("Resonant mode frequencies.")),
            ReqNumArray(TEXT("modeDecaysMs"), TEXT("ms"), 1.0, 20000.0, 1, 32, TEXT("Per-mode -60 dB decay time; same length as modeFreqsHz.")),
            ReqNumArray(TEXT("modeGainsDb"), TEXT("db"), -96.0, 24.0, 1, 32, TEXT("Per-mode level; same length as modeFreqsHz.")),
            DefEnum(TEXT("exciter"), TEXT("impulse|noise|strike"), TEXT("impulse"), TEXT("Signal that rings the mode bank.")),
            DefNum(TEXT("exciterMs"), TEXT("ms"), 0.1, 100.0, 2.0, TEXT("Exciter burst length."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& FormantParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("f0Hz"), TEXT("hz"), 20.0, 2000.0, TEXT("Glottal fundamental frequency.")),
            ReqEnum(TEXT("vowel"), TEXT("a|e|i|o|u"), TEXT("Vowel preset selecting the F1..F5 formant set.")),
            DefNum(TEXT("formantShift"), TEXT("ratio"), 0.5, 2.0, 1.0, TEXT("Scales the formant frequencies (vocal-tract length).")),
            DefNum(TEXT("voicing"), TEXT("ratio"), 0.0, 1.0, 1.0, TEXT("Pulse-vs-noise balance of the glottal source.")),
            DefNum(TEXT("breathiness"), TEXT("ratio"), 0.0, 1.0, 0.0, TEXT("Aspiration noise mixed into the source."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& GranularParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqStr(TEXT("sourcePath"), TEXT("Asset path of the source USoundWave.")),
            ReqNum(TEXT("grainMs"), TEXT("ms"), 1.0, 500.0, TEXT("Length of one grain.")),
            ReqNum(TEXT("densityHz"), TEXT("hz"), 0.1, 1000.0, TEXT("Grains emitted per second.")),
            DefNum(TEXT("positionStart"), TEXT("ratio"), 0.0, 1.0, 0.0, TEXT("Normalized read-head start in the source.")),
            DefNum(TEXT("positionEnd"), TEXT("ratio"), 0.0, 1.0, 1.0, TEXT("Normalized read-head end in the source.")),
            DefNum(TEXT("positionJitter"), TEXT("ratio"), 0.0, 1.0, 0.0, TEXT("Random read-head offset per grain.")),
            DefNum(TEXT("pitchJitterCents"), TEXT("cents"), 0.0, 2400.0, 0.0, TEXT("Random pitch spread per grain.")),
            DefNum(TEXT("reverseChance"), TEXT("ratio"), 0.0, 1.0, 0.0, TEXT("Probability that a grain plays backwards."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& SampleParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqStr(TEXT("sourcePath"), TEXT("Asset path of the source USoundWave.")),
            // Named sourceStartMs, not startMs: the layer already has a startMs
            // meaning "when the layer begins", and two same-named knobs one level
            // apart is exactly the confusion this schema exists to prevent. Level
            // has no generator knob at all - layers[].gainDb is the one place for it.
            DefNum(TEXT("sourceStartMs"), TEXT("ms"), 0.0, 600000.0, 0.0, TEXT("Read offset into the source.")),
            DefNum(TEXT("playbackRate"), TEXT("ratio"), 0.01, 8.0, 1.0, TEXT("Resample ratio; also shifts pitch.")),
            DefBool(TEXT("loop"), false, TEXT("Repeat the source until the layer ends."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& FilterParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqEnum(TEXT("type"), TEXT("lowpass|highpass|bandpass|notch|lowshelf|highshelf|peaking"), TEXT("Filter response.")),
            ReqNum(TEXT("cutoffHz"), TEXT("hz"), 20.0, 20000.0, TEXT("Corner or centre frequency.")),
            DefNum(TEXT("resonance"), TEXT("q"), 0.1, 20.0, 0.707, TEXT("Q factor.")),
            DefNum(TEXT("gainDb"), TEXT("db"), -24.0, 24.0, 0.0, TEXT("Band gain; shelf and peaking types only."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& DistortParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqEnum(TEXT("type"), TEXT("soft|hard|foldback|bitcrush|tube"), TEXT("Waveshaping curve.")),
            ReqNum(TEXT("drive"), TEXT("ratio"), 0.0, 100.0, TEXT("Pre-gain into the shaper.")),
            DefNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, 1.0, TEXT("Wet/dry blend.")),
            DefNum(TEXT("outGainDb"), TEXT("db"), -48.0, 24.0, 0.0, TEXT("Post-shaper level trim."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& DelayParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("timeMs"), TEXT("ms"), 0.1, 5000.0, TEXT("Delay time.")),
            ReqNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, TEXT("Wet/dry blend.")),
            DefNum(TEXT("feedback"), TEXT("ratio"), 0.0, 0.99, 0.0, TEXT("Regeneration; 0 gives a single echo.")),
            DefNum(TEXT("dampingHz"), TEXT("hz"), 20.0, 20000.0, 20000.0, TEXT("Low-pass inside the feedback path."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& ReverbParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("decayMs"), TEXT("ms"), 10.0, 20000.0, TEXT("RT60 tail length.")),
            ReqNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, TEXT("Wet/dry blend.")),
            DefNum(TEXT("preDelayMs"), TEXT("ms"), 0.0, 500.0, 0.0, TEXT("Gap before the tail starts.")),
            DefNum(TEXT("dampingHz"), TEXT("hz"), 20.0, 20000.0, 8000.0, TEXT("High-frequency absorption of the tail."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& ChorusParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("rateHz"), TEXT("hz"), 0.01, 20.0, TEXT("LFO rate.")),
            ReqNum(TEXT("depthMs"), TEXT("ms"), 0.1, 50.0, TEXT("Delay modulation depth.")),
            ReqNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, TEXT("Wet/dry blend.")),
            DefInt(TEXT("voices"), nullptr, 1.0, 8.0, 2.0, TEXT("Number of modulated taps."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& FlangerParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("rateHz"), TEXT("hz"), 0.01, 20.0, TEXT("LFO rate.")),
            ReqNum(TEXT("depthMs"), TEXT("ms"), 0.05, 20.0, TEXT("Delay modulation depth.")),
            ReqNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, TEXT("Wet/dry blend.")),
            DefNum(TEXT("feedback"), TEXT("ratio"), -0.99, 0.99, 0.0, TEXT("Signed feedback around the delay."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& PhaserParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("rateHz"), TEXT("hz"), 0.01, 20.0, TEXT("LFO rate.")),
            ReqNum(TEXT("depth"), TEXT("ratio"), 0.0, 1.0, TEXT("All-pass sweep range.")),
            ReqNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, TEXT("Wet/dry blend.")),
            DefInt(TEXT("stages"), nullptr, 2.0, 12.0, 4.0, TEXT("Number of all-pass stages.")),
            DefNum(TEXT("feedback"), TEXT("ratio"), -0.99, 0.99, 0.0, TEXT("Feedback around the all-pass chain."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& RingModParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("rateHz"), TEXT("hz"), 0.1, 20000.0, TEXT("Carrier frequency.")),
            ReqNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, TEXT("Wet/dry blend.")),
            DefEnum(TEXT("waveform"), TEXT("sine|triangle|saw|square"), TEXT("sine"), TEXT("Carrier wave shape."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& PitchShiftParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("semitones"), TEXT("semitones"), -48.0, 48.0, TEXT("Pitch offset.")),
            DefNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, 1.0, TEXT("Wet/dry blend.")),
            DefBool(TEXT("formantPreserve"), false, TEXT("Keep formants fixed while shifting pitch."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& CompressorParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("thresholdDb"), TEXT("db"), -60.0, 0.0, TEXT("Level above which gain reduction starts.")),
            ReqNum(TEXT("ratio"), TEXT("ratio"), 1.0, 50.0, TEXT("Input-to-output slope above the threshold.")),
            DefNum(TEXT("attackMs"), TEXT("ms"), 0.1, 500.0, 5.0, TEXT("Gain-reduction onset time.")),
            DefNum(TEXT("releaseMs"), TEXT("ms"), 1.0, 5000.0, 100.0, TEXT("Gain-recovery time.")),
            DefNum(TEXT("kneeDb"), TEXT("db"), 0.0, 24.0, 0.0, TEXT("Soft-knee width around the threshold.")),
            DefNum(TEXT("makeupDb"), TEXT("db"), -24.0, 24.0, 0.0, TEXT("Post-compression level trim."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& EqParams()
    {
        // The three gains are required: an EQ whose bands all default to 0 dB is a
        // no-op the caller did not ask for (rpc-design §3).
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("lowGainDb"), TEXT("db"), -24.0, 24.0, TEXT("Low shelf gain.")),
            ReqNum(TEXT("midGainDb"), TEXT("db"), -24.0, 24.0, TEXT("Mid peaking gain.")),
            ReqNum(TEXT("highGainDb"), TEXT("db"), -24.0, 24.0, TEXT("High shelf gain.")),
            DefNum(TEXT("lowHz"), TEXT("hz"), 20.0, 2000.0, 200.0, TEXT("Low shelf corner.")),
            DefNum(TEXT("midHz"), TEXT("hz"), 20.0, 20000.0, 1000.0, TEXT("Mid band centre.")),
            DefNum(TEXT("midQ"), TEXT("q"), 0.1, 20.0, 1.0, TEXT("Mid band Q.")),
            DefNum(TEXT("highHz"), TEXT("hz"), 1000.0, 20000.0, 4000.0, TEXT("High shelf corner."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& GainParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("gainDb"), TEXT("db"), PwSynthLimits::MinGainDb, PwSynthLimits::MaxGainDb, TEXT("Level change."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& WidthParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("width"), TEXT("ratio"), 0.0, 2.0, TEXT("Stereo width; 0 collapses to mono, 1 leaves the image unchanged."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& ReverseParams()
    {
        static const TArray<FPwSynthParamSpec> Table;
        return Table;
    }

    const TArray<FPwSynthParamSpec>& ConvolveParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqStr(TEXT("impulsePath"), TEXT("Asset path of the impulse-response USoundWave.")),
            ReqNum(TEXT("mix"), TEXT("ratio"), 0.0, 1.0, TEXT("Wet/dry blend.")),
            DefBool(TEXT("normalize"), true, TEXT("Scale the impulse response to unity energy before convolving."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& ModulationParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqNum(TEXT("depth"), TEXT("ratio"), 0.0, 100.0, TEXT("Amount: FM index for fm, 0..1 wet depth for am and ring.")),
            ReqNum(TEXT("rateHz"), TEXT("hz"), 0.01, 20000.0, TEXT("Modulator frequency.")),
            DefEnum(TEXT("source"), TEXT("sine|triangle|saw|square|noise"), TEXT("sine"), TEXT("Modulator wave shape."))
        };
        return Table;
    }

    FPwSynthKindSpec MakeKind(const TCHAR* Name, const TCHAR* Summary,
        const TArray<FPwSynthParamSpec>& Params, FPwSynthParamsValidator Validator = nullptr,
        const TCHAR* Constraint = nullptr, bool bMasterOnly = false)
    {
        FPwSynthKindSpec Spec;
        Spec.Name = Name;
        Spec.Summary = Summary;
        Spec.Params = &Params;
        Spec.Validator = Validator;
        Spec.Constraint = Constraint;
        Spec.bMasterOnly = bMasterOnly;
        return Spec;
    }

    // Indexed by EPwSynthGeneratorKind; slot 0 (Unspecified) is a deliberately
    // empty spec so a default-constructed generator cannot render as a real kind.
    const TArray<FPwSynthKindSpec>& GeneratorTable()
    {
        static const TArray<FPwSynthKindSpec> Table = {
            MakeKind(TEXT(""), TEXT(""), ReverseParams()),
            MakeKind(TEXT("osc"), TEXT("Bandlimited single-voice oscillator with optional unison detune."), OscParams()),
            MakeKind(TEXT("noise"), TEXT("Coloured noise source with a band limit."), NoiseParams()),
            MakeKind(TEXT("modal"), TEXT("Bank of exponentially decaying resonant modes; metallic bodies and impacts."), ModalParams(), &ValidateModalModes,
                TEXT("modeFreqsHz, modeDecaysMs and modeGainsDb describe the same modes and must be the same length.")),
            MakeKind(TEXT("formant"), TEXT("Voiced glottal source through a vowel formant bank."), FormantParams()),
            MakeKind(TEXT("granular"), TEXT("Grain cloud scattered from a source USoundWave."), GranularParams()),
            MakeKind(TEXT("sample"), TEXT("Plays a source USoundWave straight into the layer."), SampleParams())
        };
        return Table;
    }

    const TArray<FPwSynthKindSpec>& FxTable()
    {
        static const TArray<FPwSynthKindSpec> Table = {
            MakeKind(TEXT(""), TEXT(""), ReverseParams()),
            MakeKind(TEXT("filter"), TEXT("Biquad filter."), FilterParams()),
            MakeKind(TEXT("distort"), TEXT("Waveshaping saturation."), DistortParams()),
            MakeKind(TEXT("delay"), TEXT("Feedback delay line."), DelayParams()),
            MakeKind(TEXT("reverb"), TEXT("Algorithmic room tail."), ReverbParams()),
            MakeKind(TEXT("chorus"), TEXT("Multi-tap modulated delay thickener."), ChorusParams()),
            MakeKind(TEXT("flanger"), TEXT("Short modulated delay with feedback."), FlangerParams()),
            MakeKind(TEXT("phaser"), TEXT("Swept all-pass notch chain."), PhaserParams()),
            MakeKind(TEXT("ringmod"), TEXT("Amplitude multiplication against a carrier."), RingModParams()),
            MakeKind(TEXT("pitchshift"), TEXT("Pitch transposition without a length change."), PitchShiftParams()),
            MakeKind(TEXT("compressor"), TEXT("Dynamic range compression."), CompressorParams()),
            MakeKind(TEXT("eq"), TEXT("Three-band shelf/peak equalizer."), EqParams()),
            MakeKind(TEXT("gain"), TEXT("Static level change."), GainParams()),
            MakeKind(TEXT("width"), TEXT("Mid/side stereo width control. Master only: layers are mono."), WidthParams(), nullptr, nullptr, /*bMasterOnly=*/true),
            MakeKind(TEXT("reverse"), TEXT("Reverses the buffer. Takes no parameters."), ReverseParams()),
            MakeKind(TEXT("convolve"), TEXT("Convolution with an impulse-response USoundWave."), ConvolveParams())
        };
        return Table;
    }
}

// ===========================================================================
// Vocabulary accessors
// ===========================================================================

const FPwSynthKindSpec& PwSynthGeneratorSpec(EPwSynthGeneratorKind Kind)
{
    const TArray<FPwSynthKindSpec>& Table = PwSynthSpecInternal::GeneratorTable();
    const int32 Index = static_cast<int32>(Kind);
    return Table.IsValidIndex(Index) ? Table[Index] : Table[0];
}

const FPwSynthKindSpec& PwSynthFxSpec(EPwSynthFxKind Kind)
{
    const TArray<FPwSynthKindSpec>& Table = PwSynthSpecInternal::FxTable();
    const int32 Index = static_cast<int32>(Kind);
    return Table.IsValidIndex(Index) ? Table[Index] : Table[0];
}

const TCHAR* PwSynthGeneratorKindToString(EPwSynthGeneratorKind Kind)
{
    return PwSynthGeneratorSpec(Kind).Name;
}

bool PwSynthGeneratorKindFromString(const FString& In, EPwSynthGeneratorKind& Out)
{
    const TArray<FPwSynthKindSpec>& Table = PwSynthSpecInternal::GeneratorTable();
    for (int32 Index = 1; Index < Table.Num(); ++Index)
    {
        if (In.Equals(Table[Index].Name, ESearchCase::IgnoreCase))
        {
            Out = static_cast<EPwSynthGeneratorKind>(Index);
            return true;
        }
    }
    return false;
}

const TCHAR* PwSynthFxKindToString(EPwSynthFxKind Kind)
{
    return PwSynthFxSpec(Kind).Name;
}

bool PwSynthFxKindFromString(const FString& In, EPwSynthFxKind& Out)
{
    const TArray<FPwSynthKindSpec>& Table = PwSynthSpecInternal::FxTable();
    for (int32 Index = 1; Index < Table.Num(); ++Index)
    {
        if (In.Equals(Table[Index].Name, ESearchCase::IgnoreCase))
        {
            Out = static_cast<EPwSynthFxKind>(Index);
            return true;
        }
    }
    return false;
}

namespace PwSynthSpecInternal
{
    struct FTokenSpec
    {
        const TCHAR* Name;
        const TCHAR* Meaning;
    };

    const TArray<FTokenSpec>& CurveTable()
    {
        static const TArray<FTokenSpec> Table = {
            { TEXT("linear"), TEXT("Straight line to the next point. Default.") },
            { TEXT("exp"), TEXT("Fast start, slow finish; natural for decays.") },
            { TEXT("log"), TEXT("Slow start, fast finish; natural for swells.") },
            { TEXT("step"), TEXT("Holds this point's value until the next point.") },
            { TEXT("scurve"), TEXT("Eased at both ends; click-free crossfades.") }
        };
        return Table;
    }

    const TArray<FTokenSpec>& NormalizeModeTable()
    {
        static const TArray<FTokenSpec> Table = {
            { TEXT("none"), TEXT("No normalization.") },
            { TEXT("peak"), TEXT("Scale so the true peak lands on target, in dBFS.") },
            { TEXT("lufs"), TEXT("Scale so the integrated loudness lands on target, in LUFS.") }
        };
        return Table;
    }

    const TArray<FTokenSpec>& ModRoutingTable()
    {
        static const TArray<FTokenSpec> Table = {
            { TEXT("none"), TEXT("No modulation.") },
            { TEXT("fm"), TEXT("Frequency modulation of the generator by the modulator.") },
            { TEXT("am"), TEXT("Amplitude modulation; retains the carrier.") },
            { TEXT("ring"), TEXT("Ring modulation; suppresses the carrier.") }
        };
        return Table;
    }

    const TArray<FTokenSpec>& ModSourceTable()
    {
        static const TArray<FTokenSpec> Table = {
            { TEXT("sine"), TEXT("Sine modulator. Default.") },
            { TEXT("triangle"), TEXT("Triangle modulator.") },
            { TEXT("saw"), TEXT("Sawtooth modulator.") },
            { TEXT("square"), TEXT("Square modulator.") },
            { TEXT("noise"), TEXT("White-noise modulator.") }
        };
        return Table;
    }

    const TArray<FPwSynthMetricSpec>& MetricTable()
    {
        static const TArray<FPwSynthMetricSpec> Table = {
            { TEXT("peakDb"), TEXT("dbfs"), TEXT("Highest absolute sample, in dBFS.") },
            { TEXT("rmsDb"), TEXT("dbfs"), TEXT("Root-mean-square level over the whole render.") },
            { TEXT("lufs"), TEXT("lufs"), TEXT("Integrated loudness (BS.1770).") },
            { TEXT("crestDb"), TEXT("db"), TEXT("Peak minus RMS; how punchy versus how dense.") },
            { TEXT("durationMs"), TEXT("ms"), TEXT("Rendered length, including tails.") },
            { TEXT("centroidHz"), TEXT("hz"), TEXT("Spectral centroid; perceived brightness.") },
            { TEXT("rolloffHz"), TEXT("hz"), TEXT("Frequency below which 85% of the energy sits.") },
            { TEXT("flatness"), TEXT("ratio"), TEXT("Spectral flatness 0..1; 1 is noise-like, 0 is tonal.") },
            { TEXT("zeroCrossingRate"), TEXT("hz"), TEXT("Zero crossings per second; noisiness proxy.") },
            { TEXT("attackMs"), TEXT("ms"), TEXT("Time from start to the peak.") },
            { TEXT("decayMs"), TEXT("ms"), TEXT("Time from the peak down to -60 dB.") },
            { TEXT("dcOffset"), TEXT("ratio"), TEXT("Mean sample value; should sit near 0.") },
            { TEXT("clippedSamples"), TEXT("count"), TEXT("Samples at or beyond full scale.") },
            { TEXT("stereoCorrelation"), TEXT("ratio"), TEXT("-1..1 correlation of L and R; mono-compatibility check.") }
        };
        return Table;
    }

    template <typename EnumType>
    bool TokenFromString(const TArray<FTokenSpec>& Table, const FString& In, EnumType& Out)
    {
        for (int32 Index = 0; Index < Table.Num(); ++Index)
        {
            if (In.Equals(Table[Index].Name, ESearchCase::IgnoreCase))
            {
                Out = static_cast<EnumType>(Index);
                return true;
            }
        }
        return false;
    }

    const TCHAR* TokenToString(const TArray<FTokenSpec>& Table, int32 Index)
    {
        return Table.IsValidIndex(Index) ? Table[Index].Name : TEXT("");
    }
}

const TCHAR* PwSynthCurveToString(EPwSynthCurve Curve)
{
    return PwSynthSpecInternal::TokenToString(PwSynthSpecInternal::CurveTable(), static_cast<int32>(Curve));
}

bool PwSynthCurveFromString(const FString& In, EPwSynthCurve& Out)
{
    return PwSynthSpecInternal::TokenFromString(PwSynthSpecInternal::CurveTable(), In, Out);
}

const TCHAR* PwSynthCurveMeaning(EPwSynthCurve Curve)
{
    const TArray<PwSynthSpecInternal::FTokenSpec>& Table = PwSynthSpecInternal::CurveTable();
    const int32 Index = static_cast<int32>(Curve);
    return Table.IsValidIndex(Index) ? Table[Index].Meaning : TEXT("");
}

const TCHAR* PwSynthNormalizeModeToString(EPwSynthNormalizeMode Mode)
{
    return PwSynthSpecInternal::TokenToString(PwSynthSpecInternal::NormalizeModeTable(), static_cast<int32>(Mode));
}

bool PwSynthNormalizeModeFromString(const FString& In, EPwSynthNormalizeMode& Out)
{
    return PwSynthSpecInternal::TokenFromString(PwSynthSpecInternal::NormalizeModeTable(), In, Out);
}

const TCHAR* PwSynthModRoutingToString(EPwSynthModulationRouting Routing)
{
    return PwSynthSpecInternal::TokenToString(PwSynthSpecInternal::ModRoutingTable(), static_cast<int32>(Routing));
}

bool PwSynthModRoutingFromString(const FString& In, EPwSynthModulationRouting& Out)
{
    return PwSynthSpecInternal::TokenFromString(PwSynthSpecInternal::ModRoutingTable(), In, Out);
}

const TCHAR* PwSynthModSourceToString(EPwSynthModSource Source)
{
    return PwSynthSpecInternal::TokenToString(PwSynthSpecInternal::ModSourceTable(), static_cast<int32>(Source));
}

bool PwSynthModSourceFromString(const FString& In, EPwSynthModSource& Out)
{
    return PwSynthSpecInternal::TokenFromString(PwSynthSpecInternal::ModSourceTable(), In, Out);
}

const FPwSynthMetricSpec& PwSynthMetricSpec(EPwSynthMetric Metric)
{
    const TArray<FPwSynthMetricSpec>& Table = PwSynthSpecInternal::MetricTable();
    const int32 Index = static_cast<int32>(Metric);
    return Table.IsValidIndex(Index) ? Table[Index] : Table[0];
}

const TCHAR* PwSynthMetricToString(EPwSynthMetric Metric)
{
    return PwSynthMetricSpec(Metric).Name;
}

bool PwSynthMetricFromString(const FString& In, EPwSynthMetric& Out)
{
    const TArray<FPwSynthMetricSpec>& Table = PwSynthSpecInternal::MetricTable();
    for (int32 Index = 0; Index < Table.Num(); ++Index)
    {
        if (In.Equals(Table[Index].Name, ESearchCase::IgnoreCase))
        {
            Out = static_cast<EPwSynthMetric>(Index);
            return true;
        }
    }
    return false;
}

const TArray<FPwSynthParamSpec>& PwSynthModulationParamSpecs()
{
    return PwSynthSpecInternal::ModulationParams();
}

const TCHAR* PwSynthParamTypeToString(EPwSynthParamType Type)
{
    switch (Type)
    {
    case EPwSynthParamType::Number:      return TEXT("number");
    case EPwSynthParamType::Integer:     return TEXT("integer");
    case EPwSynthParamType::Boolean:     return TEXT("boolean");
    case EPwSynthParamType::String:      return TEXT("string");
    case EPwSynthParamType::Enum:        return TEXT("enum");
    case EPwSynthParamType::NumberArray: return TEXT("number[]");
    default:                             return TEXT("number");
    }
}

// ===========================================================================
// FPwSynthParams accessors
// ===========================================================================

bool FPwSynthParams::Has(FName Name) const
{
    return Values.Contains(Name);
}

double FPwSynthParams::GetNumber(FName Name, double Default) const
{
    const FPwSynthParamValue* Value = Values.Find(Name);
    return Value ? Value->Number : Default;
}

int32 FPwSynthParams::GetInt(FName Name, int32 Default) const
{
    const FPwSynthParamValue* Value = Values.Find(Name);
    return Value ? static_cast<int32>(FMath::RoundToDouble(Value->Number)) : Default;
}

bool FPwSynthParams::GetBool(FName Name, bool bDefault) const
{
    const FPwSynthParamValue* Value = Values.Find(Name);
    return Value ? Value->bBool : bDefault;
}

FString FPwSynthParams::GetString(FName Name, const FString& Default) const
{
    const FPwSynthParamValue* Value = Values.Find(Name);
    return Value ? Value->String : Default;
}

const TArray<double>* FPwSynthParams::GetNumbers(FName Name) const
{
    const FPwSynthParamValue* Value = Values.Find(Name);
    return Value ? &Value->Numbers : nullptr;
}

// ===========================================================================
// FPwSynthRecipeError
// ===========================================================================

FString FPwSynthRecipeError::ToString() const
{
    if (!IsSet())
    {
        return FString();
    }
    return Field.IsEmpty()
        ? Message
        : FString::Printf(TEXT("%s: %s"), *Field, *Message);
}

void FPwSynthRecipeError::Reset()
{
    Code.Reset();
    Field.Reset();
    Message.Reset();
}

// ===========================================================================
// Parsing
// ===========================================================================
namespace PwSynthParseInternal
{
    bool Fail(FPwSynthRecipeError& Err, const TCHAR* Code, const FString& Field, const FString& Message)
    {
        Err.Code = Code;
        Err.Field = Field;
        Err.Message = Message;
        return false;
    }

    FString Join(const FString& Base, const TCHAR* Leaf)
    {
        return Base.IsEmpty() ? FString(Leaf) : FString::Printf(TEXT("%s.%s"), *Base, Leaf);
    }

    FString JoinStr(const FString& Base, const FString& Leaf)
    {
        return Base.IsEmpty() ? Leaf : FString::Printf(TEXT("%s.%s"), *Base, *Leaf);
    }

    FString Indexed(const FString& Base, int32 Index)
    {
        return FString::Printf(TEXT("%s[%d]"), *Base, Index);
    }

    FString JoinList(const TArray<FString>& Items)
    {
        return FString::Join(Items, TEXT(", "));
    }

    // Rejects any key the schema does not know, so a misspelled field surfaces as
    // a precise error instead of a silently dropped setting (rpc-design §3).
    bool RejectUnknownKeys(const TSharedPtr<FJsonObject>& Obj, const FString& Path,
        const TArray<FString>& Known, FPwSynthRecipeError& Err)
    {
        TArray<FString> Unknown;
        if (!::RejectUnknownKeys(Obj, Known, Unknown, ERejectUnknownKeysMode::First, ESearchCase::IgnoreCase))
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, JoinStr(Path, Unknown[0]),
                FString::Printf(TEXT("unknown field. Accepted fields here: %s."), *JoinList(Known)));
        }
        return true;
    }

    // Explicit JSON null reads as absent so a caller can clear an optional field.
    TSharedPtr<FJsonValue> FindField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
    {
        if (!Obj.IsValid())
        {
            return nullptr;
        }
        TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
        if (Value.IsValid() && Value->Type == EJson::Null)
        {
            return nullptr;
        }
        return Value;
    }

    bool AsNumber(const TSharedPtr<FJsonValue>& Value, const FString& Path, double& Out, FPwSynthRecipeError& Err)
    {
        if (Value->Type != EJson::Number)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path, TEXT("expected a JSON number."));
        }
        Out = Value->AsNumber();
        return true;
    }

    bool AsBool(const TSharedPtr<FJsonValue>& Value, const FString& Path, bool& Out, FPwSynthRecipeError& Err)
    {
        if (Value->Type != EJson::Boolean)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path, TEXT("expected a JSON boolean."));
        }
        Out = Value->AsBool();
        return true;
    }

    bool AsString(const TSharedPtr<FJsonValue>& Value, const FString& Path, FString& Out, FPwSynthRecipeError& Err)
    {
        if (Value->Type != EJson::String)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path, TEXT("expected a JSON string."));
        }
        Out = Value->AsString();
        return true;
    }

    bool AsObject(const TSharedPtr<FJsonValue>& Value, const FString& Path, TSharedPtr<FJsonObject>& Out, FPwSynthRecipeError& Err)
    {
        if (Value->Type != EJson::Object)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path, TEXT("expected a JSON object."));
        }
        Out = Value->AsObject();
        return Out.IsValid()
            ? true
            : Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path, TEXT("expected a JSON object."));
    }

    bool AsArray(const TSharedPtr<FJsonValue>& Value, const FString& Path,
        const TArray<TSharedPtr<FJsonValue>>*& Out, FPwSynthRecipeError& Err)
    {
        if (Value->Type != EJson::Array)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path, TEXT("expected a JSON array."));
        }
        Out = &Value->AsArray();
        return true;
    }

    bool CheckRange(double Number, bool bHasMin, double Min, bool bHasMax, double Max,
        const FString& Path, const TCHAR* Unit, FPwSynthRecipeError& Err)
    {
        if ((bHasMin && Number < Min) || (bHasMax && Number > Max))
        {
            const FString UnitText = Unit ? FString::Printf(TEXT(" %s"), Unit) : FString();
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                FString::Printf(TEXT("%g%s is outside the accepted range %g..%g."),
                    Number, *UnitText, Min, Max));
        }
        return true;
    }

    bool CheckIntegral(double Number, const FString& Path, FPwSynthRecipeError& Err)
    {
        if (!FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                FString::Printf(TEXT("%g is not a whole number."), Number));
        }
        return true;
    }

    void SplitEnumValues(const TCHAR* EnumValues, TArray<FString>& Out)
    {
        Out.Reset();
        if (EnumValues)
        {
            FString(EnumValues).ParseIntoArray(Out, TEXT("|"), true);
        }
    }

    // ---- scalar readers used by the fixed (non-bag) recipe fields ----

    bool ReadRequiredNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Parent,
        double Min, double Max, const TCHAR* Unit, double& Out, FPwSynthRecipeError& Err)
    {
        const FString Path = Join(Parent, Key);
        TSharedPtr<FJsonValue> Value = FindField(Obj, Key);
        if (!Value.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                TEXT("required field is missing; it has no safe default."));
        }
        return AsNumber(Value, Path, Out, Err) && CheckRange(Out, true, Min, true, Max, Path, Unit, Err);
    }

    bool ReadOptionalNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Parent,
        double Min, double Max, double Default, const TCHAR* Unit, double& Out, FPwSynthRecipeError& Err)
    {
        Out = Default;
        TSharedPtr<FJsonValue> Value = FindField(Obj, Key);
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(Parent, Key);
        return AsNumber(Value, Path, Out, Err) && CheckRange(Out, true, Min, true, Max, Path, Unit, Err);
    }

    bool ReadOptionalInt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Parent,
        double Min, double Max, int32 Default, int32& Out, FPwSynthRecipeError& Err)
    {
        Out = Default;
        TSharedPtr<FJsonValue> Value = FindField(Obj, Key);
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(Parent, Key);
        double Number = 0.0;
        if (!AsNumber(Value, Path, Number, Err) ||
            !CheckIntegral(Number, Path, Err) ||
            !CheckRange(Number, true, Min, true, Max, Path, nullptr, Err))
        {
            return false;
        }
        Out = static_cast<int32>(FMath::RoundToDouble(Number));
        return true;
    }

    // ---- per-kind parameter bag ----

    FPwSynthParamValue DefaultValueFor(const FPwSynthParamSpec& Spec)
    {
        FPwSynthParamValue Value;
        Value.Type = Spec.Type;
        switch (Spec.Type)
        {
        case EPwSynthParamType::Boolean:
            Value.bBool = Spec.DefaultNumber != 0.0;
            break;
        case EPwSynthParamType::String:
        case EPwSynthParamType::Enum:
            Value.String = Spec.DefaultString ? FString(Spec.DefaultString) : FString();
            break;
        case EPwSynthParamType::NumberArray:
            break;
        default:
            Value.Number = Spec.DefaultNumber;
            break;
        }
        return Value;
    }

    bool ParseParams(const TSharedPtr<FJsonObject>& ParamsObj, const FPwSynthKindSpec& Kind,
        const TCHAR* KindNoun, const FString& ParamsPath, FPwSynthParams& Out, FPwSynthRecipeError& Err)
    {
        const TArray<FPwSynthParamSpec>& Table = *Kind.Params;

        TArray<FString> Known;
        Known.Reserve(Table.Num());
        for (const FPwSynthParamSpec& Spec : Table)
        {
            Known.Add(Spec.DisplayName);
        }

        if (ParamsObj.IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : ParamsObj->Values)
            {
                if (!Known.Contains(Pair.Key))
                {
                    const FString KnownText = Known.Num() > 0
                        ? FString::Printf(TEXT("Known parameters: %s."), *JoinList(Known))
                        : FString::Printf(TEXT("The '%s' %s takes no parameters."), Kind.Name, KindNoun);
                    return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, JoinStr(ParamsPath, Pair.Key),
                        FString::Printf(TEXT("'%s' is not a parameter of the '%s' %s. %s"),
                            *Pair.Key, Kind.Name, KindNoun, *KnownText));
                }
            }
        }

        for (const FPwSynthParamSpec& Spec : Table)
        {
            const FString Path = Join(ParamsPath, Spec.DisplayName);
            TSharedPtr<FJsonValue> Value = FindField(ParamsObj, Spec.DisplayName);

            if (!Value.IsValid())
            {
                if (Spec.bRequired)
                {
                    return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                        FString::Printf(TEXT("required parameter of the '%s' %s is missing and has no safe default. %s"),
                            Kind.Name, KindNoun, Spec.Meaning ? Spec.Meaning : TEXT("")));
                }
                Out.Values.Add(Spec.Name, DefaultValueFor(Spec));
                continue;
            }

            FPwSynthParamValue Parsed;
            Parsed.Type = Spec.Type;

            switch (Spec.Type)
            {
            case EPwSynthParamType::Number:
            {
                if (!AsNumber(Value, Path, Parsed.Number, Err) ||
                    !CheckRange(Parsed.Number, Spec.bHasMin, Spec.Min, Spec.bHasMax, Spec.Max, Path, Spec.Unit, Err))
                {
                    return false;
                }
                break;
            }
            case EPwSynthParamType::Integer:
            {
                if (!AsNumber(Value, Path, Parsed.Number, Err) ||
                    !CheckIntegral(Parsed.Number, Path, Err) ||
                    !CheckRange(Parsed.Number, Spec.bHasMin, Spec.Min, Spec.bHasMax, Spec.Max, Path, Spec.Unit, Err))
                {
                    return false;
                }
                Parsed.Number = FMath::RoundToDouble(Parsed.Number);
                break;
            }
            case EPwSynthParamType::Boolean:
            {
                if (!AsBool(Value, Path, Parsed.bBool, Err))
                {
                    return false;
                }
                break;
            }
            case EPwSynthParamType::String:
            {
                if (!AsString(Value, Path, Parsed.String, Err))
                {
                    return false;
                }
                if (Parsed.String.IsEmpty())
                {
                    return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path, TEXT("must not be empty."));
                }
                break;
            }
            case EPwSynthParamType::Enum:
            {
                FString Token;
                if (!AsString(Value, Path, Token, Err))
                {
                    return false;
                }
                TArray<FString> Accepted;
                SplitEnumValues(Spec.EnumValues, Accepted);
                const int32 Match = Accepted.IndexOfByPredicate([&Token](const FString& Candidate)
                {
                    return Candidate.Equals(Token, ESearchCase::IgnoreCase);
                });
                if (Match == INDEX_NONE)
                {
                    // Never degrade an unrecognised token into the default - that is
                    // how a caller ends up chasing a sound it did not ask for.
                    return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                        FString::Printf(TEXT("'%s' is not a recognised value. Accepted: %s."),
                            *Token, *JoinList(Accepted)));
                }
                Parsed.String = Accepted[Match];
                break;
            }
            case EPwSynthParamType::NumberArray:
            {
                const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
                if (!AsArray(Value, Path, Entries, Err))
                {
                    return false;
                }
                if (Entries->Num() < Spec.MinArrayNum || Entries->Num() > Spec.MaxArrayNum)
                {
                    return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                        FString::Printf(TEXT("has %d entries; accepted range is %d..%d."),
                            Entries->Num(), Spec.MinArrayNum, Spec.MaxArrayNum));
                }
                Parsed.Numbers.Reserve(Entries->Num());
                for (int32 Index = 0; Index < Entries->Num(); ++Index)
                {
                    const FString ElementPath = Indexed(Path, Index);
                    double Element = 0.0;
                    if (!(*Entries)[Index].IsValid() ||
                        !AsNumber((*Entries)[Index], ElementPath, Element, Err) ||
                        !CheckRange(Element, Spec.bHasMin, Spec.Min, Spec.bHasMax, Spec.Max, ElementPath, Spec.Unit, Err))
                    {
                        return false;
                    }
                    Parsed.Numbers.Add(Element);
                }
                break;
            }
            default:
                break;
            }

            Out.Values.Add(Spec.Name, MoveTemp(Parsed));
        }

        if (Kind.Validator)
        {
            FString BadParam;
            FString Message;
            if (!Kind.Validator(Out, BadParam, Message))
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE,
                    BadParam.IsEmpty() ? ParamsPath : JoinStr(ParamsPath, BadParam), Message);
            }
        }

        return true;
    }

    bool ParseGenerator(const TSharedPtr<FJsonObject>& Obj, const FString& Path,
        FPwSynthGenerator& Out, FPwSynthRecipeError& Err)
    {
        static const TArray<FString> Known = { TEXT("kind"), TEXT("params") };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        const FString KindPath = Join(Path, TEXT("kind"));
        TSharedPtr<FJsonValue> KindValue = FindField(Obj, TEXT("kind"));
        if (!KindValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, KindPath,
                TEXT("every layer needs a generator kind; there is no default generator."));
        }
        FString KindName;
        if (!AsString(KindValue, KindPath, KindName, Err))
        {
            return false;
        }
        if (!PwSynthGeneratorKindFromString(KindName, Out.Kind))
        {
            TArray<FString> Names;
            for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthGeneratorKind::Count); ++Index)
            {
                Names.Add(PwSynthGeneratorKindToString(static_cast<EPwSynthGeneratorKind>(Index)));
            }
            return Fail(Err, ErrorCodes::ERR_UNKNOWN_GENERATOR, KindPath,
                FString::Printf(TEXT("'%s' is not a generator kind. Accepted: %s. Call audio.synth.describe_schema with section='generators' for their parameters."),
                    *KindName, *JoinList(Names)));
        }

        TSharedPtr<FJsonObject> ParamsObj;
        if (TSharedPtr<FJsonValue> ParamsValue = FindField(Obj, TEXT("params")))
        {
            if (!AsObject(ParamsValue, Join(Path, TEXT("params")), ParamsObj, Err))
            {
                return false;
            }
        }

        return ParseParams(ParamsObj, PwSynthGeneratorSpec(Out.Kind), TEXT("generator"),
            Join(Path, TEXT("params")), Out.Params, Err);
    }

    bool ParseFx(const TSharedPtr<FJsonObject>& Obj, const FString& Path, bool bMasterChain,
        FPwSynthFx& Out, FPwSynthRecipeError& Err)
    {
        static const TArray<FString> Known = { TEXT("kind"), TEXT("params") };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        const FString KindPath = Join(Path, TEXT("kind"));
        TSharedPtr<FJsonValue> KindValue = FindField(Obj, TEXT("kind"));
        if (!KindValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, KindPath,
                TEXT("every effect entry needs a kind; there is no default effect."));
        }
        FString KindName;
        if (!AsString(KindValue, KindPath, KindName, Err))
        {
            return false;
        }
        if (!PwSynthFxKindFromString(KindName, Out.Kind))
        {
            TArray<FString> Names;
            for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthFxKind::Count); ++Index)
            {
                Names.Add(PwSynthFxKindToString(static_cast<EPwSynthFxKind>(Index)));
            }
            return Fail(Err, ErrorCodes::ERR_UNKNOWN_EFFECT, KindPath,
                FString::Printf(TEXT("'%s' is not an effect kind. Accepted: %s. Call audio.synth.describe_schema with section='effects' for their parameters."),
                    *KindName, *JoinList(Names)));
        }

        const FPwSynthKindSpec& Kind = PwSynthFxSpec(Out.Kind);
        if (Kind.bMasterOnly && !bMasterChain)
        {
            // Layers are mono, so a stereo-bus effect placed in a layer chain would
            // do nothing. Refuse and name the remedy rather than emit a no-op.
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, KindPath,
                FString::Printf(TEXT("the '%s' effect operates on the stereo mix bus, but layers are mono, so it would be a no-op here. Move it to master.fx."),
                    Kind.Name));
        }

        TSharedPtr<FJsonObject> ParamsObj;
        if (TSharedPtr<FJsonValue> ParamsValue = FindField(Obj, TEXT("params")))
        {
            if (!AsObject(ParamsValue, Join(Path, TEXT("params")), ParamsObj, Err))
            {
                return false;
            }
        }

        return ParseParams(ParamsObj, Kind, TEXT("effect"), Join(Path, TEXT("params")), Out.Params, Err);
    }

    bool ParseFxChain(const TSharedPtr<FJsonObject>& Owner, const FString& OwnerPath, bool bMasterChain,
        int32 Cap, TArray<FPwSynthFx>& Out, FPwSynthRecipeError& Err)
    {
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("fx"));
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(OwnerPath, TEXT("fx"));
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!AsArray(Value, Path, Entries, Err))
        {
            return false;
        }
        if (Entries->Num() > Cap)
        {
            // Error, not a clamp: a silently truncated chain does not round-trip.
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                FString::Printf(TEXT("%d effects exceeds the cap of %d for this chain. Split the work across layers or drop an effect."),
                    Entries->Num(), Cap));
        }

        Out.Reserve(Entries->Num());
        for (int32 Index = 0; Index < Entries->Num(); ++Index)
        {
            const FString EntryPath = Indexed(Path, Index);
            TSharedPtr<FJsonObject> EntryObj;
            if (!(*Entries)[Index].IsValid() || !AsObject((*Entries)[Index], EntryPath, EntryObj, Err))
            {
                return false;
            }
            FPwSynthFx Fx;
            if (!ParseFx(EntryObj, EntryPath, bMasterChain, Fx, Err))
            {
                return false;
            }
            Out.Add(MoveTemp(Fx));
        }
        return true;
    }

    bool ParseAmpEnvelope(const TSharedPtr<FJsonObject>& Owner, const FString& OwnerPath, double LayerSpanMs,
        TArray<FPwSynthEnvelopePoint>& Out, FPwSynthRecipeError& Err)
    {
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("ampEnvelope"));
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(OwnerPath, TEXT("ampEnvelope"));
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!AsArray(Value, Path, Entries, Err))
        {
            return false;
        }
        if (Entries->Num() < PwSynthLimits::MinEnvelopePoints || Entries->Num() > PwSynthLimits::MaxEnvelopePoints)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                FString::Printf(TEXT("has %d points; accepted range is %d..%d. Omit the field entirely for no amplitude shaping."),
                    Entries->Num(), PwSynthLimits::MinEnvelopePoints, PwSynthLimits::MaxEnvelopePoints));
        }

        static const TArray<FString> Known = { TEXT("timeMs"), TEXT("value"), TEXT("curve") };
        double PreviousTime = -1.0;
        for (int32 Index = 0; Index < Entries->Num(); ++Index)
        {
            const FString PointPath = Indexed(Path, Index);
            TSharedPtr<FJsonObject> PointObj;
            if (!(*Entries)[Index].IsValid() || !AsObject((*Entries)[Index], PointPath, PointObj, Err) ||
                !RejectUnknownKeys(PointObj, PointPath, Known, Err))
            {
                return false;
            }

            FPwSynthEnvelopePoint Point;
            if (!ReadRequiredNumber(PointObj, TEXT("timeMs"), PointPath, 0.0, PwSynthLimits::MaxDurationMs, TEXT("ms"), Point.TimeMs, Err) ||
                !ReadRequiredNumber(PointObj, TEXT("value"), PointPath, 0.0, 1.0, nullptr, Point.Value, Err))
            {
                return false;
            }
            if (Point.TimeMs < PreviousTime)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(PointPath, TEXT("timeMs")),
                    FString::Printf(TEXT("%g ms is earlier than the previous point at %g ms; envelope points must be ordered."),
                        Point.TimeMs, PreviousTime));
            }
            if (Point.TimeMs > LayerSpanMs)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(PointPath, TEXT("timeMs")),
                    FString::Printf(TEXT("%g ms is past the end of the layer (%g ms). Envelope times are relative to the layer's startMs, not absolute."),
                        Point.TimeMs, LayerSpanMs));
            }
            PreviousTime = Point.TimeMs;

            if (TSharedPtr<FJsonValue> CurveValue = FindField(PointObj, TEXT("curve")))
            {
                const FString CurvePath = Join(PointPath, TEXT("curve"));
                FString CurveName;
                if (!AsString(CurveValue, CurvePath, CurveName, Err))
                {
                    return false;
                }
                if (!PwSynthCurveFromString(CurveName, Point.Curve))
                {
                    TArray<FString> Names;
                    for (uint8 CurveIndex = 0; CurveIndex < static_cast<uint8>(EPwSynthCurve::Count); ++CurveIndex)
                    {
                        Names.Add(PwSynthCurveToString(static_cast<EPwSynthCurve>(CurveIndex)));
                    }
                    return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, CurvePath,
                        FString::Printf(TEXT("'%s' is not a curve. Accepted: %s."), *CurveName, *JoinList(Names)));
                }
            }

            Out.Add(Point);
        }
        return true;
    }

    bool ParsePitchEnvelope(const TSharedPtr<FJsonObject>& Owner, const FString& OwnerPath, double LayerSpanMs,
        TArray<FPwSynthPitchPoint>& Out, FPwSynthRecipeError& Err)
    {
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("pitchEnvelope"));
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(OwnerPath, TEXT("pitchEnvelope"));
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!AsArray(Value, Path, Entries, Err))
        {
            return false;
        }
        if (Entries->Num() < PwSynthLimits::MinEnvelopePoints || Entries->Num() > PwSynthLimits::MaxEnvelopePoints)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                FString::Printf(TEXT("has %d points; accepted range is %d..%d. Omit the field entirely for a constant pitch."),
                    Entries->Num(), PwSynthLimits::MinEnvelopePoints, PwSynthLimits::MaxEnvelopePoints));
        }

        static const TArray<FString> Known = { TEXT("timeMs"), TEXT("semitones") };
        double PreviousTime = -1.0;
        for (int32 Index = 0; Index < Entries->Num(); ++Index)
        {
            const FString PointPath = Indexed(Path, Index);
            TSharedPtr<FJsonObject> PointObj;
            if (!(*Entries)[Index].IsValid() || !AsObject((*Entries)[Index], PointPath, PointObj, Err) ||
                !RejectUnknownKeys(PointObj, PointPath, Known, Err))
            {
                return false;
            }

            FPwSynthPitchPoint Point;
            if (!ReadRequiredNumber(PointObj, TEXT("timeMs"), PointPath, 0.0, PwSynthLimits::MaxDurationMs, TEXT("ms"), Point.TimeMs, Err) ||
                !ReadRequiredNumber(PointObj, TEXT("semitones"), PointPath, -48.0, 48.0, TEXT("semitones"), Point.Semitones, Err))
            {
                return false;
            }
            if (Point.TimeMs < PreviousTime)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(PointPath, TEXT("timeMs")),
                    FString::Printf(TEXT("%g ms is earlier than the previous point at %g ms; envelope points must be ordered."),
                        Point.TimeMs, PreviousTime));
            }
            if (Point.TimeMs > LayerSpanMs)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(PointPath, TEXT("timeMs")),
                    FString::Printf(TEXT("%g ms is past the end of the layer (%g ms). Envelope times are relative to the layer's startMs, not absolute."),
                        Point.TimeMs, LayerSpanMs));
            }
            PreviousTime = Point.TimeMs;

            Out.Add(Point);
        }
        return true;
    }

    bool ParseModulation(const TSharedPtr<FJsonObject>& Owner, const FString& OwnerPath,
        FPwSynthModulation& Out, FPwSynthRecipeError& Err)
    {
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("modulation"));
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(OwnerPath, TEXT("modulation"));
        TSharedPtr<FJsonObject> Obj;
        if (!AsObject(Value, Path, Obj, Err))
        {
            return false;
        }

        TArray<FString> Routings;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthModulationRouting::Count); ++Index)
        {
            Routings.Add(PwSynthModRoutingToString(static_cast<EPwSynthModulationRouting>(Index)));
        }
        if (!RejectUnknownKeys(Obj, Path, Routings, Err))
        {
            return false;
        }
        if (Obj->Values.Num() != 1)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                FString::Printf(TEXT("carries %d routings; exactly one of %s is required. Omit the field entirely for no modulation."),
                    Obj->Values.Num(), *JoinList(Routings)));
        }

        const FString RoutingName(*Obj->Values.CreateConstIterator().Key());
        if (!PwSynthModRoutingFromString(RoutingName, Out.Routing) ||
            Out.Routing == EPwSynthModulationRouting::None)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, JoinStr(Path, RoutingName),
                FString::Printf(TEXT("'%s' is not a modulation routing. Accepted: %s."), *RoutingName, *JoinList(Routings)));
        }

        const FString BlockPath = JoinStr(Path, RoutingName);
        const TSharedPtr<FJsonValue> BlockValue = Obj->Values.CreateConstIterator().Value();
        TSharedPtr<FJsonObject> BlockObj;
        if (!BlockValue.IsValid() || !AsObject(BlockValue, BlockPath, BlockObj, Err))
        {
            return false;
        }

        FPwSynthKindSpec ModKind;
        ModKind.Name = PwSynthModRoutingToString(Out.Routing);
        ModKind.Summary = TEXT("");
        ModKind.Params = &PwSynthModulationParamSpecs();

        FPwSynthParams Params;
        if (!ParseParams(BlockObj, ModKind, TEXT("modulation"), BlockPath, Params, Err))
        {
            return false;
        }

        Out.Depth = Params.GetNumber(FName(TEXT("depth")));
        Out.RateHz = Params.GetNumber(FName(TEXT("rateHz")));
        const FString SourceName = Params.GetString(FName(TEXT("source")));
        if (!PwSynthModSourceFromString(SourceName, Out.Source))
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Join(BlockPath, TEXT("source")),
                FString::Printf(TEXT("'%s' is not a modulator source."), *SourceName));
        }
        return true;
    }

    bool ParseLayer(const TSharedPtr<FJsonObject>& Obj, const FString& Path, double DurationMs,
        FPwSynthLayer& Out, FPwSynthRecipeError& Err)
    {
        static const TArray<FString> Known = {
            TEXT("startMs"), TEXT("gainDb"), TEXT("pan"), TEXT("generator"),
            TEXT("ampEnvelope"), TEXT("pitchEnvelope"), TEXT("modulation"), TEXT("fx")
        };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        if (!ReadOptionalNumber(Obj, TEXT("startMs"), Path, 0.0, PwSynthLimits::MaxDurationMs,
                PwSynthLimits::DefaultLayerStartMs, TEXT("ms"), Out.StartMs, Err) ||
            !ReadOptionalNumber(Obj, TEXT("gainDb"), Path, PwSynthLimits::MinGainDb, PwSynthLimits::MaxGainDb,
                PwSynthLimits::DefaultLayerGainDb, TEXT("db"), Out.GainDb, Err) ||
            !ReadOptionalNumber(Obj, TEXT("pan"), Path, -1.0, 1.0,
                PwSynthLimits::DefaultLayerPan, nullptr, Out.Pan, Err))
        {
            return false;
        }
        if (Out.StartMs >= DurationMs)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(Path, TEXT("startMs")),
                FString::Printf(TEXT("%g ms starts at or after the recipe's durationMs of %g ms, so the layer would never sound."),
                    Out.StartMs, DurationMs));
        }

        const FString GeneratorPath = Join(Path, TEXT("generator"));
        TSharedPtr<FJsonValue> GeneratorValue = FindField(Obj, TEXT("generator"));
        if (!GeneratorValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, GeneratorPath,
                TEXT("every layer needs a generator; there is no default source."));
        }
        TSharedPtr<FJsonObject> GeneratorObj;
        if (!AsObject(GeneratorValue, GeneratorPath, GeneratorObj, Err) ||
            !ParseGenerator(GeneratorObj, GeneratorPath, Out.Generator, Err))
        {
            return false;
        }

        const double LayerSpanMs = DurationMs - Out.StartMs;
        return ParseAmpEnvelope(Obj, Path, LayerSpanMs, Out.AmpEnvelope, Err) &&
            ParsePitchEnvelope(Obj, Path, LayerSpanMs, Out.PitchEnvelope, Err) &&
            ParseModulation(Obj, Path, Out.Modulation, Err) &&
            ParseFxChain(Obj, Path, /*bMasterChain=*/false, PwSynthLimits::MaxLayerFx, Out.Fx, Err);
    }

    bool ParseNormalize(const TSharedPtr<FJsonObject>& Owner, const FString& OwnerPath,
        FPwSynthNormalize& Out, FPwSynthRecipeError& Err)
    {
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("normalize"));
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(OwnerPath, TEXT("normalize"));
        TSharedPtr<FJsonObject> Obj;
        if (!AsObject(Value, Path, Obj, Err))
        {
            return false;
        }

        static const TArray<FString> Known = { TEXT("mode"), TEXT("target") };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        const FString ModePath = Join(Path, TEXT("mode"));
        TSharedPtr<FJsonValue> ModeValue = FindField(Obj, TEXT("mode"));
        if (!ModeValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, ModePath,
                TEXT("required when a normalize block is present. Accepted: peak, lufs."));
        }
        FString ModeName;
        if (!AsString(ModeValue, ModePath, ModeName, Err))
        {
            return false;
        }
        if (!PwSynthNormalizeModeFromString(ModeName, Out.Mode) || Out.Mode == EPwSynthNormalizeMode::None)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, ModePath,
                FString::Printf(TEXT("'%s' is not a normalize mode. Accepted: peak, lufs. Omit the normalize block entirely for no normalization."),
                    *ModeName));
        }

        // -1 dBFS and -16 LUFS are not interchangeable, so there is no safe default.
        const double MinTarget = (Out.Mode == EPwSynthNormalizeMode::Lufs) ? -70.0 : -96.0;
        const TCHAR* Unit = (Out.Mode == EPwSynthNormalizeMode::Lufs) ? TEXT("lufs") : TEXT("dbfs");
        return ReadRequiredNumber(Obj, TEXT("target"), Path, MinTarget, 0.0, Unit, Out.Target, Err);
    }

    bool ParseMaster(const TSharedPtr<FJsonObject>& Owner, const FString& OwnerPath, double DurationMs,
        FPwSynthMaster& Out, FPwSynthRecipeError& Err)
    {
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("master"));
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(OwnerPath, TEXT("master"));
        TSharedPtr<FJsonObject> Obj;
        if (!AsObject(Value, Path, Obj, Err))
        {
            return false;
        }

        static const TArray<FString> Known = {
            TEXT("fx"), TEXT("normalize"), TEXT("fadeInMs"), TEXT("fadeOutMs")
        };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        if (!ParseFxChain(Obj, Path, /*bMasterChain=*/true, PwSynthLimits::MaxMasterFx, Out.Fx, Err) ||
            !ParseNormalize(Obj, Path, Out.Normalize, Err) ||
            !ReadOptionalNumber(Obj, TEXT("fadeInMs"), Path, 0.0, PwSynthLimits::MaxDurationMs,
                PwSynthLimits::DefaultFadeMs, TEXT("ms"), Out.FadeInMs, Err) ||
            !ReadOptionalNumber(Obj, TEXT("fadeOutMs"), Path, 0.0, PwSynthLimits::MaxDurationMs,
                PwSynthLimits::DefaultFadeMs, TEXT("ms"), Out.FadeOutMs, Err))
        {
            return false;
        }

        if (Out.FadeInMs + Out.FadeOutMs > DurationMs)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(Path, TEXT("fadeOutMs")),
                FString::Printf(TEXT("fadeInMs %g + fadeOutMs %g exceeds the recipe's durationMs of %g ms."),
                    Out.FadeInMs, Out.FadeOutMs, DurationMs));
        }
        return true;
    }

    bool ParseTargets(const TSharedPtr<FJsonObject>& Owner, const FString& OwnerPath,
        TArray<FPwSynthTargetRange>& Out, FPwSynthRecipeError& Err)
    {
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("targets"));
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(OwnerPath, TEXT("targets"));
        TSharedPtr<FJsonObject> Obj;
        if (!AsObject(Value, Path, Obj, Err))
        {
            return false;
        }

        static const TArray<FString> Known = { TEXT("min"), TEXT("max") };
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Obj->Values)
        {
            const FString MetricPath = JoinStr(Path, Pair.Key);
            FPwSynthTargetRange Range;
            if (!PwSynthMetricFromString(Pair.Key, Range.Metric))
            {
                TArray<FString> Names;
                for (uint8 Index = 0; Index < static_cast<uint8>(EPwSynthMetric::Count); ++Index)
                {
                    Names.Add(PwSynthMetricToString(static_cast<EPwSynthMetric>(Index)));
                }
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, MetricPath,
                    FString::Printf(TEXT("'%s' is not a measurable metric. Accepted: %s."), *Pair.Key, *JoinList(Names)));
            }

            TSharedPtr<FJsonObject> RangeObj;
            if (!Pair.Value.IsValid() || !AsObject(Pair.Value, MetricPath, RangeObj, Err) ||
                !RejectUnknownKeys(RangeObj, MetricPath, Known, Err))
            {
                return false;
            }

            if (TSharedPtr<FJsonValue> MinValue = FindField(RangeObj, TEXT("min")))
            {
                double Number = 0.0;
                if (!AsNumber(MinValue, Join(MetricPath, TEXT("min")), Number, Err))
                {
                    return false;
                }
                Range.Min = Number;
            }
            if (TSharedPtr<FJsonValue> MaxValue = FindField(RangeObj, TEXT("max")))
            {
                double Number = 0.0;
                if (!AsNumber(MaxValue, Join(MetricPath, TEXT("max")), Number, Err))
                {
                    return false;
                }
                Range.Max = Number;
            }
            if (!Range.Min.IsSet() && !Range.Max.IsSet())
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, MetricPath,
                    TEXT("needs at least one of 'min' or 'max'; an empty range constrains nothing."));
            }
            if (Range.Min.IsSet() && Range.Max.IsSet() && Range.Min.GetValue() > Range.Max.GetValue())
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, MetricPath,
                    FString::Printf(TEXT("min %g is above max %g."), Range.Min.GetValue(), Range.Max.GetValue()));
            }

            Out.Add(MoveTemp(Range));
        }
        return true;
    }
}

bool ParseSynthRecipe(const TSharedPtr<FJsonObject>& In, FPwSynthRecipe& Out, FPwSynthRecipeError& OutError)
{
    using namespace PwSynthParseInternal;

    OutError.Reset();

    if (!In.IsValid())
    {
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("recipe"),
            TEXT("expected a recipe object. Call audio.synth.describe_schema for the grammar."));
    }

    // Everything lands in a local until the whole recipe validates, so a failed
    // parse can never leave a half-built recipe observable (rpc-design §1).
    FPwSynthRecipe Work;

    static const TArray<FString> Known = {
        TEXT("version"), TEXT("seed"), TEXT("sampleRate"), TEXT("durationMs"),
        TEXT("layers"), TEXT("master"), TEXT("targets")
    };
    if (!RejectUnknownKeys(In, FString(), Known, OutError))
    {
        return false;
    }

    if (!ReadOptionalInt(In, TEXT("version"), FString(), 0.0, 1000.0,
            PwSynthLimits::RecipeVersion, Work.Version, OutError))
    {
        return false;
    }
    if (Work.Version != PwSynthLimits::RecipeVersion)
    {
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("version"),
            FString::Printf(TEXT("recipe version %d is not supported; this build speaks version %d."),
                Work.Version, PwSynthLimits::RecipeVersion));
    }

    if (!ReadOptionalInt(In, TEXT("seed"), FString(), 0.0, static_cast<double>(MAX_int32),
            PwSynthLimits::DefaultSeed, Work.Seed, OutError) ||
        !ReadOptionalInt(In, TEXT("sampleRate"), FString(),
            PwSynthLimits::MinSampleRate, PwSynthLimits::MaxSampleRate,
            PwSynthLimits::DefaultSampleRate, Work.SampleRate, OutError) ||
        !ReadRequiredNumber(In, TEXT("durationMs"), FString(),
            PwSynthLimits::MinDurationMs, PwSynthLimits::MaxDurationMs, TEXT("ms"), Work.DurationMs, OutError))
    {
        return false;
    }

    TSharedPtr<FJsonValue> LayersValue = FindField(In, TEXT("layers"));
    if (!LayersValue.IsValid())
    {
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers"),
            TEXT("a recipe needs at least one layer."));
    }
    const TArray<TSharedPtr<FJsonValue>>* LayerEntries = nullptr;
    if (!AsArray(LayersValue, TEXT("layers"), LayerEntries, OutError))
    {
        return false;
    }
    if (LayerEntries->Num() == 0)
    {
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers"),
            TEXT("a recipe needs at least one layer."));
    }
    if (LayerEntries->Num() > PwSynthLimits::MaxLayers)
    {
        // Error, not a clamp: a silently truncated recipe does not round-trip.
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers"),
            FString::Printf(TEXT("%d layers exceeds the cap of %d. Render the extra layers as a second recipe and mix them, or fold them into fewer layers."),
                LayerEntries->Num(), PwSynthLimits::MaxLayers));
    }

    Work.Layers.Reserve(LayerEntries->Num());
    for (int32 Index = 0; Index < LayerEntries->Num(); ++Index)
    {
        const FString LayerPath = Indexed(TEXT("layers"), Index);
        TSharedPtr<FJsonObject> LayerObj;
        if (!(*LayerEntries)[Index].IsValid() || !AsObject((*LayerEntries)[Index], LayerPath, LayerObj, OutError))
        {
            return false;
        }
        FPwSynthLayer Layer;
        if (!ParseLayer(LayerObj, LayerPath, Work.DurationMs, Layer, OutError))
        {
            return false;
        }
        Work.Layers.Add(MoveTemp(Layer));
    }

    if (!ParseMaster(In, FString(), Work.DurationMs, Work.Master, OutError) ||
        !ParseTargets(In, FString(), Work.Targets, OutError))
    {
        return false;
    }

    Out = MoveTemp(Work);
    return true;
}

bool ParseSynthRecipe(const TSharedPtr<FJsonObject>& In, FPwSynthRecipe& Out, FString& OutError)
{
    FPwSynthRecipeError Error;
    if (ParseSynthRecipe(In, Out, Error))
    {
        OutError.Reset();
        return true;
    }
    OutError = Error.ToString();
    return false;
}

// ===========================================================================
// Serializing. Emission walks the spec tables, never the value bag, so keys use
// the canonical spelling in the canonical order regardless of how the caller
// spelled them - which is what makes the round-trip byte-stable.
// ===========================================================================
namespace PwSynthSerializeInternal
{
    TSharedPtr<FJsonObject> SerializeParams(const FPwSynthParams& Params, const FPwSynthKindSpec& Kind)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        for (const FPwSynthParamSpec& Spec : *Kind.Params)
        {
            const FPwSynthParamValue* Value = Params.Values.Find(Spec.Name);
            if (!Value)
            {
                // A hand-built recipe that skipped a row: emit nothing rather than a
                // fabricated value that would read as something the caller asked for.
                continue;
            }
            switch (Spec.Type)
            {
            case EPwSynthParamType::Boolean:
                Obj->SetBoolField(Spec.DisplayName, Value->bBool);
                break;
            case EPwSynthParamType::String:
            case EPwSynthParamType::Enum:
                Obj->SetStringField(Spec.DisplayName, Value->String);
                break;
            case EPwSynthParamType::NumberArray:
            {
                TArray<TSharedPtr<FJsonValue>> Entries;
                Entries.Reserve(Value->Numbers.Num());
                for (double Number : Value->Numbers)
                {
                    Entries.Add(MakeShared<FJsonValueNumber>(Number));
                }
                Obj->SetArrayField(Spec.DisplayName, Entries);
                break;
            }
            default:
                Obj->SetNumberField(Spec.DisplayName, Value->Number);
                break;
            }
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> SerializeFx(const FPwSynthFx& Fx)
    {
        const FPwSynthKindSpec& Kind = PwSynthFxSpec(Fx.Kind);
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("kind"), Kind.Name);
        Obj->SetObjectField(TEXT("params"), SerializeParams(Fx.Params, Kind));
        return Obj;
    }

    TSharedPtr<FJsonObject> SerializeLayer(const FPwSynthLayer& Layer)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("startMs"), Layer.StartMs);
        Obj->SetNumberField(TEXT("gainDb"), Layer.GainDb);
        Obj->SetNumberField(TEXT("pan"), Layer.Pan);

        const FPwSynthKindSpec& GeneratorKind = PwSynthGeneratorSpec(Layer.Generator.Kind);
        TSharedPtr<FJsonObject> GeneratorObj = MakeShared<FJsonObject>();
        GeneratorObj->SetStringField(TEXT("kind"), GeneratorKind.Name);
        GeneratorObj->SetObjectField(TEXT("params"), SerializeParams(Layer.Generator.Params, GeneratorKind));
        Obj->SetObjectField(TEXT("generator"), GeneratorObj);

        if (Layer.AmpEnvelope.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Points;
            Points.Reserve(Layer.AmpEnvelope.Num());
            for (const FPwSynthEnvelopePoint& Point : Layer.AmpEnvelope)
            {
                TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
                PointObj->SetNumberField(TEXT("timeMs"), Point.TimeMs);
                PointObj->SetNumberField(TEXT("value"), Point.Value);
                PointObj->SetStringField(TEXT("curve"), PwSynthCurveToString(Point.Curve));
                Points.Add(MakeShared<FJsonValueObject>(PointObj));
            }
            Obj->SetArrayField(TEXT("ampEnvelope"), Points);
        }

        if (Layer.PitchEnvelope.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Points;
            Points.Reserve(Layer.PitchEnvelope.Num());
            for (const FPwSynthPitchPoint& Point : Layer.PitchEnvelope)
            {
                TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
                PointObj->SetNumberField(TEXT("timeMs"), Point.TimeMs);
                PointObj->SetNumberField(TEXT("semitones"), Point.Semitones);
                Points.Add(MakeShared<FJsonValueObject>(PointObj));
            }
            Obj->SetArrayField(TEXT("pitchEnvelope"), Points);
        }

        if (Layer.Modulation.IsSet())
        {
            TSharedPtr<FJsonObject> BlockObj = MakeShared<FJsonObject>();
            BlockObj->SetNumberField(TEXT("depth"), Layer.Modulation.Depth);
            BlockObj->SetNumberField(TEXT("rateHz"), Layer.Modulation.RateHz);
            BlockObj->SetStringField(TEXT("source"), PwSynthModSourceToString(Layer.Modulation.Source));

            TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
            ModObj->SetObjectField(PwSynthModRoutingToString(Layer.Modulation.Routing), BlockObj);
            Obj->SetObjectField(TEXT("modulation"), ModObj);
        }

        if (Layer.Fx.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Entries;
            Entries.Reserve(Layer.Fx.Num());
            for (const FPwSynthFx& Fx : Layer.Fx)
            {
                Entries.Add(MakeShared<FJsonValueObject>(SerializeFx(Fx)));
            }
            Obj->SetArrayField(TEXT("fx"), Entries);
        }

        return Obj;
    }
}

TSharedPtr<FJsonObject> SerializeSynthRecipe(const FPwSynthRecipe& In)
{
    using namespace PwSynthSerializeInternal;

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("version"), In.Version);
    Root->SetNumberField(TEXT("seed"), In.Seed);
    Root->SetNumberField(TEXT("sampleRate"), In.SampleRate);
    Root->SetNumberField(TEXT("durationMs"), In.DurationMs);

    TArray<TSharedPtr<FJsonValue>> LayerEntries;
    LayerEntries.Reserve(In.Layers.Num());
    for (const FPwSynthLayer& Layer : In.Layers)
    {
        LayerEntries.Add(MakeShared<FJsonValueObject>(SerializeLayer(Layer)));
    }
    Root->SetArrayField(TEXT("layers"), LayerEntries);

    // The master block always round-trips, because fadeInMs / fadeOutMs are
    // defaults made explicit rather than omitted state.
    TSharedPtr<FJsonObject> MasterObj = MakeShared<FJsonObject>();
    if (In.Master.Fx.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Reserve(In.Master.Fx.Num());
        for (const FPwSynthFx& Fx : In.Master.Fx)
        {
            Entries.Add(MakeShared<FJsonValueObject>(SerializeFx(Fx)));
        }
        MasterObj->SetArrayField(TEXT("fx"), Entries);
    }
    if (In.Master.Normalize.IsSet())
    {
        TSharedPtr<FJsonObject> NormalizeObj = MakeShared<FJsonObject>();
        NormalizeObj->SetStringField(TEXT("mode"), PwSynthNormalizeModeToString(In.Master.Normalize.Mode));
        NormalizeObj->SetNumberField(TEXT("target"), In.Master.Normalize.Target);
        MasterObj->SetObjectField(TEXT("normalize"), NormalizeObj);
    }
    MasterObj->SetNumberField(TEXT("fadeInMs"), In.Master.FadeInMs);
    MasterObj->SetNumberField(TEXT("fadeOutMs"), In.Master.FadeOutMs);
    Root->SetObjectField(TEXT("master"), MasterObj);

    if (In.Targets.Num() > 0)
    {
        TSharedPtr<FJsonObject> TargetsObj = MakeShared<FJsonObject>();
        for (const FPwSynthTargetRange& Range : In.Targets)
        {
            TSharedPtr<FJsonObject> RangeObj = MakeShared<FJsonObject>();
            if (Range.Min.IsSet())
            {
                RangeObj->SetNumberField(TEXT("min"), Range.Min.GetValue());
            }
            if (Range.Max.IsSet())
            {
                RangeObj->SetNumberField(TEXT("max"), Range.Max.GetValue());
            }
            TargetsObj->SetObjectField(PwSynthMetricToString(Range.Metric), RangeObj);
        }
        Root->SetObjectField(TEXT("targets"), TargetsObj);
    }

    return Root;
}

// ===========================================================================
// Parameter-bag reuse seam. Thin forwarders, deliberately not reimplementations:
// AudioGen/PwMusicScore.cpp validates its generation-mode blocks through the same
// table walker the generators and effects use, so a range check or a closed
// vocabulary cannot mean two different things in the two schemas.
// ===========================================================================

bool ParseSynthParamBag(const TSharedPtr<FJsonObject>& ParamsObj, const FPwSynthKindSpec& Kind,
    const TCHAR* KindNoun, const FString& ParamsPath, FPwSynthParams& Out, FPwSynthRecipeError& OutError)
{
    return PwSynthParseInternal::ParseParams(ParamsObj, Kind, KindNoun, ParamsPath, Out, OutError);
}

TSharedPtr<FJsonObject> SerializeSynthParamBag(const FPwSynthParams& In, const FPwSynthKindSpec& Kind)
{
    return PwSynthSerializeInternal::SerializeParams(In, Kind);
}
