// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwMetaSoundRender.h"

#if PW_HAS_METASOUND_RENDER

#include "Handlers/ErrorCodes.h"

#include "Compat/EngineVersionCompat.h"
#include "CoreGlobals.h"
#include "HAL/IConsoleManager.h"
#include "MetasoundSource.h"
#include "Sound/SoundGenerator.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two
// anonymous namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwMetaSoundRenderInternal
{
    /**
     * The engine cvar that decides whether FMetasoundConstGraphGenerator::BuildGraph runs
     * StartBackgroundTask or StartSynchronousTask (MetasoundGenerator.cpp:1184-1199).
     * Present on UE 5.3-5.8 under this exact name.
     */
    const TCHAR* const AsyncBuilderCVarName = TEXT("au.MetaSound.EnableAsyncGeneratorBuilder");

    /**
     * Fixed transmitter identity for every offline render.
     *
     * UMetaSoundSource::CreateSoundGenerator asks the parameter router for a data channel
     * keyed on (AudioDeviceID, InstanceID) and the router NEVER prunes that map - it only
     * holds a TWeakPtr and re-adds under the same key when the pin fails
     * (MetasoundSource.cpp:165-202). A fresh id per render would therefore leak one map
     * entry per call; one constant id leaves exactly one entry for the life of the editor.
     *
     * Deliberately not a "random enough" value: it is paired with a device id of
     * INDEX_NONE, which no real audio device is issued, so the key cannot collide with a
     * live playing voice's channel no matter what a real InstanceID happens to be.
     */
    constexpr uint64 OfflineRenderInstanceId = 0x50696E5752656E64ull; // 'PinWRend'

    /**
     * Environment values the mixer would supply. Nothing in the render path allocates
     * against them - they land in FMetasoundEnvironment as AudioMixerNumOutputFrames /
     * NumFramesPerCallback for graph nodes that ask - so they carry the mixer's own
     * defaults rather than a number invented here.
     */
    constexpr int32 MixerOutputFrames = 1024;

    /**
     * Forces MetaSound operator builds synchronous for the lifetime of the scope, and
     * MEASURES whether the write landed.
     *
     * bApplied is read back off the cvar rather than assumed: IConsoleVariable::Set
     * silently no-ops when the caller's priority is below the value's current SetBy
     * priority, so a cvar somebody typed into the console outranks ECVF_SetByCode and the
     * build would stay asynchronous. A render that started with an unknown number of
     * silent blocks is exactly the "plausible but wrong" result the caller cannot see, so
     * PwRenderMetaSoundSource refuses on !bApplied instead of rendering anyway.
     */
    struct FScopedSyncGeneratorBuild
    {
        FScopedSyncGeneratorBuild()
        {
            Var = IConsoleManager::Get().FindConsoleVariable(AsyncBuilderCVarName);
            if (!Var)
            {
                return;
            }
            Previous = Var->GetString();
            Var->Set(TEXT("0"), ECVF_SetByCode);
            bApplied = Var->GetInt() == 0;
        }

        ~FScopedSyncGeneratorBuild()
        {
            // Only undo a write that actually happened; restoring a value we never
            // changed would move the cvar's SetBy priority for no reason.
            if (Var && bApplied)
            {
                Var->Set(*Previous, ECVF_SetByCode);
            }
        }

        FScopedSyncGeneratorBuild(const FScopedSyncGeneratorBuild&) = delete;
        FScopedSyncGeneratorBuild& operator=(const FScopedSyncGeneratorBuild&) = delete;

        IConsoleVariable* Var = nullptr;
        FString Previous;
        bool bApplied = false;
    };

    /** Interleaved generator output -> the deinterleaved stereo buffer the subsystem speaks. */
    void ScatterFrames(const float* RESTRICT Interleaved, int32 NumFrames, int32 NumChannels,
                       int32 DestFrameOffset, FPwAudioBuffer& Out)
    {
        float* RESTRICT Left = Out.Left.GetData() + DestFrameOffset;
        float* RESTRICT Right = Out.Right.GetData() + DestFrameOffset;

        if (NumChannels == 1)
        {
            // Mono duplicates into both sides, matching PwDecodeSoundWave, so no
            // downstream stage has to branch on channel count.
            for (int32 Frame = 0; Frame < NumFrames; ++Frame)
            {
                Left[Frame] = Interleaved[Frame];
                Right[Frame] = Interleaved[Frame];
            }
        }
        else
        {
            for (int32 Frame = 0; Frame < NumFrames; ++Frame)
            {
                Left[Frame] = Interleaved[Frame * 2];
                Right[Frame] = Interleaved[Frame * 2 + 1];
            }
        }
    }

    /** Releases the generator the way the mixer does (AudioMixerSourceBuffer.cpp:799-802). */
    void EndGenerate(UMetaSoundSource* Source, ISoundGeneratorPtr& Generator)
    {
        if (Generator.IsValid())
        {
            Generator->OnEndGenerate();
            if (Source)
            {
                // Untracks the generator from the asset's Generators array; skipping this
                // leaves a dead weak entry on every rendered asset.
                Source->OnEndGenerate(Generator);
            }
        }
        Generator.Reset();
    }
}

