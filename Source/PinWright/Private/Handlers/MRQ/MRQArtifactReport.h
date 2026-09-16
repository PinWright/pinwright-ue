// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UObject;

// Artifact facts for mrq.run_jobs: what the render actually wrote, measured off the files — plus
// the pre-flight disclosure mrq.create_job publishes before any of it is spent (see the second
// half of this header). Both live here because both must build with the engine's
// MovieRenderPipeline plugin disabled, and because the pre-flight reuses this file's encoder
// read-back rather than growing a second copy of it.
//
// WHAT WENT WRONG. mrq.run_jobs resolved its ticket with one field — `success` — and published
// nothing about the artifact: not the output path, not the size, not the bitrate, not the encoder
// that was in force. A 20 s 1080p60 cinematic came back at 1.16 Mbps / 3.4 MB with heavy banding
// over volumetric fog; re-rendered off the engine's Quality/CRF-20 default at VariableBitRate with
// a 50 Mbps average, the same content encodes at 21.24 Mbps. Every number a careful caller CAN
// check — resolution, frame count, duration, container magic — was identical between the good file
// and the bad one, because the one number that separates them, bits per second, was never
// published (B-mrq-render-result-omits-bitrate-and-size).
//
// MEASURED VS REQUESTED. The encoder's own settings are a REQUEST, not an outcome:
// UMoviePipelineMP4EncoderOutput ships `EncodingRateControl = Quality` with
// `ConstantRateFactor = 20` (MoviePipelineMP4EncoderOutput.h), and the Quality branch of the
// Media Foundation writer sets only `CODECAPI_AVEncVideoEncodeQP` — it never sets
// `AVEncCommonMeanBitRate` or `AVEncCommonMaxBitRate` (Windows/MoviePipelineMP4Encoder.cpp), so
// the requested settings place no lower bound at all on what a low-detail shot encodes at. Only
// the finished file answers "what did this actually encode at". So `fileSizeBytes` and
// `overallBitrateBps` are measured off disk and named plainly, the encoder read-back is published
// separately as `encoderRequested`, and anything that could not be measured is OMITTED rather
// than reported as 0 — a zero size or a zero bitrate would be indistinguishable from a real
// measurement of a broken file (`docs/rpc-design.md` §1 and §4).
//
// WHEN THE STAT IS HONEST. The paths come from `FMoviePipelineOutputData`, which MRQ fills in
// `UMoviePipeline::ProcessOutstandingFutures` — called on the Finalize -> Export transition, under
// the engine's own comment "the futures won't be available until actually written to disk"
// (MoviePipeline.cpp, the Finalize -> Export arm of SetPipelineState). The PIE executor caches
// that data and broadcasts `OnIndividualJobWorkFinished` a tick later, so every path this report
// stats has already been written and closed. A path that is nevertheless absent is published as
// `exists: false` with no size, never as a size of 0.
//
// DURATION IS DERIVED, NOT DEMUXED. `overallBitrateBps` divides a MEASURED byte count by a
// duration taken from the pipeline's own output-frame count and frame rate. That is the whole
// point of the ticket's remedy: it needs no demuxer and it would have caught the bad file on its
// own. It is not a demuxed duration, so it is published only when the pipeline reported both
// halves, and the field is omitted — with a warning saying so — when it did not.
//
// EVERY FIELD ABOVE IS STILL A FILE FACT, AND THAT WAS THE NEXT DEFECT. Size, bitrate and
// bits/pixel cannot distinguish a rendered frame from a frame that is half a solid colour: four
// 8.8 MB 4K PNGs at 8.53 bits/pixel came back `jobSucceeded: true` with no warnings, and the lower
// 47 % of every one of them was a flat pale-grey void (B-mrq-run-jobs-succeeds-on-unrenderable-
// frames). So BuildArtifactReport now also DECODES a sample of the written frames and publishes
// what the pixels are — `imageStats`, the `blank` / `crushed` / `blownOut` verdicts the capture
// verbs already use, a largest-flat-region measure that is the only one of them able to see a
// half-void frame, per-channel means and variances, and per-frame `suspect` / `unrenderable`
// verdicts. That half lives in Handlers/MRQ/MRQFrameEvidence.h; read its header for what is
// measured, why it is sampled, and why only a uniformly unrenderable sample is refused.
namespace PinWrightMRQ
{
    // One file the render pipeline reported writing, before anything has been measured about it.
    struct FRenderedFile
    {
        FString Path;
        // Render pass / layer the file belongs to. Empty when the pipeline did not name one.
        FString RenderPass;
    };

