// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/AnimSequenceDumpBuilder.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Misc/FrameRate.h"
#include "UObject/Class.h"
#include "Utils/MovieSceneJsonUtils.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"
#include "Compat/EngineVersionCompat.h"

#if UE_VERSION_OLDER_THAN(5, 6, 0)
#include "MovieSceneSequence.h"
#endif

namespace
{
    using MovieSceneJsonUtils::MakeFrameRateObject;

    FString AdditiveTypeToString(EAdditiveAnimationType Type)
    {
        if (const UEnum* Enum = StaticEnum<EAdditiveAnimationType>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Type));
        }
        return FString();
    }

    // On UE < 5.6, the sequencer-backed data model (UAnimationSequencerDataModel) hard-asserts
    // (checkf MovieScene, via ValidateSequencerData()) inside bone-track accessors like
    // GetNumBoneTracks() / GetBoneTrackNames() when it has no MovieScene — the case for a freshly
    // created, un-populated UAnimSequence. 5.6+ rewrote that validation to null-tolerant if-guards.
    // The un-initialized sequencer model IS a UMovieSceneSequence with a null MovieScene, so detect
    // that and let callers skip the asserting accessor. Compiled out (always false) on 5.6+.
    bool IsSequencerDataModelMissingScene(const UAnimSequence* Sequence)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        const UObject* DataModelObject = Sequence->GetDataModelInterface().GetObject();
        const UMovieSceneSequence* SequencerModel = Cast<const UMovieSceneSequence>(DataModelObject);
        return SequencerModel && SequencerModel->GetMovieScene() == nullptr;
#else
        (void)Sequence;
        return false;
#endif
    }

}

TArray<TSharedPtr<FJsonValue>> AnimSequenceDumpBuilder::BuildSyncMarkersArrayJson(const UAnimSequence* Sequence)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    if (!Sequence)
    {
        return Arr;
    }

    struct FMarkerEntry
    {
        FString Name;
        float Time = 0.f;
    };

    TArray<FMarkerEntry> Entries;
    Entries.Reserve(Sequence->AuthoredSyncMarkers.Num());
    for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
    {
        FMarkerEntry Entry;
        Entry.Name = Marker.MarkerName.ToString();
        Entry.Time = Marker.Time;
        Entries.Add(MoveTemp(Entry));
    }

    Entries.Sort([](const FMarkerEntry& A, const FMarkerEntry& B)
    {
        if (A.Time != B.Time)
        {
            return A.Time < B.Time;
        }
        return A.Name < B.Name;
    });

    Arr.Reserve(Entries.Num());
    for (const FMarkerEntry& Entry : Entries)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Entry.Name);
        Obj->SetNumberField(TEXT("time"), Entry.Time);
        Arr.Add(MakeShared<FJsonValueObject>(Obj));
    }
    return Arr;
}

TArray<TSharedPtr<FJsonValue>> AnimSequenceDumpBuilder::BuildNotifiesArrayJson(const UAnimSequenceBase* Sequence)
{
    if (!Sequence)
    {
        return {};
    }

    struct FNotifyEntry
    {
        FString Name;
        float Time = 0.f;
        float Duration = 0.f;
        bool bBranchingPoint = false;
    };

    TArray<FNotifyEntry> Entries;
    Entries.Reserve(Sequence->Notifies.Num());
    for (const FAnimNotifyEvent& Event : Sequence->Notifies)
    {
        FNotifyEntry Entry;
        Entry.Name = Event.NotifyName.ToString();
        Entry.Time = Event.GetTime();
        Entry.Duration = Event.GetDuration();
        Entry.bBranchingPoint = Event.IsBranchingPoint();
        Entries.Add(MoveTemp(Entry));
    }

    Entries.Sort([](const FNotifyEntry& A, const FNotifyEntry& B)
    {
        if (A.Time != B.Time)
        {
            return A.Time < B.Time;
        }
        return A.Name < B.Name;
    });

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Reserve(Entries.Num());
    for (const FNotifyEntry& Entry : Entries)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Entry.Name);
        Obj->SetNumberField(TEXT("time"), Entry.Time);
        Obj->SetNumberField(TEXT("duration"), Entry.Duration);
        Obj->SetBoolField(TEXT("branchingPoint"), Entry.bBranchingPoint);
        Arr.Add(MakeShared<FJsonValueObject>(Obj));
    }
    return Arr;
}

TArray<TSharedPtr<FJsonValue>> AnimSequenceDumpBuilder::BuildCurvesArrayJson(const UAnimSequence* Sequence)
{
    struct FCurveEntry
    {
        FString Name;
        FString Type;
        int32 KeyCount = 0;
    };

    TArray<FCurveEntry> Entries;
    if (!Sequence)
    {
        return {};
    }

    for (const FFloatCurve& Curve : Sequence->GetCurveData().FloatCurves)
    {
        FCurveEntry Entry;
        Entry.Name = Curve.GetName().ToString();
        Entry.Type = TEXT("Float");
        Entry.KeyCount = Curve.FloatCurve.GetNumKeys();
        Entries.Add(MoveTemp(Entry));
    }
    for (const FTransformCurve& Curve : Sequence->GetCurveData().TransformCurves)
    {
        FCurveEntry Entry;
        Entry.Name = Curve.GetName().ToString();
        Entry.Type = TEXT("Transform");
        // total keys across X/Y/Z of translation, rotation, scale
        int32 KeyCount = 0;
        for (int32 Channel = 0; Channel < 3; ++Channel)
        {
            KeyCount += Curve.TranslationCurve.FloatCurves[Channel].GetNumKeys();
            KeyCount += Curve.RotationCurve.FloatCurves[Channel].GetNumKeys();
            KeyCount += Curve.ScaleCurve.FloatCurves[Channel].GetNumKeys();
        }
        Entry.KeyCount = KeyCount;
        Entries.Add(MoveTemp(Entry));
    }

    Entries.Sort([](const FCurveEntry& A, const FCurveEntry& B)
    {
        return A.Name < B.Name;
    });

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Reserve(Entries.Num());
    for (const FCurveEntry& Entry : Entries)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Entry.Name);
        Obj->SetStringField(TEXT("type"), Entry.Type);
        Obj->SetNumberField(TEXT("keyCount"), Entry.KeyCount);
        Arr.Add(MakeShared<FJsonValueObject>(Obj));
    }
    return Arr;
}

