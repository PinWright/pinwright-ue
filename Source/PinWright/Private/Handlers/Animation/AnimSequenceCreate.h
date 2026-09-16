// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameRate.h"
#include "PwSource/PwDiagnostic.h"
#include "Utils/AssetSaveState.h"

class UAnimSequence;
class USkeleton;

// One dense local-space animation track.  The controller accepts these native float types;
// keeping them here avoids an implicit double-to-float conversion at the write boundary.
struct FPwBoneTrackSpec
{
    FName BoneName;
    TArray<FVector3f> Pos;
    TArray<FQuat4f> Rot;
    TArray<FVector3f> Scale;
};

// A marker label and its measured timeline position. The source compiler converts integer
// frames to seconds before crossing into the engine asset API; RPC callers use the same type.
struct FPwSyncMarkerSpec
{
    FName MarkerName;
    float Time = 0.0f;
};

struct FAnimSequenceCreateSpec
{
    // A long package name, for example /Game/Animations/Walk.  The creator appends no
    // extension and does not accept an object-path suffix.
    FString AssetPath;
    USkeleton* Skeleton = nullptr;
    FFrameRate FrameRate;
    int32 NumberOfFrames = 0;
    // The source format's requested default playback behavior. This is distinct from the
    // compiler's optional seam validation: a sequence can be authored as one-shot or looping,
    // and the resulting UAnimSequence must carry that choice for asset players that honor it.
    bool bLoop = false;
    bool bOverwrite = false;
    bool bSave = false;
    TArray<FPwBoneTrackSpec> Tracks;

    // Direct library callers may choose replacement. A source compiler always sets this true;
    // an omitted marker statement is the empty source-owned marker set.
    bool bReplaceSyncMarkers = false;
    TArray<FPwSyncMarkerSpec> SyncMarkers;

    // Optional source provenance supplied by a source-format compiler.  Empty means that the
    // caller is not asking this library to write a source stamp; it never matches an existing
    // stamp for overwrite permission.
    FString SourcePath;
    FString SourceHash;

#if WITH_DEV_AUTOMATION_TESTS
    // Failure-direction seam: rejects an otherwise finished write before provenance and save.
    TFunction<bool(UAnimSequence*)> PostWriteCheckForTest;
#endif
};

// State that an IN-PLACE rebuild carries across on the reused object but cannot keep coherent,
// because it is bound to the timeline or the skeleton the object used to have. A rebuild keeps
// referencers alive on purpose; the cost is that these ride along, and nothing in a source format
// that cannot express them can put them right. Every field is MEASURED either side of the
// rebuild - none of it is inferred from what the source asked for.
struct FAnimSequenceRebuildCarryOver
{
    bool bRebuiltInPlace = false;
    bool bSkeletonChanged = false;
    bool bSyncMarkersReplaced = false;

    // Non-empty means the sequence still names a retarget source on the OLD skeleton. It is a
    // plain FName lookup, so a skeleton that has no such entry leaves it dangling and silent.
    FName InheritedRetargetSource;

    double PreviousLengthSeconds = -1.0;
    double LengthSeconds = -1.0;

    // Moved = the engine changed the time while the sequence was being re-timed, which for a
    // shorter sequence means clamped to the new end. Several markers clamped to the same instant
    // is not a preserved marker set; it is a destroyed one that still has the right count.
    int32 SyncMarkerCount = 0;
    int32 SyncMarkersMoved = 0;
    int32 NotifyCount = 0;
    int32 NotifiesMoved = 0;
};

struct FAnimSequenceCreateResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    FString AssetPath;
    FFrameRate AssetFrameRate;
    int32 AssetNumberOfFrames = -1;
    int32 AssetNumberOfKeys = -1;
    int32 AssetTrackCount = -1;
    TArray<int32> AssetKeyCountPerTrack;

    // Parallel to AssetKeyCountPerTrack: how many keys the ENGINE is expected to store for that
    // track given what was requested. It is NumberOfFrames + 1 for a varying track and 1 for a
    // constant one - see AnimSequenceCreate_TrackIsConstant. Published so a caller can re-assert
    // the readback without re-deriving constancy and drifting from the definition used here.
    TArray<int32> ExpectedKeyCountPerTrack;
    TArray<FName> AssetTrackNames;
    TArray<FPwSyncMarkerSpec> AssetSyncMarkers;
    TArray<FPwDiagnostic> Diagnostics;
    FAnimSequenceRebuildCarryOver CarryOver;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
};

struct FAnimSequenceWriteResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    FString AssetPath;
    FFrameRate AssetFrameRate;
    int32 AssetNumberOfFrames = -1;
    int32 AssetNumberOfKeys = -1;
    int32 AssetTrackCount = -1;
    int32 TrackKeyCount = -1;
    TArray<int32> AssetKeyCountPerTrack;
    TArray<int32> ExpectedKeyCountPerTrack;
    TArray<FName> AssetTrackNames;
    TArray<FPwSyncMarkerSpec> AssetSyncMarkers;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
};

struct FAnimSequenceSyncMarkerWriteResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    int32 RemovedCount = 0;
};

// Creates or rebuilds an AnimSequence, applies all tracks in the required controller order,
// and measures the resulting data model before returning.
PINWRIGHT_API FAnimSequenceCreateResult CreateAnimSequence(const FAnimSequenceCreateSpec& Spec);

// Replaces all authored markers atomically after validating every name and time. The helper
// rebuilds UE's derived marker state and notify-track links before returning.
PINWRIGHT_API FAnimSequenceSyncMarkerWriteResult SetAnimSequenceSyncMarkers(
    UAnimSequence* Sequence,
    TArrayView<const FPwSyncMarkerSpec> Markers);

// Removes every authored occurrence of one label and refreshes the same derived state as a set.
PINWRIGHT_API FAnimSequenceSyncMarkerWriteResult RemoveAnimSequenceSyncMarkers(
    UAnimSequence* Sequence,
    FName MarkerName);

PINWRIGHT_API TArray<FPwSyncMarkerSpec> ReadAnimSequenceSyncMarkers(
    const UAnimSequence* Sequence);

// Writes dense tracks to an existing sequence.  Every track must contain exactly
// Sequence->GetDataModel()->GetNumberOfFrames() + 1 keys; this boundary refuses the engine's
// permissive short-track behavior before it can make runtime evaluation and model readback
// disagree.
PINWRIGHT_API FAnimSequenceWriteResult WriteBoneTracks(
    UAnimSequence* Sequence,
    TArrayView<const FPwBoneTrackSpec> Tracks,
    bool bSave);