    // Everything known about the encode that is NOT read off the finished file. Every member is
    // optional on purpose: absent means "the pipeline did not report this", and an absent value
    // must never be substituted with a zero that a caller could read as a measurement.
    struct FEncodeContext
    {
        // Output frames the pipeline reported producing, summed over shots.
        TOptional<int32> FrameCount;
        // The frame rate the pipeline cached for the shots it rendered.
        TOptional<double> FrameRate;
        TOptional<int32> Width;
        TOptional<int32> Height;
        // Read back off the resolved video output setting by ReadRequestedEncoderSettings.
        TSharedPtr<FJsonObject> RequestedEncoder;
    };

    // Plausibility floor in bits per pixel per frame — bitrate / (width * height * fps).
    // The delivered bad file measured 0.0093 bpp; its correct re-render measured 0.171 bpp. The
    // floor is expressed per pixel so it holds at any resolution and frame rate: 0.04 bpp is
    // 4.98 Mbps at 1080p60, 1.24 Mbps at 720p30 and 19.9 Mbps at 4K30, with no table to maintain.
    // It is a smell test, not a verdict — it does not block anything and it does not have to be
    // right about WHY; it only has to make the caller look.
    constexpr double MinPlausibleBitsPerPixel = 0.04;

    // Read the effective encoder settings off a resolved video output setting object. Done by
    // reflection rather than by including MoviePipelineMP4EncoderOutput.h so this costs no module
    // dependency and so a project using a different video output (ProRes, DNx, the command-line
    // encoder) still gets whatever of the same-named knobs that class carries. Returns null for a
    // null object; every field is omitted when the property does not exist on the class.
    TSharedPtr<FJsonObject> ReadRequestedEncoderSettings(const UObject* VideoOutputSetting);

    // True for the rate-control modes that have no lower bitrate bound on low-detail content —
    // `Quality` and `ConstantQP`. Both target a quality and let the bitrate fall as far as the
    // content allows, which is exactly the trap this ticket documents.
    bool RateControlIsUnboundedBelow(const FString& RateControlName);

    // Build the per-job artifact block published by mrq.run_jobs. Stats every file, DECODES a
    // bounded sample of the still frames and measures their pixels, derives the bitrate where the
    // inputs allow, and appends a `warnings` array naming every measurement it could not make,
    // every implausible number it did make, and every frame that does not look like a picture.
    //
    // Opens files, so it is not free: at most MaxAnalyzedFramesPerJob decodes per job, on the
    // calling (game) thread, after the render has already finished writing them.
    TSharedPtr<FJsonObject> BuildArtifactReport(const TArray<FRenderedFile>& Files,
        const FEncodeContext& Context);

    // ---- Pre-flight disclosure for mrq.create_job ----
    //
    // WHY IT IS HERE AND NOT AFTER THE RENDER. The artifact report above is the honest answer, but
    // it arrives after minutes of wall time and a deliverable that is already written. Everything
    // that decides the encode — resolution, the output path shape, which encoder is in force, and
    // the rate-control mode that is unbounded below on smooth content — is knowable the moment the
    // job is queued, off the job's RESOLVED configuration. Being wrong is cheapest here.
    //
    // READ BACK, NOT ECHOED. Every field comes from the configuration the queued job actually
    // carries after SetConfiguration copied the preset into it — never from the request. That is
    // what stops `presetPath` from being the caller's only evidence that their preset took effect.
    // The encoder block is produced by ReadRequestedEncoderSettings, the same reflection read-back
    // mrq.run_jobs publishes, and it is named `encoderRequested` there for the same reason: these
    // are the values the config asks the encoder for, and on the Quality path they bound nothing.

    // What mrq.create_job discloses about the job it just queued. Optional members mean "the
    // resolved config did not carry this", and are omitted rather than published as a zero.
    struct FPreflightContext
    {
        TOptional<int32> Width;
        TOptional<int32> Height;
        // UNRESOLVED on purpose: MRQ expands {project_dir}, {sequence_name}, {frame_number} and
        // friends at render time, from state that does not exist yet at queue time. Publishing the
        // format string is the honest disclosure of the path SHAPE; pretending to resolve it here
        // would be a claim about a filename nothing has computed.
        FString OutputDirectory;
        FString FileNameFormat;
        // Set only when the config overrides the sequence's own rate (bUseCustomFrameRate).
        TOptional<double> FrameRateOverride;
        // Class path of every enabled output setting on the resolved config — what this job will
        // write. Empty means it writes nothing.
        TArray<FString> OutputClassPaths;
        // Read back off the resolved video output, when the config carries one.
        TSharedPtr<FJsonObject> RequestedEncoder;
    };

    // Build the `preflight` block plus the warnings mrq.create_job publishes beside it. Warnings
    // are returned separately so the handler can merge them into one top-level `warnings` array,
    // matching the plugin's dominant convention of emitting the field only when non-empty.
    TSharedPtr<FJsonObject> BuildPreflightReport(const FPreflightContext& Context,
        TArray<FString>& OutWarnings);
}
