// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Evaluation/Blending/MovieSceneBlendType.h"
#include "Misc/FrameRate.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "MovieSceneObjectBindingID.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneCameraShakeSection.h"
#include "Sections/MovieSceneCameraShakeSourceShakeSection.h"
#include "Sections/MovieSceneCVarSection.h"
#include "Sections/MovieSceneDataLayerSection.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Sections/MovieSceneSubSection.h"
// MovieSceneSkeletalAnimationSection.h only forward-declares UMirrorDataTable, so
// FMovieSceneSkeletalAnimationParams::MirrorDataTable.Get() yields a pointer the compiler
// cannot prove derives from UObject. The full definition is what makes it convertible.
#include "Animation/MirrorDataTable.h"
#include "Materials/MaterialParameterCollection.h"
#include "Sound/SoundBase.h"
#include "Tracks/MovieSceneMaterialParameterCollectionTrack.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Curves/RichCurve.h"
#include "Compat/EngineVersionCompat.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
// UE 5.5+ models sub-section play rate as a time-warp variant; the header is absent in 5.4.
#if __has_include("Variants/MovieSceneTimeWarpVariant.h")
#include "Variants/MovieSceneTimeWarpVariant.h"
#endif

namespace MovieSceneJsonUtils
{
    inline TSharedPtr<FJsonObject> MakeFrameRateObject(const FFrameRate& Rate)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("numerator"), Rate.Numerator);
        Obj->SetNumberField(TEXT("denominator"), Rate.Denominator);
        return Obj;
    }

    inline TSharedPtr<FJsonObject> MakeFrameRangeObject(const TRange<FFrameNumber>& Range)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (Range.HasLowerBound())
        {
            Obj->SetNumberField(TEXT("start"), Range.GetLowerBoundValue().Value);
        }
        if (Range.HasUpperBound())
        {
            Obj->SetNumberField(TEXT("end"), Range.GetUpperBoundValue().Value);
        }
        return Obj;
    }

    inline FString BlendTypeToString(const FOptionalMovieSceneBlendType& Optional)
    {
        if (!Optional.IsValid())
        {
            return FString();
        }
        if (const UEnum* Enum = StaticEnum<EMovieSceneBlendType>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Optional.Get()));
        }
        return FString();
    }

    inline void SetNullableObjectPath(const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldName, const UObject* Value)
    {
        if (Value)
        {
            Obj->SetStringField(FieldName, Value->GetPathName());
        }
        else
        {
            Obj->SetField(FieldName, MakeShared<FJsonValueNull>());
        }
    }

#if __has_include("Variants/MovieSceneTimeWarpVariant.h")
    inline FString TimeWarpTypeToString(EMovieSceneTimeWarpType Type)
    {
        if (const UEnum* Enum = StaticEnum<EMovieSceneTimeWarpType>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Type));
        }
        return FString();
    }
