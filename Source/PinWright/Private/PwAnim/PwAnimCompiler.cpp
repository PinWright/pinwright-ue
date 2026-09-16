// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PwAnim/PwAnimCompiler.h"

#include "Handlers/Animation/AnimSequenceCreate.h"
#include "Handlers/ErrorCodes.h"
#include "PwAnim/PwAnimAst.h"
#include "PwAnim/PwAnimParser.h"
#include "PwSource/PwSkeletonRef.h"
#include "PwSource/PwSuggest.h"
#include "PwSource/PwValueRead.h"

#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"

namespace PwAnimCompilerPrivate
{
    constexpr float LoopSeamTolerance = 0.001f;

    template <typename TValue>
    struct TChannelKey
    {
        int32 Frame = 0;
        FString Ease;
        TValue Value{};
    };

    struct FBakedBoneTrack
    {
        FString AuthoredName;
        FName BoneName;
        int32 Line = 0;
        int32 Column = 0;
        TArray<FVector> Positions;
        TArray<FQuat> Rotations;
        TArray<FVector> Scales;
    };

    void AddDiagnostic(TArray<FPwDiagnostic>& Diagnostics, EPwSeverity Severity,
                       const TCHAR* Code, int32 Line, int32 Column, FString Message,
                       const FString& ScopeName = FString(),
                       TArray<FString> Suggestions = {})
    {
        FPwDiagnostic Diagnostic = Severity == EPwSeverity::Warning
            ? FPwDiagnostic::MakeWarning(Code, Line, Column, MoveTemp(Message))
            : FPwDiagnostic::MakeError(Code, Line, Column, MoveTemp(Message));
        if (!ScopeName.IsEmpty())
        {
            Diagnostic.ScopeLabel = TEXT("bone");
            Diagnostic.ScopeName = ScopeName;
        }
        Diagnostic.Suggestions = MoveTemp(Suggestions);
        Diagnostics.Add(MoveTemp(Diagnostic));
    }

    void AddError(TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code, int32 Line,
                  int32 Column, FString Message, const FString& ScopeName = FString(),
                  TArray<FString> Suggestions = {})
    {
        AddDiagnostic(Diagnostics, EPwSeverity::Error, Code, Line, Column, MoveTemp(Message),
                      ScopeName, MoveTemp(Suggestions));
    }

