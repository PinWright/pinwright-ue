// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/PoseListCapture.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/FlatRegionStats.h"

#include "AssetViewerSettings.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace PinWrightPoseCapture
{
namespace
{
    // Fill the per-frame request from one pose plus the set-level state. One function so a
    // set-level field cannot be applied to some shots and not others - the failure mode a
    // hand-rolled loop per verb keeps reproducing.
    PinWrightRenderCapture::FViewportCaptureRequest MakeFrameRequest(
        const FPoseListCaptureRequest& Request, const FCameraPose& Pose)
    {
        PinWrightRenderCapture::FViewportCaptureRequest Frame;
        Frame.Width = Request.Width;
        Frame.Height = Request.Height;
        Frame.ProjectionMode = Pose.ProjectionMode;
        Frame.Location = Pose.Location;
        Frame.Rotation = Pose.Rotation;
        // The generator computed both, so neither is a parser default: telling the capture that
        // they were provided is what stops it framing the no-args path instead of this pose.
        Frame.bLocationProvided = true;
        Frame.bRotationProvided = true;
        Frame.Fov = Pose.Fov;
        Frame.OrthoWidth = Pose.OrthoWidth;
        Frame.Filename = Pose.Filename;
        Frame.Exposure = Request.Exposure;
        Frame.bHideEditorSprites = Request.bHideEditorSprites;
        Frame.ViewMode = Request.ViewMode;
        Frame.PreviewSceneRig = Request.PreviewSceneRig;
        Frame.bPreviewSceneRigAlreadyScoped = Request.bPreviewSceneRigAlreadyScoped;
        Frame.PreviewSceneRigAtSetEntry = Request.PreviewSceneRigAtSetEntry;
        Frame.bPreviewSceneCaptureUpdatedAtSetEntry =
            Request.bPreviewSceneCaptureUpdatedAtSetEntry;
        Frame.bPreviewSceneCaptureIncompleteAtSetEntry =
            Request.bPreviewSceneCaptureIncompleteAtSetEntry;
        Frame.ViewportCaptureSetContext = Request.ViewportCaptureSetContext;
        Frame.ViewDistanceScale = Request.ViewDistanceScale;
        Frame.bViewDistanceScaleProvided = Request.bViewDistanceScaleProvided;
        Frame.bAutoViewDistanceScale = Request.bAutoViewDistanceScale;
        Frame.bRejectBlankCapture = Request.bRejectBlankCapture;
        Frame.bAllowBlank = Request.bAllowBlank;
        Frame.bRetainPixels = Request.bRetainPixels;
        // The same bounds EvaluateBoundsFraming is given after the capture, handed to the capture
        // as well so it can measure the subject's own luminance while the readback buffer is still
        // alive. Framing answers "is the subject IN this frame"; the subject region answers "did
        // it carry any light once it was".
        Frame.BoundsOrigin = Request.BoundsOrigin;
        Frame.BoundsRadius = Request.BoundsRadius;
        return Frame;
    }
}

bool RunPoseListCapture(
    const FPoseListCaptureRequest& Request,
    const FPoseFrameCapturer& CaptureFrame,
    FPoseListCaptureOutput& OutResult,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutResult.bPoseRepeatabilityControlShotTaken = false;
    OutResult.bPoseRepeatabilityControlShotDiscarded = false;
    OutResult.bPoseRepeatabilityMeasured = false;
    OutResult.PoseRepeatabilityNotMeasuredReason.Reset();
    OutResult.PoseRepeatabilityMeanAbsDelta = 0.0;
    OutResult.PoseRepeatabilityMaxDelta = 0;
    OutResult.PoseRepeatabilityChangedPixelFraction = 0.0;

    OutResult.PosesRequested = Request.Poses.Num();
    OutResult.MaxPoses = FMath::Max(Request.MaxPoses, 1);
    const int32 CaptureCount = FMath::Min(Request.Poses.Num(), OutResult.MaxPoses);
    OutResult.PosesTruncated = Request.Poses.Num() - CaptureCount;
    // Recorded from the REQUEST, not from what happened, so "this set had a time axis / had
    // bounds" survives a set that then failed on its first pose.
    OutResult.bSubjectTimeDriven = static_cast<bool>(Request.SubjectTimeSetter);
    OutResult.bFramingRequested = Request.BoundsRadius > 0.0;
    // BOTH conditions, and the conjunction is the contract: asking for coverage on a kind that
    // cannot hide its subject is not an error, it is an unmeasurable request, and it must read as
    // "nobody could measure this" rather than as "nobody asked".
    const bool bMeasureCoverage =
        Request.bMeasureSubjectCoverage && static_cast<bool>(Request.SubjectVisibilitySetter);
    OutResult.bCoverageRequested = bMeasureCoverage;
    if (Request.bMeasurePoseRepeatability)
    {
        OutResult.PoseRepeatabilityNotMeasuredReason =
            TEXT("the set ended before its final repeatability control shot");
    }
    ON_SCOPE_EXIT
    {
        if (Request.bMeasurePoseRepeatability && !Request.bRetainPixels
            && OutResult.Captures.Num() > 0)
        {
            OutResult.Captures[0].Pixels.Empty();
        }
    };

    if (!CaptureFrame)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("A pose list capture needs a frame capturer; none was bound.");
        return false;
    }

    if (CaptureCount <= 0)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = TEXT("A pose list capture needs at least one {location, rotation} pose.");
        return false;
    }

    // ---- the subject time driver ----
    // ONE application per pose. The warm-up frame reuses Poses[0], so pose 0's instant is applied
    // before the warm-up and the shot loop below finds it already applied; without that bookkeeping
    // the setter would run Poses.Num() + 1 times, and a setter is not required to be idempotent -
    // a Niagara advance from the current state applied twice lands at twice the age.
    //
    // Truncated poses are never driven: they are not captured, and advancing a simulation for a
    // frame nobody photographs is both a cost and a state change the response does not report.
    int32 SubjectTimeAppliedForPose = INDEX_NONE;
    auto DriveSubjectTime = [&Request, &OutResult, &OutErrorCode, &OutErrorMessage,
                             &SubjectTimeAppliedForPose](int32 PoseIndex) -> bool
    {
        const FCameraPose& Pose = Request.Poses[PoseIndex];
        if (!Pose.SubjectTimeSeconds.IsSet())
        {
            // No instant asked for: the subject stays where it is. Not an error, and not counted.
            return true;
        }
        const double TimeSeconds = static_cast<double>(Pose.SubjectTimeSeconds.GetValue());
        if (!Request.SubjectTimeSetter)
        {
            // REFUSED rather than ignored. A dropped instant returns a valid PNG of the wrong
            // moment, which is the failure mode this whole cluster of verbs keeps paying for.
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(
                TEXT("Pose %d asks for the subject at %.6f s, but this capture set has no subject ")
                TEXT("time setter bound, so the instant could only have been dropped and the frame ")
                TEXT("captured at whatever moment the subject happened to be at. Either the subject ")
                TEXT("kind has no time axis - in which case do not set a pose time - or the caller ")
                TEXT("did not pass the resolver's setter into FPoseListCaptureRequest."),
                PoseIndex, TimeSeconds);
            return false;
        }
        if (SubjectTimeAppliedForPose == PoseIndex)
        {
            return true;
        }
        if (!Request.SubjectTimeSetter(TimeSeconds, OutErrorCode, OutErrorMessage))
        {
            // The setter owns the refusal and fills the pair. These fallbacks exist only so a
            // setter that forgets cannot produce a failure with no code attached to it.
            if (OutErrorCode.IsEmpty())
            {
                OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
            }
            if (OutErrorMessage.IsEmpty())
            {
                OutErrorMessage = FString::Printf(
                    TEXT("The subject could not be set to %.6f s for pose %d, and the time setter ")
                    TEXT("gave no reason."), TimeSeconds, PoseIndex);
            }
            return false;
        }
        SubjectTimeAppliedForPose = PoseIndex;
        ++OutResult.SubjectTimesApplied;
        return true;
    };

    // ---- warm-up shot ----
    // Taken at the FIRST real pose rather than at some neutral camera, so whatever the viewport
    // has to page in - streamed textures, shadow maps, the ambient contribution the settle loop
    // does not catch - is paged in for content the set actually shows. Its output is deleted and
    // it is not among the reported shots; a caller reads `warmupShotTaken` to know it happened.
    if (Request.bWarmupShot)
    {
        // The subject is driven to pose 0's instant BEFORE the throwaway frame, so the warm-up
        // pages in the content the set actually shows at the moment it shows it - and so this is
        // the one and only application for pose 0.
        //
        // A time-setter failure here IS fatal, unlike a warm-up capture failure: the subject is
        // not where the set needs it, so pose 0 is about to fail identically and every later pose
        // would be captured from a subject state nobody asked for.
        if (!DriveSubjectTime(0))
        {
            return false;
        }

        PinWrightRenderCapture::FViewportCaptureRequest WarmupFrame =
            MakeFrameRequest(Request, Request.Poses[0]);
        // The GUID preserves the named warm-up request contract without reintroducing the fixed
        // throwaway name that could overwrite and then delete a caller-owned artifact.
        WarmupFrame.Filename = FString::Printf(TEXT("%s_warmup_%s.png"),
            Request.FilenamePrefix.IsEmpty() ? TEXT("PoseSet") : *Request.FilenamePrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        // A blank warm-up frame is not a failure of the set: it is a throwaway, and rejecting it
        // would turn a mitigation into a new way for the call to fail.
        WarmupFrame.bRejectBlankCapture = false;
        WarmupFrame.bAllowBlank = true;

        PinWrightRenderCapture::FViewportCaptureOutput WarmupCapture;
        FString WarmupErrorCode;
        FString WarmupErrorMessage;
        if (CaptureFrame(WarmupFrame, /*bWarmupFrame=*/true, WarmupCapture,
                WarmupErrorCode, WarmupErrorMessage))
        {
            OutResult.bWarmupShotTaken = true;
            OutResult.WarmupShotPath = WarmupCapture.Path;
            if (!WarmupCapture.Path.IsEmpty())
            {
                OutResult.bWarmupShotDiscarded = IFileManager::Get().Delete(
                    *WarmupCapture.Path, /*RequireExists=*/false, /*EvenReadOnly=*/true,
                    /*Quiet=*/true);
            }
        }
        // A failed warm-up shot is deliberately NOT fatal and deliberately not reported as an
        // error: the set's own first shot is about to run the same path and will report the same
        // failure with the caller's own filename. Failing here would convert a warm-up
        // optimisation into a hard dependency.
    }

    OutResult.Captures.Reserve(CaptureCount);
    OutResult.Requests.Reserve(CaptureCount);
    OutResult.Poses.Reserve(CaptureCount);
    OutResult.Framings.Reserve(CaptureCount);
    OutResult.SubjectCoverage.Reserve(CaptureCount);

    for (int32 Index = 0; Index < CaptureCount; ++Index)
    {
        if (!DriveSubjectTime(Index))
        {
            // Same contract as a failed capture: what was captured before this pose stays in
            // OutResult, so a caller reports "three of five, then the subject would not move".
            return false;
        }

        const FCameraPose& Pose = Request.Poses[Index];
        PinWrightRenderCapture::FViewportCaptureRequest Frame = MakeFrameRequest(Request, Pose);

        // ---- the coverage reference frame ----
        //
        // Same pose, same instant, same lighting, subject hidden. Drawn IMMEDIATELY before the real
        // shot rather than once for the whole set, because the only thing that may differ between
        // the two frames is the subject: a reference taken at another pose would measure the camera
        // move, and one taken at another instant would measure the simulation.
        //
        // The file is a throwaway and is deleted the way the warm-up's is. It is captured with
        // bWarmupFrame=true for the same reason - it is not one of the set's shots and must never
        // be counted, reported or left on disk as one.
        TArray<FColor> ReferencePixels;
        bool bReferenceDrawn = false;
        if (bMeasureCoverage)
        {
            FString HideErrorCode;
            FString HideErrorMessage;
            if (Request.SubjectVisibilitySetter(/*bVisible=*/false, HideErrorCode, HideErrorMessage))
            {
                PinWrightRenderCapture::FViewportCaptureRequest ReferenceFrame = Frame;
                // Like the warm-up and repeatability control, this path is deleted below. Let the
                // screenshot helper generate a collision-checked name so it cannot be a caller's.
                ReferenceFrame.Filename.Reset();
                ReferenceFrame.bRetainPixels = true;
                // A frame with the subject hidden is SUPPOSED to be empty, and on an unlit backdrop
                // it is supposed to be black. Letting the blank gate reject it would turn the
                // measurement of an empty frame into a failure to take one - which is precisely the
                // case this differential exists to report rather than to refuse.
                ReferenceFrame.bRejectBlankCapture = false;
                ReferenceFrame.bAllowBlank = true;

                PinWrightRenderCapture::FViewportCaptureOutput ReferenceCapture;
                FString ReferenceErrorCode;
                FString ReferenceErrorMessage;
                if (CaptureFrame(ReferenceFrame, /*bWarmupFrame=*/true, ReferenceCapture,
                        ReferenceErrorCode, ReferenceErrorMessage))
                {
                    ReferencePixels = MoveTemp(ReferenceCapture.Pixels);
                    bReferenceDrawn = true;
                    ++OutResult.CoverageReferenceShots;
                    if (!ReferenceCapture.Path.IsEmpty())
                    {
                        IFileManager::Get().Delete(*ReferenceCapture.Path, /*RequireExists=*/false,
                            /*EvenReadOnly=*/true, /*Quiet=*/true);
                    }
                }
                // A failed reference capture costs this shot its coverage number and nothing else.
                // The real shot below runs the same path and reports the same failure with the
                // caller's own filename if it is a genuine capture fault.

                // RESTORING VISIBILITY IS FATAL IF IT FAILS, and this is the one place in the whole
                // measurement that must not be lenient. Every remaining shot of the set would be
                // captured with the subject hidden, come back as pure backdrop, and - measured
                // against an equally hidden reference - report a coverage of 0.000 while `blank`,
                // `litPixelFraction` and `boundsInFrame` all still read healthy. That is the exact
                // silent-empty-frame failure this seam was built to catch, so it may not be
                // introduced by the catcher.
                FString ShowErrorCode;
                FString ShowErrorMessage;
                if (!Request.SubjectVisibilitySetter(/*bVisible=*/true, ShowErrorCode, ShowErrorMessage))
                {
                    OutErrorCode = ShowErrorCode.IsEmpty()
                        ? FString(ErrorCodes::ERR_CAPTURE_FAILED) : ShowErrorCode;
                    OutErrorMessage = FString::Printf(
                        TEXT("The subject was hidden to measure coverage for pose %d and could not be ")
                        TEXT("shown again%s%s. The set was stopped here: every later shot would have ")
                        TEXT("been a capture of an invisible subject that no published signal - not ")
                        TEXT("`blank`, not `litPixelFraction`, not `framing` - would have reported as ")
                        TEXT("empty."),
                        Index,
                        ShowErrorMessage.IsEmpty() ? TEXT("") : TEXT(": "),
                        ShowErrorMessage.IsEmpty() ? TEXT("") : *ShowErrorMessage);
                    return false;
                }
            }
            // A refused HIDE is deliberately NOT fatal: the subject was never touched, so the set is
            // still exactly the set that was asked for - it simply carries no coverage figure for
            // this shot, which `coverageReferenceShots` below the shot count is what tells a caller.
        }
        // Pose 0 is retained until the final same-pose control is taken. Every later buffer still
        // follows the caller/coverage policy, so a 24-shot set does not accumulate 96 MB.
        Frame.bRetainPixels = Request.bRetainPixels || bReferenceDrawn
            || (Request.bMeasurePoseRepeatability && Index == 0);

        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        if (!CaptureFrame(Frame, /*bWarmupFrame=*/false, Capture, OutErrorCode, OutErrorMessage))
        {
            // Everything captured so far stays in OutResult so the caller can say which pose
            // failed rather than discarding a partially finished set.
            return false;
        }

        // ---- the differential ----
        //
        // Unset unless BOTH frames came back and are comparable, so a shot whose reference failed,
        // whose readback was short, or whose viewport resized mid-set reports no number rather than
        // a number measured against the wrong pixels.
        TOptional<double> Coverage;
        if (bReferenceDrawn)
        {
            double Fraction = 0.0;
            if (MeasureChangedPixelFraction(ReferencePixels, Capture.Pixels,
                    Request.CoverageChannelThreshold, Fraction))
            {
                Coverage = Fraction;
            }
            // Both buffers go NOW, before the capture is moved into the result. A 1024x1024 frame is
            // 4 MB and a 24-shot set holds one FViewportCaptureOutput per pose for the whole call,
            // so keeping them would cost ~96 MB to carry data nothing reads again.
            ReferencePixels.Empty();
            if (!Request.bRetainPixels
                && !(Request.bMeasurePoseRepeatability && Index == 0))
            {
                Capture.Pixels.Empty();
            }
        }

        // Was the subject in this frame at all? Measured against the pose the RENDERER resolved
        // to, never the requested one: an orbit-mode preview viewport derives the eye from its own
        // pivot and keeps only the requested location's distance, so a mis-aimed capture returns a
        // perfectly valid PNG and framing measured off the request would call it framed. `blank`
        // cannot answer this either - it catches BLACK frames, and a lit backdrop is not black.
        PinWrightRenderCapture::FBoundsFramingCheck Framing;
        if (Request.BoundsRadius > 0.0)
        {
            Framing = PinWrightRenderCapture::EvaluateBoundsFraming(Frame,
                Capture.EffectiveLocation, Capture.EffectiveRotation,
                Request.BoundsOrigin, Request.BoundsRadius);
        }
        if (Framing.bEvaluated)
        {
            ++OutResult.FramingsEvaluated;
            if (!Framing.bBoundsInFrame)
            {
                ++OutResult.PosesOutOfFrame;
            }
        }

        OutResult.Captures.Add(MoveTemp(Capture));
        OutResult.Requests.Add(Frame);
        OutResult.Poses.Add(Pose);
        OutResult.Framings.Add(MoveTemp(Framing));
        // Added on EVERY captured pose, measured or not, because the array's contract is that it is
        // parallel to Captures. An entry skipped when nothing was measured would silently shift
        // every later shot's number onto the wrong shot.
        OutResult.SubjectCoverage.Add(Coverage);
    }

    // ---- same-pose set control ----
    // Re-shoot pose 0 only when the subject did not move on a time axis. Subject setters are not
    // required to be idempotent (a Niagara setter may advance from current state), so replaying
    // pose 0 after the set could create the very difference this control is meant to detect.
    bool bAnySubjectTime = false;
    for (int32 Index = 0; Request.bMeasurePoseRepeatability && Index < CaptureCount; ++Index)
    {
        bAnySubjectTime |= Request.Poses[Index].SubjectTimeSeconds.IsSet();
    }
    if (!Request.bMeasurePoseRepeatability)
    {
        // Only injected sequence tests use this path. Every real viewport pose set enables the
        // control in CaptureCameraPoses below.
    }
    else if (bAnySubjectTime)
    {
        OutResult.PoseRepeatabilityNotMeasuredReason =
            TEXT("the set drove subject time and cannot safely replay pose 0 without changing simulation state");
    }
    else if (OutResult.Captures.Num() > 0)
    {
        PinWrightRenderCapture::FViewportCaptureRequest ControlFrame =
            MakeFrameRequest(Request, Request.Poses[0]);
        // The generated-name path checks for collisions, so the file deleted below is one this
        // capture created rather than a same-named caller artifact it overwrote.
        ControlFrame.Filename.Reset();
        ControlFrame.bRetainPixels = true;
        ControlFrame.bRejectBlankCapture = false;
        ControlFrame.bAllowBlank = true;

        PinWrightRenderCapture::FViewportCaptureOutput ControlCapture;
        FString ControlErrorCode;
        FString ControlErrorMessage;
        if (CaptureFrame(ControlFrame, /*bWarmupFrame=*/true, ControlCapture,
                ControlErrorCode, ControlErrorMessage))
        {
            OutResult.bPoseRepeatabilityControlShotTaken = true;
            if (!ControlCapture.Path.IsEmpty())
            {
                OutResult.bPoseRepeatabilityControlShotDiscarded = IFileManager::Get().Delete(
                    *ControlCapture.Path, /*RequireExists=*/false, /*EvenReadOnly=*/true,
                    /*Quiet=*/true);
            }
            const PinWrightFlatRegion::FFrameDifferenceStats Difference =
                PinWrightFlatRegion::MeasureFrameDifference(
                    OutResult.Captures[0].Pixels, ControlCapture.Pixels,
                    OutResult.PoseRepeatabilityChannelThreshold);
            OutResult.bPoseRepeatabilityMeasured = Difference.bMeasured;
            OutResult.PoseRepeatabilityMeanAbsDelta = Difference.MeanAbsDelta;
            OutResult.PoseRepeatabilityMaxDelta = Difference.MaxDelta;
            OutResult.PoseRepeatabilityChangedPixelFraction = Difference.ChangedPixelFraction;
            if (Difference.bMeasured)
            {
                OutResult.PoseRepeatabilityNotMeasuredReason.Reset();
            }
            else
            {
                OutResult.PoseRepeatabilityNotMeasuredReason =
                    TEXT("the original and control pixel buffers were empty or different sizes");
            }
        }
        else
        {
            OutResult.PoseRepeatabilityNotMeasuredReason = ControlErrorMessage.IsEmpty()
                ? TEXT("the repeatability control capture failed") : ControlErrorMessage;
        }
    }

    return true;
}

