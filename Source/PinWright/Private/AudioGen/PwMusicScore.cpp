// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwMusicScore.cpp - generation-mode schema tables, strict parse / lossless serialize,
// the one beat<->time conversion, the loop-length measurement, and the deterministic
// note materializer.
//
// Two reuse seams are load-bearing here and are the reason this file is a schema and not
// a second synth:
//   1. `instrument` is parsed and serialized by wrapping it as a ONE-LAYER FPwSynthRecipe
//      and calling ParseSynthRecipe / SerializeSynthRecipe. Every generator, effect,
//      envelope and modulation rule the SFX side gains is instantly an instrument here,
//      validated by the same code, with no edit in this file.
//   2. Generation parameters go through ParseSynthParamBag / SerializeSynthParamBag
//      (the seam at the end of PwSynthRecipe.cpp), so a range, a default or a closed
//      vocabulary is enforced by one implementation for both documents.
//
// Error-code split, published verbatim so a caller can route on the code:
//   INVALID_PARAMS  - a VALUE problem: missing required value, wrong JSON type, out of
//                     range, outside a closed vocabulary, or a pitch that does not resolve
//   INVALID_RECIPE  - a SHAPE problem: unknown key, cap exceeded, missing container,
//                     ordering violation, duplicate name, section tiling gap, cross-field
//                     contradiction
//   UNKNOWN_GENERATOR / UNKNOWN_EFFECT - forwarded unchanged out of the instrument

#include "AudioGen/PwMusicScore.h"

#include "AudioGen/PwSeededRandom.h"
#include "Containers/ArrayView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/JsonUtils.h"

#include "DSP/Dsp.h"
#include "DSP/MidiNoteQuantizer.h"

// ===========================================================================
// Vocabulary + spec tables. Named (not anonymous) namespace with a PwMusic prefix so a
// Unity merge cannot collide these with another translation unit's helpers.
// ===========================================================================
namespace PwMusicSpecInternal
{
    // ---- pitch classes ----------------------------------------------------

    struct FPitchClassRow
    {
        const TCHAR* Name;
        int32 Semitone;
    };

    const TArray<FPitchClassRow>& PitchClassRows()
    {
        static const TArray<FPitchClassRow> Rows = {
            { TEXT("c"),  0 },
            { TEXT("c#"), 1 },
            { TEXT("d"),  2 },
            { TEXT("d#"), 3 },
            { TEXT("e"),  4 },
            { TEXT("f"),  5 },
            { TEXT("f#"), 6 },
            { TEXT("g"),  7 },
            { TEXT("g#"), 8 },
            { TEXT("a"),  9 },
            { TEXT("a#"), 10 },
            { TEXT("b"),  11 }
        };
        return Rows;
    }

    // ---- scales -----------------------------------------------------------

    struct FScaleRow
    {
        const TCHAR* Name;
        Audio::EMusicalScale::Scale Engine;
    };

    // Order matches EPwMusicScale's declaration order, which matches the engine's.
    // A row added to Audio::EMusicalScale::Scale is a row added here and an enumerator
    // added to EPwMusicScale - the three are checked against each other by
    // PinWright.audio.music.score.PitchResolution, which asserts the counts match and that
    // every scale resolves to a non-empty degree set in the engine's table.
    const TArray<FScaleRow>& ScaleRows()
    {
        static const TArray<FScaleRow> Rows = {
            { TEXT("major"),                            Audio::EMusicalScale::Major },
            { TEXT("minor_dorian"),                     Audio::EMusicalScale::Minor_Dorian },
            { TEXT("phrygian"),                         Audio::EMusicalScale::Phrygian },
            { TEXT("lydian"),                           Audio::EMusicalScale::Lydian },
            { TEXT("dominant7th_mixolydian"),           Audio::EMusicalScale::Dominant7th_Mixolydian },
            { TEXT("natural_minor_aeolian"),            Audio::EMusicalScale::NaturalMinor_Aeolian },
            { TEXT("half_diminished_locrian"),          Audio::EMusicalScale::HalfDiminished_Locrian },
            { TEXT("chromatic"),                        Audio::EMusicalScale::Chromatic },
            { TEXT("whole_tone"),                       Audio::EMusicalScale::WholeTone },
            { TEXT("diminished_whole_tone"),            Audio::EMusicalScale::DiminishedWholeTone },
            { TEXT("major_pentatonic"),                 Audio::EMusicalScale::MajorPentatonic },
            { TEXT("minor_pentatonic"),                 Audio::EMusicalScale::MinorPentatonic },
            { TEXT("blues"),                            Audio::EMusicalScale::Blues },
            { TEXT("bebop_major"),                      Audio::EMusicalScale::Bebop_Major },
            { TEXT("bebop_minor"),                      Audio::EMusicalScale::Bebop_Minor },
            { TEXT("bebop_minor_2"),                    Audio::EMusicalScale::Bebop_MinorNumber2 },
            { TEXT("bebop_dominant"),                   Audio::EMusicalScale::Bebop_Dominant },
            { TEXT("harmonic_major"),                   Audio::EMusicalScale::HarmonicMajor },
            { TEXT("harmonic_minor"),                   Audio::EMusicalScale::HarmonicMinor },
            { TEXT("melodic_minor"),                    Audio::EMusicalScale::MelodicMinor },
            { TEXT("sixth_mode_of_harmonic_minor"),     Audio::EMusicalScale::SixthModeOfHarmonicMinor },
            { TEXT("lydian_augmented"),                 Audio::EMusicalScale::LydianAugmented },
            { TEXT("lydian_dominant"),                  Audio::EMusicalScale::LydianDominant },
            { TEXT("augmented"),                        Audio::EMusicalScale::Augmented },
            { TEXT("diminished"),                       Audio::EMusicalScale::Diminished },
            { TEXT("diminished_begin_with_half_step"),  Audio::EMusicalScale::Diminished_BeginWithHalfStep },
            { TEXT("diminished_begin_with_whole_step"), Audio::EMusicalScale::Diminished_BeginWithWholeStep },
            { TEXT("half_diminished_locrian_2"),        Audio::EMusicalScale::HalfDiminished_LocrianNumber2 },
            { TEXT("spanish_or_jewish"),                Audio::EMusicalScale::Spanish_or_Jewish },
            { TEXT("hindu"),                            Audio::EMusicalScale::Hindu }
        };
        return Rows;
    }

    // ---- roles ------------------------------------------------------------

    struct FRoleRow
    {
        const TCHAR* Name;
        const TCHAR* Meaning;
    };

    const TArray<FRoleRow>& RoleRows()
    {
        static const TArray<FRoleRow> Rows = {
            { TEXT("drone"),      TEXT("Sustained bed with no articulation. The floor of an ambient mix.") },
            { TEXT("pad"),        TEXT("Slow harmonic bed. Overlapping swells that move without drawing attention.") },
            { TEXT("bass"),       TEXT("Low-register foundation.") },
            { TEXT("lead"),       TEXT("Foreground melodic line.") },
            { TEXT("texture"),    TEXT("Noise or atmosphere - wind, hiss, granular debris.") },
            { TEXT("percussion"), TEXT("Rhythmic, usually unpitched.") },
            { TEXT("fx"),         TEXT("Transitional accents: risers, impacts, stingers.") }
        };
        return Rows;
    }

    // ---- generation-mode parameter tables ---------------------------------
    // Same builder shapes as PwSynthSpecInternal's, deliberately: a row here is read by
    // the same table walker, so it means exactly what a generator's row means.

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

    FPwSynthParamSpec DefInt(const TCHAR* Name, const TCHAR* Unit, double Min, double Max, double Default, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = DefNum(Name, Unit, Min, Max, Default, Meaning);
        Spec.Type = EPwSynthParamType::Integer;
        return Spec;
    }

    FPwSynthParamSpec ReqDegreeArray(int32 MaxNum, const TCHAR* Meaning)
    {
        FPwSynthParamSpec Spec = ReqNum(TEXT("degrees"), nullptr,
            static_cast<double>(PwMusicLimits::MinDegree), static_cast<double>(PwMusicLimits::MaxDegree), Meaning);
        Spec.Type = EPwSynthParamType::NumberArray;
        Spec.MinArrayNum = 1;
        Spec.MaxArrayNum = MaxNum;
        return Spec;
    }

