// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/WidgetAnimationJsonSerializer.h"

#include "Handlers/UI/WidgetAnimationEventIntrospection.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"

#include "Animation/WidgetAnimation.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/Widget.h"
#include "Internationalization/Text.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
// The MovieSceneText headers moved into Channels/Sections/Tracks subfolders (MovieSceneTracks
// module) in UE 5.7; the old flat paths remain only as deprecation stubs. Prefer the new
// locations when present, fall back to the flat form for UE 5.6 and earlier.
#if __has_include("Channels/MovieSceneTextChannel.h")
#include "Channels/MovieSceneTextChannel.h"
#else
#include "MovieSceneTextChannel.h"
#endif
#if __has_include("Sections/MovieSceneTextSection.h")
#include "Sections/MovieSceneTextSection.h"
#else
#include "MovieSceneTextSection.h"
#endif
#if __has_include("Tracks/MovieSceneTextTrack.h")
#include "Tracks/MovieSceneTextTrack.h"
#else
#include "MovieSceneTextTrack.h"
#endif
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneParameterSection.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "UObject/Class.h"
#include "Utils/MovieSceneJsonUtils.h"
#include "Utils/PropertyUtils.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace
{
    using MovieSceneJsonUtils::MakeFrameRateObject;

    constexpr const TCHAR* SchemaName = TEXT("pinwright.widget-animations.v1");

    FString InterpToString(ERichCurveInterpMode Mode)
    {
        switch (Mode)
        {
        case RCIM_Linear: return TEXT("linear");
        case RCIM_Constant: return TEXT("constant");
        case RCIM_Cubic: return TEXT("cubic");
        default: return TEXT("none");
        }
    }

    ERichCurveInterpMode StringToInterp(const FString& Raw)
    {
        if (Raw.Equals(TEXT("linear"), ESearchCase::IgnoreCase)) return RCIM_Linear;
        if (Raw.Equals(TEXT("constant"), ESearchCase::IgnoreCase)) return RCIM_Constant;
        if (Raw.Equals(TEXT("none"), ESearchCase::IgnoreCase)) return RCIM_None;
        return RCIM_Cubic;
    }

    FString TangentModeToString(ERichCurveTangentMode Mode)
    {
        switch (Mode)
        {
        case RCTM_Auto: return TEXT("auto");
        case RCTM_User: return TEXT("user");
        case RCTM_Break: return TEXT("break");
        case RCTM_SmartAuto: return TEXT("smartAuto");
        default: return TEXT("none");
        }
    }

    ERichCurveTangentMode StringToTangentMode(const FString& Raw)
    {
        if (Raw.Equals(TEXT("user"), ESearchCase::IgnoreCase)) return RCTM_User;
        if (Raw.Equals(TEXT("break"), ESearchCase::IgnoreCase)) return RCTM_Break;
        if (Raw.Equals(TEXT("smartAuto"), ESearchCase::IgnoreCase)) return RCTM_SmartAuto;
        if (Raw.Equals(TEXT("none"), ESearchCase::IgnoreCase)) return RCTM_None;
        return RCTM_Auto;
    }

    FString TangentWeightModeToString(ERichCurveTangentWeightMode Mode)
    {
        switch (Mode)
        {
        case RCTWM_WeightedArrive: return TEXT("arrive");
        case RCTWM_WeightedLeave: return TEXT("leave");
        case RCTWM_WeightedBoth: return TEXT("both");
        default: return TEXT("none");
        }
    }

    ERichCurveTangentWeightMode StringToTangentWeightMode(const FString& Raw)
    {
        if (Raw.Equals(TEXT("arrive"), ESearchCase::IgnoreCase)) return RCTWM_WeightedArrive;
        if (Raw.Equals(TEXT("leave"), ESearchCase::IgnoreCase)) return RCTWM_WeightedLeave;
        if (Raw.Equals(TEXT("both"), ESearchCase::IgnoreCase)) return RCTWM_WeightedBoth;
        return RCTWM_WeightedNone;
    }

    FFrameRate ReadFrameRateObject(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldName,
        const FFrameRate& DefaultRate)
    {
        const TSharedPtr<FJsonObject>* RateObj = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, RateObj) || !RateObj || !RateObj->IsValid())
        {
            return DefaultRate;
        }

        double Numerator = DefaultRate.Numerator;
        double Denominator = DefaultRate.Denominator;
        (*RateObj)->TryGetNumberField(TEXT("numerator"), Numerator);
        (*RateObj)->TryGetNumberField(TEXT("denominator"), Denominator);
        const int32 Num = FMath::Max(1, FMath::RoundToInt(Numerator));
        const int32 Den = FMath::Max(1, FMath::RoundToInt(Denominator));
        return FFrameRate(Num, Den);
    }

    TSharedPtr<FJsonObject> MakeRangeObject(const TRange<FFrameNumber>& Range)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        // An open side is written as an explicit `xBounded: false` so it cannot be mistaken
        // for an omitted field (UMG creates every widget animation section as (-inf, +inf)).
        if (Range.HasLowerBound())
        {
            Obj->SetNumberField(TEXT("startFrame"), Range.GetLowerBoundValue().Value);
        }
        else
        {
            Obj->SetBoolField(TEXT("startBounded"), false);
        }
        if (Range.HasUpperBound())
        {
            Obj->SetNumberField(TEXT("endFrame"), Range.GetUpperBoundValue().Value);
        }
        else
        {
            Obj->SetBoolField(TEXT("endBounded"), false);
        }
        return Obj;
    }

    TRange<FFrameNumber> ReadRangeObject(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldName,
        const TRange<FFrameNumber>& DefaultRange)
    {
        const TSharedPtr<FJsonObject>* RangeObj = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, RangeObj) || !RangeObj || !RangeObj->IsValid())
        {
            return DefaultRange;
        }

        // Each side resolves independently: `xBounded: false` is open, a frame number is
        // [start, end), and an omitted side keeps DefaultRange's bound (open for sections).
        const auto ReadBound = [&RangeObj](const TCHAR* BoundedField, const TCHAR* FrameField, bool bLower,
            const TRangeBound<FFrameNumber>& DefaultBound)
        {
            bool bBounded = true;
            if ((*RangeObj)->TryGetBoolField(BoundedField, bBounded) && !bBounded)
            {
                return TRangeBound<FFrameNumber>::Open();
            }
            double Frame = 0.0;
            if (!(*RangeObj)->TryGetNumberField(FrameField, Frame))
            {
                return DefaultBound;
            }
            const FFrameNumber FrameNumber(FMath::RoundToInt32(Frame));
            return bLower
                ? TRangeBound<FFrameNumber>::Inclusive(FrameNumber)
                : TRangeBound<FFrameNumber>::Exclusive(FrameNumber);
        };
        return TRange<FFrameNumber>(
            ReadBound(TEXT("startBounded"), TEXT("startFrame"), true, DefaultRange.GetLowerBound()),
            ReadBound(TEXT("endBounded"), TEXT("endFrame"), false, DefaultRange.GetUpperBound()));
    }

    TSharedPtr<FJsonObject> MakeTangentObject(const FMovieSceneTangentData& Tangent)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("arrive"), Tangent.ArriveTangent);
        Obj->SetNumberField(TEXT("leave"), Tangent.LeaveTangent);
        Obj->SetNumberField(TEXT("arriveWeight"), Tangent.ArriveTangentWeight);
        Obj->SetNumberField(TEXT("leaveWeight"), Tangent.LeaveTangentWeight);
        Obj->SetStringField(TEXT("weightMode"), TangentWeightModeToString(Tangent.TangentWeightMode));
        return Obj;
    }

    FMovieSceneTangentData ReadTangentObject(const TSharedPtr<FJsonObject>& KeyObj)
    {
        FMovieSceneTangentData Tangent;
        const TSharedPtr<FJsonObject>* TangentObj = nullptr;
        if (!KeyObj.IsValid() || !KeyObj->TryGetObjectField(TEXT("tangent"), TangentObj) || !TangentObj || !TangentObj->IsValid())
        {
            return Tangent;
        }

        double Number = 0.0;
        if ((*TangentObj)->TryGetNumberField(TEXT("arrive"), Number)) Tangent.ArriveTangent = static_cast<float>(Number);
        if ((*TangentObj)->TryGetNumberField(TEXT("leave"), Number)) Tangent.LeaveTangent = static_cast<float>(Number);
        if ((*TangentObj)->TryGetNumberField(TEXT("arriveWeight"), Number)) Tangent.ArriveTangentWeight = static_cast<float>(Number);
        if ((*TangentObj)->TryGetNumberField(TEXT("leaveWeight"), Number)) Tangent.LeaveTangentWeight = static_cast<float>(Number);
        FString WeightMode;
        if ((*TangentObj)->TryGetStringField(TEXT("weightMode"), WeightMode))
        {
            Tangent.TangentWeightMode = StringToTangentWeightMode(WeightMode);
        }
        return Tangent;
    }

    TSharedPtr<FJsonObject> MakeFloatKeyObject(
        FFrameNumber Frame,
        const FMovieSceneFloatValue& Value,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("frame"), Frame.Value);
        Obj->SetNumberField(TEXT("time"), TickResolution.AsSeconds(Frame));
        Obj->SetNumberField(TEXT("value"), Value.Value);
        Obj->SetStringField(TEXT("interp"), InterpToString(Value.InterpMode));
        Obj->SetStringField(TEXT("tangentMode"), TangentModeToString(Value.TangentMode));
        Obj->SetObjectField(TEXT("tangent"), MakeTangentObject(Value.Tangent));
        return Obj;
    }

    FMovieSceneFloatValue ReadFloatKeyValue(const TSharedPtr<FJsonObject>& KeyObj)
    {
        double Number = 0.0;
        KeyObj->TryGetNumberField(TEXT("value"), Number);

        FMovieSceneFloatValue Value(static_cast<float>(Number));
        FString RawMode;
        if (KeyObj->TryGetStringField(TEXT("interp"), RawMode))
        {
            Value.InterpMode = StringToInterp(RawMode);
        }
        if (KeyObj->TryGetStringField(TEXT("tangentMode"), RawMode))
        {
            Value.TangentMode = StringToTangentMode(RawMode);
        }
        Value.Tangent = ReadTangentObject(KeyObj);
        return Value;
    }

    bool ReadFloatKeyFrame(
        const TSharedPtr<FJsonObject>& KeyObj,
        const FFrameRate& TickResolution,
        FFrameNumber& OutFrame,
        FString& OutError)
    {
        double Number = 0.0;
        if (KeyObj->TryGetNumberField(TEXT("frame"), Number))
        {
            OutFrame = FFrameNumber(FMath::RoundToInt32(Number));
            return true;
        }

        if (KeyObj->TryGetNumberField(TEXT("time"), Number))
        {
            OutFrame = TickResolution.AsFrameTime(Number).RoundToFrame();
            return true;
        }

        OutError = TEXT("Each key must include either frame or time.");
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> MakeFloatChannelKeyArray(
        const FMovieSceneFloatChannel& Channel,
        const FFrameRate& TickResolution)
    {
        TArray<TSharedPtr<FJsonValue>> KeyArray;
        TMovieSceneChannelData<const FMovieSceneFloatValue> Data = Channel.GetData();
        const TArrayView<const FFrameNumber> Times = Data.GetTimes();
        const TArrayView<const FMovieSceneFloatValue> Values = Data.GetValues();
        for (int32 KeyIndex = 0; KeyIndex < Times.Num() && KeyIndex < Values.Num(); ++KeyIndex)
        {
            KeyArray.Add(MakeShared<FJsonValueObject>(
                MakeFloatKeyObject(Times[KeyIndex], Values[KeyIndex], TickResolution)));
        }
        return KeyArray;
    }

    bool ReadFloatChannelKeys(
        const TArray<TSharedPtr<FJsonValue>>& Keys,
        const FFrameRate& TickResolution,
        TArray<FFrameNumber>& OutTimes,
        TArray<FMovieSceneFloatValue>& OutValues,
        FString& OutError)
    {
        for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
        {
            const TSharedPtr<FJsonObject>* KeyObj = nullptr;
            if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
            {
                OutError = TEXT("Each key must be an object.");
                return false;
            }

            double Number = 0.0;
            if (!(*KeyObj)->TryGetNumberField(TEXT("value"), Number))
            {
                OutError = TEXT("Each key must include a numeric value.");
                return false;
            }

            FFrameNumber Frame;
            if (!ReadFloatKeyFrame(*KeyObj, TickResolution, Frame, OutError))
            {
                return false;
            }

            OutTimes.Add(Frame);
            OutValues.Add(ReadFloatKeyValue(*KeyObj));
        }
        return true;
    }

    bool SetFloatChannelFromKeys(
        FMovieSceneFloatChannel& Channel,
        const TArray<TSharedPtr<FJsonValue>>& Keys,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        TArray<FFrameNumber> Times;
        TArray<FMovieSceneFloatValue> Values;
        if (!ReadFloatChannelKeys(Keys, TickResolution, Times, Values, OutError))
        {
            return false;
        }
        Channel.Set(Times, Values);
        return true;
    }

    void AddFloatChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FMovieSceneFloatChannel& Channel,
        const FFrameRate& TickResolution)
    {
        Owner->SetArrayField(FieldName, MakeFloatChannelKeyArray(Channel, TickResolution));
    }

    bool ReadFloatChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FFrameRate& TickResolution,
        FMovieSceneFloatChannel& Channel,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
        if (!Owner.IsValid() || !Owner->TryGetArrayField(FieldName, Keys) || !Keys)
        {
            return true;
        }
        return SetFloatChannelFromKeys(Channel, *Keys, TickResolution, OutError);
    }

    bool ValidateFloatChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
        if (!Owner.IsValid() || !Owner->TryGetArrayField(FieldName, Keys) || !Keys)
        {
            return true;
        }

        TArray<FFrameNumber> Times;
        TArray<FMovieSceneFloatValue> Values;
        return ReadFloatChannelKeys(*Keys, TickResolution, Times, Values, OutError);
    }

    TArray<TSharedPtr<FJsonValue>> MakeNameArray(const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FName& Name : Names)
        {
            Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
        }
        return Values;
    }

    bool ReadNameArray(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        TArray<FName>& OutNames,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Owner.IsValid() || !Owner->TryGetArrayField(FieldName, Values) || !Values)
        {
            OutError = FString::Printf(TEXT("Missing required array: %s."), FieldName);
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                OutError = FString::Printf(TEXT("%s must contain only strings."), FieldName);
                return false;
            }
            OutNames.Add(FName(*Value->AsString()));
        }
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Make2DTransformMaskArray(EMovieScene2DTransformChannel Channels)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        auto AddIfPresent = [&Values, Channels](EMovieScene2DTransformChannel Flag, const TCHAR* Name)
        {
            if (EnumHasAllFlags(Channels, Flag))
            {
                Values.Add(MakeShared<FJsonValueString>(Name));
            }
        };

        AddIfPresent(EMovieScene2DTransformChannel::TranslationX, TEXT("translationX"));
        AddIfPresent(EMovieScene2DTransformChannel::TranslationY, TEXT("translationY"));
        AddIfPresent(EMovieScene2DTransformChannel::Rotation, TEXT("rotation"));
        AddIfPresent(EMovieScene2DTransformChannel::ScaleX, TEXT("scaleX"));
        AddIfPresent(EMovieScene2DTransformChannel::ScaleY, TEXT("scaleY"));
        AddIfPresent(EMovieScene2DTransformChannel::ShearX, TEXT("shearX"));
        AddIfPresent(EMovieScene2DTransformChannel::ShearY, TEXT("shearY"));
        return Values;
    }

    FMovieScene2DTransformMask Read2DTransformMask(const TSharedPtr<FJsonObject>& TrackObj)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!TrackObj.IsValid() || !TrackObj->TryGetArrayField(TEXT("mask"), Values) || !Values)
        {
            return FMovieScene2DTransformMask(EMovieScene2DTransformChannel::AllTransform);
        }

        EMovieScene2DTransformChannel Channels = EMovieScene2DTransformChannel::None;
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const FString Name = Value.IsValid() ? Value->AsString() : FString();
            if (Name.Equals(TEXT("translationX"), ESearchCase::IgnoreCase)) Channels |= EMovieScene2DTransformChannel::TranslationX;
            else if (Name.Equals(TEXT("translationY"), ESearchCase::IgnoreCase)) Channels |= EMovieScene2DTransformChannel::TranslationY;
            else if (Name.Equals(TEXT("rotation"), ESearchCase::IgnoreCase)) Channels |= EMovieScene2DTransformChannel::Rotation;
            else if (Name.Equals(TEXT("scaleX"), ESearchCase::IgnoreCase)) Channels |= EMovieScene2DTransformChannel::ScaleX;
            else if (Name.Equals(TEXT("scaleY"), ESearchCase::IgnoreCase)) Channels |= EMovieScene2DTransformChannel::ScaleY;
            else if (Name.Equals(TEXT("shearX"), ESearchCase::IgnoreCase)) Channels |= EMovieScene2DTransformChannel::ShearX;
            else if (Name.Equals(TEXT("shearY"), ESearchCase::IgnoreCase)) Channels |= EMovieScene2DTransformChannel::ShearY;
        }
        return FMovieScene2DTransformMask(Channels);
    }

    FString TextToPersistedString(const FText& Text)
    {
        FString Value;
        FTextStringHelper::WriteToBuffer(Value, Text);
        return Value;
    }

    TSharedPtr<FJsonObject> MakeTextKeyObject(
        FFrameNumber Frame,
        const FText& Value,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("frame"), Frame.Value);
        Obj->SetNumberField(TEXT("time"), TickResolution.AsSeconds(Frame));
        Obj->SetStringField(TEXT("value"), TextToPersistedString(Value));
        Obj->SetStringField(TEXT("string"), Value.ToString());
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> MakeTextChannelKeyArray(
        const FMovieSceneTextChannel& Channel,
        const FFrameRate& TickResolution)
    {
        TArray<TSharedPtr<FJsonValue>> KeyArray;
        TMovieSceneChannelData<const FText> Data = Channel.GetData();
        const TArrayView<const FFrameNumber> Times = Data.GetTimes();
        const TArrayView<const FText> Values = Data.GetValues();
        for (int32 KeyIndex = 0; KeyIndex < Times.Num() && KeyIndex < Values.Num(); ++KeyIndex)
        {
            KeyArray.Add(MakeShared<FJsonValueObject>(
                MakeTextKeyObject(Times[KeyIndex], Values[KeyIndex], TickResolution)));
        }
        return KeyArray;
    }

    bool ReadTextChannelKeys(
        const TArray<TSharedPtr<FJsonValue>>& Keys,
        const FFrameRate& TickResolution,
        TArray<FFrameNumber>& OutTimes,
        TArray<FText>& OutValues,
        FString& OutError)
    {
        for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
        {
            const TSharedPtr<FJsonObject>* KeyObj = nullptr;
            if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
            {
                OutError = TEXT("Each text key must be an object.");
                return false;
            }

            FString TextValue;
            if (!(*KeyObj)->TryGetStringField(TEXT("value"), TextValue))
            {
                OutError = TEXT("Each text key must include a string value.");
                return false;
            }

            FFrameNumber Frame;
            if (!ReadFloatKeyFrame(*KeyObj, TickResolution, Frame, OutError))
            {
                return false;
            }

            FText PersistedText;
            if (!CoerceStringToPersistedFText(TextValue, nullptr, PersistedText, OutError))
            {
                return false;
            }

            OutTimes.Add(Frame);
            OutValues.Add(PersistedText);
        }
        return true;
    }

    bool ValidateTextChannelKeys(
        const TArray<TSharedPtr<FJsonValue>>& Keys,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        TArray<FFrameNumber> Times;
        TArray<FText> Values;
        return ReadTextChannelKeys(Keys, TickResolution, Times, Values, OutError);
    }

    FString GetBindingWidgetName(const UWidgetAnimation* Animation, const FGuid& BindingGuid)
    {
        if (!Animation)
        {
            return FString();
        }

        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.AnimationGuid == BindingGuid)
            {
                return Binding.WidgetName.ToString();
            }
        }
        return FString();
    }

    const FWidgetAnimationBinding* FindAnimationBinding(const UWidgetAnimation* Animation, const FGuid& BindingGuid)
    {
        if (!Animation)
        {
            return nullptr;
        }

        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.AnimationGuid == BindingGuid)
            {
                return &Binding;
            }
        }
        return nullptr;
    }

    using WidgetAuthoringHelpers::FindWidgetByName;
    using WidgetAuthoringHelpers::FindAnimationByName;

    void RemoveAnimationByName(UWidgetBlueprint* WidgetBlueprint, const FString& AnimationName)
    {
        if (!WidgetBlueprint)
        {
            return;
        }

        for (int32 Index = WidgetBlueprint->Animations.Num() - 1; Index >= 0; --Index)
        {
            UWidgetAnimation* Animation = WidgetBlueprint->Animations[Index];
            if (Animation && Animation->GetName().Equals(AnimationName, ESearchCase::IgnoreCase))
            {
                // 5.4 has no WidgetVariableNameToGuidMap; helper guards the version split.
                WidgetAuthoringHelpers::RemoveWidgetVariableGuid(WidgetBlueprint, Animation->GetFName());
                WidgetBlueprint->Animations.RemoveAt(Index);
            }
        }
    }

    void EnsureAnimationVariable(UWidgetBlueprint* WidgetBlueprint, UWidgetAnimation* Animation)
    {
        if (WidgetBlueprint && Animation)
        {
            // 5.4 has no OnVariableAdded; helper guards the version split.
            WidgetAuthoringHelpers::EnsureWidgetVariableGuid(WidgetBlueprint, Animation->GetFName());
        }
    }

    bool ValidateDocumentShape(const TSharedPtr<FJsonObject>& Document, FString& OutError)
    {
        if (!Document.IsValid())
        {
            OutError = TEXT("Animation JSON document must be an object.");
            return false;
        }

        FString Schema;
        if (!Document->TryGetStringField(TEXT("schema"), Schema) || Schema != SchemaName)
        {
            OutError = FString::Printf(TEXT("Animation JSON schema must be '%s'."), SchemaName);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
        if (!Document->TryGetArrayField(TEXT("animations"), Animations))
        {
            OutError = TEXT("Animation JSON document must include an animations array.");
            return false;
        }

        for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
        {
            const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
            if (!AnimationValue.IsValid() || !AnimationValue->TryGetObject(AnimationObj) || !AnimationObj || !AnimationObj->IsValid())
            {
                OutError = TEXT("Each animation entry must be an object.");
                return false;
            }

            FString AnimationName;
            if (!(*AnimationObj)->TryGetStringField(TEXT("name"), AnimationName) || AnimationName.IsEmpty())
            {
                OutError = TEXT("Each animation entry must include a non-empty name.");
                return false;
            }
        }
        return true;
    }

    bool ReadObjectArray(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const TArray<TSharedPtr<FJsonValue>>*& OutArray,
        FString& OutError)
    {
        if (!Owner->TryGetArrayField(FieldName, OutArray))
        {
            OutError = FString::Printf(TEXT("Missing required array: %s."), FieldName);
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeFloatTrackObject(
        UMovieSceneFloatTrack* FloatTrack,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("type"), TEXT("float"));
        TrackObj->SetStringField(TEXT("propertyName"), FloatTrack->GetPropertyName().ToString());
        TrackObj->SetStringField(TEXT("propertyPath"), FloatTrack->GetPropertyPath().ToString());

        TArray<TSharedPtr<FJsonValue>> SectionArray;
        for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
        {
            UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
            if (!FloatSection)
            {
                continue;
            }

            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetObjectField(TEXT("range"), MakeRangeObject(FloatSection->GetRange()));
            SectionObj->SetArrayField(TEXT("keys"), MakeFloatChannelKeyArray(FloatSection->GetChannel(), TickResolution));
            SectionArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);
        return TrackObj;
    }

    TSharedPtr<FJsonObject> Make2DTransformTrackObject(
        UMovieScene2DTransformTrack* TransformTrack,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("type"), TEXT("2dTransform"));
        TrackObj->SetStringField(TEXT("propertyName"), TransformTrack->GetPropertyName().ToString());
        TrackObj->SetStringField(TEXT("propertyPath"), TransformTrack->GetPropertyPath().ToString());

        TArray<TSharedPtr<FJsonValue>> SectionArray;
        for (UMovieSceneSection* Section : TransformTrack->GetAllSections())
        {
            UMovieScene2DTransformSection* TransformSection = Cast<UMovieScene2DTransformSection>(Section);
            if (!TransformSection)
            {
                continue;
            }

            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetObjectField(TEXT("range"), MakeRangeObject(TransformSection->GetRange()));
            SectionObj->SetArrayField(TEXT("mask"), Make2DTransformMaskArray(TransformSection->GetMask().GetChannels()));

            TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
            AddFloatChannelField(ChannelsObj, TEXT("translationX"), TransformSection->Translation[0], TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("translationY"), TransformSection->Translation[1], TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("rotation"), TransformSection->Rotation, TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("scaleX"), TransformSection->Scale[0], TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("scaleY"), TransformSection->Scale[1], TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("shearX"), TransformSection->Shear[0], TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("shearY"), TransformSection->Shear[1], TickResolution);
            SectionObj->SetObjectField(TEXT("channels"), ChannelsObj);

            SectionArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);
        return TrackObj;
    }

    TSharedPtr<FJsonObject> MakeWidgetMaterialTrackObject(
        UMovieSceneWidgetMaterialTrack* MaterialTrack,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("type"), TEXT("widgetMaterial"));
        TrackObj->SetArrayField(TEXT("brushPropertyNamePath"), MakeNameArray(MaterialTrack->GetBrushPropertyNamePath()));

        TArray<TSharedPtr<FJsonValue>> SectionArray;
        for (UMovieSceneSection* Section : MaterialTrack->GetAllSections())
        {
            UMovieSceneParameterSection* ParameterSection = Cast<UMovieSceneParameterSection>(Section);
            if (!ParameterSection)
            {
                continue;
            }

            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetObjectField(TEXT("range"), MakeRangeObject(ParameterSection->GetRange()));

            TArray<TSharedPtr<FJsonValue>> ScalarArray;
            for (const FScalarParameterNameAndCurve& Scalar : ParameterSection->GetScalarParameterNamesAndCurves())
            {
                TSharedPtr<FJsonObject> ScalarObj = MakeShared<FJsonObject>();
                ScalarObj->SetStringField(TEXT("name"), Scalar.ParameterName.ToString());
                ScalarObj->SetArrayField(TEXT("keys"), MakeFloatChannelKeyArray(Scalar.ParameterCurve, TickResolution));
                ScalarArray.Add(MakeShared<FJsonValueObject>(ScalarObj));
            }
            SectionObj->SetArrayField(TEXT("scalarParameters"), ScalarArray);

            TArray<TSharedPtr<FJsonValue>> ColorArray;
            for (const FColorParameterNameAndCurves& Color : ParameterSection->GetColorParameterNamesAndCurves())
            {
                TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
                ColorObj->SetStringField(TEXT("name"), Color.ParameterName.ToString());
                TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
                AddFloatChannelField(ChannelsObj, TEXT("red"), Color.RedCurve, TickResolution);
                AddFloatChannelField(ChannelsObj, TEXT("green"), Color.GreenCurve, TickResolution);
                AddFloatChannelField(ChannelsObj, TEXT("blue"), Color.BlueCurve, TickResolution);
                AddFloatChannelField(ChannelsObj, TEXT("alpha"), Color.AlphaCurve, TickResolution);
                ColorObj->SetObjectField(TEXT("channels"), ChannelsObj);
                ColorArray.Add(MakeShared<FJsonValueObject>(ColorObj));
            }
            SectionObj->SetArrayField(TEXT("colorParameters"), ColorArray);

            SectionArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);
        return TrackObj;
    }

    TSharedPtr<FJsonObject> MakeTextTrackObject(
        UMovieSceneTextTrack* TextTrack,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("type"), TEXT("text"));
        TrackObj->SetStringField(TEXT("propertyName"), TextTrack->GetPropertyName().ToString());
        TrackObj->SetStringField(TEXT("propertyPath"), TextTrack->GetPropertyPath().ToString());

        TArray<TSharedPtr<FJsonValue>> SectionArray;
        for (UMovieSceneSection* Section : TextTrack->GetAllSections())
        {
            UMovieSceneTextSection* TextSection = Cast<UMovieSceneTextSection>(Section);
            if (!TextSection)
            {
                continue;
            }

            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetObjectField(TEXT("range"), MakeRangeObject(TextSection->GetRange()));
            SectionObj->SetArrayField(TEXT("keys"), MakeTextChannelKeyArray(TextSection->GetChannel(), TickResolution));
            SectionArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);
        return TrackObj;
    }


    TArray<TSharedPtr<FJsonValue>> MakeByteChannelKeyArray(
        const FMovieSceneByteChannel& Channel,
        const FFrameRate& TickResolution)
    {
        TArray<TSharedPtr<FJsonValue>> KeyArray;
        TMovieSceneChannelData<const uint8> Data = Channel.GetData();
        const TArrayView<const FFrameNumber> Times = Data.GetTimes();
        const TArrayView<const uint8> Values = Data.GetValues();
        for (int32 KeyIndex = 0; KeyIndex < Times.Num() && KeyIndex < Values.Num(); ++KeyIndex)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetNumberField(TEXT("frame"), Times[KeyIndex].Value);
            Obj->SetNumberField(TEXT("time"), TickResolution.AsSeconds(Times[KeyIndex]));
            Obj->SetNumberField(TEXT("value"), static_cast<int32>(Values[KeyIndex]));
            KeyArray.Add(MakeShared<FJsonValueObject>(Obj));
        }
        return KeyArray;
    }

    bool ReadByteChannelKeys(
        const TArray<TSharedPtr<FJsonValue>>& Keys,
        const FFrameRate& TickResolution,
        TArray<FFrameNumber>& OutTimes,
        TArray<uint8>& OutValues,
        FString& OutError)
    {
        for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
        {
            const TSharedPtr<FJsonObject>* KeyObj = nullptr;
            if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
            {
                OutError = TEXT("Each byte key must be an object.");
                return false;
            }

            double Number = 0.0;
            if (!(*KeyObj)->TryGetNumberField(TEXT("value"), Number))
            {
                OutError = TEXT("Each byte key must include a numeric value.");
                return false;
            }

            FFrameNumber Frame;
            if (!ReadFloatKeyFrame(*KeyObj, TickResolution, Frame, OutError))
            {
                return false;
            }

            OutTimes.Add(Frame);
            OutValues.Add(static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Number), 0, 255)));
        }
        return true;
    }

    bool SetByteChannelFromKeys(
        FMovieSceneByteChannel& Channel,
        const TArray<TSharedPtr<FJsonValue>>& Keys,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        TArray<FFrameNumber> Times;
        TArray<uint8> Values;
        if (!ReadByteChannelKeys(Keys, TickResolution, Times, Values, OutError))
        {
            return false;
        }
        Channel.AddKeys(Times, Values);
        return true;
    }

    void AddByteChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FMovieSceneByteChannel& Channel,
        const FFrameRate& TickResolution)
    {
        Owner->SetArrayField(FieldName, MakeByteChannelKeyArray(Channel, TickResolution));
    }

    bool ReadByteChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FFrameRate& TickResolution,
        FMovieSceneByteChannel& Channel,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
        if (!Owner.IsValid() || !Owner->TryGetArrayField(FieldName, Keys) || !Keys)
        {
            return true;
        }
        return SetByteChannelFromKeys(Channel, *Keys, TickResolution, OutError);
    }

    bool ValidateByteChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
        if (!Owner.IsValid() || !Owner->TryGetArrayField(FieldName, Keys) || !Keys)
        {
            return true;
        }
        TArray<FFrameNumber> Times;
        TArray<uint8> Values;
        return ReadByteChannelKeys(*Keys, TickResolution, Times, Values, OutError);
    }


    TSharedPtr<FJsonObject> MakeDoubleKeyObject(
        FFrameNumber Frame,
        const FMovieSceneDoubleValue& Value,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("frame"), Frame.Value);
        Obj->SetNumberField(TEXT("time"), TickResolution.AsSeconds(Frame));
        Obj->SetNumberField(TEXT("value"), Value.Value);
        Obj->SetStringField(TEXT("interp"), InterpToString(Value.InterpMode));
        Obj->SetStringField(TEXT("tangentMode"), TangentModeToString(Value.TangentMode));
        Obj->SetObjectField(TEXT("tangent"), MakeTangentObject(Value.Tangent));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> MakeDoubleChannelKeyArray(
        const FMovieSceneDoubleChannel& Channel,
        const FFrameRate& TickResolution)
    {
        TArray<TSharedPtr<FJsonValue>> KeyArray;
        TMovieSceneChannelData<const FMovieSceneDoubleValue> Data = Channel.GetData();
        const TArrayView<const FFrameNumber> Times = Data.GetTimes();
        const TArrayView<const FMovieSceneDoubleValue> Values = Data.GetValues();
        for (int32 KeyIndex = 0; KeyIndex < Times.Num() && KeyIndex < Values.Num(); ++KeyIndex)
        {
            KeyArray.Add(MakeShared<FJsonValueObject>(
                MakeDoubleKeyObject(Times[KeyIndex], Values[KeyIndex], TickResolution)));
        }
        return KeyArray;
    }

    bool ReadDoubleChannelKeys(
        const TArray<TSharedPtr<FJsonValue>>& Keys,
        const FFrameRate& TickResolution,
        TArray<FFrameNumber>& OutTimes,
        TArray<FMovieSceneDoubleValue>& OutValues,
        FString& OutError)
    {
        for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
        {
            const TSharedPtr<FJsonObject>* KeyObj = nullptr;
            if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
            {
                OutError = TEXT("Each double key must be an object.");
                return false;
            }

            double Number = 0.0;
            if (!(*KeyObj)->TryGetNumberField(TEXT("value"), Number))
            {
                OutError = TEXT("Each double key must include a numeric value.");
                return false;
            }

            FFrameNumber Frame;
            if (!ReadFloatKeyFrame(*KeyObj, TickResolution, Frame, OutError))
            {
                return false;
            }

            FMovieSceneDoubleValue DoubleValue(Number);
            FString RawMode;
            if ((*KeyObj)->TryGetStringField(TEXT("interp"), RawMode))
            {
                DoubleValue.InterpMode = StringToInterp(RawMode);
            }
            if ((*KeyObj)->TryGetStringField(TEXT("tangentMode"), RawMode))
            {
                DoubleValue.TangentMode = StringToTangentMode(RawMode);
            }
            DoubleValue.Tangent = ReadTangentObject(*KeyObj);

            OutTimes.Add(Frame);
            OutValues.Add(DoubleValue);
        }
        return true;
    }

    bool SetDoubleChannelFromKeys(
        FMovieSceneDoubleChannel& Channel,
        const TArray<TSharedPtr<FJsonValue>>& Keys,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        TArray<FFrameNumber> Times;
        TArray<FMovieSceneDoubleValue> Values;
        if (!ReadDoubleChannelKeys(Keys, TickResolution, Times, Values, OutError))
        {
            return false;
        }
        Channel.Set(Times, Values);
        return true;
    }

    void AddDoubleChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FMovieSceneDoubleChannel& Channel,
        const FFrameRate& TickResolution)
    {
        Owner->SetArrayField(FieldName, MakeDoubleChannelKeyArray(Channel, TickResolution));
    }

    bool ReadDoubleChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FFrameRate& TickResolution,
        FMovieSceneDoubleChannel& Channel,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
        if (!Owner.IsValid() || !Owner->TryGetArrayField(FieldName, Keys) || !Keys)
        {
            return true;
        }
        return SetDoubleChannelFromKeys(Channel, *Keys, TickResolution, OutError);
    }

    bool ValidateDoubleChannelField(
        const TSharedPtr<FJsonObject>& Owner,
        const TCHAR* FieldName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
        if (!Owner.IsValid() || !Owner->TryGetArrayField(FieldName, Keys) || !Keys)
        {
            return true;
        }
        TArray<FFrameNumber> Times;
        TArray<FMovieSceneDoubleValue> Values;
        return ReadDoubleChannelKeys(*Keys, TickResolution, Times, Values, OutError);
    }


    TSharedPtr<FJsonObject> MakeColorTrackObject(
        UMovieSceneColorTrack* ColorTrack,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("type"), TEXT("color"));
        TrackObj->SetStringField(TEXT("propertyName"), ColorTrack->GetPropertyName().ToString());
        TrackObj->SetStringField(TEXT("propertyPath"), ColorTrack->GetPropertyPath().ToString());

        TArray<TSharedPtr<FJsonValue>> SectionArray;
        for (UMovieSceneSection* Section : ColorTrack->GetAllSections())
        {
            UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section);
            if (!ColorSection)
            {
                continue;
            }

            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetObjectField(TEXT("range"), MakeRangeObject(ColorSection->GetRange()));

            TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
            AddFloatChannelField(ChannelsObj, TEXT("red"), ColorSection->GetRedChannel(), TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("green"), ColorSection->GetGreenChannel(), TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("blue"), ColorSection->GetBlueChannel(), TickResolution);
            AddFloatChannelField(ChannelsObj, TEXT("alpha"), ColorSection->GetAlphaChannel(), TickResolution);
            SectionObj->SetObjectField(TEXT("channels"), ChannelsObj);

            SectionArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);
        return TrackObj;
    }

    TSharedPtr<FJsonObject> MakeByteTrackObject(
        UMovieSceneByteTrack* ByteTrack,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("type"), TEXT("byte"));
        TrackObj->SetStringField(TEXT("propertyName"), ByteTrack->GetPropertyName().ToString());
        TrackObj->SetStringField(TEXT("propertyPath"), ByteTrack->GetPropertyPath().ToString());
        if (UEnum* Enum = ByteTrack->GetEnum())
        {
            TrackObj->SetStringField(TEXT("enumPath"), Enum->GetPathName());
        }

        TArray<TSharedPtr<FJsonValue>> SectionArray;
        for (UMovieSceneSection* Section : ByteTrack->GetAllSections())
        {
            UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(Section);
            if (!ByteSection)
            {
                continue;
            }

            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetObjectField(TEXT("range"), MakeRangeObject(ByteSection->GetRange()));
            SectionObj->SetArrayField(TEXT("keys"), MakeByteChannelKeyArray(ByteSection->ByteCurve, TickResolution));
            SectionArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);
        return TrackObj;
    }

    TSharedPtr<FJsonObject> MakeFloatVectorTrackObject(
        UMovieSceneFloatVectorTrack* VectorTrack,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("type"), TEXT("floatVector"));
        TrackObj->SetStringField(TEXT("propertyName"), VectorTrack->GetPropertyName().ToString());
        TrackObj->SetStringField(TEXT("propertyPath"), VectorTrack->GetPropertyPath().ToString());
        const int32 ChannelsUsed = VectorTrack->GetNumChannelsUsed();
        TrackObj->SetNumberField(TEXT("channelsUsed"), ChannelsUsed);

        static const TCHAR* ChannelNames[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };

        TArray<TSharedPtr<FJsonValue>> SectionArray;
        for (UMovieSceneSection* Section : VectorTrack->GetAllSections())
        {
            UMovieSceneFloatVectorSection* VectorSection = Cast<UMovieSceneFloatVectorSection>(Section);
            if (!VectorSection)
            {
                continue;
            }

            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetObjectField(TEXT("range"), MakeRangeObject(VectorSection->GetRange()));

            TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
            for (int32 i = 0; i < ChannelsUsed && i < 4; ++i)
            {
                AddFloatChannelField(ChannelsObj, ChannelNames[i], VectorSection->GetChannel(i), TickResolution);
            }
            SectionObj->SetObjectField(TEXT("channels"), ChannelsObj);

            SectionArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);
        return TrackObj;
    }

    TSharedPtr<FJsonObject> MakeDoubleVectorTrackObject(
        UMovieSceneDoubleVectorTrack* VectorTrack,
        const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("type"), TEXT("doubleVector"));
        TrackObj->SetStringField(TEXT("propertyName"), VectorTrack->GetPropertyName().ToString());
        TrackObj->SetStringField(TEXT("propertyPath"), VectorTrack->GetPropertyPath().ToString());
        const int32 ChannelsUsed = VectorTrack->GetNumChannelsUsed();
        TrackObj->SetNumberField(TEXT("channelsUsed"), ChannelsUsed);

        static const TCHAR* ChannelNames[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };

        TArray<TSharedPtr<FJsonValue>> SectionArray;
        for (UMovieSceneSection* Section : VectorTrack->GetAllSections())
        {
            UMovieSceneDoubleVectorSection* VectorSection = Cast<UMovieSceneDoubleVectorSection>(Section);
            if (!VectorSection)
            {
                continue;
            }

            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetObjectField(TEXT("range"), MakeRangeObject(VectorSection->GetRange()));

            TSharedPtr<FJsonObject> ChannelsObj = MakeShared<FJsonObject>();
            for (int32 i = 0; i < ChannelsUsed && i < 4; ++i)
            {
                AddDoubleChannelField(ChannelsObj, ChannelNames[i], VectorSection->GetChannel(i), TickResolution);
            }
            SectionObj->SetObjectField(TEXT("channels"), ChannelsObj);

            SectionArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);
        return TrackObj;
    }

    bool ValidateFloatTrackObject(
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& AnimationName,
        const FString& WidgetName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        FString PropertyName;
        if (!TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Animation '%s' widget '%s' float track must include propertyName."),
                *AnimationName, *WidgetName);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' float track contains a non-object section."),
                    *AnimationName, *WidgetName);
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (!ReadObjectArray(*SectionObj, TEXT("keys"), Keys, OutError))
            {
                return false;
            }

            TArray<FFrameNumber> Times;
            TArray<FMovieSceneFloatValue> Values;
            if (!ReadFloatChannelKeys(*Keys, TickResolution, Times, Values, OutError))
            {
                return false;
            }
        }
        return true;
    }

    bool Validate2DTransformTrackObject(
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& AnimationName,
        const FString& WidgetName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        FString PropertyName;
        if (!TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Animation '%s' widget '%s' 2dTransform track must include propertyName."),
                *AnimationName, *WidgetName);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' 2dTransform track contains a non-object section."),
                    *AnimationName, *WidgetName);
                return false;
            }

            const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
            if (!(*SectionObj)->TryGetObjectField(TEXT("channels"), ChannelsObj) || !ChannelsObj || !ChannelsObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' 2dTransform section must include channels."),
                    *AnimationName, *WidgetName);
                return false;
            }

            if (!ValidateFloatChannelField(*ChannelsObj, TEXT("translationX"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("translationY"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("rotation"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("scaleX"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("scaleY"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("shearX"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("shearY"), TickResolution, OutError))
            {
                return false;
            }
        }
        return true;
    }

    bool ValidateWidgetMaterialTrackObject(
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& AnimationName,
        const FString& WidgetName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        TArray<FName> BrushPath;
        if (!ReadNameArray(TrackObj, TEXT("brushPropertyNamePath"), BrushPath, OutError))
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' widgetMaterial track contains a non-object section."),
                    *AnimationName, *WidgetName);
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* ScalarParameters = nullptr;
            if ((*SectionObj)->TryGetArrayField(TEXT("scalarParameters"), ScalarParameters) && ScalarParameters)
            {
                for (const TSharedPtr<FJsonValue>& ScalarValue : *ScalarParameters)
                {
                    const TSharedPtr<FJsonObject>* ScalarObj = nullptr;
                    if (!ScalarValue.IsValid() || !ScalarValue->TryGetObject(ScalarObj) || !ScalarObj || !ScalarObj->IsValid())
                    {
                        OutError = TEXT("Each scalar material parameter must be an object.");
                        return false;
                    }
                    FString Name;
                    if (!(*ScalarObj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
                    {
                        OutError = TEXT("Each scalar material parameter must include name.");
                        return false;
                    }
                    const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
                    if (!ReadObjectArray(*ScalarObj, TEXT("keys"), Keys, OutError))
                    {
                        return false;
                    }
                    TArray<FFrameNumber> Times;
                    TArray<FMovieSceneFloatValue> Values;
                    if (!ReadFloatChannelKeys(*Keys, TickResolution, Times, Values, OutError))
                    {
                        return false;
                    }
                }
            }

            const TArray<TSharedPtr<FJsonValue>>* ColorParameters = nullptr;
            if ((*SectionObj)->TryGetArrayField(TEXT("colorParameters"), ColorParameters) && ColorParameters)
            {
                for (const TSharedPtr<FJsonValue>& ColorValue : *ColorParameters)
                {
                    const TSharedPtr<FJsonObject>* ColorObj = nullptr;
                    if (!ColorValue.IsValid() || !ColorValue->TryGetObject(ColorObj) || !ColorObj || !ColorObj->IsValid())
                    {
                        OutError = TEXT("Each color material parameter must be an object.");
                        return false;
                    }
                    FString Name;
                    if (!(*ColorObj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
                    {
                        OutError = TEXT("Each color material parameter must include name.");
                        return false;
                    }
                    const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
                    if (!(*ColorObj)->TryGetObjectField(TEXT("channels"), ChannelsObj) || !ChannelsObj || !ChannelsObj->IsValid())
                    {
                        OutError = TEXT("Each color material parameter must include channels.");
                        return false;
                    }
                    if (!ValidateFloatChannelField(*ChannelsObj, TEXT("red"), TickResolution, OutError) ||
                        !ValidateFloatChannelField(*ChannelsObj, TEXT("green"), TickResolution, OutError) ||
                        !ValidateFloatChannelField(*ChannelsObj, TEXT("blue"), TickResolution, OutError) ||
                        !ValidateFloatChannelField(*ChannelsObj, TEXT("alpha"), TickResolution, OutError))
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool ValidateTextTrackObject(
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& AnimationName,
        const FString& WidgetName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        FString PropertyName;
        if (!TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Animation '%s' widget '%s' text track must include propertyName."),
                *AnimationName, *WidgetName);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' text track contains a non-object section."),
                    *AnimationName, *WidgetName);
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (!ReadObjectArray(*SectionObj, TEXT("keys"), Keys, OutError))
            {
                return false;
            }
            if (!ValidateTextChannelKeys(*Keys, TickResolution, OutError))
            {
                return false;
            }
        }
        return true;
    }

    bool ValidateColorTrackObject(
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& AnimationName,
        const FString& WidgetName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        FString PropertyName;
        if (!TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Animation '%s' widget '%s' color track must include propertyName."),
                *AnimationName, *WidgetName);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' color track contains a non-object section."),
                    *AnimationName, *WidgetName);
                return false;
            }

            const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
            if (!(*SectionObj)->TryGetObjectField(TEXT("channels"), ChannelsObj) || !ChannelsObj || !ChannelsObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' color section must include channels."),
                    *AnimationName, *WidgetName);
                return false;
            }

            if (!ValidateFloatChannelField(*ChannelsObj, TEXT("red"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("green"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("blue"), TickResolution, OutError) ||
                !ValidateFloatChannelField(*ChannelsObj, TEXT("alpha"), TickResolution, OutError))
            {
                return false;
            }
        }
        return true;
    }

    bool ValidateByteTrackObject(
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& AnimationName,
        const FString& WidgetName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        FString PropertyName;
        if (!TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Animation '%s' widget '%s' byte track must include propertyName."),
                *AnimationName, *WidgetName);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' byte track contains a non-object section."),
                    *AnimationName, *WidgetName);
                return false;
            }

            if (!ValidateByteChannelField(*SectionObj, TEXT("keys"), TickResolution, OutError))
            {
                return false;
            }
        }
        return true;
    }

    bool ValidateFloatVectorTrackObject(
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& AnimationName,
        const FString& WidgetName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        FString PropertyName;
        if (!TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Animation '%s' widget '%s' floatVector track must include propertyName."),
                *AnimationName, *WidgetName);
            return false;
        }

        double ChannelsUsedNum = 2.0;
        TrackObj->TryGetNumberField(TEXT("channelsUsed"), ChannelsUsedNum);
        const int32 ChannelsUsed = FMath::Clamp(FMath::RoundToInt(ChannelsUsedNum), 2, 4);

        static const TCHAR* ChannelNames[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' floatVector track contains a non-object section."),
                    *AnimationName, *WidgetName);
                return false;
            }

            const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
            if (!(*SectionObj)->TryGetObjectField(TEXT("channels"), ChannelsObj) || !ChannelsObj || !ChannelsObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' floatVector section must include channels."),
                    *AnimationName, *WidgetName);
                return false;
            }

            for (int32 i = 0; i < ChannelsUsed && i < 4; ++i)
            {
                if (!ValidateFloatChannelField(*ChannelsObj, ChannelNames[i], TickResolution, OutError))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool ValidateDoubleVectorTrackObject(
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& AnimationName,
        const FString& WidgetName,
        const FFrameRate& TickResolution,
        FString& OutError)
    {
        FString PropertyName;
        if (!TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Animation '%s' widget '%s' doubleVector track must include propertyName."),
                *AnimationName, *WidgetName);
            return false;
        }

        double ChannelsUsedNum = 2.0;
        TrackObj->TryGetNumberField(TEXT("channelsUsed"), ChannelsUsedNum);
        const int32 ChannelsUsed = FMath::Clamp(FMath::RoundToInt(ChannelsUsedNum), 2, 4);

        static const TCHAR* ChannelNames[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' doubleVector track contains a non-object section."),
                    *AnimationName, *WidgetName);
                return false;
            }

            const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
            if (!(*SectionObj)->TryGetObjectField(TEXT("channels"), ChannelsObj) || !ChannelsObj || !ChannelsObj->IsValid())
            {
                OutError = FString::Printf(TEXT("Animation '%s' widget '%s' doubleVector section must include channels."),
                    *AnimationName, *WidgetName);
                return false;
            }

            for (int32 i = 0; i < ChannelsUsed && i < 4; ++i)
            {
                if (!ValidateDoubleChannelField(*ChannelsObj, ChannelNames[i], TickResolution, OutError))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool ApplyFloatTrackObject(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& WidgetName,
        FString& OutError)
    {
        FString PropertyName;
        TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName);
        FString PropertyPath;
        if (!TrackObj->TryGetStringField(TEXT("propertyPath"), PropertyPath) || PropertyPath.IsEmpty())
        {
            PropertyPath = PropertyName;
        }

        UMovieSceneFloatTrack* FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
        if (!FloatTrack)
        {
            OutError = FString::Printf(TEXT("Failed to create float track for '%s.%s'."), *WidgetName, *PropertyName);
            return false;
        }
        FloatTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            SectionValue->TryGetObject(SectionObj);

            UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(FloatTrack->CreateNewSection());
            if (!FloatSection)
            {
                OutError = FString::Printf(TEXT("Failed to create float section for '%s.%s'."), *WidgetName, *PropertyName);
                return false;
            }
            FloatSection->SetRange(ReadRangeObject(*SectionObj, TEXT("range"), TRange<FFrameNumber>::All()));

            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (!ReadObjectArray(*SectionObj, TEXT("keys"), Keys, OutError))
            {
                return false;
            }
            if (!SetFloatChannelFromKeys(FloatSection->GetChannel(), *Keys, MovieScene->GetTickResolution(), OutError))
            {
                return false;
            }
            FloatTrack->AddSection(*FloatSection);
        }
        return true;
    }

    bool Apply2DTransformTrackObject(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& WidgetName,
        FString& OutError)
    {
        FString PropertyName;
        TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName);
        FString PropertyPath;
        if (!TrackObj->TryGetStringField(TEXT("propertyPath"), PropertyPath) || PropertyPath.IsEmpty())
        {
            PropertyPath = PropertyName;
        }

        UMovieScene2DTransformTrack* TransformTrack = MovieScene->AddTrack<UMovieScene2DTransformTrack>(BindingGuid);
        if (!TransformTrack)
        {
            OutError = FString::Printf(TEXT("Failed to create 2dTransform track for '%s.%s'."), *WidgetName, *PropertyName);
            return false;
        }
        TransformTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            SectionValue->TryGetObject(SectionObj);

            UMovieScene2DTransformSection* TransformSection = Cast<UMovieScene2DTransformSection>(TransformTrack->CreateNewSection());
            if (!TransformSection)
            {
                OutError = FString::Printf(TEXT("Failed to create 2dTransform section for '%s.%s'."), *WidgetName, *PropertyName);
                return false;
            }
            TransformSection->SetRange(ReadRangeObject(*SectionObj, TEXT("range"), TRange<FFrameNumber>::All()));
            TransformSection->SetMask(Read2DTransformMask(*SectionObj));

            const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
            (*SectionObj)->TryGetObjectField(TEXT("channels"), ChannelsObj);
            if (!ReadFloatChannelField(*ChannelsObj, TEXT("translationX"), MovieScene->GetTickResolution(), TransformSection->Translation[0], OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("translationY"), MovieScene->GetTickResolution(), TransformSection->Translation[1], OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("rotation"), MovieScene->GetTickResolution(), TransformSection->Rotation, OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("scaleX"), MovieScene->GetTickResolution(), TransformSection->Scale[0], OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("scaleY"), MovieScene->GetTickResolution(), TransformSection->Scale[1], OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("shearX"), MovieScene->GetTickResolution(), TransformSection->Shear[0], OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("shearY"), MovieScene->GetTickResolution(), TransformSection->Shear[1], OutError))
            {
                return false;
            }
            TransformTrack->AddSection(*TransformSection);
        }
        return true;
    }

    bool ApplyWidgetMaterialTrackObject(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& WidgetName,
        FString& OutError)
    {
        TArray<FName> BrushPath;
        if (!ReadNameArray(TrackObj, TEXT("brushPropertyNamePath"), BrushPath, OutError))
        {
            return false;
        }

        UMovieSceneWidgetMaterialTrack* MaterialTrack = MovieScene->AddTrack<UMovieSceneWidgetMaterialTrack>(BindingGuid);
        if (!MaterialTrack)
        {
            OutError = FString::Printf(TEXT("Failed to create widgetMaterial track for '%s'."), *WidgetName);
            return false;
        }
        MaterialTrack->SetBrushPropertyNamePath(BrushPath);

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            SectionValue->TryGetObject(SectionObj);

            UMovieSceneParameterSection* ParameterSection = Cast<UMovieSceneParameterSection>(MaterialTrack->CreateNewSection());
            if (!ParameterSection)
            {
                OutError = FString::Printf(TEXT("Failed to create widgetMaterial section for '%s'."), *WidgetName);
                return false;
            }
            ParameterSection->SetRange(ReadRangeObject(*SectionObj, TEXT("range"), TRange<FFrameNumber>::All()));

            const TArray<TSharedPtr<FJsonValue>>* ScalarParameters = nullptr;
            if ((*SectionObj)->TryGetArrayField(TEXT("scalarParameters"), ScalarParameters) && ScalarParameters)
            {
                for (const TSharedPtr<FJsonValue>& ScalarValue : *ScalarParameters)
                {
                    const TSharedPtr<FJsonObject>* ScalarObj = nullptr;
                    ScalarValue->TryGetObject(ScalarObj);
                    FString Name;
                    (*ScalarObj)->TryGetStringField(TEXT("name"), Name);

                    const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
                    if (!ReadObjectArray(*ScalarObj, TEXT("keys"), Keys, OutError))
                    {
                        return false;
                    }

                    FScalarParameterNameAndCurve& Scalar = ParameterSection->GetScalarParameterNamesAndCurves().Add_GetRef(
                        FScalarParameterNameAndCurve(FName(*Name)));
                    if (!SetFloatChannelFromKeys(Scalar.ParameterCurve, *Keys, MovieScene->GetTickResolution(), OutError))
                    {
                        return false;
                    }
                }
            }

            const TArray<TSharedPtr<FJsonValue>>* ColorParameters = nullptr;
            if ((*SectionObj)->TryGetArrayField(TEXT("colorParameters"), ColorParameters) && ColorParameters)
            {
                for (const TSharedPtr<FJsonValue>& ColorValue : *ColorParameters)
                {
                    const TSharedPtr<FJsonObject>* ColorObj = nullptr;
                    ColorValue->TryGetObject(ColorObj);
                    FString Name;
                    (*ColorObj)->TryGetStringField(TEXT("name"), Name);

                    const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
                    (*ColorObj)->TryGetObjectField(TEXT("channels"), ChannelsObj);

                    FColorParameterNameAndCurves& Color = ParameterSection->GetColorParameterNamesAndCurves().Add_GetRef(
                        FColorParameterNameAndCurves(FName(*Name)));
                    if (!ReadFloatChannelField(*ChannelsObj, TEXT("red"), MovieScene->GetTickResolution(), Color.RedCurve, OutError) ||
                        !ReadFloatChannelField(*ChannelsObj, TEXT("green"), MovieScene->GetTickResolution(), Color.GreenCurve, OutError) ||
                        !ReadFloatChannelField(*ChannelsObj, TEXT("blue"), MovieScene->GetTickResolution(), Color.BlueCurve, OutError) ||
                        !ReadFloatChannelField(*ChannelsObj, TEXT("alpha"), MovieScene->GetTickResolution(), Color.AlphaCurve, OutError))
                    {
                        return false;
                    }
                }
            }

            MaterialTrack->AddSection(*ParameterSection);
        }
        return true;
    }

    bool ApplyTextTrackObject(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& WidgetName,
        FString& OutError)
    {
        FString PropertyName;
        TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName);
        FString PropertyPath;
        if (!TrackObj->TryGetStringField(TEXT("propertyPath"), PropertyPath) || PropertyPath.IsEmpty())
        {
            PropertyPath = PropertyName;
        }

        UMovieSceneTextTrack* TextTrack = MovieScene->AddTrack<UMovieSceneTextTrack>(BindingGuid);
        if (!TextTrack)
        {
            OutError = FString::Printf(TEXT("Failed to create text track for '%s.%s'."), *WidgetName, *PropertyName);
            return false;
        }
        TextTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            SectionValue->TryGetObject(SectionObj);

            UMovieSceneTextSection* TextSection = Cast<UMovieSceneTextSection>(TextTrack->CreateNewSection());
            if (!TextSection)
            {
                OutError = FString::Printf(TEXT("Failed to create text section for '%s.%s'."), *WidgetName, *PropertyName);
                return false;
            }
            TextSection->SetRange(ReadRangeObject(*SectionObj, TEXT("range"), TRange<FFrameNumber>::All()));

            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (!ReadObjectArray(*SectionObj, TEXT("keys"), Keys, OutError))
            {
                return false;
            }

            TArray<FFrameNumber> Times;
            TArray<FText> Values;
            if (!ReadTextChannelKeys(*Keys, MovieScene->GetTickResolution(), Times, Values, OutError))
            {
                return false;
            }

            FMovieSceneTextChannel& TextChannel = const_cast<FMovieSceneTextChannel&>(TextSection->GetChannel());
            TextChannel.AddKeys(Times, Values);
            TextTrack->AddSection(*TextSection);
        }
        return true;
    }

    bool ApplyColorTrackObject(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& WidgetName,
        FString& OutError)
    {
        FString PropertyName;
        TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName);
        FString PropertyPath;
        if (!TrackObj->TryGetStringField(TEXT("propertyPath"), PropertyPath) || PropertyPath.IsEmpty())
        {
            PropertyPath = PropertyName;
        }

        UMovieSceneColorTrack* ColorTrack = MovieScene->AddTrack<UMovieSceneColorTrack>(BindingGuid);
        if (!ColorTrack)
        {
            OutError = FString::Printf(TEXT("Failed to create color track for '%s.%s'."), *WidgetName, *PropertyName);
            return false;
        }
        ColorTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            SectionValue->TryGetObject(SectionObj);

            UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(ColorTrack->CreateNewSection());
            if (!ColorSection)
            {
                OutError = FString::Printf(TEXT("Failed to create color section for '%s.%s'."), *WidgetName, *PropertyName);
                return false;
            }
            ColorSection->SetRange(ReadRangeObject(*SectionObj, TEXT("range"), TRange<FFrameNumber>::All()));

            const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
            (*SectionObj)->TryGetObjectField(TEXT("channels"), ChannelsObj);
            if (!ReadFloatChannelField(*ChannelsObj, TEXT("red"), MovieScene->GetTickResolution(), ColorSection->GetRedChannel(), OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("green"), MovieScene->GetTickResolution(), ColorSection->GetGreenChannel(), OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("blue"), MovieScene->GetTickResolution(), ColorSection->GetBlueChannel(), OutError) ||
                !ReadFloatChannelField(*ChannelsObj, TEXT("alpha"), MovieScene->GetTickResolution(), ColorSection->GetAlphaChannel(), OutError))
            {
                return false;
            }
            ColorTrack->AddSection(*ColorSection);
        }
        return true;
    }

    bool ApplyByteTrackObject(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& WidgetName,
        FString& OutError)
    {
        FString PropertyName;
        TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName);
        FString PropertyPath;
        if (!TrackObj->TryGetStringField(TEXT("propertyPath"), PropertyPath) || PropertyPath.IsEmpty())
        {
            PropertyPath = PropertyName;
        }

        UMovieSceneByteTrack* ByteTrack = MovieScene->AddTrack<UMovieSceneByteTrack>(BindingGuid);
        if (!ByteTrack)
        {
            OutError = FString::Printf(TEXT("Failed to create byte track for '%s.%s'."), *WidgetName, *PropertyName);
            return false;
        }
        ByteTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);

        FString EnumPath;
        if (TrackObj->TryGetStringField(TEXT("enumPath"), EnumPath) && !EnumPath.IsEmpty())
        {
            if (UEnum* Enum = FindObject<UEnum>(nullptr, *EnumPath))
            {
                ByteTrack->SetEnum(Enum);
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            SectionValue->TryGetObject(SectionObj);

            UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(ByteTrack->CreateNewSection());
            if (!ByteSection)
            {
                OutError = FString::Printf(TEXT("Failed to create byte section for '%s.%s'."), *WidgetName, *PropertyName);
                return false;
            }
            ByteSection->SetRange(ReadRangeObject(*SectionObj, TEXT("range"), TRange<FFrameNumber>::All()));

            if (!ReadByteChannelField(*SectionObj, TEXT("keys"), MovieScene->GetTickResolution(), ByteSection->ByteCurve, OutError))
            {
                return false;
            }
            ByteTrack->AddSection(*ByteSection);
        }
        return true;
    }

    bool ApplyFloatVectorTrackObject(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& WidgetName,
        FString& OutError)
    {
        FString PropertyName;
        TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName);
        FString PropertyPath;
        if (!TrackObj->TryGetStringField(TEXT("propertyPath"), PropertyPath) || PropertyPath.IsEmpty())
        {
            PropertyPath = PropertyName;
        }

        double ChannelsUsedNum = 2.0;
        TrackObj->TryGetNumberField(TEXT("channelsUsed"), ChannelsUsedNum);
        const int32 ChannelsUsed = FMath::Clamp(FMath::RoundToInt(ChannelsUsedNum), 2, 4);

        static const TCHAR* ChannelNames[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };

        UMovieSceneFloatVectorTrack* VectorTrack = MovieScene->AddTrack<UMovieSceneFloatVectorTrack>(BindingGuid);
        if (!VectorTrack)
        {
            OutError = FString::Printf(TEXT("Failed to create floatVector track for '%s.%s'."), *WidgetName, *PropertyName);
            return false;
        }
        VectorTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);
        VectorTrack->SetNumChannelsUsed(ChannelsUsed);

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            SectionValue->TryGetObject(SectionObj);

            UMovieSceneFloatVectorSection* VectorSection = Cast<UMovieSceneFloatVectorSection>(VectorTrack->CreateNewSection());
            if (!VectorSection)
            {
                OutError = FString::Printf(TEXT("Failed to create floatVector section for '%s.%s'."), *WidgetName, *PropertyName);
                return false;
            }
            VectorSection->SetRange(ReadRangeObject(*SectionObj, TEXT("range"), TRange<FFrameNumber>::All()));
            VectorSection->SetChannelsUsed(ChannelsUsed);

            const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
            (*SectionObj)->TryGetObjectField(TEXT("channels"), ChannelsObj);
            for (int32 i = 0; i < ChannelsUsed && i < 4; ++i)
            {
                FMovieSceneFloatChannel& Ch = const_cast<FMovieSceneFloatChannel&>(VectorSection->GetChannel(i));
                if (!ReadFloatChannelField(*ChannelsObj, ChannelNames[i], MovieScene->GetTickResolution(), Ch, OutError))
                {
                    return false;
                }
            }
            VectorTrack->AddSection(*VectorSection);
        }
        return true;
    }

    bool ApplyDoubleVectorTrackObject(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const TSharedPtr<FJsonObject>& TrackObj,
        const FString& WidgetName,
        FString& OutError)
    {
        FString PropertyName;
        TrackObj->TryGetStringField(TEXT("propertyName"), PropertyName);
        FString PropertyPath;
        if (!TrackObj->TryGetStringField(TEXT("propertyPath"), PropertyPath) || PropertyPath.IsEmpty())
        {
            PropertyPath = PropertyName;
        }

        double ChannelsUsedNum = 2.0;
        TrackObj->TryGetNumberField(TEXT("channelsUsed"), ChannelsUsedNum);
        const int32 ChannelsUsed = FMath::Clamp(FMath::RoundToInt(ChannelsUsedNum), 2, 4);

        static const TCHAR* ChannelNames[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };

        UMovieSceneDoubleVectorTrack* VectorTrack = MovieScene->AddTrack<UMovieSceneDoubleVectorTrack>(BindingGuid);
        if (!VectorTrack)
        {
            OutError = FString::Printf(TEXT("Failed to create doubleVector track for '%s.%s'."), *WidgetName, *PropertyName);
            return false;
        }
        VectorTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);
        VectorTrack->SetNumChannelsUsed(ChannelsUsed);

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!ReadObjectArray(TrackObj, TEXT("sections"), Sections, OutError))
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject>* SectionObj = nullptr;
            SectionValue->TryGetObject(SectionObj);

            UMovieSceneDoubleVectorSection* VectorSection = Cast<UMovieSceneDoubleVectorSection>(VectorTrack->CreateNewSection());
            if (!VectorSection)
            {
                OutError = FString::Printf(TEXT("Failed to create doubleVector section for '%s.%s'."), *WidgetName, *PropertyName);
                return false;
            }
            VectorSection->SetRange(ReadRangeObject(*SectionObj, TEXT("range"), TRange<FFrameNumber>::All()));
            VectorSection->SetChannelsUsed(ChannelsUsed);

            const TSharedPtr<FJsonObject>* ChannelsObj = nullptr;
            (*SectionObj)->TryGetObjectField(TEXT("channels"), ChannelsObj);
            for (int32 i = 0; i < ChannelsUsed && i < 4; ++i)
            {
                FMovieSceneDoubleChannel& Ch = const_cast<FMovieSceneDoubleChannel&>(VectorSection->GetChannel(i));
                if (!ReadDoubleChannelField(*ChannelsObj, ChannelNames[i], MovieScene->GetTickResolution(), Ch, OutError))
                {
                    return false;
                }
            }
            VectorTrack->AddSection(*VectorSection);
        }
        return true;
    }

    bool ValidateImportTargets(UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>& Document, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
        Document->TryGetArrayField(TEXT("animations"), Animations);
        for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
        {
            const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
            AnimationValue->TryGetObject(AnimationObj);

            FString AnimationName;
            (*AnimationObj)->TryGetStringField(TEXT("name"), AnimationName);
            const FFrameRate TickResolution = ReadFrameRateObject(*AnimationObj, TEXT("tickResolution"), FFrameRate(30, 1));

            const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
            if (!ReadObjectArray(*AnimationObj, TEXT("bindings"), Bindings, OutError))
            {
                return false;
            }

            for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
            {
                const TSharedPtr<FJsonObject>* BindingObj = nullptr;
                if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !BindingObj->IsValid())
                {
                    OutError = FString::Printf(TEXT("Animation '%s' contains a non-object binding."), *AnimationName);
                    return false;
                }

                FString WidgetName;
                if (!(*BindingObj)->TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.IsEmpty())
                {
                    OutError = FString::Printf(TEXT("Animation '%s' binding must include widgetName."), *AnimationName);
                    return false;
                }

                FString SlotWidgetName;
                (*BindingObj)->TryGetStringField(TEXT("slotWidgetName"), SlotWidgetName);
                bool bIsRootWidget = false;
                (*BindingObj)->TryGetBoolField(TEXT("isRootWidget"), bIsRootWidget);
                if (!SlotWidgetName.IsEmpty() || bIsRootWidget)
                {
                    OutError = FString::Printf(TEXT("Animation '%s' binding '%s' uses an unsupported v1 binding shape."),
                        *AnimationName, *WidgetName);
                    return false;
                }

                if (!FindWidgetByName(WidgetBlueprint, WidgetName))
                {
                    OutError = FString::Printf(TEXT("Animation '%s' targets missing widget '%s'."), *AnimationName, *WidgetName);
                    return false;
                }

                const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
                if (!ReadObjectArray(*BindingObj, TEXT("tracks"), Tracks, OutError))
                {
                    return false;
                }

                for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
                {
                    const TSharedPtr<FJsonObject>* TrackObj = nullptr;
                    if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !TrackObj->IsValid())
                    {
                        OutError = FString::Printf(TEXT("Animation '%s' widget '%s' contains a non-object track."), *AnimationName, *WidgetName);
                        return false;
                    }

                    FString TrackType;
                    (*TrackObj)->TryGetStringField(TEXT("type"), TrackType);
                    if (TrackType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
                    {
                        if (!ValidateFloatTrackObject(*TrackObj, AnimationName, WidgetName, TickResolution, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("2dTransform"), ESearchCase::IgnoreCase))
                    {
                        if (!Validate2DTransformTrackObject(*TrackObj, AnimationName, WidgetName, TickResolution, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("widgetMaterial"), ESearchCase::IgnoreCase))
                    {
                        if (!ValidateWidgetMaterialTrackObject(*TrackObj, AnimationName, WidgetName, TickResolution, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("text"), ESearchCase::IgnoreCase))
                    {
                        if (!ValidateTextTrackObject(*TrackObj, AnimationName, WidgetName, TickResolution, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("color"), ESearchCase::IgnoreCase))
                    {
                        if (!ValidateColorTrackObject(*TrackObj, AnimationName, WidgetName, TickResolution, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("byte"), ESearchCase::IgnoreCase))
                    {
                        if (!ValidateByteTrackObject(*TrackObj, AnimationName, WidgetName, TickResolution, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("floatVector"), ESearchCase::IgnoreCase))
                    {
                        if (!ValidateFloatVectorTrackObject(*TrackObj, AnimationName, WidgetName, TickResolution, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("doubleVector"), ESearchCase::IgnoreCase))
                    {
                        if (!ValidateDoubleVectorTrackObject(*TrackObj, AnimationName, WidgetName, TickResolution, OutError))
                        {
                            return false;
                        }
                    }
                }
            }
        }

        return true;
    }
}

namespace WidgetAnimationJson
{
    bool ParseImportMode(const FString& RawMode, EImportMode& OutMode, FString& OutError)
    {
        if (RawMode.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
        {
            OutMode = EImportMode::Replace;
            return true;
        }
        if (RawMode.Equals(TEXT("merge"), ESearchCase::IgnoreCase))
        {
            OutMode = EImportMode::Merge;
            return true;
        }

        OutError = TEXT("mode must be 'replace' or 'merge'.");
        return false;
    }

    bool ExportAnimations(
        UWidgetBlueprint* WidgetBlueprint,
        TSharedPtr<FJsonObject>& OutDocument,
        TArray<FString>& OutWarnings,
        FString& OutError,
        bool bIncludeEventMetadata)
    {
        if (!WidgetBlueprint)
        {
            OutError = TEXT("Widget blueprint not found.");
            return false;
        }

        OutDocument = MakeShared<FJsonObject>();
        OutDocument->SetStringField(TEXT("schema"), SchemaName);
        OutDocument->SetStringField(TEXT("widgetPath"), WidgetBlueprint->GetOutermost()->GetName());

        TArray<TSharedPtr<FJsonValue>> AnimationArray;
        for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
        {
            if (!Animation)
            {
                continue;
            }

            UMovieScene* MovieScene = Animation->GetMovieScene();
            if (!MovieScene)
            {
                OutWarnings.Add(FString::Printf(TEXT("Animation '%s' has no MovieScene and was skipped."), *Animation->GetName()));
                continue;
            }

            TSharedPtr<FJsonObject> AnimationObj = MakeShared<FJsonObject>();
            AnimationObj->SetStringField(TEXT("name"), Animation->GetName());
            AnimationObj->SetObjectField(TEXT("displayRate"), MakeFrameRateObject(MovieScene->GetDisplayRate()));
            AnimationObj->SetObjectField(TEXT("tickResolution"), MakeFrameRateObject(MovieScene->GetTickResolution()));
            AnimationObj->SetObjectField(TEXT("playbackRange"), MakeRangeObject(MovieScene->GetPlaybackRange()));

            TArray<TSharedPtr<FJsonValue>> BindingArray;
            for (const FMovieSceneBinding& MovieBinding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
            {
                const FGuid BindingGuid = MovieBinding.GetObjectGuid();
                const FWidgetAnimationBinding* WidgetBinding = FindAnimationBinding(Animation, BindingGuid);
                const FString WidgetName = WidgetBinding ? WidgetBinding->WidgetName.ToString() : GetBindingWidgetName(Animation, BindingGuid);
                if (WidgetName.IsEmpty())
                {
                    OutWarnings.Add(FString::Printf(TEXT("Animation '%s' binding '%s' has no widget name and was skipped."),
                        *Animation->GetName(), *BindingGuid.ToString()));
                    continue;
                }
                TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
                BindingObj->SetStringField(TEXT("widgetName"), WidgetName);
                BindingObj->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
                if (WidgetBinding && WidgetBinding->bIsRootWidget)
                {
                    BindingObj->SetBoolField(TEXT("isRootWidget"), true);
                }
                if (WidgetBinding && !WidgetBinding->SlotWidgetName.IsNone())
                {
                    BindingObj->SetStringField(TEXT("slotWidgetName"), WidgetBinding->SlotWidgetName.ToString());
                }

                TArray<TSharedPtr<FJsonValue>> TrackArray;
                for (UMovieSceneTrack* Track : MovieBinding.GetTracks())
                {
                    if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
                    {
                        TrackArray.Add(MakeShared<FJsonValueObject>(
                            MakeFloatTrackObject(FloatTrack, MovieScene->GetTickResolution())));
                    }
                    else if (UMovieSceneWidgetMaterialTrack* MaterialTrack = Cast<UMovieSceneWidgetMaterialTrack>(Track))
                    {
                        TrackArray.Add(MakeShared<FJsonValueObject>(
                            MakeWidgetMaterialTrackObject(MaterialTrack, MovieScene->GetTickResolution())));
                    }
                    else if (UMovieScene2DTransformTrack* TransformTrack = Cast<UMovieScene2DTransformTrack>(Track))
                    {
                        TrackArray.Add(MakeShared<FJsonValueObject>(
                            Make2DTransformTrackObject(TransformTrack, MovieScene->GetTickResolution())));
                    }
                    else if (UMovieSceneTextTrack* TextTrack = Cast<UMovieSceneTextTrack>(Track))
                    {
                        TrackArray.Add(MakeShared<FJsonValueObject>(
                            MakeTextTrackObject(TextTrack, MovieScene->GetTickResolution())));
                    }
                    else if (UMovieSceneColorTrack* ColorTrack = Cast<UMovieSceneColorTrack>(Track))
                    {
                        TrackArray.Add(MakeShared<FJsonValueObject>(
                            MakeColorTrackObject(ColorTrack, MovieScene->GetTickResolution())));
                    }
                    else if (UMovieSceneByteTrack* ByteTrack = Cast<UMovieSceneByteTrack>(Track))
                    {
                        TrackArray.Add(MakeShared<FJsonValueObject>(
                            MakeByteTrackObject(ByteTrack, MovieScene->GetTickResolution())));
                    }
                    else if (UMovieSceneFloatVectorTrack* FloatVectorTrack = Cast<UMovieSceneFloatVectorTrack>(Track))
                    {
                        TrackArray.Add(MakeShared<FJsonValueObject>(
                            MakeFloatVectorTrackObject(FloatVectorTrack, MovieScene->GetTickResolution())));
                    }
                    else if (UMovieSceneDoubleVectorTrack* DoubleVectorTrack = Cast<UMovieSceneDoubleVectorTrack>(Track))
                    {
                        TrackArray.Add(MakeShared<FJsonValueObject>(
                            MakeDoubleVectorTrackObject(DoubleVectorTrack, MovieScene->GetTickResolution())));
                    }
                    else
                    {
                        OutWarnings.Add(FString::Printf(TEXT("Animation '%s' widget '%s' has unsupported track '%s'."),
                            *Animation->GetName(), *WidgetName, Track ? *Track->GetClass()->GetName() : TEXT("<null>")));
                    }
                }

                BindingObj->SetArrayField(TEXT("tracks"), TrackArray);
                BindingArray.Add(MakeShared<FJsonValueObject>(BindingObj));
            }

            AnimationObj->SetArrayField(TEXT("bindings"), BindingArray);
            if (bIncludeEventMetadata)
            {
                WidgetAnimationEventIntrospection::AppendAnimationEventMetadata(WidgetBlueprint, Animation, AnimationObj);
            }
            AnimationArray.Add(MakeShared<FJsonValueObject>(AnimationObj));
        }

        OutDocument->SetArrayField(TEXT("animations"), AnimationArray);
        if (!OutWarnings.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> WarningValues;
            for (const FString& Warning : OutWarnings)
            {
                WarningValues.Add(MakeShared<FJsonValueString>(Warning));
            }
            OutDocument->SetArrayField(TEXT("warnings"), WarningValues);
        }
        return true;
    }

    bool ParseDocument(const TSharedPtr<FJsonObject>& Document, FString& OutError)
    {
        return ValidateDocumentShape(Document, OutError);
    }

    bool ApplyAnimations(
        UWidgetBlueprint* WidgetBlueprint,
        const TSharedPtr<FJsonObject>& Document,
        EImportMode Mode,
        TArray<FString>& OutWarnings,
        FString& OutError)
    {
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
        {
            OutError = TEXT("Widget blueprint not found or has no WidgetTree.");
            return false;
        }
        if (!ValidateDocumentShape(Document, OutError))
        {
            return false;
        }
        if (!ValidateImportTargets(WidgetBlueprint, Document, OutError))
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
        Document->TryGetArrayField(TEXT("animations"), Animations);
        for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
        {
            const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
            AnimationValue->TryGetObject(AnimationObj);

            FString AnimationName;
            (*AnimationObj)->TryGetStringField(TEXT("name"), AnimationName);
            RemoveAnimationByName(WidgetBlueprint, AnimationName);

            UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(WidgetBlueprint, FName(*AnimationName), RF_Transactional);
            if (!Animation)
            {
                OutError = FString::Printf(TEXT("Failed to create animation '%s'."), *AnimationName);
                return false;
            }

            // Shared with the widget.create_widget_animation path so both mint a MovieScene
            // named after the animation — the equality UWidgetBlueprintGeneratedClass binds
            // the generated animation property on. See EnsureAnimationMovieScene.
            UMovieScene* MovieScene = WidgetAuthoringHelpers::EnsureAnimationMovieScene(Animation).MovieScene;
            if (!MovieScene)
            {
                OutError = FString::Printf(TEXT("Failed to create MovieScene for animation '%s'."), *AnimationName);
                return false;
            }

            MovieScene->SetDisplayRate(ReadFrameRateObject(*AnimationObj, TEXT("displayRate"), FFrameRate(30, 1)));
            MovieScene->SetTickResolutionDirectly(ReadFrameRateObject(*AnimationObj, TEXT("tickResolution"), MovieScene->GetTickResolution()));
            MovieScene->SetPlaybackRange(ReadRangeObject(*AnimationObj, TEXT("playbackRange"), MovieScene->GetPlaybackRange()));

            const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
            if (!ReadObjectArray(*AnimationObj, TEXT("bindings"), Bindings, OutError))
            {
                return false;
            }

            for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
            {
                const TSharedPtr<FJsonObject>* BindingObj = nullptr;
                if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !BindingObj->IsValid())
                {
                    OutError = FString::Printf(TEXT("Animation '%s' contains a non-object binding."), *AnimationName);
                    return false;
                }

                FString WidgetName;
                if (!(*BindingObj)->TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.IsEmpty())
                {
                    OutError = FString::Printf(TEXT("Animation '%s' binding must include widgetName."), *AnimationName);
                    return false;
                }
                FString SlotWidgetName;
                (*BindingObj)->TryGetStringField(TEXT("slotWidgetName"), SlotWidgetName);
                bool bIsRootWidget = false;
                (*BindingObj)->TryGetBoolField(TEXT("isRootWidget"), bIsRootWidget);
                if (!SlotWidgetName.IsEmpty() || bIsRootWidget)
                {
                    OutError = FString::Printf(TEXT("Animation '%s' binding '%s' uses an unsupported v1 binding shape."),
                        *AnimationName, *WidgetName);
                    return false;
                }

                UWidget* TargetWidget = WidgetAuthoringHelpers::FindWidgetByName(WidgetBlueprint, WidgetName);
                if (!TargetWidget)
                {
                    OutError = FString::Printf(TEXT("Animation '%s' targets missing widget '%s'."), *AnimationName, *WidgetName);
                    return false;
                }

                const FGuid BindingGuid = MovieScene->AddPossessable(TargetWidget->GetFName().ToString(), TargetWidget->GetClass());
                if (!BindingGuid.IsValid())
                {
                    OutError = FString::Printf(TEXT("Failed to create MovieScene binding for widget '%s'."), *WidgetName);
                    return false;
                }

                FWidgetAnimationBinding WidgetBinding;
                WidgetBinding.WidgetName = TargetWidget->GetFName();
                WidgetBinding.AnimationGuid = BindingGuid;
                WidgetBinding.SlotWidgetName = NAME_None;
                WidgetBinding.bIsRootWidget = false;
                Animation->AnimationBindings.Add(WidgetBinding);

                const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
                if (!ReadObjectArray(*BindingObj, TEXT("tracks"), Tracks, OutError))
                {
                    return false;
                }

                for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
                {
                    const TSharedPtr<FJsonObject>* TrackObj = nullptr;
                    if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !TrackObj->IsValid())
                    {
                        OutError = FString::Printf(TEXT("Animation '%s' widget '%s' contains a non-object track."), *AnimationName, *WidgetName);
                        return false;
                    }

                    FString TrackType;
                    (*TrackObj)->TryGetStringField(TEXT("type"), TrackType);
                    if (TrackType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
                    {
                        if (!ApplyFloatTrackObject(MovieScene, BindingGuid, *TrackObj, WidgetName, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("widgetMaterial"), ESearchCase::IgnoreCase))
                    {
                        if (!ApplyWidgetMaterialTrackObject(MovieScene, BindingGuid, *TrackObj, WidgetName, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("2dTransform"), ESearchCase::IgnoreCase))
                    {
                        if (!Apply2DTransformTrackObject(MovieScene, BindingGuid, *TrackObj, WidgetName, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("text"), ESearchCase::IgnoreCase))
                    {
                        if (!ApplyTextTrackObject(MovieScene, BindingGuid, *TrackObj, WidgetName, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("color"), ESearchCase::IgnoreCase))
                    {
                        if (!ApplyColorTrackObject(MovieScene, BindingGuid, *TrackObj, WidgetName, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("byte"), ESearchCase::IgnoreCase))
                    {
                        if (!ApplyByteTrackObject(MovieScene, BindingGuid, *TrackObj, WidgetName, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("floatVector"), ESearchCase::IgnoreCase))
                    {
                        if (!ApplyFloatVectorTrackObject(MovieScene, BindingGuid, *TrackObj, WidgetName, OutError))
                        {
                            return false;
                        }
                    }
                    else if (TrackType.Equals(TEXT("doubleVector"), ESearchCase::IgnoreCase))
                    {
                        if (!ApplyDoubleVectorTrackObject(MovieScene, BindingGuid, *TrackObj, WidgetName, OutError))
                        {
                            return false;
                        }
                    }
                    else
                    {
                        OutWarnings.Add(FString::Printf(TEXT("Animation '%s' widget '%s' skipped unsupported track type '%s'."),
                            *AnimationName, *WidgetName, *TrackType));
                    }
                }
            }

            WidgetBlueprint->Animations.Add(Animation);
            EnsureAnimationVariable(WidgetBlueprint, Animation);
        }

        if (Mode == EImportMode::Replace)
        {
            OutWarnings.Add(TEXT("replace mode replaces animations named in the document; unrelated animations are left untouched."));
        }
        return true;
    }
}
