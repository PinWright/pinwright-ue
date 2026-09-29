// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compiler/BpirTimelineText.h"

#include "Compiler/BpirTypes.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "IrCore/IrTextUtils.h"
#include "Utils/GuardedLoad.h"

namespace BpirTimelineTextInternal
{
    struct FToken
    {
        const TCHAR* Text;
        uint8 Value;
    };

    // The first entry of each table is the value a key or template takes when the text omits it.
    const FToken InterpTokens[] = {
        { TEXT("linear"), RCIM_Linear },
        { TEXT("constant"), RCIM_Constant },
        { TEXT("cubic"), RCIM_Cubic },
        { TEXT("none"), RCIM_None },
    };
    const FToken TangentTokens[] = {
        { TEXT("auto"), RCTM_Auto },
        { TEXT("smart_auto"), RCTM_SmartAuto },
        { TEXT("user"), RCTM_User },
        { TEXT("break"), RCTM_Break },
        { TEXT("none"), RCTM_None },
    };
    const FToken WeightTokens[] = {
        { TEXT("none"), RCTWM_WeightedNone },
        { TEXT("arrive"), RCTWM_WeightedArrive },
        { TEXT("leave"), RCTWM_WeightedLeave },
        { TEXT("both"), RCTWM_WeightedBoth },
    };
    const FToken LengthModeTokens[] = {
        { TEXT("timeline_length"), TL_TimelineLength },
        { TEXT("last_keyframe"), TL_LastKeyFrame },
    };
    const FToken TrackKindTokens[] = {
        { TEXT("event_curve"), FTTTrackBase::TT_Event },
        { TEXT("float_curve"), FTTTrackBase::TT_FloatInterp },
        { TEXT("vector_curve"), FTTTrackBase::TT_VectorInterp },
        { TEXT("color_curve"), FTTTrackBase::TT_LinearColorInterp },
    };

    template <SIZE_T N>
    const TCHAR* TokenFor(const FToken (&Table)[N], uint8 Value)
    {
        for (const FToken& Token : Table)
        {
            if (Token.Value == Value)
            {
                return Token.Text;
            }
        }
        return Table[0].Text;
    }

    template <SIZE_T N>
    bool ValueFor(const FToken (&Table)[N], const FString& Text, uint8& OutValue)
    {
        const FString Trimmed = Text.TrimStartAndEnd();
        for (const FToken& Token : Table)
        {
            if (Trimmed.Equals(Token.Text, ESearchCase::IgnoreCase))
            {
                OutValue = Token.Value;
                return true;
            }
        }
        return false;
    }

    template <SIZE_T N>
    FString TokenList(const FToken (&Table)[N])
    {
        TArray<FString> Texts;
        for (const FToken& Token : Table)
        {
            Texts.Add(Token.Text);
        }
        return FString::Join(Texts, TEXT("|"));
    }

    // Shortest of %g / %.9g that parses back to the same float, so text round trips are exact.
    FString FormatFloat(float Value)
    {
        const FString Short = FString::Printf(TEXT("%g"), Value);
        if (static_cast<float>(FCString::Atod(*Short)) == Value)
        {
            return Short;
        }
        return FString::Printf(TEXT("%.9g"), Value);
    }

    // Strict decimal number ([+-]digits[.digits][e[+-]digits]); Atod alone accepts "1abc" as 1.
    bool ParseFloat(const FString& Text, float& OutValue)
    {
        const FString Trimmed = Text.TrimStartAndEnd();
        int32 Index = 0;
        auto SkipDigits = [&Trimmed, &Index]()
        {
            const int32 Start = Index;
            while (Index < Trimmed.Len() && FChar::IsDigit(Trimmed[Index]))
            {
                ++Index;
            }
            return Index - Start;
        };
        auto SkipSign = [&Trimmed, &Index]()
        {
            if (Index < Trimmed.Len() && (Trimmed[Index] == TEXT('+') || Trimmed[Index] == TEXT('-')))
            {
                ++Index;
            }
        };

        SkipSign();
        int32 MantissaDigits = SkipDigits();
        if (Index < Trimmed.Len() && Trimmed[Index] == TEXT('.'))
        {
            ++Index;
            MantissaDigits += SkipDigits();
        }
        if (MantissaDigits == 0)
        {
            return false;
        }
        if (Index < Trimmed.Len() && (Trimmed[Index] == TEXT('e') || Trimmed[Index] == TEXT('E')))
        {
            ++Index;
            SkipSign();
            if (SkipDigits() == 0)
            {
                return false;
            }
        }
        if (Index != Trimmed.Len())
        {
            return false;
        }
        OutValue = static_cast<float>(FCString::Atod(*Trimmed));
        return true;
    }

