// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwAnimCompiler.h - .pwanim source text to one UAnimSequence.
//
// The compiler is deliberately independent of RPC and JSON.  The animation handler owns
// source-file loading and response marshalling; this seam owns the format's semantic checks,
// dense bake, skeleton reference and call to the animation asset creator.
#pragma once

#include "CoreMinimal.h"
#include "Containers/StringView.h"
#include "Misc/FrameRate.h"

#include "Handlers/Animation/AnimSequenceCreate.h"
#include "PwAnim/PwAnimDiagnostic.h"
#include "Utils/AssetSaveState.h"

struct FPwAnimCompileOptions
{
    // A long package name such as /Game/Animations/Walk.  Required for a real compile and
    // deliberately unused by validate-only runs.
    FString OutputAssetPath;

    // The source identity written to the generated asset's provenance stamp.  An empty path
    // is allowed for a direct library caller, but the RPC compile surface supplies the source
    // file path so same-source recompiles remain distinguishable from other sources.
    FString SourcePath;
    FString SourceHash;

    // Permission for an unstamped/differently generated occupant, or for discarding live state
    // identified by the recompile/takeover guard. Rebuild mechanics remain in-place.
    bool bOverwrite = false;

    // Uses the durable AssetUtils save contract when true.  False leaves the generated package
    // dirty and reports NotRequested.
    bool bSave = true;

    // Runs parsing, reference resolution and baking without creating an asset.  The requested
    // timebase and skeleton measurements still report; asset readback fields remain -1.
    bool bValidateOnly = false;

#if WITH_DEV_AUTOMATION_TESTS
    TFunction<bool(UAnimSequence*)> PostWriteCheckForTest;
#endif
};

struct FPwAnimCompileResult
{
    bool bSuccess = false;
    int32 Version = -1;

    // Empty on validate-only or before the asset creator returns.
    FString AssetPath;

    // The normalized object path and bone count are measured from the resolved USkeleton, not
    // echoed from the source use statement.
    FString SkeletonPath;
    int32 SkeletonBoneCount = -1;

    // Requested values, derived from the parsed timebase.
    FFrameRate RequestedFrameRate;
    int32 RequestedNumberOfFrames = -1;
    int32 RequestedNumberOfKeys = -1;
    double RequestedDurationSeconds = -1.0;
    bool bLoop = false;

    // Values re-read from the animation data model after the controller bracket closes.  They stay at
    // their sentinel values when validation creates no asset or asset creation fails.
    FFrameRate AssetFrameRate;
    int32 AssetNumberOfFrames = -1;
    int32 AssetNumberOfKeys = -1;
    int32 AssetTrackCount = -1;
    TArray<int32> AssetKeyCountPerTrack;

    // Parallel to AssetKeyCountPerTrack, in the data model's own track order. Names each track,
    // and states how many keys the ENGINE was expected to store for it - frames + 1 for a varying
    // track, 1 for one that holds a single pose, INDEX_NONE for a track this compile did not
    // write. See AnimSequenceCreate_TrackIsConstant for the definition.
    TArray<int32> ExpectedKeyCountPerTrack;
    TArray<FName> AssetTrackNames;
    TArray<FPwSyncMarkerSpec> AssetSyncMarkers;

    bool bSavedToDisk = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;

    // Asset-library failure details are kept separately from source diagnostics so a caller
    // can preserve the typed engine error while still receiving the normal diagnostic array.
    FString ErrorCode;
    FString ErrorMessage;

    TArray<FPwDiagnostic> Diagnostics;
};

class PINWRIGHT_API FPwAnimCompiler
{
public:
    // Parses, validates, bakes and (unless bValidateOnly) creates exactly one UAnimSequence.
    // Must run on the game thread because skeleton loading and asset creation touch UObjects.
    static FPwAnimCompileResult Compile(FStringView Source,
                                        const FPwAnimCompileOptions& Options);
};

using FPwAnimationCompileOptions = FPwAnimCompileOptions;
using FPwAnimationCompileResult = FPwAnimCompileResult;
using FPwAnimationCompiler = FPwAnimCompiler;
