// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwMetaSoundRender - offline PCM render of a UMetaSoundSource asset.
//
// The missing edge between audio.authoring (which BUILDS MetaSound graphs) and
// audio.analysis (which MEASURES buffers). Before this, a built graph could only be
// read back structurally - describe_metasound's nodes/edges, decompile_metasound's
// MSIR - so "the three wave players are 0.6 s apart" was topology evidence, never a
// measurement. This turns the graph into samples so the analyzer can answer it.
//
// -----------------------------------------------------------------------------------
// WHY UMetaSoundSource::CreateSoundGenerator AND NOT A HAND-BUILT FMetasoundGenerator
// -----------------------------------------------------------------------------------
// The alternative is to construct Metasound::FMetasoundConstGraphGenerator directly with
// FGeneratorInitParams{ .bBuildSynchronous = true }. It is rejected, deliberately:
//
//   1. Everything that turns an ASSET into a renderable graph is private engine policy -
//      FindFirstNoninflatableGraph (preset inflation), CreateEnvironment,
//      ResolveSourceFormatAndNumChannels (output format -> channel count),
//      GetOperatorSettings (per-platform sample/block-rate overrides and quality
//      settings) and the parameter router's data channel. Reimplementing that is a
//      second writer for a policy the engine already owns (rpc-design.md §2/§5a), and
//      it would silently diverge the moment Epic changes one of them.
//   2. CreateSoundGenerator is the SAME call the audio mixer makes on playback
//      (AudioMixerSourceBuffer.cpp:135), so what this renders is what the game hears.
//      A private-path renderer would be measuring a different thing than it claims to.
//   3. It needs no audio device. The mixer supplies FSoundGeneratorInitParams; nothing
//      in CreateSoundGenerator or FMetasoundGenerator::OnGenerateAudio dereferences an
//      FAudioDevice, so a fabricated params struct is enough. That is what makes this
//      usable headless, which audio.play_sound_2d is not.
//
// The one thing that path does NOT give for free is a DETERMINISTIC start, and it is
// handled here rather than papered over: UMetaSoundSource::CreateSoundGenerator passes
// bBuildSynchronous=false (MetasoundSource.cpp:1278), so the operator is built on
// GBackgroundPriorityThreadPool and FMetasoundGenerator::OnGenerateAudio emits SILENCE
// while bIsWaitingForFirstGraph is set (MetasoundGenerator.cpp:702-708). The number of
// silent blocks would then depend on thread-pool scheduling - the render would not be
// reproducible. FScopedSyncGeneratorBuild forces the build synchronous for the duration
// of the call by driving au.MetaSound.EnableAsyncGeneratorBuilder to 0, and VERIFIES
// the write took; when it cannot, the render is REFUSED rather than returned with an
// unknown amount of leading silence.
//
// ENGINE VERSIONS. Every symbol used here exists on UE 5.3-5.8 except
// UMetaSoundSource::GetOperatorSettings, which is private before 5.4 - see the row in
// docs/engine-version-support.md. ISoundGenerator::GetNumChannels() is 5.8-only and is
// deliberately NOT used; the channel count is read from UMetaSoundSource::NumChannels,
// which CreateSoundGenerator writes on every version.

#pragma once

#include "CoreMinimal.h"

#include "AudioGen/PwAudioBuffer.h"

#if __has_include("MetasoundSource.h")
#define PW_HAS_METASOUND_RENDER 1
#else
#define PW_HAS_METASOUND_RENDER 0
#endif

#if PW_HAS_METASOUND_RENDER

#include "AudioParameter.h"
#include "Misc/Optional.h"

class UMetaSoundSource;

namespace PwMetaSoundRenderLimits
{
    /**
     * Longest render this layer will produce, in seconds. 60 s of 48 kHz stereo float is
     * ~23 MB in the candidate registry (whose whole budget is 256 MB), and a MetaSound
     * long enough to need more than a minute is not something an agent verifies by
     * measuring one take of it.
     */
    inline constexpr double MaxDurationSeconds = 60.0;

    /** Shortest render that can carry a measurement: one 10 ms analyzer envelope block. */
    inline constexpr double MinDurationSeconds = 0.01;

    /** Sample-rate window, matching PwSynthLimits' reasoning: below 8 kHz nothing useful
     *  survives the anti-alias assumptions in the analyzer, above 192 kHz is past every
     *  shipping device. */
    inline constexpr int32 MinSampleRate = 8000;
    inline constexpr int32 MaxSampleRate = 192000;

    /** Ceiling on how many OnGenerateAudio calls one render may take, so a generator that
     *  answers 0 samples forever cannot wedge the game thread. Sized from the caps above:
     *  192 kHz for 60 s at the smallest sane block (64 frames) is ~180,000 blocks. */
    inline constexpr int32 MaxBlocks = 400000;
}

/** What the caller asks for. Ranges are enforced by PwRenderMetaSoundSource, not clamped. */
struct FPwMetaSoundRenderParams
{
    /** Render length ceiling. A one-shot that finishes earlier stops early and says so. */
    double DurationSeconds = 1.0;