    // "name(inner)" where the paren closing the first '(' is the last character.
    bool SplitCall(const FString& Text, FString& OutName, FString& OutInner)
    {
        int32 Open = INDEX_NONE;
        if (!Text.FindChar(TEXT('('), Open) || Open == 0
            || FIrTextUtils::FindMatchingChar(Text, Open, TEXT('('), TEXT(')')) != Text.Len() - 1)
        {
            return false;
        }
        OutName = Text.Left(Open).TrimEnd();
        OutInner = Text.Mid(Open + 1, Text.Len() - Open - 2).TrimStartAndEnd();
        return true;
    }

    FString FormatKey(const FRichCurveKey& Key)
    {
        FString Text = FString::Printf(TEXT("(%s, %s"), *FormatFloat(Key.Time), *FormatFloat(Key.Value));
        if (Key.InterpMode == RCIM_Cubic)
        {
            Text += FString::Printf(TEXT(", cubic, %s, %s, %s"),
                TokenFor(TangentTokens, static_cast<uint8>(Key.TangentMode.GetValue())),
                *FormatFloat(Key.ArriveTangent), *FormatFloat(Key.LeaveTangent));
            if (Key.TangentWeightMode != RCTWM_WeightedNone)
            {
                Text += FString::Printf(TEXT(", %s, %s, %s"),
                    TokenFor(WeightTokens, static_cast<uint8>(Key.TangentWeightMode.GetValue())),
                    *FormatFloat(Key.ArriveTangentWeight), *FormatFloat(Key.LeaveTangentWeight));
            }
        }
        else if (Key.InterpMode != RCIM_Linear)
        {
            Text += FString::Printf(TEXT(", %s"), TokenFor(InterpTokens, static_cast<uint8>(Key.InterpMode.GetValue())));
        }
        return Text + TEXT(")");
    }

    FString FormatKeys(const FRichCurve& Curve)
    {
        TArray<FString> Keys;
        for (const FRichCurveKey& Key : Curve.Keys)
        {
            Keys.Add(FormatKey(Key));
        }
        return FString::Join(Keys, TEXT(", "));
    }

    // Channels is null when the track has no curve object. ChannelLetters is null for the
    // single-channel kinds, whose keys sit directly inside the parens.
    FString FormatTrackValue(uint8 Type, bool bExternal, const UCurveBase* Curve,
        const FRichCurve* const* Channels, int32 NumChannels, const TCHAR* ChannelLetters)
    {
        const TCHAR* Kind = TokenFor(TrackKindTokens, Type);
        if (bExternal && Curve)
        {
            return FString::Printf(TEXT("%s(%s)"), Kind, *FIrTextUtils::Quote(Curve->GetPathName()));
        }

        TArray<FString> Parts;
        for (int32 Index = 0; Channels && Index < NumChannels; ++Index)
        {
            if (!ChannelLetters)
            {
                Parts.Add(FormatKeys(*Channels[Index]));
            }
            else if (Channels[Index]->GetNumKeys() > 0)
            {
                Parts.Add(FString::Printf(TEXT("%c(%s)"), ChannelLetters[Index], *FormatKeys(*Channels[Index])));
            }
        }
        return FString::Printf(TEXT("%s(%s)"), Kind, *FString::Join(Parts, TEXT(", ")));
    }