    // The bag's per-row checks cannot say "whole number" about an ARRAY element (only the
    // Integer scalar type carries that), so degrees are checked here. A fractional degree
    // has no meaning - degrees index a scale, they do not interpolate between its notes.
    bool ValidateDegreesIntegral(const FPwSynthParams& Params, FString& OutParamName, FString& OutMessage)
    {
        const TArray<double>* Degrees = Params.GetNumbers(FName(TEXT("degrees")));
        if (!Degrees)
        {
            return true;    // a missing array already failed the per-row check
        }
        for (int32 Index = 0; Index < Degrees->Num(); ++Index)
        {
            const double Value = (*Degrees)[Index];
            if (!FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
            {
                OutParamName = FString::Printf(TEXT("degrees[%d]"), Index);
                OutMessage = FString::Printf(
                    TEXT("%g is not a whole number; a scale degree indexes the scale and does not interpolate"), Value);
                return false;
            }
        }
        return true;
    }

    bool ValidateOrderedPair(const FPwSynthParams& Params, const TCHAR* MinKey, const TCHAR* MaxKey,
        FString& OutParamName, FString& OutMessage)
    {
        const double Min = Params.GetNumber(FName(MinKey));
        const double Max = Params.GetNumber(FName(MaxKey));
        if (Min > Max)
        {
            OutParamName = MaxKey;
            OutMessage = FString::Printf(TEXT("%g is below %s (%g); the range would be empty"), Max, MinKey, Min);
            return false;
        }
        return true;
    }

    bool ValidateDrone(const FPwSynthParams& Params, FString& OutParamName, FString& OutMessage)
    {
        return ValidateDegreesIntegral(Params, OutParamName, OutMessage);
    }

    bool ValidateEvolvingPad(const FPwSynthParams& Params, FString& OutParamName, FString& OutMessage)
    {
        return ValidateDegreesIntegral(Params, OutParamName, OutMessage) &&
            ValidateOrderedPair(Params, TEXT("swellBeatsMin"), TEXT("swellBeatsMax"), OutParamName, OutMessage) &&
            ValidateOrderedPair(Params, TEXT("velocityMin"), TEXT("velocityMax"), OutParamName, OutMessage);
    }

    bool ValidateSparseEvents(const FPwSynthParams& Params, FString& OutParamName, FString& OutMessage)
    {
        return ValidateDegreesIntegral(Params, OutParamName, OutMessage) &&
            ValidateOrderedPair(Params, TEXT("durationBeatsMin"), TEXT("durationBeatsMax"), OutParamName, OutMessage) &&
            ValidateOrderedPair(Params, TEXT("velocityMin"), TEXT("velocityMax"), OutParamName, OutMessage);
    }

    const TArray<FPwSynthParamSpec>& ExplicitParams()
    {
        static const TArray<FPwSynthParamSpec> Table;
        return Table;
    }

    const TArray<FPwSynthParamSpec>& DroneParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqDegreeArray(8, TEXT("Scale degrees held for the whole section, sounded together. One entry is a single-note drone; several are a sustained chord.")),
            DefNum(TEXT("velocity"), nullptr, 0.0, 1.0, 1.0,
                TEXT("Velocity of every held note, before the section's intensity band remaps it."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& EvolvingPadParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqDegreeArray(16, TEXT("Pool of scale degrees each swell draws from.")),
            ReqNum(TEXT("swellBeatsMin"), TEXT("beats"), 0.25, PwMusicLimits::MaxBeats,
                TEXT("Shortest swell length.")),
            ReqNum(TEXT("swellBeatsMax"), TEXT("beats"), 0.25, PwMusicLimits::MaxBeats,
                TEXT("Longest swell length. Must be at or above swellBeatsMin.")),
            DefNum(TEXT("overlap"), nullptr, 0.0, 0.95, 0.5,
                TEXT("Fraction of a swell the next one starts inside. 0 lays swells end to end; 0.5 starts the next halfway through. The stride is swell * (1 - overlap).")),
            DefNum(TEXT("velocityMin"), nullptr, 0.0, 1.0, 0.35,
                TEXT("Quietest swell, before the section's intensity band remaps it.")),
            DefNum(TEXT("velocityMax"), nullptr, 0.0, 1.0, 0.85,
                TEXT("Loudest swell. Must be at or above velocityMin.")),
            DefInt(TEXT("octaveSpread"), TEXT("octaves"), 0.0, 3.0, 0.0,
                TEXT("Random octave displacement applied to each swell, +/- this many octaves. 0 keeps every swell in the track's own register."))
        };
        return Table;
    }

    const TArray<FPwSynthParamSpec>& SparseEventsParams()
    {
        static const TArray<FPwSynthParamSpec> Table = {
            ReqDegreeArray(16, TEXT("Pool of scale degrees each event draws from.")),
            ReqNum(TEXT("eventsPerBar"), nullptr, 0.05, 16.0,
                TEXT("Expected events per bar. Fractional densities are honoured: 0.25 means roughly one event every four bars, drawn from the track's seeded stream.")),
            ReqNum(TEXT("durationBeatsMin"), TEXT("beats"), 0.125, PwMusicLimits::MaxBeats,
                TEXT("Shortest event length.")),
            ReqNum(TEXT("durationBeatsMax"), TEXT("beats"), 0.125, PwMusicLimits::MaxBeats,
                TEXT("Longest event length. Must be at or above durationBeatsMin.")),
            DefNum(TEXT("velocityMin"), nullptr, 0.0, 1.0, 0.25,
                TEXT("Quietest event, before the section's intensity band remaps it.")),
            DefNum(TEXT("velocityMax"), nullptr, 0.0, 1.0, 0.8,
                TEXT("Loudest event. Must be at or above velocityMin.")),
            DefInt(TEXT("octaveSpread"), TEXT("octaves"), 0.0, 3.0, 0.0,
                TEXT("Random octave displacement applied to each event, +/- this many octaves."))
        };
        return Table;
    }

    const TArray<FPwSynthKindSpec>& GenerationSpecs()
    {
        static const TArray<FPwSynthKindSpec> Specs = []()
        {
            TArray<FPwSynthKindSpec> Built;
            Built.SetNum(static_cast<int32>(EPwMusicGenerationMode::Count));

            FPwSynthKindSpec& Unspecified = Built[static_cast<int32>(EPwMusicGenerationMode::Unspecified)];
            Unspecified.Name = TEXT("unspecified");
            Unspecified.Summary = TEXT("No mode. A track must name one.");
            Unspecified.Params = &ExplicitParams();

            FPwSynthKindSpec& Explicit = Built[static_cast<int32>(EPwMusicGenerationMode::Explicit)];
            Explicit.Name = TEXT("explicit");
            Explicit.Summary = TEXT("The track's notes[] array is authoritative. Nothing is generated and this mode takes no parameters.");
            Explicit.Params = &ExplicitParams();

            FPwSynthKindSpec& Drone = Built[static_cast<int32>(EPwMusicGenerationMode::Drone)];
            Drone.Name = TEXT("drone");
            Drone.Summary = TEXT("One sustained chord per section, spanning that section exactly. Draws no randomness, so it is identical for every seed.");
            Drone.Params = &DroneParams();
            Drone.Validator = &ValidateDrone;
            Drone.Constraint = TEXT("Every entry of degrees must be a whole number.");

            FPwSynthKindSpec& Pad = Built[static_cast<int32>(EPwMusicGenerationMode::EvolvingPad)];
            Pad.Name = TEXT("evolving_pad");
            Pad.Summary = TEXT("Overlapping swells drawn from a degree pool, each of a random length inside swellBeatsMin..swellBeatsMax.");
            Pad.Params = &EvolvingPadParams();
            Pad.Validator = &ValidateEvolvingPad;
            Pad.Constraint = TEXT("degrees entries must be whole numbers; swellBeatsMin <= swellBeatsMax; velocityMin <= velocityMax.");

            FPwSynthKindSpec& Sparse = Built[static_cast<int32>(EPwMusicGenerationMode::SparseEvents)];
            Sparse.Name = TEXT("sparse_events");
            Sparse.Summary = TEXT("Occasional notes from a degree pool at a stated events-per-bar density, placed at random points inside each section.");
            Sparse.Params = &SparseEventsParams();
            Sparse.Validator = &ValidateSparseEvents;
            Sparse.Constraint = TEXT("degrees entries must be whole numbers; durationBeatsMin <= durationBeatsMax; velocityMin <= velocityMax.");

            return Built;
        }();
        return Specs;
    }
}

// ===========================================================================
// Vocabulary accessors.
// ===========================================================================

const TCHAR* PwMusicPitchClassToString(EPwMusicPitchClass Value)
{
    const int32 Index = static_cast<int32>(Value) - 1;
    return PwMusicSpecInternal::PitchClassRows().IsValidIndex(Index)
        ? PwMusicSpecInternal::PitchClassRows()[Index].Name
        : TEXT("unspecified");
}

bool PwMusicPitchClassFromString(const FString& In, EPwMusicPitchClass& Out)
{
    const TArray<PwMusicSpecInternal::FPitchClassRow>& Rows = PwMusicSpecInternal::PitchClassRows();
    for (int32 Index = 0; Index < Rows.Num(); ++Index)
    {
        if (In.Equals(Rows[Index].Name, ESearchCase::IgnoreCase))
        {
            Out = static_cast<EPwMusicPitchClass>(Index + 1);
            return true;
        }
    }
    return false;
}

int32 PwMusicPitchClassSemitone(EPwMusicPitchClass Value)
{
    const int32 Index = static_cast<int32>(Value) - 1;
    return PwMusicSpecInternal::PitchClassRows().IsValidIndex(Index)
        ? PwMusicSpecInternal::PitchClassRows()[Index].Semitone
        : 0;
}

const TCHAR* PwMusicScaleToString(EPwMusicScale Value)
{
    const int32 Index = static_cast<int32>(Value) - 1;
    return PwMusicSpecInternal::ScaleRows().IsValidIndex(Index)
        ? PwMusicSpecInternal::ScaleRows()[Index].Name
        : TEXT("unspecified");
}

bool PwMusicScaleFromString(const FString& In, EPwMusicScale& Out)
{
    const TArray<PwMusicSpecInternal::FScaleRow>& Rows = PwMusicSpecInternal::ScaleRows();
    for (int32 Index = 0; Index < Rows.Num(); ++Index)
    {
        if (In.Equals(Rows[Index].Name, ESearchCase::IgnoreCase))
        {
            Out = static_cast<EPwMusicScale>(Index + 1);
            return true;
        }
    }
    return false;
}

const TCHAR* PwMusicTrackRoleToString(EPwMusicTrackRole Value)
{
    const int32 Index = static_cast<int32>(Value) - 1;
    return PwMusicSpecInternal::RoleRows().IsValidIndex(Index)
        ? PwMusicSpecInternal::RoleRows()[Index].Name
        : TEXT("unspecified");
}

bool PwMusicTrackRoleFromString(const FString& In, EPwMusicTrackRole& Out)
{
    const TArray<PwMusicSpecInternal::FRoleRow>& Rows = PwMusicSpecInternal::RoleRows();
    for (int32 Index = 0; Index < Rows.Num(); ++Index)
    {
        if (In.Equals(Rows[Index].Name, ESearchCase::IgnoreCase))
        {
            Out = static_cast<EPwMusicTrackRole>(Index + 1);
            return true;
        }
    }
    return false;
}

const TCHAR* PwMusicTrackRoleMeaning(EPwMusicTrackRole Value)
{
    const int32 Index = static_cast<int32>(Value) - 1;
    return PwMusicSpecInternal::RoleRows().IsValidIndex(Index)
        ? PwMusicSpecInternal::RoleRows()[Index].Meaning
        : TEXT("");
}

const TCHAR* PwMusicGenerationModeToString(EPwMusicGenerationMode Value)
{
    const TArray<FPwSynthKindSpec>& Specs = PwMusicSpecInternal::GenerationSpecs();
    const int32 Index = static_cast<int32>(Value);
    return Specs.IsValidIndex(Index) ? Specs[Index].Name : TEXT("unspecified");
}

bool PwMusicGenerationModeFromString(const FString& In, EPwMusicGenerationMode& Out)
{
    const TArray<FPwSynthKindSpec>& Specs = PwMusicSpecInternal::GenerationSpecs();
    for (int32 Index = 1; Index < Specs.Num(); ++Index)
    {
        if (In.Equals(Specs[Index].Name, ESearchCase::IgnoreCase))
        {
            Out = static_cast<EPwMusicGenerationMode>(Index);
            return true;
        }
    }
    return false;
}

const FPwSynthKindSpec& PwMusicGenerationSpec(EPwMusicGenerationMode Mode)
{
    const TArray<FPwSynthKindSpec>& Specs = PwMusicSpecInternal::GenerationSpecs();
    const int32 Index = static_cast<int32>(Mode);
    return Specs.IsValidIndex(Index) ? Specs[Index] : Specs[0];
}

bool PwMusicScaleDegreeCount(EPwMusicScale Scale, int32& OutCount)
{
    OutCount = 0;
    const int32 Index = static_cast<int32>(Scale) - 1;
    if (!PwMusicSpecInternal::ScaleRows().IsValidIndex(Index))
    {
        return false;
    }
    Audio::ScaleDegreeSet* Set =
        Audio::FMidiNoteQuantizer::ScaleDegreeSetMap.Find(PwMusicSpecInternal::ScaleRows()[Index].Engine);
    if (!Set)
    {
        return false;
    }
    const TArrayView<float> Degrees = Set->GetScaleDegreeSet();
    if (Degrees.Num() <= 0)
    {
        return false;
    }
    OutCount = Degrees.Num();
    return true;
}

const TArray<int32>& PwMusicTimeSignatureDenominators()
{
    // The five EQuartzTimeSignatureQuantization values. There is no whole-note beat type,
    // so a "/1" score could never be handed to a Quartz clock.
    static const TArray<int32> Denominators = { 2, 4, 8, 16, 32 };
    return Denominators;
}

// ===========================================================================
// Pitch.
// ===========================================================================

namespace PwMusicPitchInternal
{
    // Floored division, so a negative degree walks DOWN an octave instead of toward zero.
    int32 FloorDiv(int32 Numerator, int32 Denominator)
    {
        const int32 Quotient = Numerator / Denominator;
        const int32 Remainder = Numerator % Denominator;
        return (Remainder != 0 && ((Remainder < 0) != (Denominator < 0))) ? Quotient - 1 : Quotient;
    }
}

bool PwScoreDegreeToMidi(EPwMusicScale Scale, EPwMusicPitchClass Root, int32 Octave, int32 Degree,
    int32& OutMidi, FString& OutErrorCode, FString& OutError)
{
    OutMidi = 0;
    OutErrorCode.Reset();
    OutError.Reset();

    const int32 ScaleIndex = static_cast<int32>(Scale) - 1;
    if (!PwMusicSpecInternal::ScaleRows().IsValidIndex(ScaleIndex) || Root == EPwMusicPitchClass::Unspecified)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = TEXT("the key names no scale or no root, so a scale degree cannot be resolved.");
        return false;
    }

