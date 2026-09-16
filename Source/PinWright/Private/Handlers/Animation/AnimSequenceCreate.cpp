// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Animation/AnimSequenceCreate.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "PwSource/PwSourceAssetUserData.h"
#include "PwSource/PwSourceRecompileGuard.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/TransactionUtils.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
// UE 5.4 split IAssetCompilingManager out of AssetCompilingManager.h into its own header.
// On 5.3 the interface is declared inside AssetCompilingManager.h (included above) and the
// standalone header does not exist.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#include "IAssetCompilingManager.h"
#endif
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "Factories/AnimSequenceFactory.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace
{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // UE 5.7 and older poll a UAnimSequence's async compression task while holding that
    // sequence's compressed-data READ lock (UAnimSequence::WaitForAsyncTasks), and the task's
    // own DDC completion step takes the WRITE lock on the same sequence
    // (FAnimationSequenceAsyncCacheTask::EndCache). The poll never yields the read lock, so the
    // task can never complete: every engine call that waits on compression hangs the game
    // thread forever when a task is still in flight. UAnimSequence::SetSkeleton is one such
    // call, through OnSetSkeleton, so an in-place rebuild that follows an earlier rebuild's
    // still-running compression deadlocks. 5.7 moved FinishAsyncTasks' write scope after the
    // wait but left WaitForAsyncTasks polling under the read lock, so 5.7 still hangs (measured:
    // PinWright.Animation.Compiler.SyncMarkersAreIdempotentAndRefreshDerivedState froze the game
    // thread on the second, in-place compile). 5.8 serialises the task map behind its own mutex
    // and no longer deadlocks. Before that the only cure is to leave nothing in flight.
    //
    // The compiling manager itself is safe to drive: it takes the sequence lock only for a task
    // that has already polled complete, and releases it between polls, so this terminates.
    void AnimSequenceCreate_DrainCompilation()
    {
        static const FName AnimSequenceManagerName(TEXT("UE-AnimationSequence"));
        auto NumCompiling = []() -> int32
        {
            for (IAssetCompilingManager* Manager : FAssetCompilingManager::Get().GetRegisteredManagers())
            {
                if (Manager && Manager->GetAssetTypeName() == AnimSequenceManagerName)
                {
                    return Manager->GetNumRemainingAssets();
                }
            }
            return 0;
        };

        const double Deadline = FPlatformTime::Seconds() + 60.0;
        while (NumCompiling() > 0 && FPlatformTime::Seconds() < Deadline)
        {
            FAssetCompilingManager::Get().ProcessAsyncTasks();
            FPlatformProcess::Sleep(0.001f);
        }
    }
#endif

    class FAnimSequenceCreateRollback
    {
    public:
        FAnimSequenceCreateRollback(
            UAnimSequence* InSequence, bool bInNewAsset, bool bInPackageWasDirty)
            : Sequence(InSequence)
            , Package(InSequence ? InSequence->GetOutermost() : nullptr)
            , bNewAsset(bInNewAsset)
            , bPackageWasDirty(bInPackageWasDirty)
        {
#if WITH_EDITOR
            if (Sequence && !bNewAsset)
            {
                SequenceSnapshot = Cast<UAnimSequence>(StaticDuplicateObject(
                    Sequence, GetTransientPackage(), NAME_None, RF_Transient));
                if (SequenceSnapshot)
                {
                    // A data model must be owned by an animation asset. Root the duplicate
                    // sequence so its correctly-owned model remains available for rollback.
                    SequenceSnapshot->AddToRoot();
                }
                Transaction = MakeUnique<FScopedTransaction>(
                    NSLOCTEXT("PinWright", "CompileAnimSequence", "Compile animation sequence"));
                PinWrightTransactionUtils::PrepareTransactionalSnapshot(Sequence);
                PinWrightTransactionUtils::PrepareTransactionalSnapshot(
                    Sequence->GetDataModelInterface().GetObject());
            }
#endif
        }

        ~FAnimSequenceCreateRollback()
        {
            if (bCommitted)
            {
                return;
            }

#if WITH_EDITOR
            if (Transaction)
            {
                PinWrightTransactionUtils::ApplyAndCancelTransaction(*Transaction);
            }
            if (Sequence && SequenceSnapshot)
            {
                // UE 5.8's sequencer controller only populates from the legacy data-model
                // class. CreateAnimation uses UAnimSequenceBase::CopyDataModel, which replaces
                // either model implementation and therefore restores the actual key channels.
                Sequence->CreateAnimation(SequenceSnapshot);
            }
#endif
            if (bNewAsset && Sequence)
            {
                Sequence->ClearFlags(RF_Public | RF_Standalone);
                Sequence->Rename(nullptr, GetTransientPackage(),
                    REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
                Sequence->MarkAsGarbage();
            }
            if (Package)
            {
                Package->SetDirtyFlag(bPackageWasDirty);
            }
#if WITH_EDITOR
            if (SequenceSnapshot)
            {
                SequenceSnapshot->RemoveFromRoot();
            }
#endif
        }

        void Commit()
        {
#if WITH_EDITOR
            if (Transaction)
            {
                // Keep the successful changes but do not add an internal compile step to the
                // user's undo history.
                Transaction->Cancel();
            }
            if (SequenceSnapshot)
            {
                SequenceSnapshot->RemoveFromRoot();
                SequenceSnapshot = nullptr;
            }
#endif
            bCommitted = true;
        }

    private:
        UAnimSequence* Sequence = nullptr;
        UPackage* Package = nullptr;
        bool bNewAsset = false;
        bool bPackageWasDirty = false;
        bool bCommitted = false;
#if WITH_EDITOR
        TUniquePtr<FScopedTransaction> Transaction;
        UAnimSequence* SequenceSnapshot = nullptr;
#endif
    };

    template <typename TResult>
    TResult AnimSequenceCreate_Fail(const TCHAR* ErrorCode, FString ErrorMessage)
    {
        TResult Result;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = MoveTemp(ErrorMessage);
        return Result;
    }

    template <typename TResult>
    bool AnimSequenceCreate_FailInPlace(TResult& Result, const TCHAR* ErrorCode, FString ErrorMessage)
    {
        Result.bSuccess = false;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = MoveTemp(ErrorMessage);
        return false;
    }

    template <typename TResult>
    bool AnimSequenceCreate_ValidateTracks(
        TArrayView<const FPwBoneTrackSpec> Tracks,
        const USkeleton* Skeleton,
        int32 ExpectedKeyCount,
        TResult& OutResult)
    {
        if (!Skeleton)
        {
            return AnimSequenceCreate_FailInPlace(
                OutResult, ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("A skeleton is required"));
        }

        if (ExpectedKeyCount <= 0)
        {
            return AnimSequenceCreate_FailInPlace(
                OutResult, ErrorCodes::ERR_ANIMATION_INVALID,
                TEXT("Animation key count must be positive"));
        }

        TSet<FName> SeenBones;
        for (const FPwBoneTrackSpec& Track : Tracks)
        {
            if (Track.BoneName.IsNone())
            {
                return AnimSequenceCreate_FailInPlace(
                    OutResult, ErrorCodes::ERR_MISSING_BONE_NAME,
                    TEXT("Every animation track requires a bone name"));
            }

            if (SeenBones.Contains(Track.BoneName))
            {
                return AnimSequenceCreate_FailInPlace(
                    OutResult, ErrorCodes::ERR_ANIMATION_INVALID,
                    FString::Printf(TEXT("Bone track '%s' was supplied more than once"),
                        *Track.BoneName.ToString()));
            }
            SeenBones.Add(Track.BoneName);

            if (Skeleton->GetReferenceSkeleton().FindBoneIndex(Track.BoneName) == INDEX_NONE)
            {
                return AnimSequenceCreate_FailInPlace(
                    OutResult, ErrorCodes::ERR_BONE_NOT_FOUND,
                    FString::Printf(TEXT("Bone '%s' is not present in skeleton '%s'"),
                        *Track.BoneName.ToString(), *Skeleton->GetPathName()));
            }

            if (Track.Pos.Num() <= 0 || Track.Pos.Num() != Track.Rot.Num() ||
                Track.Pos.Num() != Track.Scale.Num())
            {
                return AnimSequenceCreate_FailInPlace(
                    OutResult, ErrorCodes::ERR_ANIMATION_INVALID,
                    FString::Printf(TEXT("Bone '%s' must have non-empty position, rotation and scale arrays of equal length"),
                        *Track.BoneName.ToString()));
            }

            // UAnimDataController only checks equal, non-empty component arrays.  It does not
            // compare their length to NumberOfKeys, so this check must remain before the call.
            if (Track.Pos.Num() != ExpectedKeyCount)
            {
                return AnimSequenceCreate_FailInPlace(
                    OutResult, ErrorCodes::ERR_ANIMATION_INVALID,
                    FString::Printf(TEXT("Bone '%s' has %d keys; expected %d (NumberOfFrames + 1)"),
                        *Track.BoneName.ToString(), Track.Pos.Num(), ExpectedKeyCount));
            }
        }

        return true;
    }

    template <typename TResult>
    bool AnimSequenceCreate_ApplyTracks(
        IAnimationDataController& Controller,
        TArrayView<const FPwBoneTrackSpec> Tracks,
        TResult& OutResult)
    {
        for (const FPwBoneTrackSpec& Track : Tracks)
        {
            const IAnimationDataModel* Model = Controller.GetModel();
            const bool bTrackExistedBefore = Model && Model->IsValidBoneTrackName(Track.BoneName);
            bool bTrackAdded = false;
            if (!bTrackExistedBefore)
            {
                bTrackAdded = Controller.AddBoneCurve(Track.BoneName, false);
                if (!bTrackAdded)
                {
                    return AnimSequenceCreate_FailInPlace(
                        OutResult, ErrorCodes::ERR_TRACK_CREATION_FAILED,
                        FString::Printf(TEXT("Could not add animation track for bone '%s'"),
                            *Track.BoneName.ToString()));
                }
            }

            const bool bKeysWritten = Controller.SetBoneTrackKeys(
                Track.BoneName, Track.Pos, Track.Rot, Track.Scale, false);
            if (!bKeysWritten)
            {
                return AnimSequenceCreate_FailInPlace(
                    OutResult, ErrorCodes::ERR_TRACK_OP_FAILED,
                    FString::Printf(TEXT("Could not write animation keys for bone '%s'"),
                        *Track.BoneName.ToString()));
            }
        }

        // Mark the model populated after all keys are written.
        Controller.NotifyPopulated();
        return true;
    }

    template <typename TResult>
    bool AnimSequenceCreate_ReadBack(
        UAnimSequence* Sequence,
        const FName* TrackToMeasure,
        int32* OutTrackKeyCount,
        TResult& OutResult)
    {
        const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
        if (!Model)
        {
            return AnimSequenceCreate_FailInPlace(
                OutResult, ErrorCodes::ERR_ASSET_DATA_INVALID,
                TEXT("Animation sequence has no readable data model"));
        }

        OutResult.AssetPath = Sequence->GetPathName();
        OutResult.AssetFrameRate = Model->GetFrameRate();
        OutResult.AssetNumberOfFrames = Model->GetNumberOfFrames();
        OutResult.AssetNumberOfKeys = Model->GetNumberOfKeys();
        OutResult.AssetTrackCount = Model->GetNumBoneTracks();
        OutResult.AssetKeyCountPerTrack.Reset();

        TArray<FName> TrackNames;
        Model->GetBoneTrackNames(TrackNames);
        OutResult.AssetKeyCountPerTrack.Reserve(TrackNames.Num());
        OutResult.AssetTrackNames = TrackNames;
        for (const FName& TrackName : TrackNames)
        {
            // UAnimationSequencerDataModel intentionally leaves the deprecated
            // FBoneAnimationTrack accessors empty. IterateBoneKeys is the common
            // data-model readback API and reports keys from the active model.
            int32 KeyCount = 0;
            Model->IterateBoneKeys(TrackName,
                [&KeyCount](const FVector3f&, const FQuat4f&, const FVector3f&, const FFrameNumber&)
                {
                    ++KeyCount;
                    return true;
                });
            OutResult.AssetKeyCountPerTrack.Add(KeyCount);

            if (TrackToMeasure && *TrackToMeasure == TrackName && OutTrackKeyCount)
            {
                *OutTrackKeyCount = KeyCount;
            }
        }

        OutResult.AssetSyncMarkers = ReadAnimSequenceSyncMarkers(Sequence);

        return true;
    }

    // UE 5.8's sequencer data model stores a bone track as nine FMovieSceneFloatChannels and
    // collapses a channel whose values never vary to ONE key; when all nine collapse, the track
    // itself is stored with one key and IterateBoneKeys reports one.
    // (AnimSequencerController.cpp `bAllKeysAreConstant` / `MaximumNumberOfKeys`, and
    // AnimSequencerDataModel.cpp `MaxNumberOfKeys` which starts at 1.)
    //
    // So "every track reads back NumberOfFrames + 1 keys" is not an invariant of a correct write
    // - it is false for every held pose. This predicate mirrors the engine's own test, including
    // its EULER comparison of the rotation channels and its UE_KINDA_SMALL_NUMBER tolerance, so
    // the count predicted here and the count the engine stores cannot disagree.
    bool AnimSequenceCreate_TrackIsConstant(const FPwBoneTrackSpec& Track)
    {
        const int32 KeyCount = Track.Pos.Num();
        if (KeyCount <= 1)
        {
            return true;
        }

        const FVector3f FirstPos = Track.Pos[0];
        const FVector3f FirstEuler = Track.Rot[0].Euler();
        const FVector3f FirstScale = Track.Scale[0];
        for (int32 KeyIndex = 1; KeyIndex < KeyCount; ++KeyIndex)
        {
            const FVector3f Euler = Track.Rot[KeyIndex].Euler();
            for (int32 Channel = 0; Channel < 3; ++Channel)
            {
                if (!FMath::IsNearlyEqual(FirstPos[Channel], Track.Pos[KeyIndex][Channel],
                        UE_KINDA_SMALL_NUMBER)
                    || !FMath::IsNearlyEqual(FirstEuler[Channel], Euler[Channel],
                        UE_KINDA_SMALL_NUMBER)
                    || !FMath::IsNearlyEqual(FirstScale[Channel], Track.Scale[KeyIndex][Channel],
                        UE_KINDA_SMALL_NUMBER))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // Per-track readback assertion. Runs BEFORE the asset is stamped and saved: a write that
    // cannot be verified must not reach the disk, and an in-place rebuild whose target has
    // referencers is exactly the case where a half-written overwrite is unrecoverable.
    template <typename TResult>
    bool AnimSequenceCreate_VerifyKeyCounts(
        TArrayView<const FPwBoneTrackSpec> Tracks,
        int32 ExpectedKeyCount,
        bool bAssetWasSaved,
        TResult& OutResult)
    {
        // Indexed the way the MODEL enumerates its tracks, not the way the request listed them:
        // the two orders need not agree, and a track already on the sequence that this call did
        // not write stays -1, meaning "not asserted by this call".
        OutResult.ExpectedKeyCountPerTrack.Init(INDEX_NONE, OutResult.AssetTrackNames.Num());

        const TCHAR* DiskNote = bAssetWasSaved
            ? TEXT("") : TEXT("; the asset on disk was NOT overwritten");
        for (const FPwBoneTrackSpec& Track : Tracks)
        {
            const int32 Position = OutResult.AssetTrackNames.IndexOfByKey(Track.BoneName);
            if (Position == INDEX_NONE)
            {
                return AnimSequenceCreate_FailInPlace(
                    OutResult, ErrorCodes::ERR_ASSET_DATA_INVALID,
                    FString::Printf(
                        TEXT("Bone '%s' was written but the data model has no track for it%s"),
                        *Track.BoneName.ToString(), DiskNote));
            }

            const int32 Expected = AnimSequenceCreate_TrackIsConstant(Track) ? 1 : ExpectedKeyCount;
            OutResult.ExpectedKeyCountPerTrack[Position] = Expected;

            const int32 Actual = OutResult.AssetKeyCountPerTrack[Position];
            if (Actual != Expected)
            {
                return AnimSequenceCreate_FailInPlace(
                    OutResult, ErrorCodes::ERR_ASSET_DATA_INVALID,
                    FString::Printf(
                        TEXT("Bone '%s' read back with %d keys; expected %d%s"),
                        *Track.BoneName.ToString(), Actual, Expected, DiskNote));
            }
        }
        return true;
    }

    const UPwSourceAssetUserData* AnimSequenceCreate_ReadProvenance(UObject* ExistingObject)
    {
        IInterface_AssetUserData* UserDataOwner = Cast<IInterface_AssetUserData>(ExistingObject);
        if (!UserDataOwner)
        {
            return nullptr;
        }

        return Cast<UPwSourceAssetUserData>(
            UserDataOwner->GetAssetUserDataOfClass(UPwSourceAssetUserData::StaticClass()));
    }

    void AnimSequenceCreate_WriteProvenance(
        UAnimSequence* Sequence,
        const FString& SourcePath,
        const FString& SourceHash,
        TArrayView<const FPwSourceStateEntry> GeneratedState)
    {
        if (!Sequence || SourcePath.IsEmpty())
        {
            return;
        }

        IInterface_AssetUserData* UserDataOwner = Cast<IInterface_AssetUserData>(Sequence);
        if (!UserDataOwner)
        {
            return;
        }

        UPwSourceAssetUserData* Stamp = Cast<UPwSourceAssetUserData>(
            UserDataOwner->GetAssetUserDataOfClass(UPwSourceAssetUserData::StaticClass()));
        if (!Stamp)
        {
            Stamp = NewObject<UPwSourceAssetUserData>(Sequence);
            UserDataOwner->AddAssetUserData(Stamp);
        }

        Stamp->SourcePath = SourcePath;
        Stamp->SourceHash = SourceHash;
        Stamp->GeneratedStateVersion = PwSourceRecompileGuard::StateVersion;
        Stamp->GeneratedState = PwSourceRecompileGuard::MakeStateMap(GeneratedState);
    }

    FPwSourceStateEntry AnimSequenceCreate_State(
        FString Key, FString Field, FString Value, FString Origin)
    {
        FPwSourceStateEntry Entry;
        Entry.Key = MoveTemp(Key);
        Entry.Field = MoveTemp(Field);
        Entry.Value = MoveTemp(Value);
        Entry.Origin = MoveTemp(Origin);
        return Entry;
    }

    FString AnimSequenceCreate_TransformValue(
        const FVector3f& Position, FQuat4f Rotation, const FVector3f& Scale)
    {
        Rotation.Normalize();
        if (Rotation.W < 0.0f)
        {
            Rotation = Rotation * -1.0f;
        }
        return FString::Printf(
            TEXT("%.9g,%.9g,%.9g|%.9g,%.9g,%.9g,%.9g|%.9g,%.9g,%.9g"),
            Position.X, Position.Y, Position.Z,
            Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
            Scale.X, Scale.Y, Scale.Z);
    }

    FString AnimSequenceCreate_TrackValue(const FPwBoneTrackSpec& Track)
    {
        TArray<FString> Keys;
        Keys.Reserve(Track.Pos.Num());
        for (int32 Index = 0; Index < Track.Pos.Num(); ++Index)
        {
            Keys.Add(AnimSequenceCreate_TransformValue(
                Track.Pos[Index], Track.Rot[Index], Track.Scale[Index]));
        }
        return FString::Join(Keys, TEXT(";"));
    }

    TArray<FPwSourceStateEntry> AnimSequenceCreate_DesiredState(
        const FAnimSequenceCreateSpec& Spec)
    {
        TArray<FPwSourceStateEntry> State;
        State.Add(AnimSequenceCreate_State(TEXT("skeleton"), TEXT("skeleton"),
            Spec.Skeleton ? Spec.Skeleton->GetPathName() : FString(),
            TEXT("the bound skeleton can be changed by animation authoring tools")));
        State.Add(AnimSequenceCreate_State(TEXT("timebase"), TEXT("timebase"),
            FString::Printf(TEXT("%d/%d:%d"), Spec.FrameRate.Numerator,
                Spec.FrameRate.Denominator, Spec.NumberOfFrames),
            TEXT("the timeline can be changed by animation authoring tools")));
        State.Add(AnimSequenceCreate_State(TEXT("loop"), TEXT("loop"),
            Spec.bLoop ? TEXT("true") : TEXT("false"),
            TEXT("the loop setting can be changed in the Animation Sequence editor")));
        for (const FPwBoneTrackSpec& Track : Spec.Tracks)
        {
            State.Add(AnimSequenceCreate_State(TEXT("bone_track:") + Track.BoneName.ToString(),
                FString::Printf(TEXT("boneTrack[%s]"), *Track.BoneName.ToString()),
                AnimSequenceCreate_TrackValue(Track),
                TEXT("bone tracks can be written by animation authoring verbs or the Animation Sequence editor")));
        }
        for (int32 Index = 0; Index < Spec.SyncMarkers.Num(); ++Index)
        {
            const FPwSyncMarkerSpec& Marker = Spec.SyncMarkers[Index];
            State.Add(AnimSequenceCreate_State(FString::Printf(TEXT("sync_marker:%d"), Index),
                FString::Printf(TEXT("syncMarker[%d]"), Index),
                FString::Printf(TEXT("%s@%.9g"), *Marker.MarkerName.ToString(), Marker.Time),
                TEXT("sync markers can be written by animation.set_sync_markers or the Animation Sequence editor")));
        }
        return State;
    }

    TArray<FPwSourceStateEntry> AnimSequenceCreate_CurrentState(const UAnimSequence* Sequence)
    {
        TArray<FPwSourceStateEntry> State;
        if (!Sequence)
        {
            return State;
        }

        const IAnimationDataModel* Model = Sequence->GetDataModel();
        State.Add(AnimSequenceCreate_State(TEXT("skeleton"), TEXT("skeleton"),
            Sequence->GetSkeleton() ? Sequence->GetSkeleton()->GetPathName() : FString(),
            TEXT("the bound skeleton can be changed by animation authoring tools")));
        State.Add(AnimSequenceCreate_State(TEXT("loop"), TEXT("loop"),
            Sequence->bLoop ? TEXT("true") : TEXT("false"),
            TEXT("the loop setting can be changed in the Animation Sequence editor")));
        if (Model)
        {
            const FFrameRate Rate = Model->GetFrameRate();
            State.Add(AnimSequenceCreate_State(TEXT("timebase"), TEXT("timebase"),
                FString::Printf(TEXT("%d/%d:%d"), Rate.Numerator, Rate.Denominator,
                    Model->GetNumberOfFrames()),
                TEXT("the timeline can be changed by animation authoring tools")));

            TArray<FName> TrackNames;
            Model->GetBoneTrackNames(TrackNames);
            for (const FName TrackName : TrackNames)
            {
                TArray<FString> Keys;
                Model->IterateBoneKeys(TrackName,
                    [&Keys](const FVector3f& Position, const FQuat4f& Rotation,
                            const FVector3f& Scale, const FFrameNumber&)
                    {
                        Keys.Add(AnimSequenceCreate_TransformValue(Position, Rotation, Scale));
                        return true;
                    });
                State.Add(AnimSequenceCreate_State(TEXT("bone_track:") + TrackName.ToString(),
                    FString::Printf(TEXT("boneTrack[%s]"), *TrackName.ToString()),
                    FString::Join(Keys, TEXT(";")),
                    TEXT("bone tracks can be written by animation authoring verbs or the Animation Sequence editor")));
            }

            for (const FFloatCurve& Curve : Model->GetFloatCurves())
            {
                State.Add(AnimSequenceCreate_State(TEXT("float_curve:") + Curve.GetName().ToString(),
                    FString::Printf(TEXT("floatCurve[%s]"), *Curve.GetName().ToString()), TEXT("present"),
                    TEXT("float curves are written by animation curve verbs or the Animation Sequence editor")));
            }
            for (const FTransformCurve& Curve : Model->GetTransformCurves())
            {
                State.Add(AnimSequenceCreate_State(TEXT("transform_curve:") + Curve.GetName().ToString(),
                    FString::Printf(TEXT("transformCurve[%s]"), *Curve.GetName().ToString()), TEXT("present"),
                    TEXT("transform curves are written by animation curve verbs or the Animation Sequence editor")));
            }
        }

        for (int32 Index = 0; Index < Sequence->AuthoredSyncMarkers.Num(); ++Index)
        {
            const FAnimSyncMarker& Marker = Sequence->AuthoredSyncMarkers[Index];
            State.Add(AnimSequenceCreate_State(FString::Printf(TEXT("sync_marker:%d"), Index),
                FString::Printf(TEXT("syncMarker[%d]"), Index),
                FString::Printf(TEXT("%s@%.9g"), *Marker.MarkerName.ToString(), Marker.Time),
                TEXT("sync markers can be written by animation.set_sync_markers or the Animation Sequence editor")));
        }
        for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
        {
            const FAnimNotifyEvent& Notify = Sequence->Notifies[Index];
            State.Add(AnimSequenceCreate_State(FString::Printf(TEXT("notify:%d"), Index),
                FString::Printf(TEXT("notify[%d]"), Index),
                FString::Printf(TEXT("%s@%.9g"), *Notify.NotifyName.ToString(), Notify.GetTime()),
                TEXT("notifies are written by animation notify verbs or the Animation Sequence editor")));
        }
        if (!Sequence->RetargetSource.IsNone())
        {
            State.Add(AnimSequenceCreate_State(TEXT("retarget_source"), TEXT("retargetSource"),
                Sequence->RetargetSource.ToString(),
                TEXT("the retarget source is authored outside .pwanim")));
        }
        return State;
    }

    template <typename TResult>
    bool AnimSequenceCreate_Save(
        UAnimSequence* Sequence,
        bool bSave,
        TResult& OutResult)
    {
        if (bSave)
        {
            SaveAssetToDiskReportingPresence(Sequence, true, nullptr, nullptr, &OutResult.SaveState);
        }
        else
        {
            Sequence->MarkPackageDirty();
            OutResult.SaveState = EAssetSaveState::NotRequested;
        }

        return true;
    }

    FAnimSequenceSyncMarkerWriteResult AnimSequenceCreate_SyncMarkerFail(
        const TCHAR* ErrorCode, FString ErrorMessage)
    {
        FAnimSequenceSyncMarkerWriteResult Result;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = MoveTemp(ErrorMessage);
        return Result;
    }

    bool AnimSequenceCreate_ValidateSyncMarker(
        const UAnimSequence* Sequence,
        const FPwSyncMarkerSpec& Spec,
        FString& OutErrorCode,
        FString& OutErrorMessage)
    {
        if (Spec.MarkerName.IsNone())
        {
            OutErrorCode = ErrorCodes::ERR_MISSING_MARKER_NAME;
            OutErrorMessage = TEXT("Every sync marker requires a non-empty name");
            return false;
        }

        const float Length = Sequence ? Sequence->GetPlayLength() : 0.0f;
        if (!FMath::IsFinite(Spec.Time) || Spec.Time < 0.0f || Spec.Time > Length)
        {
            OutErrorCode = ErrorCodes::ERR_ANIMATION_INVALID;
            OutErrorMessage = FString::Printf(
                TEXT("Sync marker '%s' time %.9g is outside the animation range 0 through %.9g seconds"),
                *Spec.MarkerName.ToString(), Spec.Time, Length);
            return false;
        }
        return true;
    }
}

FAnimSequenceSyncMarkerWriteResult SetAnimSequenceSyncMarkers(
    UAnimSequence* Sequence,
    TArrayView<const FPwSyncMarkerSpec> Markers)
{
    if (!Sequence)
    {
        return AnimSequenceCreate_SyncMarkerFail(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("An AnimSequence is required"));
    }

    // Validate and build the replacement off to the side. No asset state changes until every
    // marker has passed, so a bad item cannot leave a partial replacement behind.
    TArray<FAnimSyncMarker> Replacement;
    Replacement.Reserve(Markers.Num());
    for (const FPwSyncMarkerSpec& Spec : Markers)
    {
        FString ErrorCode;
        FString ErrorMessage;
        if (!AnimSequenceCreate_ValidateSyncMarker(
                Sequence, Spec, ErrorCode, ErrorMessage))
        {
            return AnimSequenceCreate_SyncMarkerFail(*ErrorCode, MoveTemp(ErrorMessage));
        }

        FAnimSyncMarker& Marker = Replacement.AddDefaulted_GetRef();
        Marker.MarkerName = Spec.MarkerName;
        Marker.Time = Spec.Time;
#if WITH_EDITORONLY_DATA
        Marker.TrackIndex = 0;
        Marker.Guid = FGuid::NewGuid();
#endif
    }

    Sequence->AuthoredSyncMarkers = MoveTemp(Replacement);
#if WITH_EDITORONLY_DATA
    if (Sequence->AuthoredSyncMarkers.Num() > 0 && Sequence->AnimNotifyTracks.Num() == 0)
    {
        Sequence->AnimNotifyTracks.Add(FAnimNotifyTrack(
            FName(TEXT("1")), FLinearColor::White));
    }
#endif

    // RefreshSyncMarkerDataFromAuthored rebuilds UniqueMarkerNames and blend-space marker
    // metadata. RefreshCacheData then rebuilds the per-track links and ordering from the
    // authored array; writing the array alone is not a coherent UE marker update.
    Sequence->RefreshSyncMarkerDataFromAuthored();
    Sequence->RefreshCacheData();

    FAnimSequenceSyncMarkerWriteResult Result;
    Result.bSuccess = true;
    return Result;
}

FAnimSequenceSyncMarkerWriteResult RemoveAnimSequenceSyncMarkers(
    UAnimSequence* Sequence,
    FName MarkerName)
{
    if (!Sequence)
    {
        return AnimSequenceCreate_SyncMarkerFail(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("An AnimSequence is required"));
    }
    if (MarkerName.IsNone())
    {
        return AnimSequenceCreate_SyncMarkerFail(
            ErrorCodes::ERR_MISSING_MARKER_NAME, TEXT("markerName is required"));
    }

    FAnimSequenceSyncMarkerWriteResult Result;
    Result.RemovedCount = Sequence->AuthoredSyncMarkers.RemoveAll(
        [MarkerName](const FAnimSyncMarker& Marker)
        {
            return Marker.MarkerName == MarkerName;
        });
    Sequence->RefreshSyncMarkerDataFromAuthored();
    Sequence->RefreshCacheData();
    Result.bSuccess = true;
    return Result;
}

TArray<FPwSyncMarkerSpec> ReadAnimSequenceSyncMarkers(const UAnimSequence* Sequence)
{
    TArray<FPwSyncMarkerSpec> Result;
    if (!Sequence)
    {
        return Result;
    }

    Result.Reserve(Sequence->AuthoredSyncMarkers.Num());
    for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
    {
        FPwSyncMarkerSpec& Readback = Result.AddDefaulted_GetRef();
        Readback.MarkerName = Marker.MarkerName;
        Readback.Time = Marker.Time;
    }
    return Result;
}

FAnimSequenceCreateResult CreateAnimSequence(const FAnimSequenceCreateSpec& Spec)
{
    FAnimSequenceCreateResult Out;

    if (Spec.AssetPath.IsEmpty())
    {
        return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath is required"));
    }

    FText BadPackageNameReason;
    if (!FPackageName::IsValidTextForLongPackageName(Spec.AssetPath, &BadPackageNameReason))
    {
        return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
            ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("Invalid animation asset path '%s': %s"),
                *Spec.AssetPath, *BadPackageNameReason.ToString()));
    }
    if (Spec.AssetPath.StartsWith(TEXT("/Engine/")) &&
        !Spec.AssetPath.StartsWith(TEXT("/Engine/Transient")))
    {
        return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
            ErrorCodes::ERR_SECURITY_VIOLATION,
            TEXT("Refusing to write an animation into /Engine/ - target a /Game/ path"));
    }
    if (!Spec.Skeleton)
    {
        return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("A skeleton is required"));
    }
    if (Spec.FrameRate.Numerator <= 0 || Spec.FrameRate.Denominator <= 0)
    {
        return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
            ErrorCodes::ERR_ANIMATION_INVALID, TEXT("Frame rate must have positive numerator and denominator"));
    }
    if (Spec.NumberOfFrames <= 0)
    {
        return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
            ErrorCodes::ERR_ANIMATION_INVALID, TEXT("NumberOfFrames must be positive"));
    }

    const FString PackageName = Spec.AssetPath;
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
    if (AssetName.IsEmpty())
    {
        return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
            ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("Animation asset path '%s' has no asset name"), *Spec.AssetPath));
    }

    const int32 ExpectedKeyCount = Spec.NumberOfFrames + 1;
    if (!AnimSequenceCreate_ValidateTracks(
            TArrayView<const FPwBoneTrackSpec>(Spec.Tracks), Spec.Skeleton, ExpectedKeyCount, Out))
    {
        return Out;
    }

    const TArray<FPwSourceStateEntry> DesiredState =
        AnimSequenceCreate_DesiredState(Spec);

    // Resolve without the destructive overwrite mechanism.  bOverwrite is permission for an
    // unstamped/different-source occupant; a same-class update remains in place so referencers
    // survive regeneration.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        PackageName, AssetName, UAnimSequence::StaticClass(), /*bOverwriteRequested=*/false);
    if (Resolution.IsRejected())
    {
        Out.ErrorCode = Resolution.ErrorCode;
        Out.ErrorMessage = Resolution.ErrorMessage;
        return Out;
    }

    const bool bUpdateInPlace = Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace;
    const UPwSourceAssetUserData* ExistingStamp = bUpdateInPlace
        ? AnimSequenceCreate_ReadProvenance(Resolution.Existing) : nullptr;
    const bool bSameSource = ExistingStamp && !Spec.SourcePath.IsEmpty() &&
        ExistingStamp->SourcePath == Spec.SourcePath;

    UAnimSequence* Sequence = Cast<UAnimSequence>(Resolution.Existing);
    if (bUpdateInPlace && !Sequence)
    {
        return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
            ErrorCodes::ERR_ASSET_CREATION_FAILED,
            TEXT("The existing animation occupant could not be loaded as an AnimSequence"));
    }

    bool bGuardAllowsWrite = true;
    if (bUpdateInPlace)
    {
        FPwSourceRecompileGuardRequest Guard;
        Guard.FormatName = TEXT(".pwanim");
        Guard.AssetPath = PackageName;
        Guard.bSameSourceRecompile = bSameSource;
        Guard.bTakeover = !bSameSource;
        Guard.bOverwrite = Spec.bOverwrite;
        Guard.CurrentSourcePath = ExistingStamp ? ExistingStamp->SourcePath : FString();
        Guard.RequestedSourcePath = Spec.SourcePath;
        Guard.BaselineVersion = ExistingStamp ? ExistingStamp->GeneratedStateVersion : 0;
        Guard.Baseline = ExistingStamp ? &ExistingStamp->GeneratedState : nullptr;
        Guard.Current = AnimSequenceCreate_CurrentState(Sequence);
        Guard.Desired = DesiredState;
        bGuardAllowsWrite = PwSourceRecompileGuard::Check(Guard, Out.Diagnostics);
    }

    if (bUpdateInPlace)
    {
        if (!bSameSource && !Spec.bOverwrite)
        {
            FString Why;
            if (!ExistingStamp)
            {
                Why = FString::Printf(
                    TEXT("An AnimSequence already exists at %s and carries no PinWright source provenance stamp"),
                    *PackageName);
            }
            else
            {
                Why = FString::Printf(
                    TEXT("The animation at %s was generated from '%s', not '%s'"),
                    *PackageName, *ExistingStamp->SourcePath, *Spec.SourcePath);
            }
            Out.ErrorCode = ErrorCodes::ERR_ASSET_ALREADY_EXISTS;
            Out.ErrorMessage = Why;
            if (!bGuardAllowsWrite && Out.Diagnostics.Num() > 0)
            {
                Out.ErrorMessage += TEXT(". ") + Out.Diagnostics.Last().Message;
            }
            else
            {
                Out.ErrorMessage +=
                    TEXT(" - pass overwrite=true to rebuild it in place (its referencers are preserved)");
            }
            return Out;
        }
    }

    if (!bGuardAllowsWrite)
    {
        Out.ErrorCode = ErrorCodes::ERR_ASSET_DATA_INVALID;
        Out.ErrorMessage = Out.Diagnostics.Last().Message;
        return Out;
    }

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    if (bUpdateInPlace)
    {
        AnimSequenceCreate_DrainCompilation();
    }