    bool IsPositiveWhole(double Value)
    {
        return FMath::IsFinite(Value) && Value > 0.0
            && FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value))
            && Value <= static_cast<double>(MAX_int32);
    }

    bool TryReadWholePositiveInteger(const FPwValue* Value, int32& OutValue)
    {
        if (!Value || Value->Type != EPwValueType::Number || !FMath::IsFinite(Value->Number)
            || !IsPositiveWhole(Value->Number))
        {
            return false;
        }

        OutValue = FMath::RoundToInt(Value->Number);
        return true;
    }

    bool TryReadTimebase(const FPwAnimDocument& Document, FFrameRate& OutRate,
                         int32& OutNumberOfFrames, TArray<FPwDiagnostic>& Diagnostics)
    {
        if (!Document.Timebase.IsSet())
        {
            return false;
        }

        const FPwOp& Timebase = Document.Timebase.GetValue();
        const FPwValue* Rate = Timebase.Params.Find(TEXT("rate"));
        const FPwValue* Frames = Timebase.Params.Find(TEXT("frames"));
        int32 Numerator = 0;
        int32 Denominator = 0;
        const bool bRateValid = Rate && Rate->Type == EPwValueType::Tuple
            && Rate->Tuple.Num() == 2
            && IsPositiveWhole(Rate->Tuple[0])
            && IsPositiveWhole(Rate->Tuple[1]);
        if (bRateValid)
        {
            Numerator = FMath::RoundToInt(Rate->Tuple[0]);
            Denominator = FMath::RoundToInt(Rate->Tuple[1]);
        }
        const bool bFramesValid = TryReadWholePositiveInteger(Frames, OutNumberOfFrames);

        if (!bRateValid || !bFramesValid)
        {
            AddError(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, Timebase.Line,
                     Timebase.Column,
                     TEXT("The .pwanim timebase must contain a positive integer rate tuple and "
                          "a positive integer frame count."));
            return false;
        }

        OutRate = FFrameRate(Numerator, Denominator);
        return true;
    }

    float ApplyEase(const FString& Ease, float Alpha)
    {
        const float T = FMath::Clamp(Alpha, 0.0f, 1.0f);
        if (Ease == TEXT("step"))
        {
            return 0.0f;
        }
        if (Ease == TEXT("ease_in"))
        {
            return T * T;
        }
        if (Ease == TEXT("ease_out"))
        {
            const float Inverse = 1.0f - T;
            return 1.0f - Inverse * Inverse;
        }
        if (Ease == TEXT("ease_in_out"))
        {
            return T < 0.5f
                ? 2.0f * T * T
                : 1.0f - FMath::Pow(-2.0f * T + 2.0f, 2.0f) * 0.5f;
        }
        return T;
    }

    template <typename TValue>
    TArray<TValue> BakeChannel(const TArray<TChannelKey<TValue>>& Keys,
                               const TValue& ReferenceValue, int32 NumberOfFrames,
                               TFunctionRef<TValue(const TValue&, const TValue&, float)> Interpolate)
    {
        const int32 SampleCount = NumberOfFrames + 1;
        TArray<TValue> Samples;
        Samples.SetNumUninitialized(SampleCount);

        if (Keys.Num() == 0)
        {
            Samples.Init(ReferenceValue, SampleCount);
            return Samples;
        }

        int32 RightIndex = 0;
        for (int32 Frame = 0; Frame <= NumberOfFrames; ++Frame)
        {
            while (RightIndex < Keys.Num() && Keys[RightIndex].Frame < Frame)
            {
                ++RightIndex;
            }

            if (RightIndex == 0)
            {
                Samples[Frame] = Keys[0].Value;
                continue;
            }

            if (RightIndex >= Keys.Num())
            {
                Samples[Frame] = Keys.Last().Value;
                continue;
            }

            if (Keys[RightIndex].Frame == Frame)
            {
                Samples[Frame] = Keys[RightIndex].Value;
                continue;
            }

            const TChannelKey<TValue>& Left = Keys[RightIndex - 1];
            const TChannelKey<TValue>& Right = Keys[RightIndex];
            const int32 Span = Right.Frame - Left.Frame;
            const float Alpha = Span > 0
                ? static_cast<float>(Frame - Left.Frame) / static_cast<float>(Span)
                : 1.0f;
            Samples[Frame] = Interpolate(Left.Value, Right.Value, ApplyEase(Left.Ease, Alpha));
        }

        return Samples;
    }

    FQuat ReadRotation(const FPwValue& Value)
    {
        TMap<FString, FPwValue> Params;
        Params.Add(TEXT("rotate"), Value);
        return PwValueRead::GetRotator(Params, TEXT("rotate")).Quaternion().GetNormalized();
    }

    void BuildTrack(const FPwAnimBone& Bone, const FTransform& ReferencePose,
                    int32 NumberOfFrames, FBakedBoneTrack& OutTrack)
    {
        const FString BoneEase = PwValueRead::GetIdentifier(
            Bone.Header, TEXT("ease"), TEXT("linear"));
        TArray<TChannelKey<FVector>> PositionKeys;
        TArray<TChannelKey<FQuat>> RotationKeys;
        TArray<TChannelKey<FVector>> ScaleKeys;
        PositionKeys.Reserve(Bone.Keys.Num());
        RotationKeys.Reserve(Bone.Keys.Num());
        ScaleKeys.Reserve(Bone.Keys.Num());

        for (const FPwOp& Op : Bone.Keys)
        {
            const int32 Frame = PwValueRead::GetInt(Op.Params, TEXT("frame"), 0);
            const FString Ease = PwValueRead::GetIdentifier(Op.Params, TEXT("ease"), *BoneEase);

            if (Op.Params.Find(TEXT("at")) != nullptr)
            {
                TChannelKey<FVector> Key;
                Key.Frame = Frame;
                Key.Ease = Ease;
                Key.Value = PwValueRead::GetVector3(Op.Params, TEXT("at"), FVector::ZeroVector);
                PositionKeys.Add(MoveTemp(Key));
            }

            if (const FPwValue* Value = Op.Params.Find(TEXT("rotate")))
            {
                TChannelKey<FQuat> Key;
                Key.Frame = Frame;
                Key.Ease = Ease;
                Key.Value = ReadRotation(*Value);
                RotationKeys.Add(MoveTemp(Key));
            }

            if (Op.Params.Find(TEXT("scale")) != nullptr)
            {
                TChannelKey<FVector> Key;
                Key.Frame = Frame;
                Key.Ease = Ease;
                Key.Value = PwValueRead::GetVector3(Op.Params, TEXT("scale"), FVector::OneVector);
                ScaleKeys.Add(MoveTemp(Key));
            }
        }

        const FVector ReferencePosition = ReferencePose.GetTranslation();
        const FQuat ReferenceRotation = ReferencePose.GetRotation().GetNormalized();
        const FVector ReferenceScale = ReferencePose.GetScale3D();

        OutTrack.AuthoredName = Bone.BoneName;
        OutTrack.BoneName = FName(*Bone.BoneName);
        OutTrack.Line = Bone.Line;
        OutTrack.Column = Bone.Column;
        OutTrack.Positions = BakeChannel<FVector>(
            PositionKeys, ReferencePosition, NumberOfFrames,
            [](const FVector& A, const FVector& B, float Alpha) { return FMath::Lerp(A, B, Alpha); });
        OutTrack.Rotations = BakeChannel<FQuat>(
            RotationKeys, ReferenceRotation, NumberOfFrames,
            [](const FQuat& A, const FQuat& B, float Alpha)
            {
                return FQuat::Slerp(A, B, Alpha).GetNormalized();
            });
        OutTrack.Scales = BakeChannel<FVector>(
            ScaleKeys, ReferenceScale, NumberOfFrames,
            [](const FVector& A, const FVector& B, float Alpha) { return FMath::Lerp(A, B, Alpha); });
    }

    float RotationDeltaDegrees(const FQuat& A, const FQuat& B)
    {
        const float AbsoluteDot = FMath::Clamp(FMath::Abs(A | B), 0.0f, 1.0f);
        return FMath::RadiansToDegrees(2.0f * FMath::Acos(AbsoluteDot));
    }

    bool HasLoopSeamError(const FBakedBoneTrack& Track, FString& OutDelta)
    {
        if (Track.Positions.Num() == 0 || Track.Rotations.Num() == 0 || Track.Scales.Num() == 0)
        {
            return false;
        }

        const float PositionDelta = static_cast<float>(
            (Track.Positions[0] - Track.Positions.Last()).Size());
        const float RotationDelta = RotationDeltaDegrees(
            Track.Rotations[0], Track.Rotations.Last());
        const float ScaleDelta = static_cast<float>(
            (Track.Scales[0] - Track.Scales.Last()).Size());
        if (PositionDelta <= LoopSeamTolerance && RotationDelta <= LoopSeamTolerance
            && ScaleDelta <= LoopSeamTolerance)
        {
            return false;
        }

        OutDelta = FString::Printf(TEXT("translation %.6g, rotation %.6g degrees, scale %.6g"),
            PositionDelta, RotationDelta, ScaleDelta);
        return true;
    }

    FQuat4f ToQuat4f(const FQuat& Value)
    {
        return FQuat4f(static_cast<float>(Value.X), static_cast<float>(Value.Y),
                       static_cast<float>(Value.Z), static_cast<float>(Value.W));
    }

    FVector3f ToVector3f(const FVector& Value)
    {
        return FVector3f(static_cast<float>(Value.X), static_cast<float>(Value.Y),
                         static_cast<float>(Value.Z));
    }

    FPwBoneTrackSpec ToCreateSpec(const FBakedBoneTrack& Track)
    {
        FPwBoneTrackSpec Result;
        Result.BoneName = Track.BoneName;
        Result.Pos.Reserve(Track.Positions.Num());
        Result.Rot.Reserve(Track.Rotations.Num());
        Result.Scale.Reserve(Track.Scales.Num());
        for (int32 Index = 0; Index < Track.Positions.Num(); ++Index)
        {
            Result.Pos.Add(ToVector3f(Track.Positions[Index]));
            Result.Rot.Add(ToQuat4f(Track.Rotations[Index]));
            Result.Scale.Add(ToVector3f(Track.Scales[Index]));
        }
        return Result;
    }

    void CopyCreateResult(const FAnimSequenceCreateResult& Created, FPwAnimCompileResult& Result)
    {
        Result.AssetPath = Created.AssetPath;
        Result.AssetFrameRate = Created.AssetFrameRate;
        Result.AssetNumberOfFrames = Created.AssetNumberOfFrames;
        Result.AssetNumberOfKeys = Created.AssetNumberOfKeys;
        Result.AssetTrackCount = Created.AssetTrackCount;
        Result.AssetKeyCountPerTrack = Created.AssetKeyCountPerTrack;
        Result.ExpectedKeyCountPerTrack = Created.ExpectedKeyCountPerTrack;
        Result.AssetTrackNames = Created.AssetTrackNames;
        Result.AssetSyncMarkers = Created.AssetSyncMarkers;
        Result.SaveState = Created.SaveState;
        Result.bSavedToDisk = IsAssetSaveStateDurable(Created.SaveState);
        Result.ErrorCode = Created.ErrorCode;
        Result.ErrorMessage = Created.ErrorMessage;
    }

    void ReportCreateFailure(const FAnimSequenceCreateResult& Created,
                             const FPwAnimDocument& Document, FPwAnimCompileResult& Result)
    {
        CopyCreateResult(Created, Result);
        Result.Diagnostics.Append(Created.Diagnostics);
        const int32 Line = Document.Header.VersionLine > 0 ? Document.Header.VersionLine : 1;
        const int32 Column = Document.Header.VersionColumn > 0 ? Document.Header.VersionColumn : 1;
        if (Created.ErrorCode == ErrorCodes::ERR_ASSET_ALREADY_EXISTS)
        {
            AddError(Result.Diagnostics,
                PwAnimDiagnosticCodes::PWANIM_ASSET_PROVENANCE_CONFLICT, Line, Column,
                Created.ErrorMessage);
            return;
        }
        if (Created.Diagnostics.Num() > 0)
        {
            return;
        }
        AddError(Result.Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, Line, Column,
            FString::Printf(TEXT("Animation asset creation failed [%s]: %s"),
                *Created.ErrorCode, *Created.ErrorMessage));
    }

    bool ReadBackMatchesRequest(const FAnimSequenceCreateResult& Created, int32 ExpectedFrames,
                                int32 ExpectedKeys, FString& OutMessage)
    {
        if (Created.AssetNumberOfFrames != ExpectedFrames
            || Created.AssetNumberOfKeys != ExpectedKeys)
        {
            OutMessage = FString::Printf(
                TEXT("Animation data model read back frames=%d, keys=%d; expected frames=%d, keys=%d."),
                Created.AssetNumberOfFrames, Created.AssetNumberOfKeys,
                ExpectedFrames, ExpectedKeys);
            return false;
        }

        // Per-track counts are asserted against ExpectedKeyCountPerTrack, not against ExpectedKeys:
        // UE 5.8 stores a bone track whose nine channels never vary with ONE key, so demanding
        // frames+1 everywhere rejected every held pose - including the single-key form this
        // format documents. CreateAnimSequence owns the constancy predicate and publishes what it
        // expected, so this stays an independent assertion without a second definition to drift.
        if (Created.ExpectedKeyCountPerTrack.Num() != Created.AssetKeyCountPerTrack.Num())
        {
            OutMessage = FString::Printf(
                TEXT("Animation data model read back %d tracks; %d were verified."),
                Created.AssetKeyCountPerTrack.Num(), Created.ExpectedKeyCountPerTrack.Num());
            return false;
        }

        for (int32 Index = 0; Index < Created.AssetKeyCountPerTrack.Num(); ++Index)
        {
            if (Created.ExpectedKeyCountPerTrack[Index] == INDEX_NONE)
            {
                continue;   // a track this compile did not write; it has no expectation to meet
            }
            if (Created.AssetKeyCountPerTrack[Index] != Created.ExpectedKeyCountPerTrack[Index])
            {
                const FString BoneName = Created.AssetTrackNames.IsValidIndex(Index)
                    ? Created.AssetTrackNames[Index].ToString() : FString(TEXT("<unnamed>"));
                OutMessage = FString::Printf(
                    TEXT("Animation data model read back bone '%s' with %d keys; expected %d."),
                    *BoneName, Created.AssetKeyCountPerTrack[Index],
                    Created.ExpectedKeyCountPerTrack[Index]);
                return false;
            }
        }
        return true;
    }
}