    bool ParseKey(const TArray<FString>& Fields, FRichCurveKey& Key, FString& OutError)
    {
        const int32 Num = Fields.Num();
        if (Num != 2 && Num != 3 && Num != 6 && Num != 9)
        {
            OutError = FString::Printf(
                TEXT("has %d fields; a key is (time, value), (time, value, %s), (time, value, cubic, tangentMode, arrive, leave) or (time, value, cubic, tangentMode, arrive, leave, weightMode, arriveWeight, leaveWeight)"),
                Num, *TokenList(InterpTokens));
            return false;
        }
        if (!ParseFloat(Fields[0], Key.Time) || !ParseFloat(Fields[1], Key.Value))
        {
            OutError = TEXT("time and value must be numbers");
            return false;
        }
        if (Num == 2)
        {
            return true;
        }

        uint8 Mode = 0;
        if (!ValueFor(InterpTokens, Fields[2], Mode))
        {
            OutError = FString::Printf(TEXT("interpolation '%s' is not one of %s"), *Fields[2], *TokenList(InterpTokens));
            return false;
        }
        Key.InterpMode = static_cast<ERichCurveInterpMode>(Mode);
        if (Num == 3)
        {
            return true;
        }

        if (Key.InterpMode != RCIM_Cubic)
        {
            OutError = TEXT("tangents apply only to cubic keys");
            return false;
        }
        if (!ValueFor(TangentTokens, Fields[3], Mode))
        {
            OutError = FString::Printf(TEXT("tangent mode '%s' is not one of %s"), *Fields[3], *TokenList(TangentTokens));
            return false;
        }
        Key.TangentMode = static_cast<ERichCurveTangentMode>(Mode);
        if (!ParseFloat(Fields[4], Key.ArriveTangent) || !ParseFloat(Fields[5], Key.LeaveTangent))
        {
            OutError = TEXT("arrive and leave tangents must be numbers");
            return false;
        }
        if (Num == 6)
        {
            return true;
        }

        if (!ValueFor(WeightTokens, Fields[6], Mode))
        {
            OutError = FString::Printf(TEXT("tangent weight mode '%s' is not one of %s"), *Fields[6], *TokenList(WeightTokens));
            return false;
        }
        Key.TangentWeightMode = static_cast<ERichCurveTangentWeightMode>(Mode);
        if (!ParseFloat(Fields[7], Key.ArriveTangentWeight) || !ParseFloat(Fields[8], Key.LeaveTangentWeight))
        {
            OutError = TEXT("arrive and leave tangent weights must be numbers");
            return false;
        }
        return true;
    }

    bool ParseKeyList(const FString& Inner, TArray<FRichCurveKey>& OutKeys, FString& OutError)
    {
        for (const FString& Part : FIrTextUtils::SmartSplit(Inner, TEXT(',')))
        {
            if (!Part.StartsWith(TEXT("(")) || !Part.EndsWith(TEXT(")")))
            {
                OutError = FString::Printf(TEXT("key '%s' is not a (time, value, ...) tuple"), *Part);
                return false;
            }
            FRichCurveKey Key;
            if (!ParseKey(FIrTextUtils::SmartSplit(Part.Mid(1, Part.Len() - 2), TEXT(',')), Key, OutError))
            {
                OutError = FString::Printf(TEXT("key %s %s"), *Part, *OutError);
                return false;
            }
            OutKeys.Add(Key);
        }
        // FRichCurve::SetKeys keeps the given order, and evaluation assumes keys sorted by time.
        OutKeys.StableSort([](const FRichCurveKey& A, const FRichCurveKey& B) { return A.Time < B.Time; });
        return true;
    }

