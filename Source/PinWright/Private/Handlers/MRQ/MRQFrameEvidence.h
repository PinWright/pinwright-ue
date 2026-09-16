// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
// FCaptureImageStats and the shipped blank / tone-range classifier. Included rather than
// re-declared, for the same reason Handlers/Asset/ThumbnailFrameEvidence.h includes it: this file
// exists to give mrq.run_jobs the SAME frame measurement the render.* capture verbs already
// publish, and a second copy of the criterion is drift waiting to happen.
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
// The spatial half of the measurement -- the part no whole-frame aggregate can supply.
#include "Handlers/Render/FlatRegionStats.h"

class FJsonObject;

// Picture facts for mrq.run_jobs: what the rendered FRAMES look like, as opposed to what the
// files weigh.
//
// WHAT WENT WRONG. mrq.run_jobs published `jobSucceeded: true`, four PNGs of 8.8 MB each, all
// `exists: true`, all within 0.8 % of each other in size, at 8.53 bits/pixel -- comfortably above
// the response's own 0.04 bits/pixel plausibility floor -- and no warnings. All four 3840x2160
// frames were unusable: everything below ~53 % of frame height was a flat pale-grey void where
// the yard the camera was pointed at should have been, and one frame's container corrugation had
// additionally shattered into radial spikes (B-mrq-run-jobs-succeeds-on-unrenderable-frames).
//
// EVERY FIELD IN THAT RESPONSE WAS A FILE FACT. `outputFiles`, `fileSizeBytes`,
// `totalFileSizeBytes`, `overallBitrateBps` and `bitsPerPixel` are all stat()-derived or derived
// from a stat, and none of them can distinguish a rendered frame from a frame that is half a
// solid colour: a large flat region compresses well, but 3840x2160 of PNG keeps the byte count
// plausible, which is exactly what happened. The verb wrote images to disk and then measured only
// their size.
//
// WHAT IS MEASURED NOW, and why it is these two things. The frames are decoded and run through
// (1) PinWrightRenderCapture::CalculateCaptureImageStats -- the same luminance statistics and the
// same `blank` / `crushed` / `blownOut` verdicts render.capture_open_level publishes, so the two
// families cannot mean different things by the same field name -- and (2)
// PinWrightFlatRegion::MeasureLargestFlatRegion, which is the part the first one structurally
// cannot see: on a frame that is half good picture and half void, every whole-frame aggregate
// stays healthy and only a SPATIAL measure changes.
//
// WARNING VERSUS REFUSAL. A partial flat region remains a warning: skies, matte backdrops and
// letterbox bars can be intentional, and the files are already on disk. A stronger observation is
// safe to refuse: when EVERY decoded sample from a job has near-zero variance in R, G and B, the
// executor wrote a uniform colour rather than a rendered picture. mrq.run_jobs then fails with
// RENDER_UNRENDERABLE_FRAMES by default while preserving all file and pixel evidence; an explicit
// allowUnrenderableFrames opt-in is available for intentional uniform renders.
//
// IT DOES NOT NAME THE CAUSE, because it cannot. An unconverged Nanite / virtual-shadow-map
// stream, a ground actor that never loaded into the PIE world, a GPU timeout mid-accumulation and
// a deliberately flat backdrop all write the same pixels. The one causal discrimination that IS
// available for free is published instead: WHERE in the render the suspect frames fall. Suspect
// frames confined to the start of the sequence are the shape of an unconverged first frame, which
// more warm-up fixes; suspect frames throughout are not, which is the conclusion that cost the
// original reporter two builds to reach by hand.
//
// SAMPLED, NOT EXHAUSTIVE. Decoding a 4K PNG costs on the order of a hundred milliseconds and a
// render can write hundreds of them, on the game thread, after the caller has already waited
// minutes. At most MaxAnalyzedFramesPerJob frames are opened per job, evenly spaced with the
// first and last always included, and the response says so -- `framesAnalyzed` beside
// `outputFileCount`, plus a warning whenever the two differ, so nobody reads "0 suspect" as "every
// frame was looked at".
namespace PinWrightMRQ
{
    // Frames opened per job. Eight covers the start, the end and six points between -- enough to
    // separate "the first frames were unconverged" from "the whole render is broken", which is the
    // only causal question this evidence answers -- while bounding the game-thread cost at roughly
    // a second even at 4K.
    constexpr int32 MaxAnalyzedFramesPerJob = 8;

    // A channel whose standard deviation is at most two 8-bit levels is visually uniform after
    // decode. The threshold tolerates encoder/dither noise while staying far below real picture
    // variation. All three colour channels must meet it before a frame is called unrenderable.
    constexpr double UniformChannelVarianceThreshold =
        (2.0 / 255.0) * (2.0 / 255.0);

