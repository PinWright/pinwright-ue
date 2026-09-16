// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
// FCaptureImageStats and the shipped degenerate-frame classifier. Included rather than
// re-declared on purpose: this file exists to give asset.generate_thumbnail the SAME frame
// measurement the render.* single-frame verbs already publish, and a second copy of the
// criterion is exactly the drift B-exposure-pin-black-frame had to fix once already.
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

class FJsonObject;
class UObject;

// Frame readiness and frame evidence for asset.generate_thumbnail.
//
// WHAT WENT WRONG. The verb rendered once, with ThumbnailTools::EThumbnailTextureFlushMode::
// NeverFlush, and published no measurement of the pixels at all: `success`, `assetPath`,
// `width`, `height`, and (only with an outputPath) `outputPath` / `format`. So a BLANK or
// half-drawn first frame returned a payload byte-for-byte identical to the finished one
// (B-thumbnail-cold-first-frame-no-stats). Two consequences, and the second is the worse one:
// a cold frame reads as "the material is broken" when the material is fine, and a cold t0
// compared against a warm t1 produces a large measured difference that is entirely warm-up
// artefact -- two motion proofs on one build were invalidated that way and had to be re-shot.
//
// TWO HALVES, and they answer different questions.
//
//  1. READINESS (WaitForThumbnailSubjectReadiness). NeverFlush skips FlushAsyncLoading,
//     FAssetCompilingManager::FinishAllCompilation, UTexture::ForceUpdateTextureStreaming and
//     IStreamingManager::StreamAllResources (ObjectTools.cpp, the AlwaysFlush arm). The engine's
//     own thumbnail-to-disk path passes NeverFlush too and COMPENSATES first, per subject class
//     (ObjectTools::GenerateThumbnailForObjectToSaveToDisk) -- and its comment says the material
//     branch is required specifically for thumbnails. This is that compensation, scoped to the
//     subject rather than to the whole editor: it is a documented precondition, not a guess, and
//     not a "render N frames and hope" warm-up.
//
//  2. EVIDENCE (AddFrameEvidenceFields). The readiness wait stops the bad frame; only the
//     published numbers let a caller KNOW they got a good one -- including for the failure modes
//     nobody has hit yet. The measurement reuses PinWrightRenderCapture::CalculateCaptureImageStats,
//     the classifier the render.* verbs publish, so `blank`, `crushed`, `blownOut` and
//     `toneLevelsUsed` mean the same thing on all four verbs.
//
// The re-render (FrameIsDegenerate) is gated on the MEASUREMENT and bounded at one extra pass.
// A readable frame pays for one render; only a frame that measures degenerate is drawn again,
// and the response publishes what BOTH passes measured plus how many pixels changed between
// them -- so "the first frame was not settled" is a reported fact rather than an assumption.
namespace PinWrightThumbnail
{
    // What the readiness pass actually waited for, all MEASURED off the subject rather than
    // echoed from the request (rpc-design.md section 4). ShaderMapCompleteBefore/After is the
    // load-bearing pair: a `false` -> `true` transition is direct evidence that this call would
    // have rendered a cold frame without the wait.
    struct FThumbnailReadinessReport
    {
        // FAssetCompilingManager::FinishCompilationForObjects was called on the subject. Always
        // true for a non-null asset; published so an absent block cannot be read as "waited".
        bool bWaitedForAssetCompilation = false;
        // At least one UTexture was blocked on and streamed: the subject itself when it is a
        // texture, or every texture the subject material samples.
        bool bWaitedForTextureStreaming = false;
        int32 TexturesWaitedFor = 0;
        // The subject is a UMaterialInterface with a resolvable FMaterialResource, so the
        // game-thread shader-map completeness question was asked. False leaves both bools below
        // meaningless and unpublished.
        bool bShaderMapChecked = false;
        bool bShaderMapCompleteBefore = false;
        bool bShaderMapCompleteAfter = false;
    };

    // The first pass's measurement, kept only when a second pass ran.
    struct FColdFrameRetryReport
    {
        bool bRetried = false;
        PinWrightRenderCapture::FCaptureImageStats FirstPassStats;
        // Pixels that differ between the two passes, over the common prefix of the two buffers.
        // The evidence that the first frame was unfinished: identical inputs rendered twice
        // should not disagree.
        int64 DifferingPixels = 0;
        int64 ComparedPixels = 0;
    };

    // Wait until the subject can actually be drawn: its async build, its shader map, and the
    // mips of the textures it samples. Scoped to this asset (and, for a material, to the textures
    // it uses) rather than to the whole editor, so it does not re-create the multi-minute global
    // stall B-compile-material-blocks-and-mislabels complains about.
    //
    // Call this BEFORE constructing FScopedPreviewOverride. FinishCompilationForObjects can fall
    // back to FinishAllCompilation and pump, and the override guard's safety argument
    // (ThumbnailPreviewOverride.h) rests on nothing observing the asset while the ThumbnailInfo
    // is swapped -- waiting inside that scope would widen the window it closes by construction.
    void WaitForThumbnailSubjectReadiness(UObject* Asset, FThumbnailReadinessReport& OutReport);

    // FObjectThumbnail's uncompressed image data is BGRA8; FColor is laid out B,G,R,A. Extracted
    // from the handler so the statistics and the encoder read the same buffer -- the pixels are
    // now measured on every call, with or without an outputPath.
    TArray<FColor> ThumbnailBytesToColors(TConstArrayView<uint8> Bgra8);

    // "This frame carries nothing a caller can read": either nothing was drawn into it
    // (`blank`), or what was drawn resolves fewer than PinWrightRenderCapture::MinUsableToneLevels
    // of 256 luminance levels. Unmeasured pixels count as degenerate, since a render that
    // returned no pixels is the strongest cold-frame signal there is. This is the re-render gate
    // and it is exported so it can be asserted without a GPU.
    bool FrameIsDegenerate(const PinWrightRenderCapture::FCaptureImageStats& Stats);

    // Pixels that differ over the common prefix of the two buffers.
    int64 CountDifferingPixels(TConstArrayView<FColor> First, TConstArrayView<FColor> Second);

    // The frame-level `frameWarning`: what was measured and what it usually means on THIS verb.
    // Empty when the frame is readable or was never measured, so the caller's test is "is this
    // string present" -- the same shape as the render verbs' `rangeWarning`.
    //
    // Deliberately NOT PinWrightRenderCapture::MakeToneRangeWarning: that text names `ev100` and
    // tells the caller which way to move it, and asset.generate_thumbnail has no exposure
    // parameter. The verdict is the same quantity; only the remedy differs.
    FString MakeThumbnailFrameWarning(const PinWrightRenderCapture::FCaptureImageStats& Stats,
        int32 RenderPasses);

    // Writes the whole evidence block into a generate_thumbnail response: `imageStats`, the
    // `blank` / `crushed` / `blownOut` verdicts, `frameWarning`, `renderPasses`, `coldFrameRetry`
    // and `readiness`. One call so the handler adds the signal in one line, and so no future
    // caller can publish half of it.
    void AddFrameEvidenceFields(
        const PinWrightRenderCapture::FCaptureImageStats& Stats,
        const FColdFrameRetryReport& ColdRetry,
        int32 RenderPasses,
        const FThumbnailReadinessReport& Readiness,
        const TSharedPtr<FJsonObject>& Result);
}