    bool ParseTrackBody(const FString& Inner, BpirTimelineText::FTrackSpec& Track, FString& OutError)
    {
        if (Inner.StartsWith(TEXT("\"")))
        {
            FString Path;
            FString Error;
            if (!FIrTextUtils::TryUnwrapStringLiteral(Inner, Path, Error))
            {
                OutError = FString::Printf(TEXT("curve asset path %s is malformed: %s"), *Inner, *Error);
                return false;
            }
            UClass* CurveClass = Track.Type == FTTTrackBase::TT_VectorInterp ? UCurveVector::StaticClass()
                : Track.Type == FTTTrackBase::TT_LinearColorInterp ? UCurveLinearColor::StaticClass()
                : UCurveFloat::StaticClass();
            FString Refusal;
            // A miss is reported as this track's compile error, so the engine's load warning is suppressed.
            UCurveBase* Curve = PinWrightGuardedLoad::LoadObjectChecked<UCurveBase>(Path, &Refusal, LOAD_NoWarn | LOAD_Quiet);
            if (!Curve || !Curve->IsA(CurveClass))
            {
                OutError = FString::Printf(TEXT("curve asset '%s' %s"), *Path,
                    !Refusal.IsEmpty() ? *Refusal
                    : Curve ? *FString::Printf(TEXT("is a %s, not a %s"), *Curve->GetClass()->GetName(), *CurveClass->GetName())
                    : TEXT("was not found"));
                return false;
            }
            Track.ExternalCurve = Curve;
            return true;
        }

        if (Track.Type == FTTTrackBase::TT_Event || Track.Type == FTTTrackBase::TT_FloatInterp)
        {
            return ParseKeyList(Inner, Track.Channels[0], OutError);
        }

        const FString Letters = Track.Type == FTTTrackBase::TT_VectorInterp ? TEXT("xyz") : TEXT("rgba");
        bool bSeen[4] = {};
        for (const FString& Part : FIrTextUtils::SmartSplit(Inner, TEXT(',')))
        {
            FString Channel;
            FString Keys;
            const int32 ChannelIndex = SplitCall(Part, Channel, Keys) && Channel.Len() == 1
                ? Letters.Find(Channel, ESearchCase::IgnoreCase)
                : INDEX_NONE;
            if (ChannelIndex == INDEX_NONE)
            {
                OutError = FString::Printf(TEXT("'%s' is not a channel; write %s(<keys>) for each of the channels %s"),
                    *Part, *Letters.Left(1), *Letters);
                return false;
            }
            if (bSeen[ChannelIndex])
            {
                OutError = FString::Printf(TEXT("channel '%s' appears twice"), *Channel);
                return false;
            }
            bSeen[ChannelIndex] = true;
            if (!ParseKeyList(Keys, Track.Channels[ChannelIndex], OutError))
            {
                OutError = FString::Printf(TEXT("channel %s: %s"), *Channel, *OutError);
                return false;
            }
        }
        return true;
    }

    bool ParseSetting(const FString& Name, const FString& Value, BpirTimelineText::FSpec& Spec, FString& OutError)
    {
        auto ParseFlag = [&](TOptional<bool>& OutFlag)
        {
            if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
            {
                OutFlag = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
                return true;
            }
            OutError = FString::Printf(TEXT("setting '%s' takes true or false, got '%s'"), *Name, *Value);
            return false;
        };

        if (Name.Equals(TEXT("length"), ESearchCase::IgnoreCase))
        {
            float Length = 0.f;
            if (!ParseFloat(Value, Length))
            {
                OutError = FString::Printf(TEXT("setting 'length' takes a number of seconds, got '%s'"), *Value);
                return false;
            }
            Spec.Length = Length;
            return true;
        }
        if (Name.Equals(TEXT("length_mode"), ESearchCase::IgnoreCase))
        {
            uint8 Mode = 0;
            if (!ValueFor(LengthModeTokens, Value, Mode))
            {
                OutError = FString::Printf(TEXT("setting 'length_mode' takes %s, got '%s'"), *TokenList(LengthModeTokens), *Value);
                return false;
            }
            Spec.LengthMode = static_cast<ETimelineLengthMode>(Mode);
            return true;
        }
        if (Name.Equals(TEXT("autoplay"), ESearchCase::IgnoreCase)) { return ParseFlag(Spec.bAutoPlay); }
        if (Name.Equals(TEXT("loop"), ESearchCase::IgnoreCase)) { return ParseFlag(Spec.bLoop); }
        if (Name.Equals(TEXT("replicated"), ESearchCase::IgnoreCase)) { return ParseFlag(Spec.bReplicated); }
        if (Name.Equals(TEXT("ignore_time_dilation"), ESearchCase::IgnoreCase)) { return ParseFlag(Spec.bIgnoreTimeDilation); }

        OutError = FString::Printf(
            TEXT("'%s: %s' is neither a setting (length, length_mode, autoplay, loop, replicated, ignore_time_dilation) nor a track (Name: %s(...))"),
            *Name, *Value, *TokenList(TrackKindTokens));
        return false;
    }