    struct FChannelImageStats
    {
        double MeanRed = 0.0;
        double MeanGreen = 0.0;
        double MeanBlue = 0.0;
        double RedVariance = 0.0;
        double GreenVariance = 0.0;
        double BlueVariance = 0.0;
        bool bUniformColor = false;
    };

    // What one decoded output frame turned out to be.
    struct FFrameEvidence
    {
        // FALSE IS THE DEFAULT AND IT MATTERS: every field below is meaningless until the pixels
        // were actually read, and a zeroed FCaptureImageStats reads as the most degenerate frame
        // possible (rpc-design.md section 4).
        bool bAnalyzed = false;
        // Why not: the decoder's own message, or the sampling note. Published, because "this file
        // would not decode" and "this file was not in the sample" are different facts and only one
        // of them is a problem.
        FString NotAnalyzedReason;

        int32 Width = 0;
        int32 Height = 0;
        PinWrightRenderCapture::FCaptureImageStats ImageStats;
        FChannelImageStats ChannelStats;
        PinWrightFlatRegion::FFlatRegionStats FlatRegion;

        // Stronger than `suspect`: the whole decoded frame is one near-uniform colour. A job is
        // refused only when every analyzed frame has this verdict; partial flat regions continue
        // to warn because skies, backdrops and letterbox bars are legitimate content.
        bool bUnrenderable = false;

        // Any of the four verdicts below fired. Partial suspect content is a warning; the stronger
        // bUnrenderable verdict is what the terminal handler can refuse safely.
        bool bSuspect = false;
        // Machine tokens, in the order they were tested: FLAT_REGION, BLANK, CRUSHED, BLOWN_OUT.
        // Published as a list rather than folded into one string so a caller can branch.
        TArray<FString> SuspectReasons;
    };

    // True for an extension FImageUtils can decode into pixels. A video container is NOT one of
    // them: a .mp4 is reported with `imageAnalyzed: false` and a reason rather than being silently
    // counted as a clean frame, because "not looked at" and "looked at and fine" must not share a
    // representation.
    bool PathIsStillImage(const FString& Path);

    // Which ordinals of a FrameCount-long list to open, at most MaxFrames of them: evenly spaced,
    // always including the first and the last. The first matters because an unconverged render is
    // worst there; the last because a render that degraded partway through shows it there.
    TArray<int32> SelectFrameSample(int32 FrameCount, int32 MaxFrames);

    // Decode one written frame and measure it. Never throws and never partially fills: a file that
    // will not decode comes back bAnalyzed false with the decoder's message.
    FFrameEvidence AnalyzeRenderedFrame(const FString& AbsolutePath);

    // Write the evidence into one `outputFiles[]` entry: `imageAnalyzed`, and then either
    // `imageNotAnalyzedReason` or the full `imageStats` block plus the `blank` / `crushed` /
    // `blownOut` / `suspect` verdicts and `suspectReasons`. One call so no future caller can
    // publish half of it.
    void AddFrameEvidenceFields(const FFrameEvidence& Evidence,
        const TSharedPtr<FJsonObject>& FileEntry);

    // The job-level roll-up. Accumulated as frames are measured so the warning can speak about the
    // SEQUENCE -- which frames were affected and where they sit -- rather than about one frame at
    // a time.
    struct FFrameEvidenceSummary
    {
        // Reported output files that are still images at all. Set by the caller before any frame
        // is measured; it is the denominator the sampling caveat is about.
        int32 StillImageCount = 0;
        int32 AnalyzedCount = 0;
        int32 SuspectCount = 0;
        int32 UnrenderableCount = 0;
        int32 DecodeFailureCount = 0;
        // One entry per analyzed frame, in the order the pipeline reported them. The ORDER is the
        // whole point: a suspect prefix and a scattered set of suspects mean different things.
        TArray<bool> SuspectInOrder;
        // The worst frame seen, so the warning can quote a concrete file and region instead of an
        // average nobody can open.
        double WorstFlatRegionFraction = 0.0;
        FString WorstFramePath;
        FString WorstFrameRegion;
    };

    void AccumulateFrameEvidence(const FFrameEvidence& Evidence, const FString& Path,
        FFrameEvidenceSummary& Summary);

    // Append the job's frame warnings: the suspect roll-up (with the where-in-the-render note),
    // the sampling caveat when fewer frames were opened than were written, and a note when a file
    // the pipeline reported would not decode. Appends nothing when every measured frame is clean
    // and every frame was measured.
    void MakeFrameEvidenceWarnings(const FFrameEvidenceSummary& Summary,
        TArray<FString>& OutWarnings);
}