    Audio::ScaleDegreeSet* Set =
        Audio::FMidiNoteQuantizer::ScaleDegreeSetMap.Find(PwMusicSpecInternal::ScaleRows()[ScaleIndex].Engine);
    if (!Set)
    {
        // The engine's own table is the authority; a missing entry is reported rather than
        // patched with a hand-rolled scale that would disagree with MetaSound's quantizer.
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(
            TEXT("the engine's scale table (Audio::FMidiNoteQuantizer::ScaleDegreeSetMap) carries no entry for '%s'."),
            PwMusicScaleToString(Scale));
        return false;
    }

    const TArrayView<float> Degrees = Set->GetScaleDegreeSet();
    if (Degrees.Num() <= 0)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(TEXT("the engine's scale table lists no degrees for '%s'."),
            PwMusicScaleToString(Scale));
        return false;
    }

    const int32 DegreeCount = Degrees.Num();
    const int32 OctaveShift = PwMusicPitchInternal::FloorDiv(Degree, DegreeCount);
    const int32 DegreeIndex = Degree - OctaveShift * DegreeCount;

    // C4 = MIDI 60, so octave -1 puts C at MIDI 0.
    const int32 RootMidi = (Octave + 1) * 12 + PwMusicPitchClassSemitone(Root);
    const int32 Midi = RootMidi + OctaveShift * 12 + FMath::RoundToInt32(static_cast<double>(Degrees[DegreeIndex]));

    if (Midi < PwMusicLimits::MinMidi || Midi > PwMusicLimits::MaxMidi)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(
            TEXT("degree %d in %s at octave %d resolves to MIDI %d, outside the playable range %d..%d. Move the track's octave or the degree; the pitch is not clamped, because a note relocated without saying so is a sound you did not ask for."),
            Degree, PwMusicScaleToString(Scale), Octave, Midi, PwMusicLimits::MinMidi, PwMusicLimits::MaxMidi);
        return false;
    }

    OutMidi = Midi;
    return true;
}

bool PwScoreResolveNoteMidi(const FPwMusicScore& Score, const FPwMusicTrack& Track, const FPwMusicNote& Note,
    int32& OutMidi, FString& OutErrorCode, FString& OutError)
{
    OutMidi = 0;
    OutErrorCode.Reset();
    OutError.Reset();

    if (!Note.HasPitch())
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = TEXT("a note carries exactly one of 'degree' or 'midi'; this one carries both or neither.");
        return false;
    }

    if (Note.Midi.IsSet())
    {
        const int32 Midi = Note.Midi.GetValue();
        if (Midi < PwMusicLimits::MinMidi || Midi > PwMusicLimits::MaxMidi)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutError = FString::Printf(TEXT("MIDI %d is outside the playable range %d..%d."),
                Midi, PwMusicLimits::MinMidi, PwMusicLimits::MaxMidi);
            return false;
        }
        // Absolute and verbatim. Audio::FMidiNoteQuantizer::QuantizeMidiNote would snap this
        // into the key, and silently moving a pitch the caller wrote down is exactly the
        // failure this schema exists to prevent.
        OutMidi = Midi;
        return true;
    }

    return PwScoreDegreeToMidi(Score.Key.Scale, Score.Key.Root, Track.Octave, Note.Degree.GetValue(),
        OutMidi, OutErrorCode, OutError);
}

double PwScoreMidiToFrequencyHz(double Midi)
{
    return static_cast<double>(Audio::GetFrequencyFromMidi(static_cast<float>(Midi)));
}

// ===========================================================================
// Time.
// ===========================================================================

double PwScoreBeatsPerBar(const FPwMusicScore& Score)
{
    // One beat is one `denominator` note, so a bar is `numerator` beats with no correction.
    return static_cast<double>(Score.TimeSignature.Numerator);
}

double PwScoreTotalBeats(const FPwMusicScore& Score)
{
    return static_cast<double>(Score.Bars) * PwScoreBeatsPerBar(Score);
}

double PwScoreBeatToMs(const FPwMusicScore& Score, double Beat)
{
    return Score.Bpm > 0.0 ? Beat * 60000.0 / Score.Bpm : 0.0;
}

double PwScoreMsToBeat(const FPwMusicScore& Score, double Ms)
{
    return Score.Bpm > 0.0 ? Ms * Score.Bpm / 60000.0 : 0.0;
}

double PwScoreDurationMs(const FPwMusicScore& Score)
{
    return PwScoreBeatToMs(Score, PwScoreTotalBeats(Score));
}

FPwScoreLoop PwScoreLoopLength(const FPwMusicScore& Score)
{
    FPwScoreLoop Loop;
    if (Score.Bpm <= 0.0 || Score.SampleRate <= 0 || Score.Bars <= 0 || Score.TimeSignature.Numerator <= 0)
    {
        return Loop;
    }

    Loop.DurationMs = PwScoreDurationMs(Score);
    Loop.ExactFrames = Loop.DurationMs * static_cast<double>(Score.SampleRate) / 1000.0;

    // Frames = Bars * Numerator * 60 * SampleRate / Bpm. The numerator is exact in int64
    // for every score this schema accepts (512 * 32 * 60 * 192000 fits comfortably), so
    // when Bpm is a whole number the answer is an integer-divisibility question and needs
    // no epsilon at all. Only a fractional tempo falls back to a residual test.
    const int64 FrameNumerator = static_cast<int64>(Score.Bars) * static_cast<int64>(Score.TimeSignature.Numerator)
        * 60LL * static_cast<int64>(Score.SampleRate);

    // Exact equality, not a near-equality with a tolerance: a tempo of 120.0001 is NOT 120,
    // and treating it as one would report a loop as sample-exact while it is two thirds of a
    // frame long. IsNearlyEqual with a zero tolerance is exact comparison without writing
    // `==` on doubles.
    const double RoundedTempo = FMath::RoundToDouble(Score.Bpm);
    const bool bWholeTempo = FMath::IsNearlyEqual(Score.Bpm, RoundedTempo, 0.0);
    if (bWholeTempo)
    {
        const int64 Tempo = static_cast<int64>(RoundedTempo);
        if (Tempo > 0)
        {
            Loop.Frames = FrameNumerator / Tempo;
            Loop.bExact = (FrameNumerator % Tempo) == 0;
            Loop.ExactFrames = static_cast<double>(FrameNumerator) / static_cast<double>(Tempo);
            if (!Loop.bExact)
            {
                // Report the NEAREST whole frame count, not the truncated one.
                Loop.Frames = static_cast<int64>(FMath::RoundToDouble(Loop.ExactFrames));
            }
        }
    }
    else
    {
        Loop.Frames = static_cast<int64>(FMath::RoundToDouble(Loop.ExactFrames));
        // One part in 1e9 of the loop length. Tighter than any audible boundary error and
        // looser than double rounding on a 10^8-frame value.
        const double Tolerance = FMath::Max(1.0, FMath::Abs(Loop.ExactFrames)) * 1e-9;
        Loop.bExact = FMath::Abs(Loop.ExactFrames - static_cast<double>(Loop.Frames)) <= Tolerance;
    }

    Loop.ResidualFrames = Loop.bExact ? 0.0 : Loop.ExactFrames - static_cast<double>(Loop.Frames);
    Loop.NearestExactBpm = Loop.Frames > 0
        ? static_cast<double>(FrameNumerator) / static_cast<double>(Loop.Frames)
        : 0.0;
    return Loop;
}

const FPwMusicSection* PwScoreSectionAtBar(const FPwMusicScore& Score, int32 Bar)
{
    for (const FPwMusicSection& Section : Score.Sections)
    {
        if (Bar >= Section.StartBar && Bar < Section.StartBar + Section.Bars)
        {
            return &Section;
        }
    }
    return nullptr;
}

// ===========================================================================
// Parsing. Everything lands in a local until the whole score validates, so a failed
// parse can never leave a half-built score observable (rpc-design §1).
// ===========================================================================
namespace PwMusicParseInternal
{
    bool Fail(FPwMusicScoreError& Err, const TCHAR* Code, const FString& Field, const FString& Message)
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

