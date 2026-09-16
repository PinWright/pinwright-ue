// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Misc/Paths.h"

#include "Handlers/Render/PoseListCapture.h"

// CROSSING A CAMERA LIST WITH A LIST OF INSTANTS: the one rule that makes a multi-camera time
// series a set of MOMENTS rather than a set of numbers.
//
// This lives in a header, out of the verb, for the reason the Niagara mechanism does: a test that
// needs a live preview viewport is a test that takes a conditional-skip path on a busy machine and
// reports success without running its assertions (board ticket B-test-skips-assertions-silently,
// which fired on this exact cluster of verbs). Everything asserted about the layout is asserted
// here, against no editor, no GPU and no asset.
//
// WHY THE INSTANT GOES ON THE FIRST CAMERA OF EACH GROUP AND ON NO OTHER.
// PinWrightPoseCapture::RunPoseListCapture calls the subject time setter exactly once for a pose
// that carries an instant and leaves the subject alone for a pose that carries none
// (PoseListCapture.h:87-91). The Niagara driver ends in DesiredAge hold mode at the age the
// simulation actually reached, so the particle state is HELD while the remaining cameras of the
// group fire (CaptureSubjectProviders_Niagara.h:256-260). Marking every pose would instead re-drive
// the subject per shot, and AdvanceToTime resets first - with system determinism off the instance
// seed is FMath::Rand() on every reset (NiagaraSystemInstance.cpp:894), so the six sides of "one
// instant" would be six DIFFERENT effects that merely share a timestamp. That is not a performance
// argument; it is the difference between a set that can be compared and one that cannot.
//
// The same rule is right for a scrubbed animation subject for a weaker reason - one scrub per
// instant cannot land differently between angles - so the rule is stated once for every kind
// rather than per provider.
namespace PinWrightSubjectTimeSeries
{
    // Instant-major expansion of Cameras x Instants.
    //
    // OutPoses is Cameras.Num() * Instants.Num() long, ordered every camera of instant 0, then
    // every camera of instant 1, and so on. OutInstantIndex and OutCameraIndex are parallel to it,
    // so a caller can index its own per-camera tables (angles, ortho-snap flags) by the CAMERA
    // rather than by the shot - which is where a time series otherwise silently reads the wrong
    // row, because shot index and camera index stop being the same number.
    //
    // FILENAMES ARE ALWAYS REWRITTEN, never inherited. The capture util's auto-name carries a
    // one-second timestamp and a capture takes ~60 ms, so a fixed-camera series left to it writes
    // every shot to ONE file: N entries in the response and one PNG on disk. That trap has already
    // produced three "independent" readings of a single image on this project. The instant is
    // spelled in whole milliseconds so the name sorts lexically and carries no decimal point.
    //
    // CameraTags is optional and may be shorter than Cameras (or empty). A tag is appended verbatim
    // after the shot index; the caller owns its shape, because only the caller knows whether its
    // cameras have angles worth naming.
    inline void CrossCamerasWithInstants(
        TConstArrayView<PinWrightPoseCapture::FCameraPose> Cameras,
        TConstArrayView<double> Instants,
        const FString& FilenameStem,
        TConstArrayView<FString> CameraTags,
        TArray<PinWrightPoseCapture::FCameraPose>& OutPoses,
        TArray<int32>& OutInstantIndex,
        TArray<int32>& OutCameraIndex)
    {
        OutPoses.Reset();
        OutInstantIndex.Reset();
        OutCameraIndex.Reset();
        if (Cameras.Num() == 0 || Instants.Num() == 0)
        {
            return;
        }

        const int32 Total = Cameras.Num() * Instants.Num();
        OutPoses.Reserve(Total);
        OutInstantIndex.Reserve(Total);
        OutCameraIndex.Reserve(Total);

        for (int32 InstantIndex = 0; InstantIndex < Instants.Num(); ++InstantIndex)
        {
            const int32 Milliseconds = FMath::RoundToInt(Instants[InstantIndex] * 1000.0);
            for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
            {
                PinWrightPoseCapture::FCameraPose Pose = Cameras[CameraIndex];
                if (CameraIndex == 0)
                {
                    // Float on the way in, double on the way back out of the optional inside the
                    // primitive. Float widens to double exactly, so nothing is lost; FCameraPose
                    // keeps float so a caller assigning an engine animation time does not narrow.
                    Pose.SubjectTimeSeconds = static_cast<float>(Instants[InstantIndex]);
                }
                else
                {
                    // Explicit, not merely inherited from the copy: a camera that is not the first
                    // of its group must carry NO instant, or the primitive re-drives the subject.
                    Pose.SubjectTimeSeconds.Reset();
                }
                const FString Tag = CameraTags.IsValidIndex(CameraIndex) ? CameraTags[CameraIndex] : FString();
                Pose.Filename = FString::Printf(TEXT("%s_t%04dms_shot%02d%s.png"),
                    *FilenameStem, Milliseconds, CameraIndex, *Tag);
                OutPoses.Add(MoveTemp(Pose));
                OutInstantIndex.Add(InstantIndex);
                OutCameraIndex.Add(CameraIndex);
            }
        }
    }
}