    // FRichCurve::SetKeys runs AutoSetTangents, which recomputes RCTM_Auto tangents and also zeroes
    // the tangents of every key beside a constant segment, user and break keys included. Tangents
    // the text states for a non-auto key are authoritative, so they are written back afterwards.
    void SetCurveKeys(FRichCurve& Curve, const TArray<FRichCurveKey>& Keys)
    {
        Curve.SetKeys(Keys);
        for (int32 Index = 0; Index < Keys.Num(); ++Index)
        {
            if (Keys[Index].TangentMode != RCTM_Auto)
            {
                Curve.Keys[Index].ArriveTangent = Keys[Index].ArriveTangent;
                Curve.Keys[Index].LeaveTangent = Keys[Index].LeaveTangent;
            }
        }
    }

    template <typename TCurve>
    TCurve* NewOwnedCurve(UObject* Outer)
    {
        // RF_Public like the Timeline editor: timeline instances in levels reference the curve.
        return NewObject<TCurve>(Outer, NAME_None, RF_Public);
    }
}

namespace BpirTimelineText
{
    FString FormatArgs(UTimelineTemplate* Template)
    {
        using namespace BpirTimelineTextInternal;

        TArray<FString> Args;
        const UTimelineTemplate* Defaults = GetDefault<UTimelineTemplate>();
        if (Template->TimelineLength != Defaults->TimelineLength)
        {
            Args.Add(FString::Printf(TEXT("length: %s"), *FormatFloat(Template->TimelineLength)));
        }
        if (Template->LengthMode.GetValue() != Defaults->LengthMode.GetValue())
        {
            Args.Add(FString::Printf(TEXT("length_mode: %s"), TokenFor(LengthModeTokens, static_cast<uint8>(Template->LengthMode.GetValue()))));
        }
        auto AddFlag = [&Args](const TCHAR* Name, bool bValue, bool bDefault)
        {
            if (bValue != bDefault)
            {
                Args.Add(FString::Printf(TEXT("%s: %s"), Name, bValue ? TEXT("true") : TEXT("false")));
            }
        };
        AddFlag(TEXT("autoplay"), Template->bAutoPlay, Defaults->bAutoPlay);
        AddFlag(TEXT("loop"), Template->bLoop, Defaults->bLoop);
        AddFlag(TEXT("replicated"), Template->bReplicated, Defaults->bReplicated);
        AddFlag(TEXT("ignore_time_dilation"), Template->bIgnoreTimeDilation, Defaults->bIgnoreTimeDilation);

        // Display order is the node's output pin order; a track missing from it is still emitted.
        TArray<FTTTrackId> Order;
        for (int32 Index = 0; Index < Template->GetNumDisplayTracks(); ++Index)
        {
            Order.Add(Template->GetDisplayTrackId(Index));
        }
        auto AddMissing = [&Order](int32 Type, int32 Count)
        {
            for (int32 Index = 0; Index < Count; ++Index)
            {
                if (!Order.ContainsByPredicate([Type, Index](const FTTTrackId& Id) { return Id.TrackType == Type && Id.TrackIndex == Index; }))
                {
                    Order.Add(FTTTrackId(Type, Index));
                }
            }
        };
        AddMissing(FTTTrackBase::TT_Event, Template->EventTracks.Num());
        AddMissing(FTTTrackBase::TT_FloatInterp, Template->FloatTracks.Num());
        AddMissing(FTTTrackBase::TT_VectorInterp, Template->VectorTracks.Num());
        AddMissing(FTTTrackBase::TT_LinearColorInterp, Template->LinearColorTracks.Num());

        for (const FTTTrackId& Id : Order)
        {
            const FTTTrackBase* Track = nullptr;
            FString Value;
            if (Id.TrackType == FTTTrackBase::TT_Event && Template->EventTracks.IsValidIndex(Id.TrackIndex))
            {
                const FTTEventTrack& Event = Template->EventTracks[Id.TrackIndex];
                const FRichCurve* Channels[] = { Event.CurveKeys ? &Event.CurveKeys->FloatCurve : nullptr };
                Track = &Event;
                Value = FormatTrackValue(static_cast<uint8>(Id.TrackType), Event.bIsExternalCurve, Event.CurveKeys,
                    Channels[0] ? Channels : nullptr, 1, nullptr);
            }
            else if (Id.TrackType == FTTTrackBase::TT_FloatInterp && Template->FloatTracks.IsValidIndex(Id.TrackIndex))
            {
                const FTTFloatTrack& Float = Template->FloatTracks[Id.TrackIndex];
                const FRichCurve* Channels[] = { Float.CurveFloat ? &Float.CurveFloat->FloatCurve : nullptr };
                Track = &Float;
                Value = FormatTrackValue(static_cast<uint8>(Id.TrackType), Float.bIsExternalCurve, Float.CurveFloat,
                    Channels[0] ? Channels : nullptr, 1, nullptr);
            }
            else if (Id.TrackType == FTTTrackBase::TT_VectorInterp && Template->VectorTracks.IsValidIndex(Id.TrackIndex))
            {
                const FTTVectorTrack& Vector = Template->VectorTracks[Id.TrackIndex];
                const UCurveVector* Curve = Vector.CurveVector;
                const FRichCurve* Channels[] = { Curve ? &Curve->FloatCurves[0] : nullptr, Curve ? &Curve->FloatCurves[1] : nullptr, Curve ? &Curve->FloatCurves[2] : nullptr };
                Track = &Vector;
                Value = FormatTrackValue(static_cast<uint8>(Id.TrackType), Vector.bIsExternalCurve, Curve,
                    Curve ? Channels : nullptr, 3, TEXT("xyz"));
            }
            else if (Id.TrackType == FTTTrackBase::TT_LinearColorInterp && Template->LinearColorTracks.IsValidIndex(Id.TrackIndex))
            {
                const FTTLinearColorTrack& Color = Template->LinearColorTracks[Id.TrackIndex];
                const UCurveLinearColor* Curve = Color.CurveLinearColor;
                const FRichCurve* Channels[] = { Curve ? &Curve->FloatCurves[0] : nullptr, Curve ? &Curve->FloatCurves[1] : nullptr, Curve ? &Curve->FloatCurves[2] : nullptr, Curve ? &Curve->FloatCurves[3] : nullptr };
                Track = &Color;
                Value = FormatTrackValue(static_cast<uint8>(Id.TrackType), Color.bIsExternalCurve, Curve,
                    Curve ? Channels : nullptr, 4, TEXT("rgba"));
            }

            if (Track)
            {
                Args.Add(FString::Printf(TEXT("%s: %s"), *FIrTextUtils::FormatNameToken(Track->GetTrackName().ToString()), *Value));
            }
        }

        return FString::Join(Args, TEXT(", "));
    }