TArray<TSharedPtr<FJsonValue>> AnimSequenceDumpBuilder::BuildBoneTracksArrayJson(const UAnimSequence* Sequence)
{
    if (!Sequence)
    {
        return {};
    }

    const IAnimationDataModel* DataModel = Sequence->GetDataModel();
    if (!DataModel)
    {
        return {};
    }

    struct FBoneTrackEntry
    {
        FString BoneName;
        int32 KeyCount = 0;
    };

    TArray<FName> BoneTrackNames;
    if (!IsSequencerDataModelMissingScene(Sequence))
    {
        DataModel->GetBoneTrackNames(BoneTrackNames);
    }

    TArray<FBoneTrackEntry> Entries;
    Entries.Reserve(BoneTrackNames.Num());
    for (const FName& BoneName : BoneTrackNames)
    {
        FBoneTrackEntry Entry;
        Entry.BoneName = BoneName.ToString();
        // GetBoneTrackTransforms (the non-deprecated accessor) fills one FTransform per keyed
        // frame on the track, so the array length is the per-track key count.
        TArray<FTransform> Transforms;
        DataModel->GetBoneTrackTransforms(BoneName, Transforms);
        Entry.KeyCount = Transforms.Num();
        Entries.Add(MoveTemp(Entry));
    }

    Entries.Sort([](const FBoneTrackEntry& A, const FBoneTrackEntry& B)
    {
        return A.BoneName < B.BoneName;
    });

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Reserve(Entries.Num());
    for (const FBoneTrackEntry& Entry : Entries)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("boneName"), Entry.BoneName);
        Obj->SetNumberField(TEXT("keyCount"), Entry.KeyCount);
        Arr.Add(MakeShared<FJsonValueObject>(Obj));
    }
    return Arr;
}

TSharedPtr<FJsonObject> AnimSequenceDumpBuilder::BuildAnimSequenceJson(const UAnimSequence* Sequence)
{
    if (!Sequence)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("assetKind"), TEXT("AnimSequence"));
    Root->SetStringField(TEXT("path"), Sequence->GetPathName());
    Root->SetNumberField(TEXT("lengthSeconds"), Sequence->GetPlayLength());
    Root->SetObjectField(TEXT("frameRate"), MakeFrameRateObject(Sequence->GetSamplingFrameRate()));
    Root->SetNumberField(TEXT("numFrames"), Sequence->GetNumberOfSampledKeys());
    Root->SetBoolField(TEXT("loop"), Sequence->bLoop);
    if (const UEnum* Enum = StaticEnum<EAnimInterpolationType>())
    {
        Root->SetStringField(TEXT("interpolation"),
            Enum->GetNameStringByValue(static_cast<int64>(Sequence->Interpolation)));
    }
    Root->SetStringField(TEXT("additiveType"), AdditiveTypeToString(Sequence->AdditiveAnimType));

    const USkeleton* Skeleton = Sequence->GetSkeleton();
    Root->SetStringField(TEXT("skeletonAssetPath"), Skeleton ? Skeleton->GetPathName() : FString());

    int32 RawTrackCount = 0;
    if (const IAnimationDataModel* DataModel = Sequence->GetDataModel())
    {
        if (!IsSequencerDataModelMissingScene(Sequence))
        {
            RawTrackCount = DataModel->GetNumBoneTracks();
        }
    }
    Root->SetNumberField(TEXT("rawTrackCount"), RawTrackCount);

    Root->SetArrayField(TEXT("notifies"), AnimSequenceDumpBuilder::BuildNotifiesArrayJson(Sequence));
    Root->SetArrayField(TEXT("curves"), AnimSequenceDumpBuilder::BuildCurvesArrayJson(Sequence));
    // Raw bone tracks live in separate storage from curves, so list_curves / the curves[] array
    // never surface the keyed bones. boneTracks[] carries one {boneName,keyCount} per keyed bone,
    // the structured cross-check the inspect-after-mutate loop needs alongside the aggregate
    // rawTrackCount above.
    Root->SetArrayField(TEXT("boneTracks"), AnimSequenceDumpBuilder::BuildBoneTracksArrayJson(Sequence));
    Root->SetArrayField(TEXT("syncMarkers"), AnimSequenceDumpBuilder::BuildSyncMarkersArrayJson(Sequence));

    return Root;
}

namespace
{
    UClass* GetAnimSequenceSidecarClass()
    {
        return UAnimSequence::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildAnimSequenceSidecar(UObject* Asset)
    {
        return AnimSequenceDumpBuilder::BuildAnimSequenceJson(Cast<UAnimSequence>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("anim_sequence"), DumpFileNames::AnimSequence,
    &GetAnimSequenceSidecarClass, &BuildAnimSequenceSidecar,
    nullptr, nullptr, nullptr, 100);