    bool RejectUnknownKeys(const TSharedPtr<FJsonObject>& Obj, const FString& Path,
        const TArray<FString>& Known, FPwMusicScoreError& Err)
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

    bool AsNumber(const TSharedPtr<FJsonValue>& Value, const FString& Path, double& Out, FPwMusicScoreError& Err)
    {
        if (Value->Type != EJson::Number)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path, TEXT("expected a JSON number."));
        }
        Out = Value->AsNumber();
        return true;
    }

    bool AsString(const TSharedPtr<FJsonValue>& Value, const FString& Path, FString& Out, FPwMusicScoreError& Err)
    {
        if (Value->Type != EJson::String)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path, TEXT("expected a JSON string."));
        }
        Out = Value->AsString();
        return true;
    }

    bool AsObject(const TSharedPtr<FJsonValue>& Value, const FString& Path, TSharedPtr<FJsonObject>& Out, FPwMusicScoreError& Err)
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
        const TArray<TSharedPtr<FJsonValue>>*& Out, FPwMusicScoreError& Err)
    {
        if (Value->Type != EJson::Array)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path, TEXT("expected a JSON array."));
        }
        Out = &Value->AsArray();
        return true;
    }

    bool CheckRange(double Number, double Min, double Max, const FString& Path, const TCHAR* Unit, FPwMusicScoreError& Err)
    {
        if (Number < Min || Number > Max)
        {
            const FString UnitText = Unit ? FString::Printf(TEXT(" %s"), Unit) : FString();
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                FString::Printf(TEXT("%g%s is outside the accepted range %g..%g."), Number, *UnitText, Min, Max));
        }
        return true;
    }

    bool CheckIntegral(double Number, const FString& Path, FPwMusicScoreError& Err)
    {
        if (!FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                FString::Printf(TEXT("%g is not a whole number."), Number));
        }
        return true;
    }

    bool ReadRequiredNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Parent,
        double Min, double Max, const TCHAR* Unit, double& Out, FPwMusicScoreError& Err)
    {
        const FString Path = Join(Parent, Key);
        TSharedPtr<FJsonValue> Value = FindField(Obj, Key);
        if (!Value.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                TEXT("required field is missing; it has no safe default."));
        }
        return AsNumber(Value, Path, Out, Err) && CheckRange(Out, Min, Max, Path, Unit, Err);
    }

    bool ReadOptionalNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Parent,
        double Min, double Max, double Default, const TCHAR* Unit, double& Out, FPwMusicScoreError& Err)
    {
        Out = Default;
        TSharedPtr<FJsonValue> Value = FindField(Obj, Key);
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path = Join(Parent, Key);
        return AsNumber(Value, Path, Out, Err) && CheckRange(Out, Min, Max, Path, Unit, Err);
    }

    bool ReadRequiredInt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Parent,
        double Min, double Max, int32& Out, FPwMusicScoreError& Err)
    {
        const FString Path = Join(Parent, Key);
        TSharedPtr<FJsonValue> Value = FindField(Obj, Key);
        if (!Value.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                TEXT("required field is missing; it has no safe default."));
        }
        double Number = 0.0;
        if (!AsNumber(Value, Path, Number, Err) ||
            !CheckIntegral(Number, Path, Err) ||
            !CheckRange(Number, Min, Max, Path, nullptr, Err))
        {
            return false;
        }
        Out = static_cast<int32>(FMath::RoundToDouble(Number));
        return true;
    }

    bool ReadOptionalInt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Parent,
        double Min, double Max, int32 Default, int32& Out, FPwMusicScoreError& Err)
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
            !CheckRange(Number, Min, Max, Path, nullptr, Err))
        {
            return false;
        }
        Out = static_cast<int32>(FMath::RoundToDouble(Number));
        return true;
    }

    // Names become stem filenames and dynamic-music lookup keys, so the charset is
    // restricted rather than escaped later: a name carrying a path separator would place a
    // stem outside the output directory, and one carrying a space would round-trip through
    // half the shells that touch it.
    bool ReadName(const TSharedPtr<FJsonObject>& Obj, const FString& Parent, FString& Out, FPwMusicScoreError& Err)
    {
        const FString Path = Join(Parent, TEXT("name"));
        TSharedPtr<FJsonValue> Value = FindField(Obj, TEXT("name"));
        if (!Value.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                TEXT("required field is missing; a name is what the stem file and every later reference are keyed on."));
        }
        if (!AsString(Value, Path, Out, Err))
        {
            return false;
        }
        if (Out.Len() < PwMusicLimits::MinNameLength || Out.Len() > PwMusicLimits::MaxNameLength)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                FString::Printf(TEXT("is %d characters; accepted length is %d..%d."),
                    Out.Len(), PwMusicLimits::MinNameLength, PwMusicLimits::MaxNameLength));
        }
        for (const TCHAR Character : Out)
        {
            const bool bAllowed = FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-');
            if (!bAllowed)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Path,
                    FString::Printf(TEXT("'%s' contains '%c'. A name becomes a stem filename, so only letters, digits, '_' and '-' are accepted."),
                        *Out, Character));
            }
        }
        return true;
    }

    // ---- instrument -------------------------------------------------------

    struct FOwnedElsewhere
    {
        const TCHAR* Key;
        const TCHAR* Owner;
    };

    const TArray<FOwnedElsewhere>& InstrumentRejectedKeys()
    {
        static const TArray<FOwnedElsewhere> Rows = {
            { TEXT("startMs"), TEXT("the note's startBeat") },
            { TEXT("gainDb"),  TEXT("the track's gainDb") },
            { TEXT("pan"),     TEXT("the track's pan") }
        };
        return Rows;
    }

    const TArray<FString>& InstrumentKnownKeys()
    {
        static const TArray<FString> Known = {
            TEXT("generator"), TEXT("ampEnvelope"), TEXT("pitchEnvelope"), TEXT("modulation"), TEXT("fx")
        };
        return Known;
    }

    // Parses an instrument by wrapping it as a one-layer FPwSynthRecipe and handing it to
    // the SFX parser. The alternative - a second layer parser here - is the duplication this
    // whole design exists to avoid; the price is the field-path rewrite below.
    bool ParseInstrument(const TSharedPtr<FJsonObject>& Obj, const FString& Path,
        FPwSynthLayer& Out, FPwMusicScoreError& Err)
    {
        for (const FOwnedElsewhere& Row : InstrumentRejectedKeys())
        {
            if (FindField(Obj, Row.Key).IsValid())
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(Path, Row.Key),
                    FString::Printf(TEXT("an instrument does not carry '%s'; %s owns it. Two owners of one quantity is a score that can say two different things about the same sound."),
                        Row.Key, Row.Owner));
            }
        }

        if (!RejectUnknownKeys(Obj, Path, InstrumentKnownKeys(), Err))
        {
            return false;
        }

        TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
        // The longest single voice the kernel renders. An instrument's envelopes are
        // validated against this span, not against the score's length: a note lasts as long
        // as the score says it does, and the envelope describes the voice, not the note.
        Wrapper->SetNumberField(TEXT("durationMs"), PwSynthLimits::MaxDurationMs);
        TArray<TSharedPtr<FJsonValue>> Layers;
        Layers.Add(MakeShared<FJsonValueObject>(Obj));
        Wrapper->SetArrayField(TEXT("layers"), Layers);

        FPwSynthRecipe Parsed;
        if (!ParseSynthRecipe(Wrapper, Parsed, Err))
        {
            // Re-root the layer's field path onto the instrument so the caller is told
            // "tracks[2].instrument.generator.kind", not "layers[0].generator.kind" for a
            // document that has no layers.
            static const FString LayerPrefix(TEXT("layers[0]"));
            if (Err.Field.StartsWith(LayerPrefix))
            {
                Err.Field = Path + Err.Field.RightChop(LayerPrefix.Len());
            }
            return false;
        }
        if (Parsed.Layers.Num() != 1)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path, TEXT("instrument did not resolve to a single voice."));
        }

        Out = MoveTemp(Parsed.Layers[0]);
        return true;
    }

    // ---- notes ------------------------------------------------------------

    bool ParseNote(const TSharedPtr<FJsonObject>& Obj, const FString& Path, double TotalBeats,
        FPwMusicNote& Out, FPwMusicScoreError& Err)
    {
        static const TArray<FString> Known = {
            TEXT("startBeat"), TEXT("durationBeats"), TEXT("degree"), TEXT("midi"), TEXT("velocity")
        };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        if (!ReadRequiredNumber(Obj, TEXT("startBeat"), Path, PwMusicLimits::MinBeat, PwMusicLimits::MaxBeats,
                TEXT("beats"), Out.StartBeat, Err) ||
            !ReadRequiredNumber(Obj, TEXT("durationBeats"), Path, 0.0, PwMusicLimits::MaxBeats,
                TEXT("beats"), Out.DurationBeats, Err) ||
            !ReadOptionalNumber(Obj, TEXT("velocity"), Path, 0.0, 1.0, PwMusicLimits::DefaultVelocity,
                nullptr, Out.Velocity, Err))
        {
            return false;
        }

        if (Out.DurationBeats <= 0.0)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Join(Path, TEXT("durationBeats")),
                TEXT("must be greater than 0; a zero-length note is silent, which is what omitting the note says more clearly."));
        }
        if (Out.StartBeat >= TotalBeats)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(Path, TEXT("startBeat")),
                FString::Printf(TEXT("%g starts at or after the score's last beat (%g); the note would never sound."),
                    Out.StartBeat, TotalBeats));
        }
        if (Out.StartBeat + Out.DurationBeats > TotalBeats)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(Path, TEXT("durationBeats")),
                FString::Printf(TEXT("the note ends at beat %g, past the score's %g beats. A note crossing the loop point is not wrapped - shorten it or lengthen the score."),
                    Out.StartBeat + Out.DurationBeats, TotalBeats));
        }

        int32 Degree = 0;
        const bool bHasDegree = FindField(Obj, TEXT("degree")).IsValid();
        const bool bHasMidi = FindField(Obj, TEXT("midi")).IsValid();
        if (bHasDegree == bHasMidi)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                bHasDegree
                    ? TEXT("carries both 'degree' and 'midi'. A note has exactly one pitch source; two would let it say two things.")
                    : TEXT("carries neither 'degree' nor 'midi'. A note needs exactly one pitch source and there is no default pitch."));
        }
        if (bHasDegree)
        {
            if (!ReadRequiredInt(Obj, TEXT("degree"), Path,
                    static_cast<double>(PwMusicLimits::MinDegree), static_cast<double>(PwMusicLimits::MaxDegree), Degree, Err))
            {
                return false;
            }
            Out.Degree = Degree;
        }
        else
        {
            int32 Midi = 0;
            if (!ReadRequiredInt(Obj, TEXT("midi"), Path,
                    static_cast<double>(PwMusicLimits::MinMidi), static_cast<double>(PwMusicLimits::MaxMidi), Midi, Err))
            {
                return false;
            }
            Out.Midi = Midi;
        }

        return true;
    }

    // ---- generation -------------------------------------------------------

    FString GenerationModeList()
    {
        TArray<FString> Names;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicGenerationMode::Count); ++Index)
        {
            Names.Add(PwMusicGenerationModeToString(static_cast<EPwMusicGenerationMode>(Index)));
        }
        return JoinList(Names);
    }

    bool ParseGeneration(const TSharedPtr<FJsonObject>& Owner, const FString& OwnerPath,
        FPwMusicGeneration& Out, FPwMusicScoreError& Err)
    {
        const FString Path = Join(OwnerPath, TEXT("generation"));
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("generation"));
        if (!Value.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                FString::Printf(TEXT("every track says how its notes come to exist; there is no default mode. Accepted: %s."),
                    *GenerationModeList()));
        }
        TSharedPtr<FJsonObject> Obj;
        if (!AsObject(Value, Path, Obj, Err))
        {
            return false;
        }

        static const TArray<FString> Known = { TEXT("mode"), TEXT("params") };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        const FString ModePath = Join(Path, TEXT("mode"));
        TSharedPtr<FJsonValue> ModeValue = FindField(Obj, TEXT("mode"));
        if (!ModeValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, ModePath,
                FString::Printf(TEXT("required field is missing; it has no safe default. Accepted: %s."), *GenerationModeList()));
        }
        FString ModeName;
        if (!AsString(ModeValue, ModePath, ModeName, Err))
        {
            return false;
        }
        if (!PwMusicGenerationModeFromString(ModeName, Out.Mode))
        {
            // Never degrade an unrecognised mode into `explicit`: the track would fall silent
            // and the caller would hunt an instrument that was never the problem.
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, ModePath,
                FString::Printf(TEXT("'%s' is not a generation mode. Accepted: %s."), *ModeName, *GenerationModeList()));
        }

        TSharedPtr<FJsonObject> ParamsObj;
        const FString ParamsPath = Join(Path, TEXT("params"));
        if (TSharedPtr<FJsonValue> ParamsValue = FindField(Obj, TEXT("params")))
        {
            if (!AsObject(ParamsValue, ParamsPath, ParamsObj, Err))
            {
                return false;
            }
        }

        return ParseSynthParamBag(ParamsObj, PwMusicGenerationSpec(Out.Mode),
            TEXT("generation mode"), ParamsPath, Out.Params, Err);
    }

    // Every pitch a track can produce must resolve to a playable MIDI note, and the place to
    // find that out is here - where the field path still exists and the caller can patch it -
    // not at render time, where all that is left is a silent stem.
    bool ValidateTrackPitches(const FPwMusicScore& Score, const FPwMusicTrack& Track, const FString& Path,
        FPwMusicScoreError& Err)
    {
        FString Code;
        FString Message;

        if (Track.Generation.Mode == EPwMusicGenerationMode::Explicit)
        {
            for (int32 Index = 0; Index < Track.Notes.Num(); ++Index)
            {
                int32 Midi = 0;
                if (!PwScoreResolveNoteMidi(Score, Track, Track.Notes[Index], Midi, Code, Message))
                {
                    const FString NotePath = Indexed(Join(Path, TEXT("notes")), Index);
                    return Fail(Err, *Code,
                        Join(NotePath, Track.Notes[Index].Degree.IsSet() ? TEXT("degree") : TEXT("midi")), Message);
                }
            }
            return true;
        }

        int32 DegreeCount = 0;
        if (!PwMusicScaleDegreeCount(Score.Key.Scale, DegreeCount))
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, TEXT("key.scale"),
                FString::Printf(TEXT("the engine's scale table carries no degrees for '%s'."),
                    PwMusicScaleToString(Score.Key.Scale)));
        }

        const TArray<double>* Degrees = Track.Generation.Params.GetNumbers(FName(TEXT("degrees")));
        if (!Degrees)
        {
            return true;    // a mode with no degree pool has no pitch to pre-validate
        }
        const int32 Spread = Track.Generation.Params.GetInt(FName(TEXT("octaveSpread")), 0);

        const FString ParamsPath = Join(Join(Path, TEXT("generation")), TEXT("params"));
        for (int32 Index = 0; Index < Degrees->Num(); ++Index)
        {
            const int32 Base = FMath::RoundToInt32((*Degrees)[Index]);
            for (int32 Octave = -Spread; Octave <= Spread; ++Octave)
            {
                int32 Midi = 0;
                if (!PwScoreDegreeToMidi(Score.Key.Scale, Score.Key.Root, Track.Octave,
                        Base + Octave * DegreeCount, Midi, Code, Message))
                {
                    // Point at whichever field the caller can move. With no spread the degree
                    // is the only candidate; with spread it is the combination, and the spread
                    // is the cheaper of the two to shrink.
                    const FString Field = (Octave == 0)
                        ? Indexed(Join(ParamsPath, TEXT("degrees")), Index)
                        : Join(ParamsPath, TEXT("octaveSpread"));
                    return Fail(Err, *Code, Field, Message);
                }
            }
        }
        return true;
    }

    // ---- track ------------------------------------------------------------

    FString RoleList()
    {
        TArray<FString> Names;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicTrackRole::Count); ++Index)
        {
            Names.Add(PwMusicTrackRoleToString(static_cast<EPwMusicTrackRole>(Index)));
        }
        return JoinList(Names);
    }

    bool ParseTrack(const TSharedPtr<FJsonObject>& Obj, const FString& Path, double TotalBeats,
        FPwMusicTrack& Out, FPwMusicScoreError& Err)
    {
        static const TArray<FString> Known = {
            TEXT("name"), TEXT("role"), TEXT("octave"), TEXT("gainDb"), TEXT("pan"),
            TEXT("instrument"), TEXT("generation"), TEXT("notes")
        };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        if (!ReadName(Obj, Path, Out.Name, Err))
        {
            return false;
        }

        const FString RolePath = Join(Path, TEXT("role"));
        TSharedPtr<FJsonValue> RoleValue = FindField(Obj, TEXT("role"));
        if (!RoleValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, RolePath,
                FString::Printf(TEXT("required field is missing; it has no safe default. Accepted: %s."), *RoleList()));
        }
        FString RoleName;
        if (!AsString(RoleValue, RolePath, RoleName, Err))
        {
            return false;
        }
        if (!PwMusicTrackRoleFromString(RoleName, Out.Role))
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, RolePath,
                FString::Printf(TEXT("'%s' is not a track role. Accepted: %s."), *RoleName, *RoleList()));
        }

        if (!ReadRequiredInt(Obj, TEXT("octave"), Path,
                static_cast<double>(PwMusicLimits::MinOctave), static_cast<double>(PwMusicLimits::MaxOctave),
                Out.Octave, Err) ||
            !ReadOptionalNumber(Obj, TEXT("gainDb"), Path, PwSynthLimits::MinGainDb, PwSynthLimits::MaxGainDb,
                PwMusicLimits::DefaultTrackGainDb, TEXT("db"), Out.GainDb, Err) ||
            !ReadOptionalNumber(Obj, TEXT("pan"), Path, -1.0, 1.0,
                PwMusicLimits::DefaultTrackPan, nullptr, Out.Pan, Err))
        {
            return false;
        }

        const FString InstrumentPath = Join(Path, TEXT("instrument"));
        TSharedPtr<FJsonValue> InstrumentValue = FindField(Obj, TEXT("instrument"));
        if (!InstrumentValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, InstrumentPath,
                TEXT("every track needs an instrument; there is no default voice."));
        }
        TSharedPtr<FJsonObject> InstrumentObj;
        if (!AsObject(InstrumentValue, InstrumentPath, InstrumentObj, Err) ||
            !ParseInstrument(InstrumentObj, InstrumentPath, Out.Instrument, Err))
        {
            return false;
        }

        if (!ParseGeneration(Obj, Path, Out.Generation, Err))
        {
            return false;
        }

        const FString NotesPath = Join(Path, TEXT("notes"));
        TSharedPtr<FJsonValue> NotesValue = FindField(Obj, TEXT("notes"));
        const bool bExplicit = Out.Generation.Mode == EPwMusicGenerationMode::Explicit;

        if (!bExplicit)
        {
            if (NotesValue.IsValid())
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, NotesPath,
                    FString::Printf(TEXT("a '%s' track generates its own notes, so a notes[] array here would be a second source of truth for one stem. Drop the array, or switch the mode to 'explicit'."),
                        PwMusicGenerationModeToString(Out.Generation.Mode)));
            }
            return true;
        }

        if (!NotesValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, NotesPath,
                TEXT("an 'explicit' track is defined by its notes[] array, and the array is missing."));
        }
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!AsArray(NotesValue, NotesPath, Entries, Err))
        {
            return false;
        }
        if (Entries->Num() < 1 || Entries->Num() > PwMusicLimits::MaxNotesPerTrack)
        {
            // Error, not a clamp: a silently truncated track does not round-trip.
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, NotesPath,
                FString::Printf(TEXT("has %d notes; accepted range is 1..%d. Split the part across tracks, or switch to a generative mode."),
                    Entries->Num(), PwMusicLimits::MaxNotesPerTrack));
        }

        Out.Notes.Reserve(Entries->Num());
        double PreviousStart = -1.0;
        for (int32 Index = 0; Index < Entries->Num(); ++Index)
        {
            const FString NotePath = Indexed(NotesPath, Index);
            TSharedPtr<FJsonObject> NoteObj;
            if (!(*Entries)[Index].IsValid() || !AsObject((*Entries)[Index], NotePath, NoteObj, Err))
            {
                return false;
            }
            FPwMusicNote Note;
            if (!ParseNote(NoteObj, NotePath, TotalBeats, Note, Err))
            {
                return false;
            }
            if (Note.StartBeat < PreviousStart)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(NotePath, TEXT("startBeat")),
                    FString::Printf(TEXT("%g is earlier than the previous note at %g; notes must be ordered. Equal start beats are a chord and are accepted."),
                        Note.StartBeat, PreviousStart));
            }
            PreviousStart = Note.StartBeat;
            Out.Notes.Add(MoveTemp(Note));
        }

        return true;
    }

    // ---- sections ---------------------------------------------------------

    bool ParseSections(const TSharedPtr<FJsonObject>& Owner, int32 ScoreBars,
        TArray<FPwMusicSection>& Out, FPwMusicScoreError& Err)
    {
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("sections"));
        if (!Value.IsValid())
        {
            return true;
        }
        const FString Path(TEXT("sections"));
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!AsArray(Value, Path, Entries, Err))
        {
            return false;
        }
        if (Entries->Num() < 1 || Entries->Num() > PwMusicLimits::MaxSections)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                FString::Printf(TEXT("has %d sections; accepted range is 1..%d. Omit the field entirely for a score with no sections."),
                    Entries->Num(), PwMusicLimits::MaxSections));
        }

        static const TArray<FString> Known = {
            TEXT("name"), TEXT("startBar"), TEXT("bars"), TEXT("intensityFloor"), TEXT("intensityCeil")
        };

        int32 ExpectedStart = 0;
        TArray<FString> SeenNames;
        for (int32 Index = 0; Index < Entries->Num(); ++Index)
        {
            const FString SectionPath = Indexed(Path, Index);
            TSharedPtr<FJsonObject> Obj;
            if (!(*Entries)[Index].IsValid() || !AsObject((*Entries)[Index], SectionPath, Obj, Err) ||
                !RejectUnknownKeys(Obj, SectionPath, Known, Err))
            {
                return false;
            }

            FPwMusicSection Section;
            if (!ReadName(Obj, SectionPath, Section.Name, Err) ||
                !ReadRequiredInt(Obj, TEXT("startBar"), SectionPath, 0.0,
                    static_cast<double>(PwMusicLimits::MaxBars), Section.StartBar, Err) ||
                !ReadRequiredInt(Obj, TEXT("bars"), SectionPath, 1.0,
                    static_cast<double>(PwMusicLimits::MaxBars), Section.Bars, Err) ||
                !ReadOptionalNumber(Obj, TEXT("intensityFloor"), SectionPath, 0.0, 1.0,
                    PwMusicLimits::DefaultIntensityFloor, nullptr, Section.IntensityFloor, Err) ||
                !ReadOptionalNumber(Obj, TEXT("intensityCeil"), SectionPath, 0.0, 1.0,
                    PwMusicLimits::DefaultIntensityCeil, nullptr, Section.IntensityCeil, Err))
            {
                return false;
            }

            if (Section.IntensityFloor > Section.IntensityCeil)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(SectionPath, TEXT("intensityCeil")),
                    FString::Printf(TEXT("%g is below intensityFloor (%g); the band would be empty."),
                        Section.IntensityCeil, Section.IntensityFloor));
            }

            for (const FString& Seen : SeenNames)
            {
                if (Seen.Equals(Section.Name, ESearchCase::IgnoreCase))
                {
                    return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(SectionPath, TEXT("name")),
                        FString::Printf(TEXT("'%s' is already the name of an earlier section. Section names are the keys a dynamic-music transition names, so they must be unique (compared case-insensitively)."),
                            *Section.Name));
                }
            }
            SeenNames.Add(Section.Name);

            // Sections tile the score exactly, so "which section is bar N in" always has one
            // answer. A gap is an editing mistake far more often than an intention.
            if (Section.StartBar != ExpectedStart)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(SectionPath, TEXT("startBar")),
                    FString::Printf(TEXT("%d leaves a gap or overlap: the previous section ends at bar %d and sections must tile the score without holes."),
                        Section.StartBar, ExpectedStart));
            }
            ExpectedStart = Section.StartBar + Section.Bars;
            if (ExpectedStart > ScoreBars)
            {
                return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Join(SectionPath, TEXT("bars")),
                    FString::Printf(TEXT("this section ends at bar %d, past the score's %d bars."),
                        ExpectedStart, ScoreBars));
            }

            Out.Add(MoveTemp(Section));
        }

        if (ExpectedStart != ScoreBars)
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Indexed(Path, Out.Num() - 1),
                FString::Printf(TEXT("the last section ends at bar %d but the score is %d bars; sections must cover the score exactly."),
                    ExpectedStart, ScoreBars));
        }
        return true;
    }

    // ---- key / time signature ---------------------------------------------

    FString PitchClassList()
    {
        TArray<FString> Names;
        for (const PwMusicSpecInternal::FPitchClassRow& Row : PwMusicSpecInternal::PitchClassRows())
        {
            Names.Add(Row.Name);
        }
        return JoinList(Names);
    }

    FString ScaleList()
    {
        TArray<FString> Names;
        for (const PwMusicSpecInternal::FScaleRow& Row : PwMusicSpecInternal::ScaleRows())
        {
            Names.Add(Row.Name);
        }
        return JoinList(Names);
    }

    bool ParseKey(const TSharedPtr<FJsonObject>& Owner, FPwMusicKey& Out, FPwMusicScoreError& Err)
    {
        const FString Path(TEXT("key"));
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("key"));
        if (!Value.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                TEXT("a score names its key; there is no default root or scale."));
        }
        TSharedPtr<FJsonObject> Obj;
        if (!AsObject(Value, Path, Obj, Err))
        {
            return false;
        }
        static const TArray<FString> Known = { TEXT("root"), TEXT("scale") };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        const FString RootPath = Join(Path, TEXT("root"));
        TSharedPtr<FJsonValue> RootValue = FindField(Obj, TEXT("root"));
        if (!RootValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, RootPath,
                FString::Printf(TEXT("required field is missing; it has no safe default. Accepted: %s."), *PitchClassList()));
        }
        FString RootName;
        if (!AsString(RootValue, RootPath, RootName, Err))
        {
            return false;
        }
        if (!PwMusicPitchClassFromString(RootName, Out.Root))
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, RootPath,
                FString::Printf(TEXT("'%s' is not a pitch class. Accepted: %s. Flat spellings are not aliases - canonicalizing 'db' to 'c#' would stop the score round-tripping."),
                    *RootName, *PitchClassList()));
        }

        const FString ScalePath = Join(Path, TEXT("scale"));
        TSharedPtr<FJsonValue> ScaleValue = FindField(Obj, TEXT("scale"));
        if (!ScaleValue.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, ScalePath,
                FString::Printf(TEXT("required field is missing; it has no safe default. Accepted: %s."), *ScaleList()));
        }
        FString ScaleName;
        if (!AsString(ScaleValue, ScalePath, ScaleName, Err))
        {
            return false;
        }
        if (!PwMusicScaleFromString(ScaleName, Out.Scale))
        {
            // Never fall back to major. An unrecognised scale is a typo the caller can fix in
            // one edit; a silently major score is a bug they will chase by ear.
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, ScalePath,
                FString::Printf(TEXT("'%s' is not a scale. Accepted: %s."), *ScaleName, *ScaleList()));
        }
        return true;
    }

    bool ParseTimeSignature(const TSharedPtr<FJsonObject>& Owner, FPwMusicTimeSignature& Out, FPwMusicScoreError& Err)
    {
        const FString Path(TEXT("timeSignature"));
        TSharedPtr<FJsonValue> Value = FindField(Owner, TEXT("timeSignature"));
        if (!Value.IsValid())
        {
            return Fail(Err, ErrorCodes::ERR_INVALID_RECIPE, Path,
                TEXT("a score names its time signature; it decides what a beat IS, so it cannot be defaulted."));
        }
        TSharedPtr<FJsonObject> Obj;
        if (!AsObject(Value, Path, Obj, Err))
        {
            return false;
        }
        static const TArray<FString> Known = { TEXT("numerator"), TEXT("denominator") };
        if (!RejectUnknownKeys(Obj, Path, Known, Err))
        {
            return false;
        }

        if (!ReadRequiredInt(Obj, TEXT("numerator"), Path,
                static_cast<double>(PwMusicLimits::MinTimeSignatureNumerator),
                static_cast<double>(PwMusicLimits::MaxTimeSignatureNumerator), Out.Numerator, Err) ||
            !ReadRequiredInt(Obj, TEXT("denominator"), Path, 1.0, 64.0, Out.Denominator, Err))
        {
            return false;
        }

        if (!PwMusicTimeSignatureDenominators().Contains(Out.Denominator))
        {
            TArray<FString> Names;
            for (int32 Denominator : PwMusicTimeSignatureDenominators())
            {
                Names.Add(FString::FromInt(Denominator));
            }
            return Fail(Err, ErrorCodes::ERR_INVALID_PARAMS, Join(Path, TEXT("denominator")),
                FString::Printf(TEXT("%d is not a beat unit Quartz can express. Accepted: %s (EQuartzTimeSignatureQuantization has no whole-note beat type)."),
                    Out.Denominator, *JoinList(Names)));
        }
        return true;
    }
}