#endif

    // Optional MovieScene plugins are not dependencies of PinWright. These reflection helpers
    // preserve their authored asset pointers without forcing every consumer project to load those
    // plugins just to compile this shared JSON utility.
    inline const FProperty* ResolveSectionPropertyPath(const UObject* Object,
        const TCHAR* PropertyPath, const void*& OutValueAddress)
    {
        TArray<FString> Segments;
        FString(PropertyPath).ParseIntoArray(Segments, TEXT("."), true);
        if (!Object || Segments.Num() == 0)
        {
            return nullptr;
        }

        const UStruct* CurrentStruct = Object->GetClass();
        const void* CurrentContainer = Object;
        for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
        {
            const FProperty* Property = CurrentStruct->FindPropertyByName(FName(*Segments[SegmentIndex]));
            if (!Property)
            {
                return nullptr;
            }

            OutValueAddress = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
            if (SegmentIndex + 1 == Segments.Num())
            {
                return Property;
            }

            const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
            if (!StructProperty)
            {
                return nullptr;
            }
            CurrentStruct = StructProperty->Struct;
            CurrentContainer = OutValueAddress;
        }
        return nullptr;
    }

    inline TSharedPtr<FJsonValue> MakeSectionAssetReferenceValue(const FProperty* Property,
        const void* ValueAddress)
    {
        if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
        {
            const FSoftObjectPath Path = SoftObjectProperty->GetPropertyValue(ValueAddress).ToSoftObjectPath();
            if (Path.IsValid())
            {
                return MakeShared<FJsonValueString>(Path.ToString());
            }
            return MakeShared<FJsonValueNull>();
        }

        if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            if (const UObject* Value = ObjectProperty->GetObjectPropertyValue(ValueAddress))
            {
                return MakeShared<FJsonValueString>(Value->GetPathName());
            }
            return MakeShared<FJsonValueNull>();
        }

        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            if (StructProperty->Struct == TBaseStructure<FSoftObjectPath>::Get())
            {
                const FSoftObjectPath& Path = *static_cast<const FSoftObjectPath*>(ValueAddress);
                if (Path.IsValid())
                {
                    return MakeShared<FJsonValueString>(Path.ToString());
                }
                return MakeShared<FJsonValueNull>();
            }
        }

        return nullptr;
    }

    inline void AddReflectedSectionAssetPath(const TSharedPtr<FJsonObject>& References,
        const UObject* Section, const TCHAR* PropertyPath)
    {
        const void* ValueAddress = nullptr;
        const FProperty* Property = ResolveSectionPropertyPath(Section, PropertyPath, ValueAddress);
        if (!Property || !ValueAddress)
        {
            return;
        }

        if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper ArrayHelper(ArrayProperty, ValueAddress);
            TArray<TSharedPtr<FJsonValue>> Values;
            Values.Reserve(ArrayHelper.Num());
            for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
            {
                if (TSharedPtr<FJsonValue> Value = MakeSectionAssetReferenceValue(
                    ArrayProperty->Inner, ArrayHelper.GetRawPtr(Index)))
                {
                    Values.Add(Value);
                }
            }
            References->SetArrayField(PropertyPath, Values);
            return;
        }

        if (TSharedPtr<FJsonValue> Value = MakeSectionAssetReferenceValue(Property, ValueAddress))
        {
            References->SetField(PropertyPath, Value);
        }
    }

    inline bool IsSectionClassOrDerivedFrom(const UMovieSceneSection* Section,
        const TCHAR* BaseClassName)
    {
        for (const UClass* Class = Section ? Section->GetClass() : nullptr;
            Class;
            Class = Class->GetSuperClass())
        {
            if (Class->GetName() == BaseClassName)
            {
                return true;
            }
        }
        return false;
    }

    inline void AddOptionalSectionAssetReferences(const UMovieSceneSection* Section,
        const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Section)
        {
            return;
        }

        TSharedRef<FJsonObject> References = MakeShared<FJsonObject>();
        auto Add = [&References, Section](const TCHAR* PropertyPath)
        {
            AddReflectedSectionAssetPath(References, Section, PropertyPath);
        };

        if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneControlRigParameterSection")))
        {
            Add(TEXT("ControlRigAssetReference.BlueprintRigClass"));
            Add(TEXT("ControlRigAssetReference.ControlRigAsset"));
            Add(TEXT("ControlRigClass_DEPRECATED"));
            Add(TEXT("OverrideAssets"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneNiagaraCacheSection")))
        {
            Add(TEXT("Params.SimCache"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneChaosCacheSection")))
        {
            Add(TEXT("Params.CacheCollection"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneGeometryCollectionSection")))
        {
            Add(TEXT("Params.GeometryCollectionCache"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneGeometryCacheSection")))
        {
            Add(TEXT("Params.GeometryCacheAsset"));
            Add(TEXT("Params.GeometryCache_DEPRECATED"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneGroomCacheSection")))
        {
            Add(TEXT("Params.GroomCache"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneStitchAnimSection")))
        {
            Add(TEXT("StitchDatabase"));
            Add(TEXT("TargetPoseAsset"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneCinePrestreamingSection")))
        {
            Add(TEXT("PrestreamingAsset"));
            Add(TEXT("SoftPrestreamingAsset"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneSubtitleSection")))
        {
            Add(TEXT("Subtitle"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneMediaSection")))
        {
            Add(TEXT("MediaSource"));
            Add(TEXT("MediaTexture"));
            Add(TEXT("MediaSoundComponent"));
            Add(TEXT("ExternalMediaPlayer"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneMediaPlayerPropertySection")))
        {
            Add(TEXT("MediaSource"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneAudioControlBusSection")))
        {
            Add(TEXT("ControlBus"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneAudioControlBusMixSection")))
        {
            Add(TEXT("MixBus"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneDMXLibrarySection")))
        {
            Add(TEXT("DMXLibrary"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneAnimMixerMaskSection")))
        {
            Add(TEXT("BlendMask"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneAnimInertialDeadBlendTransitionSection")))
        {
            Add(TEXT("CustomBlendCurve"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneSubAssemblySection")))
        {
            Add(TEXT("AssemblyTemplate"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneGameplayCueSection")))
        {
            Add(TEXT("Cue.PhysicalMaterial"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MovieSceneLensComponentSection")))
        {
            Add(TEXT("OverrideLensFile"));
            Add(TEXT("LensModelClass"));
        }
        else if (IsSectionClassOrDerivedFrom(Section, TEXT("MetaHumanPerformanceMovieSceneAudioSection")))
        {
            Add(TEXT("PerformanceShot"));
        }

        if (References->Values.Num() > 0)
        {
            Obj->SetObjectField(TEXT("assetReferences"), References);
        }
    }

    // Normalized interp-mode string for a curve-channel key, matching the vocabulary
    // sequencer.add_keyframe / niagara.set_curve_keys accept on the write side, so a
    // write -> read round-trip round-trips the same tokens (constant|linear|cubic).
    inline FString InterpModeToString(ERichCurveInterpMode Mode)
    {
        switch (Mode)
        {
        case RCIM_Constant: return TEXT("constant");
        case RCIM_Linear:   return TEXT("linear");
        case RCIM_Cubic:    return TEXT("cubic");
        default:            return TEXT("none");
        }
    }

    // Normalized tangent-mode string for a cubic curve-channel key, matching the vocabulary
    // SequencerSectionHelpers::ParseKeyTangentMode accepts on the write side (auto|user|break|none).
    // RCTM_SmartAuto is not writable through that parser but is reachable on assets authored in the
    // Sequencer UI, so it gets its own token rather than being flattened into "none".
    inline FString TangentModeToString(ERichCurveTangentMode Mode)
    {
        switch (Mode)
        {
        case RCTM_Auto:      return TEXT("auto");
        case RCTM_User:      return TEXT("user");
        case RCTM_Break:     return TEXT("break");
        case RCTM_SmartAuto: return TEXT("smartAuto");
        default:             return TEXT("none");
        }
    }

    // Enumerate the per-key {frame, value?, interp?, tangentMode?, arriveTangent?, leaveTangent?}
    // entries for a single channel. For the common scalar
    // channel types (double/float/bool/integer) both the frame and the value are read from the same
    // type-specific GetData() view via GetTimes()/GetValues() — index-aligned by construction, the
    // way the parallel WidgetAnimation serializer does it — so each entry carries {frame, value}.
    // The channel type is resolved ONCE here, not per key. Any other channel type (enum, object
    // path, perspective, ...) has no plain scalar payload, so it falls back to the base-class
    // GetKeys() virtual for frame-only entries. Used to surface authored keyframe data so a
    // write -> read round-trip can verify the exact frames and values.
    inline TArray<TSharedPtr<FJsonValue>> BuildChannelKeysJson(const FName& TypeName, FMovieSceneChannel* Channel)
    {
        TArray<TSharedPtr<FJsonValue>> KeyArr;
        if (!Channel)
        {
            return KeyArr;
        }
        static const FName DoubleType = FMovieSceneDoubleChannel::StaticStruct()->GetFName();
        static const FName FloatType = FMovieSceneFloatChannel::StaticStruct()->GetFName();
        static const FName BoolType = FMovieSceneBoolChannel::StaticStruct()->GetFName();
        static const FName IntType = FMovieSceneIntegerChannel::StaticStruct()->GetFName();

        // Append one {frame, value, interp?} entry per key from a type-specific GetData() view; the
        // times and values arrays are index-aligned, so a single loop reads both in lockstep. DecorateKey
        // adds any per-value-type extra fields (curve channels carry an interp mode; bool/int do not).
        auto AppendScalarKeys = [&KeyArr](const auto& Data, auto MakeValue, auto DecorateKey)
        {
            const TArrayView<const FFrameNumber> Times = Data.GetTimes();
            const auto Values = Data.GetValues();
            KeyArr.Reserve(Times.Num());
            for (int32 KeyIndex = 0; KeyIndex < Times.Num() && KeyIndex < Values.Num(); ++KeyIndex)
            {
                TSharedRef<FJsonObject> KeyObj = MakeShared<FJsonObject>();
                KeyObj->SetNumberField(TEXT("frame"), Times[KeyIndex].Value);
                KeyObj->SetField(TEXT("value"), MakeValue(Values[KeyIndex]));
                DecorateKey(KeyObj, Values[KeyIndex]);
                KeyArr.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
        };

        // Curve channels (float/double) store each key as an FMovieScene*Value carrying an InterpMode,
        // so surface a per-key `interp` token — otherwise an authored constant/linear key is
        // indistinguishable from cubic through readback. bool/int keys are plain scalars with no interp.
        // The tangent mode and the two solved tangent values ride along for the same reason one step
        // deeper: a mode alone is only an intent, and a cubic/auto key whose tangents were never
        // computed reads back identically to a correct one while evaluating as a flat-in/flat-out
        // Hermite (the subject stops dead at the key). Emitting the numbers makes that visible.
        auto DecorateInterp = [](const TSharedRef<FJsonObject>& KeyObj, const auto& V)
        {
            KeyObj->SetStringField(TEXT("interp"), InterpModeToString(V.InterpMode));
            KeyObj->SetStringField(TEXT("tangentMode"), TangentModeToString(V.TangentMode));
            KeyObj->SetNumberField(TEXT("arriveTangent"), V.Tangent.ArriveTangent);
            KeyObj->SetNumberField(TEXT("leaveTangent"), V.Tangent.LeaveTangent);
        };
        auto DecorateNone = [](const TSharedRef<FJsonObject>&, const auto&) {};

        if (TypeName == DoubleType)
        {
            AppendScalarKeys(static_cast<FMovieSceneDoubleChannel*>(Channel)->GetData(),
                [](const FMovieSceneDoubleValue& V) -> TSharedPtr<FJsonValue>
                { return MakeShared<FJsonValueNumber>(V.Value); }, DecorateInterp);
        }
        else if (TypeName == FloatType)
        {
            AppendScalarKeys(static_cast<FMovieSceneFloatChannel*>(Channel)->GetData(),
                [](const FMovieSceneFloatValue& V) -> TSharedPtr<FJsonValue>
                { return MakeShared<FJsonValueNumber>(V.Value); }, DecorateInterp);
        }
        else if (TypeName == BoolType)
        {
            AppendScalarKeys(static_cast<FMovieSceneBoolChannel*>(Channel)->GetData(),
                [](bool V) -> TSharedPtr<FJsonValue>
                { return MakeShared<FJsonValueBoolean>(V); }, DecorateNone);
        }
        else if (TypeName == IntType)
        {
            AppendScalarKeys(static_cast<FMovieSceneIntegerChannel*>(Channel)->GetData(),
                [](int32 V) -> TSharedPtr<FJsonValue>
                { return MakeShared<FJsonValueNumber>(V); }, DecorateNone);
        }
        else
        {
            // Unrecognized channel type: surface frames only via the base-class virtual.
            TArray<FFrameNumber> KeyTimes;
            Channel->GetKeys(TRange<FFrameNumber>::All(), &KeyTimes, nullptr);
            KeyArr.Reserve(KeyTimes.Num());
            for (const FFrameNumber KeyTime : KeyTimes)
            {
                TSharedRef<FJsonObject> KeyObj = MakeShared<FJsonObject>();
                KeyObj->SetNumberField(TEXT("frame"), KeyTime.Value);
                KeyArr.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
        }
        return KeyArr;
    }

    // bIncludeKeys is gated off by default so the standard readback / dump payload stays compact
    // ({type, keyCount} per channel). When on, each channel also carries a keys[] array of
    // {frame, value?} so a keyframe-authoring round-trip is verifiable through readback.
    inline TArray<TSharedPtr<FJsonValue>> BuildChannelEntriesJson(const FMovieSceneChannelProxy& Proxy,
        bool bIncludeKeys = false)
    {
        TArray<TSharedPtr<FJsonValue>> ChannelArr;
        for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
        {
            const FName TypeName = Entry.GetChannelTypeName();
            for (FMovieSceneChannel* const Channel : Entry.GetChannels())
            {
                TSharedRef<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
                ChannelObj->SetStringField(TEXT("type"), TypeName.ToString());
                ChannelObj->SetNumberField(TEXT("keyCount"), Channel ? Channel->GetNumKeys() : 0);
                if (bIncludeKeys)
                {
                    ChannelObj->SetArrayField(TEXT("keys"), BuildChannelKeysJson(TypeName, Channel));
                }
                ChannelArr.Add(MakeShared<FJsonValueObject>(ChannelObj));
            }
        }
        return ChannelArr;
    }

    // Total authored keys across every channel of a section — the same
    // GetAllEntries()/GetNumKeys() walk BuildChannelEntriesJson uses for its per-channel
    // keyCount, summed into one number. A section straight out of CreateNewSection() (e.g. the
    // empty transform section sequencer.add_transform_track makes) has zero keys on all its
    // channels, so this returns 0; used to report an honest keyCount / hasDefaultKeyframes off
    // the real section state instead of a hardcoded constant.
    inline int32 CountSectionKeys(const UMovieSceneSection* Section)
    {
        if (!Section)
        {
            return 0;
        }
        int32 Total = 0;
        for (const FMovieSceneChannelEntry& Entry : Section->GetChannelProxy().GetAllEntries())
        {
            for (FMovieSceneChannel* const Channel : Entry.GetChannels())
            {
                Total += Channel ? Channel->GetNumKeys() : 0;
            }
        }
        return Total;
    }

    // bIncludeKeys forwards to BuildChannelEntriesJson — off by default keeps the section payload at
    // {type, keyCount} per channel; on adds each channel's keys[] of {frame, value?}.
    inline TSharedPtr<FJsonObject> BuildSectionJson(const UMovieSceneSection* Section, bool bIncludeKeys = false)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Section)
        {
            return Obj;
        }
        Obj->SetObjectField(TEXT("range"), MakeFrameRangeObject(Section->GetRange()));
        Obj->SetStringField(TEXT("blendType"), BlendTypeToString(Section->GetBlendType()));
        Obj->SetNumberField(TEXT("rowIndex"), Section->GetRowIndex());
        // Read-back of the lock flag written by sequencer.set_track_locked (Section->SetIsLocked).
        Obj->SetBoolField(TEXT("isLocked"), Section->IsLocked());
        Obj->SetArrayField(TEXT("channels"), BuildChannelEntriesJson(Section->GetChannelProxy(), bIncludeKeys));

        if (const UMovieSceneSkeletalAnimationSection* AnimationSection =
            Cast<UMovieSceneSkeletalAnimationSection>(Section))
        {
            const FMovieSceneSkeletalAnimationParams& Params = AnimationSection->Params;
            SetNullableObjectPath(Obj, TEXT("animationPath"), Params.Animation.Get());
            Obj->SetNumberField(TEXT("firstLoopStartFrameOffset"), Params.FirstLoopStartFrameOffset.Value);
            Obj->SetNumberField(TEXT("startFrameOffset"), Params.StartFrameOffset.Value);
            Obj->SetNumberField(TEXT("endFrameOffset"), Params.EndFrameOffset.Value);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            Obj->SetStringField(TEXT("playRateType"), TimeWarpTypeToString(Params.PlayRate.GetType()));
            if (Params.PlayRate.GetType() == EMovieSceneTimeWarpType::FixedPlayRate)
            {
                Obj->SetNumberField(TEXT("playRate"), Params.PlayRate.AsFixedPlayRate());
            }
            else
            {
                Obj->SetField(TEXT("playRate"), MakeShared<FJsonValueNull>());
            }
#else
            Obj->SetNumberField(TEXT("playRate"), Params.PlayRate);
#endif
            Obj->SetBoolField(TEXT("bReverse"), Params.bReverse);
            Obj->SetStringField(TEXT("slotName"), Params.SlotName.ToString());
            SetNullableObjectPath(Obj, TEXT("mirrorDataTablePath"), Params.MirrorDataTable.Get());
            Obj->SetBoolField(TEXT("bSkipAnimNotifiers"), Params.bSkipAnimNotifiers);
            Obj->SetBoolField(TEXT("bForceCustomMode"), Params.bForceCustomMode);
        }

        if (const UMovieSceneAudioSection* AudioSection = Cast<UMovieSceneAudioSection>(Section))
        {
            SetNullableObjectPath(Obj, TEXT("soundPath"), AudioSection->GetSound());
            SetNullableObjectPath(Obj, TEXT("attenuationSettingsPath"), AudioSection->GetAttenuationSettings());
            Obj->SetNumberField(TEXT("startFrameOffset"), AudioSection->GetStartOffset().Value);
            Obj->SetBoolField(TEXT("looping"), AudioSection->GetLooping());
            // Play Until Finished is a 5.8 audio-section feature; the field is omitted rather
            // than reported false on engines that have no such setting to report.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
            Obj->SetBoolField(TEXT("playUntilFinished"), AudioSection->GetPlayUntilFinished());
#endif
            Obj->SetBoolField(TEXT("suppressSubtitles"), AudioSection->GetSuppressSubtitles());
            Obj->SetBoolField(TEXT("overrideAttenuation"), AudioSection->GetOverrideAttenuation());
        }

        if (const UMovieSceneCameraCutSection* CameraCutSection =
            Cast<UMovieSceneCameraCutSection>(Section))
        {
            const FMovieSceneObjectBindingID& CameraBinding = CameraCutSection->GetCameraBindingID();
            if (CameraBinding.IsValid())
            {
                Obj->SetStringField(TEXT("cameraBindingId"), CameraBinding.GetGuid().ToString());
            }
            else
            {
                Obj->SetField(TEXT("cameraBindingId"), MakeShared<FJsonValueNull>());
            }
        }

        if (const UMovieSceneCameraShakeSection* CameraShakeSection =
            Cast<UMovieSceneCameraShakeSection>(Section))
        {
            SetNullableObjectPath(Obj, TEXT("shakeClassPath"), CameraShakeSection->ShakeData.ShakeClass.Get());
            Obj->SetNumberField(TEXT("playScale"), CameraShakeSection->ShakeData.PlayScale);
        }
        else if (const UMovieSceneCameraShakeSourceShakeSection* SourceShakeSection =
            Cast<UMovieSceneCameraShakeSourceShakeSection>(Section))
        {
            SetNullableObjectPath(Obj, TEXT("shakeClassPath"), SourceShakeSection->ShakeData.ShakeClass.Get());
            Obj->SetNumberField(TEXT("playScale"), SourceShakeSection->ShakeData.PlayScale);
        }

        if (const UMovieSceneDataLayerSection* DataLayerSection =
            Cast<UMovieSceneDataLayerSection>(Section))
        {
            TArray<TSharedPtr<FJsonValue>> DataLayerPaths;
            for (const UDataLayerAsset* DataLayerAsset : DataLayerSection->GetDataLayerAssets())
            {
                if (DataLayerAsset)
                {
                    DataLayerPaths.Add(MakeShared<FJsonValueString>(DataLayerAsset->GetPathName()));
                }
                else
                {
                    DataLayerPaths.Add(MakeShared<FJsonValueNull>());
                }
            }
            Obj->SetArrayField(TEXT("dataLayerAssetPaths"), DataLayerPaths);
        }

        if (const UMovieSceneCVarSection* CVarSection = Cast<UMovieSceneCVarSection>(Section))
        {
            TArray<TSharedPtr<FJsonValue>> CollectionPaths;
            for (const FMovieSceneConsoleVariableCollection& Collection : CVarSection->ConsoleVariableCollections)
            {
                if (const UObject* CollectionAsset = Collection.Interface.GetObject())
                {
                    CollectionPaths.Add(MakeShared<FJsonValueString>(CollectionAsset->GetPathName()));
                }
                else
                {
                    CollectionPaths.Add(MakeShared<FJsonValueNull>());
                }
            }
            Obj->SetArrayField(TEXT("consoleVariableCollectionPaths"), CollectionPaths);
        }

        AddOptionalSectionAssetReferences(Section, Obj);

        if (const UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section))
        {
            if (UMovieSceneSequence* Inner = SubSection->GetSequence())
            {
                Obj->SetStringField(TEXT("innerSequencePath"), Inner->GetPathName());
            }
            else
            {
                Obj->SetField(TEXT("innerSequencePath"), MakeShared<FJsonValueNull>());
            }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            // Parameters.TimeScale became an FMovieSceneTimeWarpVariant in UE 5.5.
            // Only fixed play-rate variants have a single scalar to surface; loop/clamp/custom warps
            // need their own payload shape and are intentionally out of scope here.
            const FMovieSceneTimeWarpVariant& TimeScale = SubSection->Parameters.TimeScale;
            if (TimeScale.GetType() == EMovieSceneTimeWarpType::FixedPlayRate)
            {
                Obj->SetNumberField(TEXT("timeScale"), TimeScale.AsFixedPlayRate());
            }
#else
            // UE 5.4: Parameters.TimeScale is a plain float playback-rate scalar.
            Obj->SetNumberField(TEXT("timeScale"), SubSection->Parameters.TimeScale);
#endif
        }
        return Obj;
    }

    inline TSharedPtr<FJsonObject> BuildTrackJson(const UMovieSceneTrack* Track)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Track)
        {
            return Obj;
        }
        Obj->SetStringField(TEXT("name"), Track->GetTrackName().ToString());
        Obj->SetStringField(TEXT("class"), Track->GetClass()->GetName());
        // Read-back of the eval-disable flag written by sequencer.set_track_muted and the
        // simulated sequencer.set_track_solo (Track->SetEvalDisabled). Unreal has no native
        // solo flag, so isEvalDisabled is the honest representation of mute/solo state.
        Obj->SetBoolField(TEXT("isEvalDisabled"), Track->IsEvalDisabled());

        if (const UMovieSceneMaterialParameterCollectionTrack* MPCTrack =
            Cast<UMovieSceneMaterialParameterCollectionTrack>(Track))
        {
            SetNullableObjectPath(Obj, TEXT("materialParameterCollectionPath"), MPCTrack->MPC.Get());
        }

        const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
        Obj->SetNumberField(TEXT("sectionCount"), Sections.Num());

        TArray<TSharedPtr<FJsonValue>> SectionArr;
        for (const UMovieSceneSection* Section : Sections)
        {
            SectionArr.Add(MakeShared<FJsonValueObject>(BuildSectionJson(Section).ToSharedRef()));
        }
        Obj->SetArrayField(TEXT("sections"), SectionArr);
        return Obj;
    }
}