FPwAnimCompileResult FPwAnimCompiler::Compile(
    FStringView Source, const FPwAnimCompileOptions& Options)
{
    FPwAnimCompileResult Result;
    FPwAnimDocument Document;
    if (!FPwAnimParser::Parse(Source, Document, Result.Diagnostics))
    {
        return Result;
    }

    Result.Version = Document.Header.Version;

    FFrameRate FrameRate;
    int32 NumberOfFrames = -1;
    if (!PwAnimCompilerPrivate::TryReadTimebase(
            Document, FrameRate, NumberOfFrames, Result.Diagnostics))
    {
        return Result;
    }

    const FPwUse* SkeletonUse = Document.Uses.FindByPredicate(
        [](const FPwUse& Use) { return Use.Kind == TEXT("skeleton"); });
    if (!SkeletonUse)
    {
        // The parser owns this structural diagnostic.  This guard only protects direct future
        // callers that construct an AST-like document and keeps the resolver from dereferencing
        // a missing use if the grammar changes.
        return Result;
    }

    PwSkeletonRef::FDiagnosticCodes SkeletonCodes;
    SkeletonCodes.NotAnAssetPath = PwAnimDiagnosticCodes::PWANIM_SKELETON_NOT_AN_ASSET_PATH;
    SkeletonCodes.NotFound = PwAnimDiagnosticCodes::PWANIM_SKELETON_NOT_FOUND;
    SkeletonCodes.WrongKind = PwAnimDiagnosticCodes::PWANIM_SKELETON_WRONG_KIND;
    SkeletonCodes.HasNoBones = PwAnimDiagnosticCodes::PWANIM_SKELETON_HAS_NO_BONES;
    const PwSkeletonRef::FResult Skeleton = PwSkeletonRef::Resolve(
        *SkeletonUse, SkeletonCodes, Result.Diagnostics);
    if (!Skeleton.IsResolved())
    {
        return Result;
    }

    const FReferenceSkeleton& ReferenceSkeleton = Skeleton.Skeleton->GetReferenceSkeleton();
    Result.SkeletonPath = Skeleton.ObjectPath;
    Result.SkeletonBoneCount = ReferenceSkeleton.GetRawBoneNum();
    Result.RequestedFrameRate = FrameRate;
    Result.RequestedNumberOfFrames = NumberOfFrames;
    Result.RequestedNumberOfKeys = NumberOfFrames + 1;
    Result.RequestedDurationSeconds = FrameRate.Numerator > 0
        ? static_cast<double>(NumberOfFrames) * static_cast<double>(FrameRate.Denominator)
            / static_cast<double>(FrameRate.Numerator)
        : -1.0;

    TArray<FString> ReferenceBoneNames;
    ReferenceBoneNames.Reserve(ReferenceSkeleton.GetRawBoneNum());
    for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetRawBoneNum(); ++BoneIndex)
    {
        ReferenceBoneNames.Add(ReferenceSkeleton.GetBoneName(BoneIndex).ToString());
    }

    TArray<PwAnimCompilerPrivate::FBakedBoneTrack> BakedTracks;
    BakedTracks.Reserve(Document.Bones.Num());
    const bool bLoop = PwValueRead::GetBool(
        Document.Timebase.GetValue().Params, TEXT("loop"), false);
    Result.bLoop = bLoop;
    for (const FPwAnimBone& Bone : Document.Bones)
    {
        const FName BoneName(*Bone.BoneName);
        const int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(BoneName);
        if (BoneIndex == INDEX_NONE)
        {
            TArray<FString> Suggestions;
            const FString Closest = PwSuggest::Closest(
                Bone.BoneName, TArrayView<const FString>(ReferenceBoneNames));
            if (!Closest.IsEmpty())
            {
                Suggestions.Add(Closest);
            }
            PwAnimCompilerPrivate::AddError(Result.Diagnostics,
                PwAnimDiagnosticCodes::PWANIM_UNKNOWN_BONE, Bone.Line, Bone.Column,
                FString::Printf(TEXT("Bone '%s' is not present in skeleton '%s'."),
                    *Bone.BoneName, *Skeleton.ObjectPath), Bone.BoneName, MoveTemp(Suggestions));
            continue;
        }

        PwAnimCompilerPrivate::FBakedBoneTrack Track;
        PwAnimCompilerPrivate::BuildTrack(
            Bone, ReferenceSkeleton.GetRefBonePose()[BoneIndex], NumberOfFrames, Track);

        if (bLoop)
        {
            FString Delta;
            if (PwAnimCompilerPrivate::HasLoopSeamError(Track, Delta))
            {
                PwAnimCompilerPrivate::AddError(Result.Diagnostics,
                    PwAnimDiagnosticCodes::PWANIM_LOOP_SEAM, Bone.Line, Bone.Column,
                    FString::Printf(
                        TEXT("Looping animation bone '%s' does not close at frame 0 and frame %d: %s."),
                        *Bone.BoneName, NumberOfFrames, *Delta), Bone.BoneName);
            }
        }
        BakedTracks.Add(MoveTemp(Track));
    }

    if (PwDiagnosticsHaveError(Result.Diagnostics))
    {
        return Result;
    }

    TArray<FPwBoneTrackSpec> Tracks;
    Tracks.Reserve(BakedTracks.Num());
    const int32 ExpectedKeyCount = NumberOfFrames + 1;
    for (const PwAnimCompilerPrivate::FBakedBoneTrack& Baked : BakedTracks)
    {
        // Keep this assertion immediately before the asset-library call boundary.  The library
        // repeats the check, but the compiler must never hand the engine a shape whose runtime
        // evaluation and data-model readback could disagree.
        if (!ensureMsgf(Baked.Positions.Num() == ExpectedKeyCount
                && Baked.Rotations.Num() == ExpectedKeyCount
                && Baked.Scales.Num() == ExpectedKeyCount,
                TEXT(".pwanim bake produced a track whose key count does not match the timebase")))
        {
            PwAnimCompilerPrivate::AddError(Result.Diagnostics,
                PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, Baked.Line, Baked.Column,
                FString::Printf(TEXT("Bone '%s' baked to an invalid key count; expected %d."),
                    *Baked.AuthoredName, ExpectedKeyCount), Baked.AuthoredName);
            return Result;
        }
        Tracks.Add(PwAnimCompilerPrivate::ToCreateSpec(Baked));
    }

    if (Options.bValidateOnly)
    {
        Result.bSuccess = true;
        return Result;
    }

    FAnimSequenceCreateSpec CreateSpec;
    CreateSpec.AssetPath = Options.OutputAssetPath;
    CreateSpec.Skeleton = Skeleton.Skeleton;
    CreateSpec.FrameRate = FrameRate;
    CreateSpec.NumberOfFrames = NumberOfFrames;
    CreateSpec.bLoop = bLoop;
    CreateSpec.bOverwrite = Options.bOverwrite;
    CreateSpec.bSave = Options.bSave;
    CreateSpec.SourcePath = Options.SourcePath;
    CreateSpec.SourceHash = Options.SourceHash;
    CreateSpec.Tracks = MoveTemp(Tracks);
    CreateSpec.bReplaceSyncMarkers = true;