    bool ParseArgs(const TArray<FBpirArg>& Args, FSpec& OutSpec, FString& OutError)
    {
        using namespace BpirTimelineTextInternal;

        TSet<FName> TrackNames;
        for (const FBpirArg& Arg : Args)
        {
            const FString Value = Arg.Value.TrimStartAndEnd();
            if (Arg.PinName.IsEmpty())
            {
                OutError = FString::Printf(TEXT("argument '%s' has no name; timeline args are 'Setting: value' or 'Track: %s(...)'"),
                    *Value, *TokenList(TrackKindTokens));
                return false;
            }

            FString Kind;
            FString Inner;
            uint8 Type = 0;
            if (!SplitCall(Value, Kind, Inner) || !ValueFor(TrackKindTokens, Kind, Type))
            {
                if (!ParseSetting(Arg.PinName, Value, OutSpec, OutError))
                {
                    return false;
                }
                continue;
            }

            FTrackSpec Track;
            Track.Name = FName(*Arg.PinName);
            Track.Type = static_cast<FTTTrackBase::ETrackType>(Type);
            if (TrackNames.Contains(Track.Name))
            {
                OutError = FString::Printf(TEXT("track '%s' appears twice; track names are unique across all track kinds"), *Arg.PinName);
                return false;
            }
            TrackNames.Add(Track.Name);
            if (!ParseTrackBody(Inner, Track, OutError))
            {
                OutError = FString::Printf(TEXT("track '%s': %s"), *Arg.PinName, *OutError);
                return false;
            }
            OutSpec.Tracks.Add(MoveTemp(Track));
        }
        return true;
    }