    /** Requested device sample rate. The graph may run at another rate if the asset
     *  carries a SampleRateOverride - the report publishes both, never only one. */
    int32 SampleRate = 48000;

    /** Graph input overrides, already typed. Validated by UMetaSoundSource::InitParameters
     *  before the render; ones it rejects are reported by name rather than dropped. */
    TArray<FAudioParameter> Inputs;
};

/**
 * What the render MEASURED. Every field is observed during the loop; nothing is echoed
 * back from the request (rpc-design.md §1). bMeasured stays false on every failure path,
 * so a caller that ignored the return cannot read a stale report as a result.
 */
struct FPwMetaSoundRenderReport
{
    bool bMeasured = false;

    /** Channel count the engine resolved for this source's output format. Read off
     *  UMetaSoundSource::NumChannels after CreateSoundGenerator wrote it. */
    int32 SourceChannels = 0;

    /** Requested rate, and the rate the operator actually runs at. Separate fields
     *  because an asset with a SampleRateOverride makes them differ, and a buffer
     *  labelled with the wrong rate reports the wrong duration. */
    int32 RequestedSampleRate = 0;
    int32 EffectiveSampleRate = 0;

    /** Frames per OnGenerateAudio call, from the generator's own
     *  GetDesiredNumSamplesToRenderPerCallback, and how many calls the loop made. */
    int32 BlockFrames = 0;
    int32 BlocksRendered = 0;

    /** Frames asked for (duration x effective rate) and frames the generator delivered.
     *  They differ exactly when the source finished early or stopped producing. */
    int32 RequestedFrames = 0;
    int32 RenderedFrames = 0;

    /** ISoundGenerator::IsFinished fired during the render - the source's OnFinished
     *  trigger reached the generator. */
    bool bFinished = false;

    /** When it fired, to BLOCK resolution: IsFinished is polled once per generated
     *  block, so this is the first block boundary at or after the trigger, not the
     *  sample. Unset when the source never finished. */
    TOptional<double> FinishedAtSeconds;

    /** The render hit the duration ceiling with the source still producing, i.e. the
     *  caller is looking at a prefix of the sound. */
    bool bTruncated = false;

    /** Graph inputs InitParameters accepted, and the ones it refused (wrong name for
     *  this graph, or a type the vertex cannot parse). Refusal is silent in the engine -
     *  it RemoveAtSwaps the parameter - so it is measured here by name-set difference. */
    TArray<FName> AppliedInputs;
    TArray<FName> RejectedInputs;
};

/**
 * Render a MetaSound Source offline into deinterleaved float PCM.
 *
 * Deterministic in (asset, duration, sample rate, inputs): the operator is built
 * synchronously, the graph is triggered by the build, and the loop pulls whole blocks,
 * so two calls with the same arguments produce the same samples. Any graph node that is
 * itself random (UE.RandomFloat with a re-rolled seed) remains random - that is the
 * asset's behaviour, not this layer's.
 *
 * Game thread only, editor only. NOT CHEAP: it runs the graph, so a 10 s render does 10 s
 * of DSP work on the calling stack.
 *
 * Out is emptied on entry and only filled on full success. Failure codes:
 *   ERR_INVALID_PARAMS                   - null source, or a duration / sample rate
 *                                          outside PwMetaSoundRenderLimits
 *   ERR_INVALID_STATE                    - called off the game thread
 *   ERR_NOT_SUPPORTED                    - cook commandlet; MetaSound graphs cannot
 *                                          execute there (Metasound::CanEverExecuteGraph)
 *   ERR_METASOUND_RENDER_FAILED          - the async-builder cvar could not be driven to
 *                                          0 (so the start would be non-deterministic),
 *                                          CreateSoundGenerator returned null, or the
 *                                          built operator exposes no whole audio block
 *   ERR_AUDIO_MULTICHANNEL_UNSUPPORTED   - the source resolves to more than two channels
 *   ERR_AUDIO_EMPTY_BUFFER               - the generator delivered zero frames
 *
 * A graph that FAILS TO BUILD is not one of those: the engine substitutes a no-op
 * executer, which finishes on the first callback. It surfaces as a SUCCESS whose report
 * reads bFinished with FinishedAtSeconds at the first block boundary and whose buffer
 * measures digital silence - an outcome the caller can see and name, which is why it is
 * not forced into an error the caller would have to parse.
 *
 * @param Source       The asset to render. Its InitResources() is run first, and its
 *                     OnEndGenerate is called on the way out so the generator is not
 *                     left tracked on the asset.
 * @param OutReport    Measurements. bMeasured is true only alongside a true return.
 */
bool PwRenderMetaSoundSource(UMetaSoundSource* Source,
                             const FPwMetaSoundRenderParams& Params,
                             FPwAudioBuffer& Out,
                             FPwMetaSoundRenderReport& OutReport,
                             FString& OutErrorCode,
                             FString& OutError);

#endif // PW_HAS_METASOUND_RENDER