bool ParseMusicScore(const TSharedPtr<FJsonObject>& In, FPwMusicScore& Out, FPwMusicScoreError& OutError)
{
    using namespace PwMusicParseInternal;

    OutError.Reset();

    if (!In.IsValid())
    {
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("score"),
            TEXT("expected a score object."));
    }

    // Everything lands in a local until the whole score validates (rpc-design §1).
    FPwMusicScore Work;

    static const TArray<FString> Known = {
        TEXT("version"), TEXT("seed"), TEXT("sampleRate"), TEXT("bpm"), TEXT("timeSignature"),
        TEXT("key"), TEXT("bars"), TEXT("sections"), TEXT("tracks")
    };
    if (!RejectUnknownKeys(In, FString(), Known, OutError))
    {
        return false;
    }

    if (!ReadOptionalInt(In, TEXT("version"), FString(), 0.0, 1000.0,
            PwMusicLimits::ScoreVersion, Work.Version, OutError))
    {
        return false;
    }
    if (Work.Version != PwMusicLimits::ScoreVersion)
    {
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("version"),
            FString::Printf(TEXT("score version %d is not supported; this build speaks version %d."),
                Work.Version, PwMusicLimits::ScoreVersion));
    }

    if (!ReadOptionalInt(In, TEXT("seed"), FString(), 0.0, static_cast<double>(MAX_int32),
            PwMusicLimits::DefaultSeed, Work.Seed, OutError) ||
        !ReadOptionalInt(In, TEXT("sampleRate"), FString(),
            PwSynthLimits::MinSampleRate, PwSynthLimits::MaxSampleRate,
            PwMusicLimits::DefaultSampleRate, Work.SampleRate, OutError) ||
        !ReadRequiredNumber(In, TEXT("bpm"), FString(),
            PwMusicLimits::MinBpm, PwMusicLimits::MaxBpm, TEXT("bpm"), Work.Bpm, OutError) ||
        !ReadRequiredInt(In, TEXT("bars"), FString(),
            static_cast<double>(PwMusicLimits::MinBars), static_cast<double>(PwMusicLimits::MaxBars),
            Work.Bars, OutError))
    {
        return false;
    }

    if (!ParseTimeSignature(In, Work.TimeSignature, OutError) ||
        !ParseKey(In, Work.Key, OutError) ||
        !ParseSections(In, Work.Bars, Work.Sections, OutError))
    {
        return false;
    }

    TSharedPtr<FJsonValue> TracksValue = FindField(In, TEXT("tracks"));
    if (!TracksValue.IsValid())
    {
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks"),
            TEXT("a score needs at least one track."));
    }
    const TArray<TSharedPtr<FJsonValue>>* TrackEntries = nullptr;
    if (!AsArray(TracksValue, TEXT("tracks"), TrackEntries, OutError))
    {
        return false;
    }
    if (TrackEntries->Num() == 0)
    {
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks"),
            TEXT("a score needs at least one track."));
    }
    if (TrackEntries->Num() > PwMusicLimits::MaxTracks)
    {
        // Error, not a clamp: a silently truncated score does not round-trip, and each
        // dropped track is a stem the caller believes exists.
        return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks"),
            FString::Printf(TEXT("%d tracks exceeds the cap of %d. Render the extra tracks as a second score and mix the stems."),
                TrackEntries->Num(), PwMusicLimits::MaxTracks));
    }

    const double TotalBeats = static_cast<double>(Work.Bars) * static_cast<double>(Work.TimeSignature.Numerator);

    Work.Tracks.Reserve(TrackEntries->Num());
    for (int32 Index = 0; Index < TrackEntries->Num(); ++Index)
    {
        const FString TrackPath = Indexed(TEXT("tracks"), Index);
        TSharedPtr<FJsonObject> TrackObj;
        if (!(*TrackEntries)[Index].IsValid() || !AsObject((*TrackEntries)[Index], TrackPath, TrackObj, OutError))
        {
            return false;
        }
        FPwMusicTrack Track;
        if (!ParseTrack(TrackObj, TrackPath, TotalBeats, Track, OutError))
        {
            return false;
        }

        for (const FPwMusicTrack& Seen : Work.Tracks)
        {
            if (Seen.Name.Equals(Track.Name, ESearchCase::IgnoreCase))
            {
                return Fail(OutError, ErrorCodes::ERR_INVALID_RECIPE, Join(TrackPath, TEXT("name")),
                    FString::Printf(TEXT("'%s' is already the name of an earlier track. One track is one stem file, and the comparison is case-insensitive because two stems differing only in case are one file on Windows."),
                        *Track.Name));
            }
        }

        if (!ValidateTrackPitches(Work, Track, TrackPath, OutError))
        {
            return false;
        }

        Work.Tracks.Add(MoveTemp(Track));
    }

    Out = MoveTemp(Work);
    return true;
}

