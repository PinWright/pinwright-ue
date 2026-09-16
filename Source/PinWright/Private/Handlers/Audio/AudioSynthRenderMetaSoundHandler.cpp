// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioSynthRenderMetaSoundHandler.cpp - audio.synth.render_metasound
//
// The edge between audio.authoring and audio.analysis that the surface was missing
// (board ticket F-metasound-no-render-to-pcm): authoring can BUILD a UMetaSoundSource
// and compile_metasound will call it valid, but until this verb nothing turned that
// graph into samples. The only readbacks were structural - describe_metasound's
// nodes/edges and decompile_metasound's MSIR - so a staged graph's timing, a mixer's
// headroom and a RandomFloat's audible range were all reported as topology rather than
// measured. All three are measurements, and this is what takes them.
//
// -------------------------------------------------------------------------------------
// WHY IT LIVES IN audio.synth AND NOT audio.authoring
// -------------------------------------------------------------------------------------
// The ticket suggested `audio.authoring.render_metasound`. It is registered here instead
// because the namespaces are split by VERB, not by asset family, and the split is already
// written down: audio.synth renders, audio.analysis measures, audio.authoring wires assets
// together (AudioAnalysisHandler.cpp:5). Landing a renderer in authoring would put the
// subsystem's second render path in the namespace that has none.
//
// The placement is also what makes the verb cheap. Its output is a CANDIDATE ID, the same
// currency audio.synth.generate produces, so every consumer already exists:
// audio.analysis.analyze / compare / decompose take a candidateId, audio.synth.audition
// plays one, and audio.synth.export writes one to a USoundWave with its own decode-back
// verification. Two renders of one source plus one audio.analysis.compare is the whole
// per-shot-variation check the ticket could not perform.
//
// -------------------------------------------------------------------------------------
// THE ASSET WRITE IS OPTIONAL, AND IT IS THE SAME WRITE audio.synth.export MAKES
// -------------------------------------------------------------------------------------
// Passing `name` + `path` additionally lands the render as a USoundWave through
// PwCreateSoundWaveAsset - the one writer for buffer -> asset in this subsystem - behind
// the same AssetCreatePolicy::Resolve gate, and verifies it by decoding the created asset
// back through PwDecodeSoundWaveWithCode (a different subsystem from the one that wrote
// it). Omitting them is the cheap path and costs nothing: the candidate is already
// analysable, and audio.synth.export can write it later from the same id.
//
// TICK-UNSAFE. Listed in Dispatch/SafePoint.cpp under family E for the asset-write path,
// which reaches CreatePackage + an in-place NewObject + a synchronous .uasset save exactly
// the way audio.synth.export does.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "PinWrightHelpers.h"
#include "State/PluginState.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/PathUtils.h"

#include "AudioGen/PwAudioAnalysis.h"
#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwAudioExport.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "AudioGen/PwMetaSoundRender.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Sound/SoundWave.h"

// PW_METASOUND_PATHUTILS_HAS_DOCUMENT_INTERFACE gates the MetaSound load gate this verb
// resolves `assetPath` through, so both halves have to be present for it to work at all.
#if PW_HAS_METASOUND_RENDER && PW_METASOUND_PATHUTILS_HAS_DOCUMENT_INTERFACE
#include "MetasoundSource.h"
#define PW_RENDER_METASOUND_HANDLER 1
#else
#define PW_RENDER_METASOUND_HANDLER 0
#endif

// Named (not anonymous) namespace: the main module builds with Unity on, and two anonymous
// namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwRenderMetaSoundInternal
{
    /** Longest per-name list carried in the inputs report, so a caller that sent fifty
     *  overrides cannot spill the response with names it already knows. */
    constexpr int32 MaxReportedInputNames = 24;

    void SetRounded(const TSharedPtr<FJsonObject>& Out, const TCHAR* Key, double Value,
                    int32 Decimals)
    {
        if (!FMath::IsFinite(Value))
        {
            return;
        }
        const double Scale = FMath::Pow(10.0, static_cast<double>(Decimals));
        Out->SetNumberField(Key, FMath::RoundToDouble(Value * Scale) / Scale);
    }

    void AddNameArray(const TSharedPtr<FJsonObject>& Out, const TCHAR* Key,
                      const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        const int32 Count = FMath::Min(Names.Num(), MaxReportedInputNames);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Values.Add(MakeShared<FJsonValueString>(Names[Index].ToString()));
        }
        Out->SetArrayField(Key, Values);
        // The count is published separately so a truncated list still reports the truth.
        if (Names.Num() > Count)
        {
            Out->SetNumberField(*(FString(Key) + TEXT("Total")), Names.Num());
        }
    }