    void Apply(const FSpec& Spec, UTimelineTemplate* Template, UObject* CurveOuter)
    {
        using namespace BpirTimelineTextInternal;

        Template->Modify();
        if (Spec.Length.IsSet()) { Template->TimelineLength = Spec.Length.GetValue(); }
        if (Spec.LengthMode.IsSet()) { Template->LengthMode = Spec.LengthMode.GetValue(); }
        if (Spec.bAutoPlay.IsSet()) { Template->bAutoPlay = Spec.bAutoPlay.GetValue(); }
        if (Spec.bLoop.IsSet()) { Template->bLoop = Spec.bLoop.GetValue(); }
        if (Spec.bReplicated.IsSet()) { Template->bReplicated = Spec.bReplicated.GetValue(); }
        if (Spec.bIgnoreTimeDilation.IsSet()) { Template->bIgnoreTimeDilation = Spec.bIgnoreTimeDilation.GetValue(); }

        for (const FTrackSpec& Track : Spec.Tracks)
        {
            const bool bExternal = Track.ExternalCurve != nullptr;
            FTTTrackId Id(Track.Type, 0);
            switch (Track.Type)
            {
            case FTTTrackBase::TT_Event:
            {
                FTTEventTrack NewTrack;
                NewTrack.SetTrackName(Track.Name, Template);
                NewTrack.bIsExternalCurve = bExternal;
                NewTrack.CurveKeys = bExternal ? Cast<UCurveFloat>(Track.ExternalCurve) : NewOwnedCurve<UCurveFloat>(CurveOuter);
                if (!bExternal)
                {
                    NewTrack.CurveKeys->bIsEventCurve = true;
                    SetCurveKeys(NewTrack.CurveKeys->FloatCurve, Track.Channels[0]);
                }
                Id.TrackIndex = Template->EventTracks.Add(NewTrack);
                break;
            }
            case FTTTrackBase::TT_FloatInterp:
            {
                FTTFloatTrack NewTrack;
                NewTrack.SetTrackName(Track.Name, Template);
                NewTrack.bIsExternalCurve = bExternal;
                NewTrack.CurveFloat = bExternal ? Cast<UCurveFloat>(Track.ExternalCurve) : NewOwnedCurve<UCurveFloat>(CurveOuter);
                if (!bExternal)
                {
                    SetCurveKeys(NewTrack.CurveFloat->FloatCurve, Track.Channels[0]);
                }
                Id.TrackIndex = Template->FloatTracks.Add(NewTrack);
                break;
            }
            case FTTTrackBase::TT_VectorInterp:
            {
                FTTVectorTrack NewTrack;
                NewTrack.SetTrackName(Track.Name, Template);
                NewTrack.bIsExternalCurve = bExternal;
                NewTrack.CurveVector = bExternal ? Cast<UCurveVector>(Track.ExternalCurve) : NewOwnedCurve<UCurveVector>(CurveOuter);
                for (int32 Channel = 0; !bExternal && Channel < 3; ++Channel)
                {
                    SetCurveKeys(NewTrack.CurveVector->FloatCurves[Channel], Track.Channels[Channel]);
                }
                Id.TrackIndex = Template->VectorTracks.Add(NewTrack);
                break;
            }
            case FTTTrackBase::TT_LinearColorInterp:
            {
                FTTLinearColorTrack NewTrack;
                NewTrack.SetTrackName(Track.Name, Template);
                NewTrack.bIsExternalCurve = bExternal;
                NewTrack.CurveLinearColor = bExternal ? Cast<UCurveLinearColor>(Track.ExternalCurve) : NewOwnedCurve<UCurveLinearColor>(CurveOuter);
                for (int32 Channel = 0; !bExternal && Channel < 4; ++Channel)
                {
                    SetCurveKeys(NewTrack.CurveLinearColor->FloatCurves[Channel], Track.Channels[Channel]);
                }
                Id.TrackIndex = Template->LinearColorTracks.Add(NewTrack);
                break;
            }
            }
            Template->AddDisplayTrack(Id);
        }
    }
}