bool ParseMusicScore(const TSharedPtr<FJsonObject>& In, FPwMusicScore& Out, FString& OutError)
{
    FPwMusicScoreError Error;
    if (ParseMusicScore(In, Out, Error))
    {
        OutError.Reset();
        return true;
    }
    OutError = Error.ToString();
    return false;
}

// ===========================================================================
// Serializing. Every defaulted value is made explicit, so serialize is lossless and
// re-parsing is idempotent.
// ===========================================================================
namespace PwMusicSerializeInternal
{
    // Serializes the instrument through the SFX serializer by wrapping it as a one-layer
    // recipe, then drops the three keys the score owns. The alternative - a second layer
    // serializer - would drift from the parser the moment a generator gained a parameter.
    TSharedPtr<FJsonObject> SerializeInstrument(const FPwSynthLayer& In)
    {
        FPwSynthRecipe Wrapper;
        Wrapper.DurationMs = PwSynthLimits::MaxDurationMs;
        Wrapper.Layers.Add(In);

        TSharedPtr<FJsonObject> Root = SerializeSynthRecipe(Wrapper);
        const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
        if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("layers"), Layers) || Layers->Num() != 1)
        {
            return MakeShared<FJsonObject>();
        }
        TSharedPtr<FJsonObject> LayerObj = (*Layers)[0]->AsObject();
        if (!LayerObj.IsValid())
        {
            return MakeShared<FJsonObject>();
        }
        for (const PwMusicParseInternal::FOwnedElsewhere& Row : PwMusicParseInternal::InstrumentRejectedKeys())
        {
            LayerObj->RemoveField(Row.Key);
        }
        return LayerObj;
    }

    TSharedPtr<FJsonObject> SerializeNote(const FPwMusicNote& In)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("startBeat"), In.StartBeat);
        Obj->SetNumberField(TEXT("durationBeats"), In.DurationBeats);
        if (In.Degree.IsSet())
        {
            Obj->SetNumberField(TEXT("degree"), In.Degree.GetValue());
        }
        if (In.Midi.IsSet())
        {
            Obj->SetNumberField(TEXT("midi"), In.Midi.GetValue());
        }
        Obj->SetNumberField(TEXT("velocity"), In.Velocity);
        return Obj;
    }

    TSharedPtr<FJsonObject> SerializeTrack(const FPwMusicTrack& In)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), In.Name);
        Obj->SetStringField(TEXT("role"), PwMusicTrackRoleToString(In.Role));
        Obj->SetNumberField(TEXT("octave"), In.Octave);
        Obj->SetNumberField(TEXT("gainDb"), In.GainDb);
        Obj->SetNumberField(TEXT("pan"), In.Pan);
        Obj->SetObjectField(TEXT("instrument"), SerializeInstrument(In.Instrument));

        TSharedPtr<FJsonObject> GenerationObj = MakeShared<FJsonObject>();
        GenerationObj->SetStringField(TEXT("mode"), PwMusicGenerationModeToString(In.Generation.Mode));
        GenerationObj->SetObjectField(TEXT("params"),
            SerializeSynthParamBag(In.Generation.Params, PwMusicGenerationSpec(In.Generation.Mode)));
        Obj->SetObjectField(TEXT("generation"), GenerationObj);

        // Emitted only for an explicit track, because a generative track that carried a
        // notes[] array would not re-parse - which is the round-trip half of the rule that
        // rejects it on the way in.
        if (In.Generation.Mode == EPwMusicGenerationMode::Explicit)
        {
            TArray<TSharedPtr<FJsonValue>> Entries;
            Entries.Reserve(In.Notes.Num());
            for (const FPwMusicNote& Note : In.Notes)
            {
                Entries.Add(MakeShared<FJsonValueObject>(SerializeNote(Note)));
            }
            Obj->SetArrayField(TEXT("notes"), Entries);
        }

        return Obj;
    }
}