#if PW_RENDER_METASOUND_HANDLER

    /**
     * `inputs` -> typed graph parameters.
     *
     * Two spellings, and the reason there are two: a JSON number carries no int/float
     * distinction (UE's parser makes every number a double), and MetaSound's vertex
     * validation is type-exact - a Float parameter aimed at an Int32 input is REJECTED,
     * not coerced. So the shorthand covers what JSON can express unambiguously and the
     * {type, value} form is the escape for everything else.
     *
     * A value shape this cannot type is an ERROR naming the key (§3): dropping it would
     * leave the caller reading a render that ignored the override they were testing.
     */
    bool ParseGraphInputs(FHandlerContext& Ctx, const TSharedPtr<FJsonObject>& Inputs,
                          TArray<FAudioParameter>& Out)
    {
        Out.Reset();
        if (!Inputs.IsValid())
        {
            return true;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Inputs->Values)
        {
            const FName Name(*Pair.Key);
            const TSharedPtr<FJsonValue>& Value = Pair.Value;
            if (!Value.IsValid())
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("inputs['%s'] holds no value."), *Pair.Key));
                return false;
            }

            // Typed form: {"type": "int", "value": 3}.
            if (Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject>& Spec = Value->AsObject();
                FString TypeName;
                if (!Spec.IsValid() || !Spec->TryGetStringField(TEXT("type"), TypeName))
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                        FString::Printf(
                            TEXT("inputs['%s'] is an object but carries no 'type'. The "
                                 "typed form is {\"type\":\"float|int|bool|string|trigger\","
                                 "\"value\":...}."), *Pair.Key));
                    return false;
                }

                const FString Lower = TypeName.ToLower();
                const TSharedPtr<FJsonValue> Inner = Spec->TryGetField(TEXT("value"));

                if (Lower == TEXT("trigger"))
                {
                    // A trigger has no value by construction - the engine's own Sanitize
                    // discards anything paired with it - so one is not required here.
                    Out.Emplace(Name, EAudioParameterType::Trigger);
                    continue;
                }
                if (!Inner.IsValid())
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                        FString::Printf(TEXT("inputs['%s'] declares type '%s' but carries "
                                             "no 'value'."), *Pair.Key, *TypeName));
                    return false;
                }
                if (Lower == TEXT("float"))
                {
                    Out.Emplace(Name, static_cast<float>(Inner->AsNumber()));
                    continue;
                }
                if (Lower == TEXT("int") || Lower == TEXT("integer") || Lower == TEXT("int32"))
                {
                    Out.Emplace(Name, static_cast<int32>(Inner->AsNumber()));
                    continue;
                }
                if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
                {
                    Out.Emplace(Name, Inner->AsBool());
                    continue;
                }
                if (Lower == TEXT("string"))
                {
                    Out.Emplace(Name, Inner->AsString());
                    continue;
                }

                Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_TYPE,
                    FString::Printf(
                        TEXT("inputs['%s'] declares type '%s'. Accepted: float, int, bool, "
                             "string, trigger. Array- and object-valued graph inputs are "
                             "not settable per render; bake them with "
                             "audio.authoring.set_metasound_default instead."),
                        *Pair.Key, *TypeName));
                return false;
            }

            switch (Value->Type)
            {
            case EJson::Boolean:
                Out.Emplace(Name, Value->AsBool());
                break;
            case EJson::Number:
                // Float, always. An Int32 vertex needs the typed form - see above.
                Out.Emplace(Name, static_cast<float>(Value->AsNumber()));
                break;
            case EJson::String:
                Out.Emplace(Name, Value->AsString());
                break;
            default:
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(
                        TEXT("inputs['%s'] is neither a number, a boolean, a string nor a "
                             "{type, value} object, so there is no MetaSound literal it "
                             "could become."), *Pair.Key));
                return false;
            }
        }

        return true;
    }

    /** The measured half of the render, under the report's own field spellings. */
    TSharedPtr<FJsonObject> BuildRenderBlock(const FPwMetaSoundRenderReport& In)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("measured"), In.bMeasured);
        Out->SetNumberField(TEXT("sourceChannels"), In.SourceChannels);
        Out->SetNumberField(TEXT("requestedSampleRate"), In.RequestedSampleRate);
        Out->SetNumberField(TEXT("effectiveSampleRate"), In.EffectiveSampleRate);
        Out->SetNumberField(TEXT("blockFrames"), In.BlockFrames);
        Out->SetNumberField(TEXT("blocksRendered"), In.BlocksRendered);
        Out->SetNumberField(TEXT("requestedFrames"), In.RequestedFrames);
        Out->SetNumberField(TEXT("renderedFrames"), In.RenderedFrames);
        Out->SetBoolField(TEXT("finished"), In.bFinished);
        // Omitted rather than zeroed when the source never finished: 0.0 s would read as
        // "it finished immediately", which is the opposite fact (§1).
        if (In.FinishedAtSeconds.IsSet())
        {
            SetRounded(Out, TEXT("finishedAtSeconds"), In.FinishedAtSeconds.GetValue(), 4);
        }
        Out->SetBoolField(TEXT("truncated"), In.bTruncated);
        return Out;
    }