#if WITH_DEV_AUTOMATION_TESTS
    CreateSpec.PostWriteCheckForTest = Options.PostWriteCheckForTest;
#endif
    if (Document.SyncMarkers.IsSet())
    {
        CreateSpec.SyncMarkers.Reserve(Document.SyncMarkers->Num());
        for (const FPwAnimSyncMarker& SourceMarker : Document.SyncMarkers.GetValue())
        {
            if (SourceMarker.MarkerName.IsEmpty() || FName(*SourceMarker.MarkerName).IsNone())
            {
                PwAnimCompilerPrivate::AddError(Result.Diagnostics,
                    PwSourceDiagnosticCodes::PWSRC_BAD_VALUE,
                    SourceMarker.Line, SourceMarker.Column,
                    TEXT("A sync marker name must not be empty."));
                return Result;
            }

            FPwSyncMarkerSpec& Marker = CreateSpec.SyncMarkers.AddDefaulted_GetRef();
            Marker.MarkerName = FName(*SourceMarker.MarkerName);
            Marker.Time = static_cast<float>(
                static_cast<double>(SourceMarker.Frame) * FrameRate.Denominator
                / static_cast<double>(FrameRate.Numerator));
        }
    }

    const FAnimSequenceCreateResult Created = CreateAnimSequence(CreateSpec);
    if (!Created.bSuccess)
    {
        PwAnimCompilerPrivate::ReportCreateFailure(Created, Document, Result);
        return Result;
    }

    PwAnimCompilerPrivate::CopyCreateResult(Created, Result);
    Result.Diagnostics.Append(Created.Diagnostics);
    FString ReadbackError;
    if (!PwAnimCompilerPrivate::ReadBackMatchesRequest(
            Created, NumberOfFrames, ExpectedKeyCount, ReadbackError))
    {
        Result.ErrorCode = ErrorCodes::ERR_ASSET_DATA_INVALID;
        Result.ErrorMessage = ReadbackError;
        const int32 Line = Document.Header.VersionLine > 0 ? Document.Header.VersionLine : 1;
        const int32 Column = Document.Header.VersionColumn > 0 ? Document.Header.VersionColumn : 1;
        PwAnimCompilerPrivate::AddError(Result.Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, Line, Column, MoveTemp(ReadbackError));
        return Result;
    }

    Result.bSuccess = true;
    return Result;
}