TSharedPtr<FJsonObject> SerializeMusicScore(const FPwMusicScore& In)
{
    using namespace PwMusicSerializeInternal;

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("version"), In.Version);
    Root->SetNumberField(TEXT("seed"), In.Seed);
    Root->SetNumberField(TEXT("sampleRate"), In.SampleRate);
    Root->SetNumberField(TEXT("bpm"), In.Bpm);

    TSharedPtr<FJsonObject> TimeSignatureObj = MakeShared<FJsonObject>();
    TimeSignatureObj->SetNumberField(TEXT("numerator"), In.TimeSignature.Numerator);
    TimeSignatureObj->SetNumberField(TEXT("denominator"), In.TimeSignature.Denominator);
    Root->SetObjectField(TEXT("timeSignature"), TimeSignatureObj);

    TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
    KeyObj->SetStringField(TEXT("root"), PwMusicPitchClassToString(In.Key.Root));
    KeyObj->SetStringField(TEXT("scale"), PwMusicScaleToString(In.Key.Scale));
    Root->SetObjectField(TEXT("key"), KeyObj);

    Root->SetNumberField(TEXT("bars"), In.Bars);

    if (In.Sections.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Reserve(In.Sections.Num());
        for (const FPwMusicSection& Section : In.Sections)
        {
            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetStringField(TEXT("name"), Section.Name);
            SectionObj->SetNumberField(TEXT("startBar"), Section.StartBar);
            SectionObj->SetNumberField(TEXT("bars"), Section.Bars);
            SectionObj->SetNumberField(TEXT("intensityFloor"), Section.IntensityFloor);
            SectionObj->SetNumberField(TEXT("intensityCeil"), Section.IntensityCeil);
            Entries.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        Root->SetArrayField(TEXT("sections"), Entries);
    }

    TArray<TSharedPtr<FJsonValue>> TrackEntries;
    TrackEntries.Reserve(In.Tracks.Num());
    for (const FPwMusicTrack& Track : In.Tracks)
    {
        TrackEntries.Add(MakeShared<FJsonValueObject>(SerializeTrack(Track)));
    }
    Root->SetArrayField(TEXT("tracks"), TrackEntries);

    return Root;
}

// ===========================================================================
// Generation. Deterministic by construction - see the header's determinism note.
// ===========================================================================
namespace PwMusicGenerateInternal
{
    // A span of whole bars carrying the intensity band that applies inside it. One per
    // section, or one covering the whole score when there are none.
    struct FSpan
    {
        int32 StartBar = 0;
        int32 Bars = 0;
        double IntensityFloor = PwMusicLimits::DefaultIntensityFloor;
        double IntensityCeil = PwMusicLimits::DefaultIntensityCeil;
    };

    void BuildSpans(const FPwMusicScore& Score, TArray<FSpan>& Out)
    {
        Out.Reset();
        if (Score.Sections.Num() == 0)
        {
            FSpan Span;
            Span.Bars = Score.Bars;
            Out.Add(Span);
            return;
        }
        Out.Reserve(Score.Sections.Num());
        for (const FPwMusicSection& Section : Score.Sections)
        {
            FSpan Span;
            Span.StartBar = Section.StartBar;
            Span.Bars = Section.Bars;
            Span.IntensityFloor = Section.IntensityFloor;
            Span.IntensityCeil = Section.IntensityCeil;
            Out.Add(Span);
        }
    }