#endif

    bool bPackageWasDirty = Sequence && Sequence->GetOutermost()
        ? Sequence->GetOutermost()->IsDirty() : false;
    if (!Sequence)
    {
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
                ErrorCodes::ERR_ASSET_CREATION_FAILED,
                FString::Printf(TEXT("Could not create animation package '%s'"), *PackageName));
        }
        bPackageWasDirty = Package->IsDirty();

        UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
        Factory->TargetSkeleton = Spec.Skeleton;
        Sequence = Cast<UAnimSequence>(Factory->FactoryCreateNew(
            UAnimSequence::StaticClass(), Package, FName(*AssetName),
            RF_Public | RF_Standalone, nullptr, GWarn));
        if (!Sequence)
        {
            return AnimSequenceCreate_Fail<FAnimSequenceCreateResult>(
                ErrorCodes::ERR_ASSET_CREATION_FAILED,
                FString::Printf(TEXT("Could not create AnimSequence '%s'"), *Spec.AssetPath));
        }
    }
    FAnimSequenceCreateRollback Rollback(
        Sequence, /*bInNewAsset=*/!bUpdateInPlace, bPackageWasDirty);
    // Measured BEFORE the model is reset, so what the rebuild does to it can be compared rather
    // than assumed. Empty for a freshly created asset: nothing carried over.
    TArray<float> PreviousMarkerTimes;
    TArray<float> PreviousNotifyTimes;
    if (bUpdateInPlace)
    {
        Out.CarryOver.bRebuiltInPlace = true;
        Out.CarryOver.bSkeletonChanged = Sequence->GetSkeleton() != Spec.Skeleton;
        Out.CarryOver.InheritedRetargetSource = Sequence->RetargetSource;
        Out.CarryOver.PreviousLengthSeconds = Sequence->GetPlayLength();
        for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
        {
            PreviousMarkerTimes.Add(Marker.Time);
        }
        for (const FAnimNotifyEvent& Notify : Sequence->Notifies)
        {
            PreviousNotifyTimes.Add(Notify.GetTime());
        }

        Sequence->SetSkeleton(Spec.Skeleton);
        Sequence->GetController().ResetModel(false);
        Sequence->GetController().InitializeModel();
        if (!Spec.SourcePath.IsEmpty())
        {
            Sequence->Notifies.Reset();
            Sequence->RetargetSource = NAME_None;
        }
    }

    {
        IAnimationDataController& Controller = Sequence->GetController();
#if WITH_EDITOR
        IAnimationDataController::FScopedBracket Bracket(
            Controller, FText::FromString(TEXT("Create animation sequence")), false);
#endif
        Controller.SetFrameRate(Spec.FrameRate, false);
        Controller.SetNumberOfFrames(FFrameNumber(Spec.NumberOfFrames), false);
        if (!AnimSequenceCreate_ApplyTracks(
                Controller, TArrayView<const FPwBoneTrackSpec>(Spec.Tracks), Out))
        {
            return Out;
        }
    }

    if (Spec.bReplaceSyncMarkers || !Spec.SourcePath.IsEmpty())
    {
        const FAnimSequenceSyncMarkerWriteResult MarkerWrite = SetAnimSequenceSyncMarkers(
            Sequence, TArrayView<const FPwSyncMarkerSpec>(Spec.SyncMarkers));
        if (!MarkerWrite.bSuccess)
        {
            Out.ErrorCode = MarkerWrite.ErrorCode;
            Out.ErrorMessage = MarkerWrite.ErrorMessage;
            return Out;
        }
        Out.CarryOver.bSyncMarkersReplaced = true;
    }

    // The bracket closes before measurement: readback must observe the model after every
    // controller notification has completed.
    if (!AnimSequenceCreate_ReadBack(
            Sequence, nullptr, nullptr, Out))
    {
        return Out;
    }

    // Re-measure the carried-over state against the snapshot taken before the reset. A time the
    // engine moved is a time the source could not have asked for.
    Out.CarryOver.LengthSeconds = Sequence->GetPlayLength();
    Out.CarryOver.SyncMarkerCount = Sequence->AuthoredSyncMarkers.Num();
    Out.CarryOver.NotifyCount = Sequence->Notifies.Num();
    for (int32 Index = 0; Index < Sequence->AuthoredSyncMarkers.Num(); ++Index)
    {
        if (PreviousMarkerTimes.IsValidIndex(Index)
            && !FMath::IsNearlyEqual(PreviousMarkerTimes[Index],
                   Sequence->AuthoredSyncMarkers[Index].Time, UE_KINDA_SMALL_NUMBER))
        {
            ++Out.CarryOver.SyncMarkersMoved;
        }
    }
    for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
    {
        if (PreviousNotifyTimes.IsValidIndex(Index)
            && !FMath::IsNearlyEqual(PreviousNotifyTimes[Index],
                   Sequence->Notifies[Index].GetTime(), UE_KINDA_SMALL_NUMBER))
        {
            ++Out.CarryOver.NotifiesMoved;
        }
    }

    // Verify BEFORE stamping and saving. This used to run in the caller, after the save had
    // already happened, so a rejected write still overwrote the target - and an in-place rebuild
    // targets an asset with live referencers by design.
    if (!AnimSequenceCreate_VerifyKeyCounts(
            TArrayView<const FPwBoneTrackSpec>(Spec.Tracks), ExpectedKeyCount,
            /*bAssetWasSaved=*/false, Out))
    {
        return Out;
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (Spec.PostWriteCheckForTest && !Spec.PostWriteCheckForTest(Sequence))
    {
        Out.ErrorCode = ErrorCodes::ERR_ASSET_DATA_INVALID;
        Out.ErrorMessage = TEXT("Animation asset post-condition was rejected by the test seam");
        return Out;
    }
#endif

    // UAnimSequenceBase defaults bLoop to false. Apply the source choice explicitly, including
    // in-place rebuilds; otherwise `loop=true` only affects the seam diagnostic and is silently
    // lost from the compiled asset.
    Sequence->bLoop = Spec.bLoop;
    const TArray<FPwSourceStateEntry> GeneratedState =
        AnimSequenceCreate_CurrentState(Sequence);
    AnimSequenceCreate_WriteProvenance(
        Sequence, Spec.SourcePath, Spec.SourceHash, GeneratedState);
    if (!bUpdateInPlace)
    {
        FAssetRegistryModule::AssetCreated(Sequence);
    }
    Rollback.Commit();
    AnimSequenceCreate_Save(Sequence, Spec.bSave, Out);

    Out.bSuccess = true;
    return Out;
}