bool PwRenderMetaSoundSource(UMetaSoundSource* Source,
                             const FPwMetaSoundRenderParams& Params,
                             FPwAudioBuffer& Out,
                             FPwMetaSoundRenderReport& OutReport,
                             FString& OutErrorCode,
                             FString& OutError)
{
    using namespace PwMetaSoundRenderInternal;

    // Failure is the default state of every out-param, so an ignored return can never be
    // read as a result (rpc-design.md §1).
    Out = FPwAudioBuffer();
    OutReport = FPwMetaSoundRenderReport();
    OutErrorCode.Reset();
    OutError.Reset();

    if (!Source)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = TEXT("No MetaSound Source to render (null asset).");
        return false;
    }

    if (!IsInGameThread())
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_STATE;
        OutError = TEXT("A MetaSound render must run on the game thread: "
                        "UMetaSoundSource::InitParameters asserts it outright so that "
                        "UObject proxies for asset-valued inputs are created safely.");
        return false;
    }

    // Metasound::CanEverExecuteGraph() is false in a cook commandlet, and
    // UMetaSoundSource::InitResources ensures on it (MetasoundSource.cpp:1073). Checked
    // through the Core predicate the engine's own gate is written against, so this does
    // not need the MetasoundGraphCore module linked.
    if (IsRunningCookCommandlet())
    {
        OutErrorCode = ErrorCodes::ERR_NOT_SUPPORTED;
        OutError = TEXT("MetaSound graphs cannot execute inside a cook commandlet, so "
                        "there is nothing to render here.");
        return false;
    }

    if (!(Params.DurationSeconds >= PwMetaSoundRenderLimits::MinDurationSeconds) ||
        Params.DurationSeconds > PwMetaSoundRenderLimits::MaxDurationSeconds)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(
            TEXT("durationSeconds=%.4f is outside %.2f..%.0f."),
            Params.DurationSeconds, PwMetaSoundRenderLimits::MinDurationSeconds,
            PwMetaSoundRenderLimits::MaxDurationSeconds);
        return false;
    }

    if (Params.SampleRate < PwMetaSoundRenderLimits::MinSampleRate ||
        Params.SampleRate > PwMetaSoundRenderLimits::MaxSampleRate)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(TEXT("sampleRate=%d is outside %d..%d Hz."),
            Params.SampleRate, PwMetaSoundRenderLimits::MinSampleRate,
            PwMetaSoundRenderLimits::MaxSampleRate);
        return false;
    }

    OutReport.RequestedSampleRate = Params.SampleRate;

    // Caches the runtime input map on the game thread. Without it InitParameters falls
    // back to rebuilding that map per call and says so in the log; with it the parameter
    // validation below is the cheap path the mixer uses.
    Source->InitResources();

    // ------------------------------------------------------------------------------
    // Graph inputs. InitParameters VALIDATES against this graph's vertices and silently
    // RemoveAtSwaps what it rejects (MetasoundSource.cpp:1530-1567), so which overrides
    // survived is measured by name-set difference rather than reported by the engine.
    // ------------------------------------------------------------------------------
    TArray<FAudioParameter> GraphInputs = Params.Inputs;

    TArray<FName> RequestedNames;
    RequestedNames.Reserve(GraphInputs.Num());
    for (const FAudioParameter& Parameter : GraphInputs)
    {
        RequestedNames.AddUnique(Parameter.ParamName);
    }

    if (GraphInputs.Num() > 0)
    {
        Source->InitParameters(GraphInputs, TEXT("PinWrightOfflineRender"));
    }

    for (const FAudioParameter& Parameter : GraphInputs)
    {
        OutReport.AppliedInputs.AddUnique(Parameter.ParamName);
    }
    for (const FName& Name : RequestedNames)
    {
        if (!OutReport.AppliedInputs.Contains(Name))
        {
            OutReport.RejectedInputs.AddUnique(Name);
        }
    }

    // ------------------------------------------------------------------------------
    // Build. The scope must outlive CreateSoundGenerator and nothing else: the cvar is
    // read once, inside BuildGraph, on this stack.
    // ------------------------------------------------------------------------------
    ISoundGeneratorPtr Generator;
    {
        FScopedSyncGeneratorBuild SyncBuild;
        if (!SyncBuild.bApplied)
        {
            OutErrorCode = ErrorCodes::ERR_METASOUND_RENDER_FAILED;
            OutError = FString::Printf(
                TEXT("Could not force '%s' to 0, so the operator would be built on a "
                     "background thread and the render would begin with a "
                     "scheduling-dependent number of silent blocks. Refusing rather than "
                     "returning a take whose start cannot be reproduced. Set '%s 0' "
                     "manually and retry."),
                AsyncBuilderCVarName, AsyncBuilderCVarName);
            return false;
        }

        FSoundGeneratorInitParams InitParams;
        // No audio device is involved, and INDEX_NONE is not an id any device is issued,
        // so the parameter router's (device, instance) key cannot collide with a live voice.
        InitParams.AudioDeviceID = static_cast<Audio::FDeviceId>(INDEX_NONE);
        InitParams.AudioComponentId = 0;
        InitParams.SampleRate = static_cast<float>(Params.SampleRate);
        InitParams.AudioMixerNumOutputFrames = MixerOutputFrames;
        // Only consulted as a fallback when a ChannelAgnostic source names a custom format
        // the channel registry does not hold (MetasoundSource.cpp:1211); the real channel
        // count comes from the source's output format.
        InitParams.NumChannels = 2;
        InitParams.NumFramesPerCallback = MixerOutputFrames;
        InitParams.InstanceID = OfflineRenderInstanceId;
        // false, so the graph analyzer stays off and this renders the shipping path
        // rather than the editor-preview one.
        InitParams.bIsPreviewSound = false;
        InitParams.GraphName = Source->GetName();

        Generator = Source->CreateSoundGenerator(InitParams, MoveTemp(GraphInputs));
    }

    if (!Generator.IsValid())
    {
        OutErrorCode = ErrorCodes::ERR_METASOUND_RENDER_FAILED;
        OutError = FString::Printf(
            TEXT("UMetaSoundSource::CreateSoundGenerator returned null for '%s'. The "
                 "engine returns null when it cannot resolve the source's output format "
                 "to a channel count; check the asset's Output Format and that "
                 "audio.authoring.compile_metasound reports it valid."),
            *Source->GetName());
        return false;
    }

    // Read AFTER CreateSoundGenerator: it writes the resolved format's channel count onto
    // the source (MetasoundSource.cpp:1255). ISoundGenerator::GetNumChannels() would say
    // the same thing but only exists on UE 5.8.
    const int32 SourceChannels = Source->NumChannels;
    OutReport.SourceChannels = SourceChannels;

    if (SourceChannels > 2)
    {
        EndGenerate(Source, Generator);
        OutErrorCode = ErrorCodes::ERR_AUDIO_MULTICHANNEL_UNSUPPORTED;
        OutError = FString::Printf(
            TEXT("'%s' resolves to %d output channels. FPwAudioBuffer is deinterleaved "
                 "stereo, so there is no lossless landing place for a quad/5.1/7.1 "
                 "source; this verb refuses rather than dropping channels the caller "
                 "cannot see were dropped."),
            *Source->GetName(), SourceChannels);
        return false;
    }

    if (SourceChannels < 1)
    {
        EndGenerate(Source, Generator);
        OutErrorCode = ErrorCodes::ERR_METASOUND_RENDER_FAILED;
        OutError = FString::Printf(
            TEXT("'%s' resolved to %d output channels, so the built operator has no audio "
                 "output to read."), *Source->GetName(), SourceChannels);
        return false;
    }

    // The rate the OPERATOR runs at, which is the requested device rate put through the
    // asset's own SampleRateOverride and quality settings. Published beside the request
    // because a buffer labelled with the wrong rate reports the wrong duration.
    // UMetaSoundSource::GetOperatorSettings became public in UE 5.4, the release that also gave it
    // something to say: cooked quality settings plus the sample-rate/block-rate cvars. Through 5.3
    // it is private AND inert - its whole body is FOperatorSettings(InSampleRate,
    // GetDefaultBlockRate()) (MetasoundSource.cpp:959), and that constructor stores InSampleRate
    // unchanged for any positive rate, which the range check above already guaranteed. So on 5.3
    // the operator rate IS the requested rate; there is no asset override to consult.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    const int32 EffectiveSampleRate = FMath::RoundToInt(
        Source->GetOperatorSettings(static_cast<Metasound::FSampleRate>(Params.SampleRate))
            .GetSampleRate());