#endif // PW_RENDER_METASOUND_HANDLER
}

REGISTER_RPC_HANDLER("audio.synth.render_metasound", "audio.synth",
    "Render an existing MetaSound Source asset offline to PCM and measure it, closing the "
    "loop between audio.authoring (which builds the graph) and audio.analysis (which "
    "measures sound). The graph is run through the same UMetaSoundSource::CreateSoundGenerator "
    "path the audio mixer uses on playback, with the operator built synchronously so the "
    "render is reproducible and needs no audio device or PIE session. The result lands in the "
    "session candidate registry as a candidateId, so audio.analysis.analyze / compare / "
    "decompose, audio.synth.audition and audio.synth.export all take it directly - two renders "
    "plus one compare is how you prove per-shot variation is audible. Reports what the render "
    "MEASURED: resolved channel count, the effective operator sample rate, whether the source's "
    "OnFinished fired and when, whether the duration ceiling truncated it, and which graph-input "
    "overrides the graph actually accepted. Pass name+path to additionally write the samples as "
    "a USoundWave.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path",
            "Object path to the UMetaSoundSource to render, e.g. "
            "/Game/Audio/MetaSounds/MS_Fire_AR. A MetaSound Patch is rejected: a patch has no "
            "audio output and no OnPlay, so there is nothing to render."),
        RPC_PARAM_REQ("durationSeconds", "number",
            "How much audio to render, 0.01-60. Required rather than defaulted: a MetaSound "
            "graph does not publish a duration, so any default would be an invented one. A "
            "one-shot that finishes earlier stops early and the response says at what time; a "
            "source still producing at the ceiling reports truncated:true."),
        RPC_PARAM_DEF("sampleRate", "integer",
            "Device sample rate to render at, 8000-192000. The asset's own SampleRateOverride "
            "and quality settings can move it, so the response publishes both "
            "requestedSampleRate and effectiveSampleRate; the buffer carries the effective one.",
            "48000"),
        RPC_PARAM_OPT("inputs", "object",
            "Graph input overrides applied before the render, keyed by the input's name as it "
            "appears in describe_metasound: {\"Frequency\": 440, \"Loop\": false}. A number "
            "becomes a Float; use {\"type\":\"int|float|bool|string|trigger\",\"value\":...} for "
            "anything else, because JSON cannot tell an int from a float. Names or types this "
            "graph does not accept are reported under inputs.rejected rather than dropped."),
        RPC_PARAM_DEF("analyze", "boolean",
            "Measure the rendered buffer and include the summary analysis - peak, RMS, LUFS, "
            "duration, digital-silence detection, envelope timings and spectral centroid. Same "
            "scalars-only form audio.synth.generate returns; call audio.analysis.analyze on the "
            "candidateId for onsets, the pitch track and plots.", "true"),
        RPC_PARAM_OPT("name", "string",
            "Asset name for an optional USoundWave of this render, e.g. 'SW_Fire_AR_Take1'. "
            "Requires 'path'. Omit both to keep the render in memory - the candidateId is "
            "already enough for every audio.analysis verb."),
        RPC_PARAM_OPT("path", "path",
            "Content folder for that USoundWave; must be under /Game. Requires 'name'."),
        RPC_PARAM_DEF("save", "boolean",
            "Write the .uasset to disk when name+path are given. false marks the package dirty "
            "only, and the response reports saved:false / pendingFlush:true.", "true")
    ))
{
#if !PW_RENDER_METASOUND_HANDLER
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE,
        TEXT("The MetaSound plugin is not present in this build, so there is no graph to "
             "render."));
    return true;