FAnimSequenceWriteResult WriteBoneTracks(
    UAnimSequence* Sequence,
    TArrayView<const FPwBoneTrackSpec> Tracks,
    bool bSave)
{
    FAnimSequenceWriteResult Out;
    if (!Sequence)
    {
        return AnimSequenceCreate_Fail<FAnimSequenceWriteResult>(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("An AnimSequence is required"));
    }

    const IAnimationDataModel* Model = Sequence->GetDataModel();
    if (!Model)
    {
        return AnimSequenceCreate_Fail<FAnimSequenceWriteResult>(
            ErrorCodes::ERR_ASSET_DATA_INVALID, TEXT("Animation sequence has no data model"));
    }

    USkeleton* Skeleton = Sequence->GetSkeleton();
    const int32 ExpectedKeyCount = Model->GetNumberOfFrames() + 1;
    if (!AnimSequenceCreate_ValidateTracks(Tracks, Skeleton, ExpectedKeyCount, Out))
    {
        return Out;
    }

    {
        IAnimationDataController& Controller = Sequence->GetController();
#if WITH_EDITOR
        IAnimationDataController::FScopedBracket Bracket(
            Controller, FText::FromString(TEXT("Write animation bone tracks")), false);
#endif
        if (!AnimSequenceCreate_ApplyTracks(Controller, Tracks, Out))
        {
            return Out;
        }
    }

    const FName FocusTrack = Tracks.Num() > 0 ? Tracks[0].BoneName : NAME_None;
    if (!AnimSequenceCreate_ReadBack(
            Sequence, Tracks.Num() > 0 ? &FocusTrack : nullptr,
            Tracks.Num() > 0 ? &Out.TrackKeyCount : nullptr, Out))
    {
        return Out;
    }
    // Every track, not just the first, and against the count the engine actually stores for it:
    // a constant track is legally reduced to one key (AnimSequenceCreate_TrackIsConstant).
    if (!AnimSequenceCreate_VerifyKeyCounts(Tracks, ExpectedKeyCount, /*bAssetWasSaved=*/false, Out))
    {
        return Out;
    }

    AnimSequenceCreate_Save(Sequence, bSave, Out);
    Out.bSuccess = true;
    return Out;
}