#else
    const int32 EffectiveSampleRate = Params.SampleRate;
#endif
    if (EffectiveSampleRate < PwMetaSoundRenderLimits::MinSampleRate)
    {
        EndGenerate(Source, Generator);
        OutErrorCode = ErrorCodes::ERR_METASOUND_RENDER_FAILED;
        OutError = FString::Printf(
            TEXT("'%s' resolved to a %d Hz operator sample rate from a %d Hz request, "
                 "which is below the %d Hz floor this layer can measure."),
            *Source->GetName(), EffectiveSampleRate, Params.SampleRate,
            PwMetaSoundRenderLimits::MinSampleRate);
        return false;
    }
    OutReport.EffectiveSampleRate = EffectiveSampleRate;

    const int32 BlockSamples = Generator->GetDesiredNumSamplesToRenderPerCallback();
    if (BlockSamples < SourceChannels)
    {
        EndGenerate(Source, Generator);
        OutErrorCode = ErrorCodes::ERR_METASOUND_RENDER_FAILED;
        OutError = FString::Printf(
            TEXT("'%s' asks for %d samples per render callback across %d channels, which "
                 "is not a whole audio block."),
            *Source->GetName(), BlockSamples, SourceChannels);
        return false;
    }
    const int32 BlockFrames = BlockSamples / SourceChannels;
    // Whole frames only. GetDesiredNumSamplesToRenderPerCallback is NumFramesPerExecute *
    // NumChannels so the division is already exact, but the render loop sizes its scratch
    // buffer from this figure and asks the generator for exactly this many samples - deriving
    // both from the same product is what makes a partial frame structurally impossible rather
    // than merely unlikely.
    const int32 BlockSamplesAligned = BlockFrames * SourceChannels;
    OutReport.BlockFrames = BlockFrames;

    const int32 RequestedFrames = FMath::Max(1,
        FMath::RoundToInt(Params.DurationSeconds * static_cast<double>(EffectiveSampleRate)));
    OutReport.RequestedFrames = RequestedFrames;

    Out.SampleRate = EffectiveSampleRate;
    // Zero-filled: a source that finishes before the ceiling leaves real silence behind
    // it, which is what the tail of that sound is.
    Out.SetNumFrames(RequestedFrames, /*bZeroed=*/true);

    TArray<float> Block;
    Block.SetNumUninitialized(BlockSamplesAligned);

    Generator->OnBeginGenerate();

    int32 FramesWritten = 0;
    int32 Blocks = 0;
    bool bFinished = false;
    while (FramesWritten < RequestedFrames && Blocks < PwMetaSoundRenderLimits::MaxBlocks)
    {
        const int32 SamplesWritten = Generator->GetNextBuffer(
            Block.GetData(), BlockSamplesAligned, /*bRequireNumberSamples=*/false);
        ++Blocks;

        if (SamplesWritten <= 0)
        {
            // The generator has nothing left. Reached after the block the finish trigger
            // landed in, and the only exit for a source that stops without finishing.
            break;
        }

        const int32 FramesAvailable = FMath::Min(SamplesWritten / SourceChannels, BlockFrames);
        const int32 FramesToCopy = FMath::Min(FramesAvailable, RequestedFrames - FramesWritten);
        if (FramesToCopy > 0)
        {
            ScatterFrames(Block.GetData(), FramesToCopy, SourceChannels, FramesWritten, Out);
            FramesWritten += FramesToCopy;
        }

        // Two independent signals for the same event, because they land one call apart:
        // FMetasoundGenerator truncates the block the OnFinished trigger fired in
        // (FillWithBuffer stops at FinishSample) and only sets bIsFinished on the NEXT
        // call. Taking the short block as the finish keeps FinishedAtSeconds on the block
        // the sound actually ended in rather than one block late.
        if (Generator->IsFinished() || SamplesWritten < BlockSamplesAligned)
        {
            bFinished = true;
            OutReport.FinishedAtSeconds =
                static_cast<double>(FramesWritten) / static_cast<double>(EffectiveSampleRate);
            break;
        }
    }

    EndGenerate(Source, Generator);

    OutReport.BlocksRendered = Blocks;
    OutReport.RenderedFrames = FramesWritten;
    OutReport.bFinished = bFinished;
    // The caller is looking at a prefix: the source was still producing when the duration
    // ceiling stopped the loop.
    OutReport.bTruncated = !bFinished && FramesWritten >= RequestedFrames;

    if (FramesWritten <= 0)
    {
        Out = FPwAudioBuffer();
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = FString::Printf(
            TEXT("'%s' produced no frames in %d render callbacks, so there is nothing to "
                 "measure. A source whose OnPlay never reaches an audio output does this."),
            *Source->GetName(), Blocks);
        return false;
    }

    OutReport.bMeasured = true;
    return true;
}

#endif // PW_HAS_METASOUND_RENDER