#else
    using namespace PwRenderMetaSoundInternal;

    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath)) return true;

    double DurationSeconds = 0.0;
    if (!Ctx.RequireNumber(TEXT("durationSeconds"), DurationSeconds)) return true;

    const int32 SampleRate = Ctx.GetInt(TEXT("sampleRate"), 48000);
    const bool bAnalyze = Ctx.GetBool(TEXT("analyze"), true);

    // Asset write is all-or-nothing on the pair: a name with no folder (or the reverse) is a
    // half-stated intent, and picking a folder for the caller is exactly the silent-wrong-target
    // this cluster refuses elsewhere.
    const FString AssetName = Ctx.GetString(TEXT("name"));
    const FString FolderPath = Ctx.GetString(TEXT("path"));
    const bool bWriteAsset = !AssetName.IsEmpty() || !FolderPath.IsEmpty();
    if (bWriteAsset && (AssetName.IsEmpty() || FolderPath.IsEmpty()))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("'name' and 'path' go together: pass both to write a USoundWave, or neither "
                 "to keep the render in the candidate registry only."));
        return true;
    }

    FString ValidatedFolder;
    FString ValidatedPath;
    AssetCreatePolicy::FResolution Resolution;
    if (bWriteAsset)
    {
        const FString SanitizedName = SanitizeAssetName(AssetName);
        if (SanitizedName.IsEmpty() || SanitizedName != AssetName)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("Invalid asset name '%s': contains characters that cannot "
                                     "be used in asset names. Valid name would be: '%s'"),
                    *AssetName, *SanitizedName));
            return true;
        }

        FString PathError;
        if (!ValidateAssetCreationPath(FolderPath, AssetName, ValidatedPath, PathError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
            return true;
        }
        ValidatedFolder = FPackageName::GetLongPackagePath(ValidatedPath);
        if (!ValidatedFolder.Equals(TEXT("/Game")) &&
            !ValidatedFolder.StartsWith(TEXT("/Game/")))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("SoundWave assets can only be created under /Game; '%s' "
                                     "resolves to '%s'. The engine writer hardcodes a /Game/ "
                                     "prefix, so any other mount root would land the asset "
                                     "somewhere else."), *FolderPath, *ValidatedFolder));
            return true;
        }

        // Resolved BEFORE the render, not beside the write: rendering ten seconds of DSP and
        // then refusing on an occupied path would throw the work away, and the caller would
        // lose the candidate id along with it. Non-destructive at bOverwrite=false - the only
        // outcomes are Create, UpdateInPlace and Rejected - so doing it early changes nothing
        // except when the refusal arrives. Mandatory either way: IAssetTools/CreatePackage on
        // an occupied path can reach a modal overwrite prompt that wedges the game thread.
        Resolution = AssetCreatePolicy::Resolve(ValidatedPath, AssetName,
            USoundWave::StaticClass(), /*bOverwriteRequested=*/false,
            /*bRequireExactClass=*/true);
        if (Resolution.IsRejected())
        {
            return AssetCreatePolicy::SendRejection(Ctx, Resolution);
        }
    }

    TArray<FAudioParameter> GraphInputs;
    const TSharedPtr<FJsonObject> InputsJson = Ctx.GetObject(TEXT("inputs"));
    if (!ParseGraphInputs(Ctx, InputsJson, GraphInputs)) return true;

    // The MetaSound load gate: resolves the package-vs-object path form and waits out the
    // 5.8 async document versioning that every Checked frontend accessor aborts on.
    UObject* Asset = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("No MetaSound asset at '%s'."), *AssetPath));
        return true;
    }

    UMetaSoundSource* Source = Cast<UMetaSoundSource>(Asset);
    if (!Source)
    {
        // A Patch resolves through the same load gate but is not renderable, and its remedy
        // differs from a wrong path, so the two get different codes (§7).
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_CLASS,
            FString::Printf(
                TEXT("'%s' is a %s, not a MetaSound Source. Only a Source carries the OnPlay "
                     "trigger and the audio output a render needs; wrap the patch in a Source "
                     "(audio.authoring.create_metasound_preset) and render that."),
                *AssetPath, *Asset->GetClass()->GetName()));
        return true;
    }

    FPwMetaSoundRenderParams RenderParams;
    RenderParams.DurationSeconds = DurationSeconds;
    RenderParams.SampleRate = SampleRate;
    RenderParams.Inputs = MoveTemp(GraphInputs);

    FPwAudioBuffer Buffer;
    FPwMetaSoundRenderReport Report;
    FString RenderCode;
    FString RenderError;
    if (!PwRenderMetaSoundSource(Source, RenderParams, Buffer, Report, RenderCode, RenderError))
    {
        // The renderer's own registered code, forwarded unmodified, plus whatever it managed
        // to measure before it stopped - the input verdicts in particular, which are the
        // fastest way to see that an override never reached the graph.
        TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
        Failure->SetStringField(TEXT("sourceAssetPath"), Source->GetPathName());
        Failure->SetObjectField(TEXT("render"), BuildRenderBlock(Report));
        Ctx.SendError(RenderCode.IsEmpty() ? FString(ErrorCodes::ERR_METASOUND_RENDER_FAILED)
                                           : RenderCode,
                      RenderError, Failure);
        return true;
    }

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    // Registered with a default (empty) recipe, the same way audio.music's stems are: this
    // buffer did not come from a synth recipe, and fabricating one would make
    // audio.synth.patch offer to edit a recipe that never produced these samples.
    //
    // The samples are MOVED in rather than copied - a 60 s render is ~23 MB - and everything
    // below reads them back through the registry's shared handle, which also keeps the
    // candidate alive for the rest of the call.
    FPwCandidate Candidate;
    Candidate.Buffer = MoveTemp(Buffer);
    const FString CandidateId = Registry.Add(MoveTemp(Candidate));

    const FPwCandidateLookupResult Lookup = Registry.Get(CandidateId);
    if (!Lookup.IsHit())
    {
        // Structurally unreachable - Add makes the new candidate most-recently-touched, so it is
        // never what that same call evicts - but reported rather than assumed, because the
        // alternative to this branch is dereferencing a null handle.
        Ctx.SendError(FPwCandidateRegistry::MissErrorCode(Lookup.Status),
            FPwCandidateRegistry::MakeMissMessage(CandidateId, Lookup),
            Registry.BuildMissPayload(CandidateId, Lookup));
        return true;
    }
    const FPwAudioBuffer& Rendered = Lookup.Candidate->Buffer;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("candidateId"), CandidateId);
    Result->SetStringField(TEXT("sourceAssetPath"), Source->GetPathName());
    Result->SetNumberField(TEXT("frames"), Rendered.NumFrames());
    Result->SetNumberField(TEXT("sampleRate"), Rendered.SampleRate);
    // The buffer's own width, not the source's: FPwAudioBuffer is deinterleaved stereo and a
    // mono graph was duplicated into both sides. render.sourceChannels is the graph's.
    Result->SetNumberField(TEXT("channels"), PwExportChannels);
    SetRounded(Result, TEXT("durationSeconds"), Rendered.DurationSeconds(), 4);
    Result->SetObjectField(TEXT("render"), BuildRenderBlock(Report));

    if (InputsJson.IsValid())
    {
        TSharedPtr<FJsonObject> InputsBlock = MakeShared<FJsonObject>();
        InputsBlock->SetBoolField(TEXT("measured"), true);
        AddNameArray(InputsBlock, TEXT("applied"), Report.AppliedInputs);
        AddNameArray(InputsBlock, TEXT("rejected"), Report.RejectedInputs);
        Result->SetObjectField(TEXT("inputs"), InputsBlock);
    }

    if (bAnalyze)
    {
        FPwAudioAnalysis Analysis;
        FString AnalysisCode;
        FString AnalysisError;
        if (PwAnalyzeBuffer(Rendered, Analysis, AnalysisCode, AnalysisError))
        {
            // The SUMMARY form, matching audio.synth.generate: bFullDetail=true adds the onset
            // list and the pitch track, which alone exceed the wrapped response ceiling.
            const TSharedPtr<FJsonObject> AnalysisJson =
                SerializeAudioAnalysis(Analysis, /*bFullDetail=*/false);
            Result->SetObjectField(TEXT("analysis"), AnalysisJson);
            Registry.SetAnalysis(CandidateId, AnalysisJson);
        }
        else
        {
            TSharedPtr<FJsonObject> Unavailable = MakeShared<FJsonObject>();
            Unavailable->SetStringField(TEXT("errorCode"), AnalysisCode);
            Unavailable->SetStringField(TEXT("error"), AnalysisError);
            Result->SetObjectField(TEXT("analysisUnavailable"), Unavailable);
        }
    }

    // -----------------------------------------------------------------------------------
    // Optional asset write. Same helper chain as audio.synth.export - AssetCreatePolicy's
    // non-modal resolve, PwCreateSoundWaveAsset, and a decode-back through a different
    // subsystem than the writer - so a wave written here carries the same guarantee.
    // -----------------------------------------------------------------------------------
    if (bWriteAsset)
    {
        TSharedPtr<FJsonObject> AssetBlock = MakeShared<FJsonObject>();

        // `Resolution` was taken before the render (see the path-validation block above), so by
        // the time control reaches here the target is known to be writable.
        const bool bSave = Ctx.GetBool(TEXT("save"), true);
        FString ExportError;
        FPwSoundWaveWriteReport WriteReport;
        USoundWave* Wave = PwCreateSoundWaveAsset(
            Rendered, ValidatedFolder, AssetName, bSave, WriteReport, ExportError);
        if (!Wave)
        {
            // The render itself succeeded and its candidate is resident, so the failure
            // carries the id: the caller retries the WRITE, not the render.
            Result->SetStringField(TEXT("assetError"), ExportError);
            Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED, ExportError, Result);
            return true;
        }

        const FString WavePath = Wave->GetPathName();

        // Verification: decode the created asset back through USoundWave::GetImportedSoundWaveData,
        // which parses the RIFF payload independently of the FSoundWavePCMWriter path that wrote
        // it, so nothing below is a readback of a field the writer just set (§4).
        FPwAudioBuffer Decoded;
        FString DecodeCode;
        FString DecodeError;
        const bool bDecoded = PwDecodeSoundWaveWithCode(Wave, Decoded, DecodeCode, DecodeError);

        TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
        Verification->SetBoolField(TEXT("measured"), bDecoded);
        Verification->SetStringField(TEXT("method"),
            TEXT("decoded back through USoundWave::GetImportedSoundWaveData (PwDecodeSoundWave), "
                 "a different subsystem from the writer"));
        if (bDecoded)
        {
            const bool bFramesMatch = Decoded.NumFrames() == Rendered.NumFrames();
            const bool bRateMatch = Decoded.SampleRate == Rendered.SampleRate;
            Verification->SetBoolField(TEXT("framesMatch"), bFramesMatch);
            Verification->SetBoolField(TEXT("sampleRateMatch"), bRateMatch);
            Verification->SetNumberField(TEXT("sourceFrames"), Rendered.NumFrames());
            Verification->SetNumberField(TEXT("decodedFrames"), Decoded.NumFrames());
            Verification->SetBoolField(TEXT("pass"), bFramesMatch && bRateMatch);
        }
        else
        {
            Verification->SetStringField(TEXT("errorCode"), DecodeCode);
            Verification->SetStringField(TEXT("error"), DecodeError);
            Verification->SetBoolField(TEXT("pass"), false);
        }
        AssetBlock->SetObjectField(TEXT("verification"), Verification);

        // The same {saveRequested, saved, pendingFlush} triple both save families emit, so a
        // caller never has to know which one ran (§5).
        if (bSave)
        {
            AddAssetSaveReport(AssetBlock, /*bSaveRequested=*/true, WriteReport.bSavedToDisk);
        }
        else
        {
            AddMarkDirtySaveReport(AssetBlock, Wave, /*bSaveRequested=*/false);
        }
        AssetCreatePolicy::AddCreateReport(AssetBlock, Resolution);
        AssetBlock->SetStringField(TEXT("assetName"), AssetName);
        AssetBlock->SetStringField(TEXT("assetPath"), WavePath);
        Result->SetObjectField(TEXT("asset"), AssetBlock);

        // Recorded on the candidate only after the round trip held, so list_candidates cannot
        // show an export that did not verify.
        if (bDecoded && Verification->GetBoolField(TEXT("pass")))
        {
            Registry.SetExportedAssetPath(CandidateId, WavePath);
        }
    }

    Result->SetObjectField(TEXT("registry"),
                           FPwCandidateRegistry::UsageToJson(Registry.Usage()));

    Ctx.SendSuccess(FString::Printf(
        TEXT("Rendered %s as candidate %s: %d frames @ %d Hz (%d graph channel(s), %s)."),
        *Source->GetName(), *CandidateId, Rendered.NumFrames(), Rendered.SampleRate,
        Report.SourceChannels,
        Report.bFinished ? TEXT("source finished") : (Report.bTruncated
            ? TEXT("truncated at the duration ceiling") : TEXT("still producing"))),
        Result);
    return true;
#endif // PW_RENDER_METASOUND_HANDLER
}