    // The section's intensity band is what a section IS in v1: a generated velocity of v
    // lands at floor + v*(ceil-floor), so a quiet section is quiet. Authored notes on an
    // explicit track are never remapped - the author already decided.
    double RemapVelocity(const FSpan& Span, double Velocity)
    {
        return Span.IntensityFloor + Velocity * (Span.IntensityCeil - Span.IntensityFloor);
    }

    bool CapExceeded(const TArray<FPwMusicNote>& Notes, FString& OutErrorCode, FString& OutError)
    {
        if (Notes.Num() <= PwMusicLimits::MaxNotesPerTrack)
        {
            return false;
        }
        // Error, not a truncation. A rule that runs away is a rule the caller must see,
        // and a silently cropped track would render as a bed that stops halfway.
        OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
        OutError = FString::Printf(
            TEXT("the generation rule produced more than %d notes for one track. Lower the density (eventsPerBar), lengthen the swells, or shorten the score."),
            PwMusicLimits::MaxNotesPerTrack);
        return true;
    }
}

bool PwScoreGenerateTrackNotes(const FPwMusicScore& Score, int32 TrackIndex,
    TArray<FPwMusicNote>& OutNotes, FString& OutErrorCode, FString& OutError)
{
    using namespace PwMusicGenerateInternal;

    // Failure is the default: nothing is observable until the single success path at the end.
    OutNotes.Reset();
    OutErrorCode.Reset();
    OutError.Reset();

    if (!Score.Tracks.IsValidIndex(TrackIndex))
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
        OutError = FString::Printf(TEXT("track index %d is outside the score's %d tracks."),
            TrackIndex, Score.Tracks.Num());
        return false;
    }

    const FPwMusicTrack& Track = Score.Tracks[TrackIndex];
    const double BeatsPerBar = PwScoreBeatsPerBar(Score);

    TArray<FPwMusicNote> Built;

    if (Track.Generation.Mode == EPwMusicGenerationMode::Explicit)
    {
        Built = Track.Notes;
        // Checked here too, not only at parse: a score assembled in C++ never went through
        // the parser, and this function is the one door the renderer walks through.
        if (CapExceeded(Built, OutErrorCode, OutError))
        {
            return false;
        }
    }
    else
    {
        int32 DegreeCount = 0;
        if (!PwMusicScaleDegreeCount(Score.Key.Scale, DegreeCount))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutError = FString::Printf(TEXT("the engine's scale table carries no degrees for '%s'."),
                PwMusicScaleToString(Score.Key.Scale));
            return false;
        }

        const TArray<double>* DegreePool = Track.Generation.Params.GetNumbers(FName(TEXT("degrees")));
        if (!DegreePool || DegreePool->Num() == 0)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
            OutError = FString::Printf(TEXT("the '%s' generation mode needs a non-empty degrees pool."),
                PwMusicGenerationModeToString(Track.Generation.Mode));
            return false;
        }

        // Order-independent by construction: a pure hash of the score seed and this track's
        // index, so track 3's stream is identical whether or not tracks 0-2 were materialized.
        FPwSeededRandom Rng = FPwSeededRandom(Score.Seed).Derive(TrackIndex);

        TArray<FSpan> Spans;
        BuildSpans(Score, Spans);

        const int32 Spread = Track.Generation.Params.GetInt(FName(TEXT("octaveSpread")), 0);

        for (const FSpan& Span : Spans)
        {
            const double SpanStartBeat = static_cast<double>(Span.StartBar) * BeatsPerBar;
            const double SpanBeats = static_cast<double>(Span.Bars) * BeatsPerBar;
            const double SpanEndBeat = SpanStartBeat + SpanBeats;

            switch (Track.Generation.Mode)
            {
            case EPwMusicGenerationMode::Drone:
            {
                // No RNG draws at all, so a drone is identical for every seed.
                const double Velocity = RemapVelocity(Span,
                    Track.Generation.Params.GetNumber(FName(TEXT("velocity")), 1.0));
                for (double Degree : *DegreePool)
                {
                    FPwMusicNote Note;
                    Note.StartBeat = SpanStartBeat;
                    Note.DurationBeats = SpanBeats;
                    Note.Degree = FMath::RoundToInt32(Degree);
                    Note.Velocity = Velocity;
                    Built.Add(Note);
                    if (CapExceeded(Built, OutErrorCode, OutError))
                    {
                        return false;
                    }
                }
                break;
            }

            case EPwMusicGenerationMode::EvolvingPad:
            {
                const double SwellMin = Track.Generation.Params.GetNumber(FName(TEXT("swellBeatsMin")));
                const double SwellMax = Track.Generation.Params.GetNumber(FName(TEXT("swellBeatsMax")));
                const double Overlap = Track.Generation.Params.GetNumber(FName(TEXT("overlap")), 0.5);
                const double VelocityMin = Track.Generation.Params.GetNumber(FName(TEXT("velocityMin")), 0.35);
                const double VelocityMax = Track.Generation.Params.GetNumber(FName(TEXT("velocityMax")), 0.85);

                double Cursor = SpanStartBeat;
                while (SpanEndBeat - Cursor > UE_DOUBLE_KINDA_SMALL_NUMBER)
                {
                    // Draw order is fixed, which is what makes the stream reproducible.
                    const double Swell = static_cast<double>(Rng.FloatInRange(
                        static_cast<float>(SwellMin), static_cast<float>(SwellMax)));
                    const int32 PoolIndex = Rng.IntInRange(0, DegreePool->Num() - 1);
                    const int32 OctaveOffset = Spread > 0 ? Rng.IntInRange(-Spread, Spread) : 0;
                    const double Velocity = static_cast<double>(Rng.FloatInRange(
                        static_cast<float>(VelocityMin), static_cast<float>(VelocityMax)));

                    FPwMusicNote Note;
                    Note.StartBeat = Cursor;
                    // Clipped rather than allowed to overhang: a swell ringing past the loop
                    // point is the click a seamless bed exists to avoid.
                    Note.DurationBeats = FMath::Min(Swell, SpanEndBeat - Cursor);
                    Note.Degree = FMath::RoundToInt32((*DegreePool)[PoolIndex]) + OctaveOffset * DegreeCount;
                    Note.Velocity = RemapVelocity(Span, Velocity);
                    Built.Add(Note);
                    if (CapExceeded(Built, OutErrorCode, OutError))
                    {
                        return false;
                    }

                    const double Stride = Swell * (1.0 - Overlap);
                    if (Stride <= UE_DOUBLE_KINDA_SMALL_NUMBER)
                    {
                        OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
                        OutError = FString::Printf(
                            TEXT("swell %g beats at overlap %g advances the cursor by %g beats, which would never reach the end of the section."),
                            Swell, Overlap, Stride);
                        return false;
                    }
                    Cursor += Stride;
                }
                break;
            }

            case EPwMusicGenerationMode::SparseEvents:
            {
                const double EventsPerBar = Track.Generation.Params.GetNumber(FName(TEXT("eventsPerBar")));
                const double DurationMin = Track.Generation.Params.GetNumber(FName(TEXT("durationBeatsMin")));
                const double DurationMax = Track.Generation.Params.GetNumber(FName(TEXT("durationBeatsMax")));
                const double VelocityMin = Track.Generation.Params.GetNumber(FName(TEXT("velocityMin")), 0.25);
                const double VelocityMax = Track.Generation.Params.GetNumber(FName(TEXT("velocityMax")), 0.8);

                const double Expected = EventsPerBar * static_cast<double>(Span.Bars);
                int32 Count = FMath::FloorToInt32(Expected);
                // The draw happens unconditionally so the stream advances identically whether
                // or not the fractional part rounds up - a conditional draw would make the
                // whole rest of the track depend on a coin flip.
                const double FractionRoll = static_cast<double>(Rng.FloatInRange(0.f, 1.f));
                if (FractionRoll < (Expected - static_cast<double>(Count)))
                {
                    ++Count;
                }

                // A span is at least one bar, so this floor is always inside it.
                constexpr double MinNoteBeats = 1.0e-3;
                TArray<FPwMusicNote> SpanNotes;
                SpanNotes.Reserve(Count);
                for (int32 Event = 0; Event < Count; ++Event)
                {
                    const double Offset = static_cast<double>(Rng.FloatInRange(0.f, static_cast<float>(SpanBeats)));
                    const double Duration = static_cast<double>(Rng.FloatInRange(
                        static_cast<float>(DurationMin), static_cast<float>(DurationMax)));
                    const int32 PoolIndex = Rng.IntInRange(0, DegreePool->Num() - 1);
                    const int32 OctaveOffset = Spread > 0 ? Rng.IntInRange(-Spread, Spread) : 0;
                    const double Velocity = static_cast<double>(Rng.FloatInRange(
                        static_cast<float>(VelocityMin), static_cast<float>(VelocityMax)));

                    FPwMusicNote Note;
                    Note.StartBeat = FMath::Min(SpanStartBeat + Offset, SpanEndBeat - MinNoteBeats);
                    Note.DurationBeats = FMath::Min(Duration, SpanEndBeat - Note.StartBeat);
                    Note.Degree = FMath::RoundToInt32((*DegreePool)[PoolIndex]) + OctaveOffset * DegreeCount;
                    Note.Velocity = RemapVelocity(Span, Velocity);
                    SpanNotes.Add(Note);
                }

                // Events are drawn at random points, so they arrive unordered; the schema
                // requires non-decreasing start beats, and a stable sort keeps the ordering a
                // pure function of the draws.
                SpanNotes.StableSort([](const FPwMusicNote& A, const FPwMusicNote& B)
                {
                    return A.StartBeat < B.StartBeat;
                });
                Built.Append(SpanNotes);
                if (CapExceeded(Built, OutErrorCode, OutError))
                {
                    return false;
                }
                break;
            }

            default:
                OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
                OutError = TEXT("the track names no generation mode.");
                return false;
            }
        }
    }

    // Every note this function hands back resolves to a playable pitch. Asserting that here
    // rather than trusting the parse makes the guarantee real for a hand-built score too.
    for (int32 Index = 0; Index < Built.Num(); ++Index)
    {
        int32 Midi = 0;
        FString Code;
        FString Message;
        if (!PwScoreResolveNoteMidi(Score, Track, Built[Index], Midi, Code, Message))
        {
            OutErrorCode = Code;
            OutError = FString::Printf(TEXT("note %d of track '%s': %s"), Index, *Track.Name, *Message);
            return false;
        }
    }

    OutNotes = MoveTemp(Built);
    return true;
}