bool CaptureCameraPoses(
    FEditorViewportClient& ViewportClient,
    const TSharedPtr<FSceneViewport>& SceneViewport,
    const FPoseListCaptureRequest& Request,
    FPoseListCaptureOutput& OutResult,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    if (!PinWrightPreviewSceneRig::CanApplyRig(ViewportClient, Request.PreviewSceneRig,
            OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    const PinWrightPreviewSceneRig::FSharedProfileSnapshot RigSnapshotAtEntry =
        PinWrightPreviewSceneRig::CaptureSharedProfiles();
    FPoseListCaptureRequest ScopedRequest = Request;
    PinWrightRenderCapture::FViewportCaptureRestoreReceipt ViewportRestoreReceipt;
    bool bCaptureSucceeded = false;
    {
        // One rig lifetime for warm-up, every requested pose, and, for non-time-driven sets, the
        // final same-pose control. Individual captures only measure the rig; they do not restore it.
        PinWrightPreviewSceneRig::FScopedPreviewSceneRig RigScope(
            ViewportClient, Request.PreviewSceneRig);
        PinWrightRenderCapture::FViewportCaptureSetContext ViewportCaptureSetContext(
            ViewportClient, SceneViewport, Request.Exposure, ViewportRestoreReceipt);
        if (!ViewportCaptureSetContext.IsValid())
        {
            OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
            OutErrorMessage = TEXT("The editor preview viewport is already owned by another capture context");
            return false;
        }
        ScopedRequest.bPreviewSceneRigAlreadyScoped = true;
        ScopedRequest.PreviewSceneRigAtSetEntry = RigScope.GetPreviousRig();
        ScopedRequest.bPreviewSceneCaptureUpdatedAtSetEntry = RigScope.CaptureContentsUpdated();
        ScopedRequest.bPreviewSceneCaptureIncompleteAtSetEntry =
            RigScope.CaptureContentsIncomplete();
        ScopedRequest.ViewportCaptureSetContext = &ViewportCaptureSetContext;
        ScopedRequest.bMeasurePoseRepeatability = true;

        bCaptureSucceeded = RunPoseListCapture(ScopedRequest,
            [&ViewportClient, &SceneViewport, &ScopedRequest](
                const PinWrightRenderCapture::FViewportCaptureRequest& Frame,
                bool /*bWarmupFrame*/,
                PinWrightRenderCapture::FViewportCaptureOutput& OutCapture,
                FString& OutFrameErrorCode,
                FString& OutFrameErrorMessage)
            {
                return PinWrightRenderCapture::CaptureEditorViewportToPng(
                    ViewportClient, SceneViewport, Frame, ScopedRequest.FilenamePrefix,
                    ScopedRequest.Subdirectory, OutCapture, OutFrameErrorCode,
                    OutFrameErrorMessage);
            },
            OutResult, OutErrorCode, OutErrorMessage);
    }

    const PinWrightPreviewSceneRig::FPreviewSceneRigReport RigAfter =
        PinWrightPreviewSceneRig::MeasureRig(ViewportClient);
    bool bSharedProfilesRestored = true;
    if (RigSnapshotAtEntry.bCaptured)
    {
        UAssetViewerSettings* RigSettings = UAssetViewerSettings::Get();
        bSharedProfilesRestored = RigSettings
            && PinWrightPreviewSceneRig::SharedProfilesMatch(
                RigSettings->Profiles, RigSnapshotAtEntry.Profiles);
    }
    const FString ConfigDigestAfter = PinWrightPreviewSceneRig::DigestFile(
        PinWrightPreviewSceneRig::PreviewSceneConfigFilePath());
    for (PinWrightRenderCapture::FViewportCaptureOutput& Capture : OutResult.Captures)
    {
        Capture.bExposureRestored = ViewportRestoreReceipt.bExposureRestored;
        Capture.PreviewSceneRigAfter = RigAfter;
        Capture.bPreviewSceneRigRestored = PinWrightPreviewSceneRig::RigReportsMatch(
            RigAfter, ScopedRequest.PreviewSceneRigAtSetEntry);
        Capture.bSharedProfilesRestored = bSharedProfilesRestored;
        Capture.ConfigFileDigest = ConfigDigestAfter;
        Capture.bConfigFileUnchanged = ConfigDigestAfter == RigSnapshotAtEntry.ConfigDigest;
    }
    return bCaptureSucceeded;
}

TSharedPtr<FJsonObject> MakePoseSetInfoObject(const FPoseListCaptureOutput& Result)
{
    TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
    Info->SetNumberField(TEXT("posesRequested"), Result.PosesRequested);
    Info->SetNumberField(TEXT("posesCaptured"), Result.Captures.Num());
    Info->SetNumberField(TEXT("posesTruncated"), Result.PosesTruncated);
    Info->SetNumberField(TEXT("maxPosesPerCall"), Result.MaxPoses);
    Info->SetBoolField(TEXT("warmupShotTaken"), Result.bWarmupShotTaken);
    Info->SetBoolField(TEXT("warmupShotDiscarded"), Result.bWarmupShotDiscarded);
    {
        TSharedPtr<FJsonObject> Repeatability = MakeShared<FJsonObject>();
        Repeatability->SetBoolField(TEXT("measured"), Result.bPoseRepeatabilityMeasured);
        Repeatability->SetBoolField(TEXT("controlShotTaken"),
            Result.bPoseRepeatabilityControlShotTaken);
        Repeatability->SetBoolField(TEXT("controlShotDiscarded"),
            Result.bPoseRepeatabilityControlShotDiscarded);
        Repeatability->SetNumberField(TEXT("channelThreshold"),
            Result.PoseRepeatabilityChannelThreshold);
        if (Result.bPoseRepeatabilityMeasured)
        {
            Repeatability->SetNumberField(TEXT("meanAbsDelta"),
                Result.PoseRepeatabilityMeanAbsDelta);
            Repeatability->SetNumberField(TEXT("maxDelta"),
                Result.PoseRepeatabilityMaxDelta);
            Repeatability->SetNumberField(TEXT("changedPixelFraction"),
                Result.PoseRepeatabilityChangedPixelFraction);
        }
        else if (!Result.PoseRepeatabilityNotMeasuredReason.IsEmpty())
        {
            Repeatability->SetStringField(TEXT("notMeasuredReason"),
                Result.PoseRepeatabilityNotMeasuredReason);
        }
        Info->SetObjectField(TEXT("poseRepeatability"), Repeatability);
    }

    if (Result.PosesTruncated > 0)
    {
        Info->SetStringField(TEXT("truncationWarning"), FString::Printf(
            TEXT("%d of the %d requested poses were dropped: a capture set is capped at %d shots ")
            TEXT("because each one moves a real editor camera and does a full offscreen readback. ")
            TEXT("The first %d poses were captured, in order. Split the rest into a second call."),
            Result.PosesTruncated, Result.PosesRequested, Result.MaxPoses, Result.Captures.Num()));
    }
    // `warmupFileNotDeletedWarning`, NOT `warmupWarning`. MakeViewportInfoObject publishes its own
    // `warmupWarning` inside `viewport.warmup` meaning something entirely different - "the frame
    // never settled, these pixels are mid-warm-up". Different parents, so the two never clash in
    // one JSON object, but one name for two unrelated facts is ambiguous to anyone grepping the
    // tree and outright wrong for a caller that flattens the response. This producer names the
    // fact it is actually reporting: a file is still on disk.
    //
    // The empty-path guard is not defensive noise: without it a warm-up capture that reported no
    // file at all would still warn the caller about a leftover PNG that does not exist, and the
    // message would have nothing to name.
    if (Result.bWarmupShotTaken && !Result.bWarmupShotDiscarded && !Result.WarmupShotPath.IsEmpty())
    {
        Info->SetStringField(TEXT("warmupFileNotDeletedWarning"), FString::Printf(
            TEXT("The throwaway warm-up frame was captured but its file could not be deleted, so ")
            TEXT("one extra PNG is in the output directory: %s. It is not one of the shots in this ")
            TEXT("set and the next call with the same prefix overwrites it."),
            *Result.WarmupShotPath));
    }

    // Only on a set that HAD a time axis. On the verbs whose subject has none, an unconditional
    // `subjectTimesApplied: 0` would be a field that never says anything.
    if (Result.bSubjectTimeDriven)
    {
        Info->SetNumberField(TEXT("subjectTimesApplied"), Result.SubjectTimesApplied);
    }

    // Only when bounds were supplied. `posesFramingEvaluated: 0` then reads as "bounds were given
    // and nothing could be measured", which is a different fact from "no bounds were given" and
    // must not collapse into it.
    if (Result.bFramingRequested)
    {
        Info->SetNumberField(TEXT("posesFramingEvaluated"), Result.FramingsEvaluated);
        Info->SetNumberField(TEXT("posesOutOfFrame"), Result.PosesOutOfFrame);
        if (Result.PosesOutOfFrame > 0)
        {
            // Named for the set, not `framingWarning`: that name belongs to the per-shot `framing`
            // block, where a test asserts its ABSENCE on a framed capture. Two producers, one name
            // is the defect this file just finished fixing one field above.
            Info->SetStringField(TEXT("outOfFrameWarning"), FString::Printf(
                TEXT("%d of the %d captured shots provably do not contain the subject: its bounding ")
                TEXT("sphere cannot project into those frames. They are backdrop only, and neither ")
                TEXT("`blank` nor the luminance stats can tell you that. Each shot's own `framing` ")
                TEXT("block names the miss. Re-aim those poses at the subject's bounds."),
                Result.PosesOutOfFrame, Result.Captures.Num()));
        }
    }

    // Only when a differential was actually attempted. `coverageReferenceShots: 0` on the verbs
    // whose subject cannot be hidden would be a field that never says anything, and on a caller who
    // never asked it would look like a measurement that failed.
    if (Result.bCoverageRequested)
    {
        Info->SetNumberField(TEXT("coverageReferenceShots"), Result.CoverageReferenceShots);
        if (Result.CoverageReferenceShots < Result.Captures.Num())
        {
            Info->SetStringField(TEXT("coverageWarning"), FString::Printf(
                TEXT("Subject coverage was measured for %d of the %d captured shots: the remaining ")
                TEXT("shots could not be drawn a second time with the subject hidden, so they carry ")
                TEXT("no `subjectCoverage` figure at all. Their absence is not a coverage of zero."),
                Result.CoverageReferenceShots, Result.Captures.Num()));
        }
    }
    return Info;
}

void AddPoseFramingField(
    const FPoseListCaptureOutput& Result,
    int32 PoseIndex,
    const TSharedPtr<FJsonObject>& ShotObj)
{
    if (!ShotObj.IsValid() || !Result.Framings.IsValidIndex(PoseIndex))
    {
        return;
    }
    const PinWrightRenderCapture::FBoundsFramingCheck& Framing = Result.Framings[PoseIndex];
    if (!Framing.bEvaluated)
    {
        // Nothing was measured, so nothing is said. The set-level `posesFramingEvaluated` is where
        // a caller who DID supply bounds learns that no verdict came back.
        return;
    }
    ShotObj->SetObjectField(TEXT("framing"),
        PinWrightRenderCapture::MakeBoundsFramingObject(Framing));
}

TOptional<double> GetPoseSubjectCoverage(const FPoseListCaptureOutput& Result, int32 PoseIndex)
{
    if (!Result.SubjectCoverage.IsValidIndex(PoseIndex))
    {
        // An out-of-range index is the same answer as a shot that was never measured, and it is
        // deliberately not a 0.0: the whole point of the optional is that "not measured" can never
        // be read as "measured, and empty".
        return TOptional<double>();
    }
    return Result.SubjectCoverage[PoseIndex];
}

bool MeasureChangedPixelFraction(
    TConstArrayView<FColor> Reference,
    TConstArrayView<FColor> Subject,
    int32 ChannelThreshold,
    double& OutFraction)
{
    const PinWrightFlatRegion::FFrameDifferenceStats Difference =
        PinWrightFlatRegion::MeasureFrameDifference(
            Reference, Subject, ChannelThreshold);
    if (!Difference.bMeasured)
    {
        return false;
    }
    OutFraction = Difference.ChangedPixelFraction;
    return true;
}

void AddPoseCoverageField(
    const FPoseListCaptureOutput& Result,
    int32 PoseIndex,
    double WarnFraction,
    const TSharedPtr<FJsonObject>& ShotObj)
{
    if (!ShotObj.IsValid())
    {
        return;
    }
    const TOptional<double> Coverage = GetPoseSubjectCoverage(Result, PoseIndex);
    if (!Coverage.IsSet())
    {
        // No differential ran for this shot, so nothing is written. The set-level
        // `coverageReferenceShots` is where a caller who DID ask learns that some shots carry no
        // figure; a `subjectCoverage: 0` here would be a measurement claim nobody made.
        return;
    }
    ShotObj->SetNumberField(TEXT("subjectCoverage"), Coverage.GetValue());
    if (Coverage.GetValue() < WarnFraction)
    {
        ShotObj->SetStringField(TEXT("coverageWarning"), FString::Printf(
            TEXT("The subject changes %.4f%% of this frame's pixels, below the %.4f%% floor: hiding ")
            TEXT("it and redrawing the same pose produced an almost identical picture, so there is ")
            TEXT("effectively nothing of the subject here. `framing`/`boundsInFrame` cannot tell you ")
            TEXT("that - a bounding sphere is still geometrically in frame when the subject is a dot, ")
            TEXT("is behind the floor, or has no particles alive at this instant. Move the camera ")
            TEXT("closer, pick an instant where the effect is alive, or hide the preview floor."),
            Coverage.GetValue() * 100.0, WarnFraction * 100.0));
    }
}
}
