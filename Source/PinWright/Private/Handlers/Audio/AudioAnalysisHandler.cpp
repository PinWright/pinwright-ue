// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioAnalysisHandler.cpp - audio.analysis.analyze / decompose / compare / to_recipe / audit_folder
//
// The measurement half of the audio subsystem. audio.synth.* renders; nothing here renders and
// nothing here writes an audio asset. What these five verbs add over the synth namespace is that
// they work on ANY SoundWave in the project, not only on a buffer this plugin produced.
//
// -------------------------------------------------------------------------------------------
// SOURCE RESOLUTION - ONE SEAM, TWO KINDS, NO FILE PATH (rpc-design.md §3, §7)
// -------------------------------------------------------------------------------------------
// Every verb here takes its audio from exactly one of two places: a session `candidateId` from
// the synth registry, or an `assetPath` naming a USoundWave. There is deliberately no
// file-reading path - PwAudioDecode.h states the reason, and it is a contract rather than a gap:
// one decode seam means "what was measured" and "what the project ships" are the same bytes.
// An agent pointing at a loose .wav is told to import it first and given the parameter to pass
// afterwards, because an error that only says "no" is a dead end with extra steps (§7).
//
// Neither source supplied is an ERROR, and both supplied is an ERROR. There is no safe default
// for "which sound did you mean" (§3): guessing measures a sound the caller never asked about,
// and every number that came back would be about the wrong thing while looking exactly right.
//
// Registry misses keep their three distinct codes. FPwCandidateRegistry::MissErrorCode maps
// Evicted -> CANDIDATE_EVICTED, RegistryEmpty -> NO_CANDIDATES and NeverExisted/Discarded ->
// CANDIDATE_NOT_FOUND, because "re-render, mind the budget", "generate something first" and
// "your id is wrong" are three different next moves. Flattening them is what makes an agent
// "fix" an id it got right. Decode failures forward PwDecodeSoundWaveWithCode's own code
// unmodified for the same reason: a procedural wave and a 5.1 wave need different remedies.
//
// -------------------------------------------------------------------------------------------
// WHICH VERBS ARE JOBS (rpc-design.md §9)
// -------------------------------------------------------------------------------------------
// analyze runs INLINE. It is one STFT plus a handful of time-domain passes over one buffer -
// bounded by the source, and the same work audio.synth.generate already does inline on every
// render. Answering directly also keeps its failures as real error CODES rather than as a job's
// bare error string (§7).
//
// decompose, compare and to_recipe run as JOBS. decompose is a multi-resolution STFT plus
// median-filter source separation plus peak tracking plus per-track exponential fitting;
// to_recipe is that same pipeline plus a mapping pass; compare is that pipeline TWICE, on both
// sides, plus two full analyses. All three can genuinely run past the transport's response
// timeout on a multi-second source, which is the one thing a ticket exists for.
//
// audit_folder is a JOB because its cost is the caller's: it loads and decodes up to `limit`
// assets, and a package load plus a blocking bulk-data read per asset is unbounded by anything
// this file controls.
//
// Ctx.StartJob is NOT a deferral primitive - FHandlerContext::StartJob invokes the bind delegate
// synchronously - so the work still runs on the caller's stack. What the ticket buys is that the
// RESPONSE is sent before the work starts, so a non-streaming client gets its ticket in
// milliseconds instead of holding a connection open for the whole pipeline.
//
// Everything that can produce a real error code is therefore validated and resolved SYNCHRONOUSLY,
// before StartJob: the source lookup, the decode, the folder enumeration. Only the pipeline runs
// inside the job, and its failures carry their code in the result payload (the wire shape of a
// failed job is a bare string, which system.job_status publishes beside the result).
//
// NO CANCELLATION, SAID SO RATHER THAN FAKED (§1). Nothing here registers a cancel callback,
// because nothing in the pipeline can stop mid-flight without chunking the work loops. So
// FJobRegistry::Cancel answers JOB_CANCEL_UNSUPPORTED, the started payload says
// `cancellable: false`, and the message names the code the caller will get. Claiming a
// cancellation that did not happen is strictly worse than not having one - cancelling is exactly
// what an agent does in response to a hang.
//
// -------------------------------------------------------------------------------------------
// RESPONSE SIZE
// -------------------------------------------------------------------------------------------
// The wrapped MCP ToolResult carries the payload twice - escaped in content[0].text and verbatim
// in structuredContent - so the real ceiling for a bare result is about 4,250 characters, not the
// 10,000 the spill constant suggests (board ticket E-spill-threshold-measured-post-wrap). Every
// verb here answers in a capped form, the three that have a fuller one behind a `detail` parameter:
//   analyze     SerializeAudioAnalysis(..., false)   ~1.6 KB
//   decompose   SerializeDecomposition(..., false)   ~2.0 KB, capped by the report's own caps
//   compare     the serializer's full report minus the per-mode and per-band arrays, with the
//               deviation list capped - see TrimCompareReportToSummary, which REPORTS every trim
//   to_recipe   the whole draft recipe, with its two audio-scaled lists capped - see
//               TrimRecipeToDraft, which REPORTS every cut and re-validates what it cut
//   audit_folder counts, histograms and at most MaxReportedAuditAssets named assets
// Measurement uses the PRETTY writer the transport actually takes; a condensed measurement
// understates the budget by roughly 20%. TestAudioAnalysisHandler.cpp holds the gates so growth
// trips CI instead of a user's response.
//
// Audio never travels inline in either direction. Images leave as file paths plus sizeBytes and
// mimeType, exactly as audio.synth.generate emits them.
//
// -------------------------------------------------------------------------------------------
// ENGINE VERSION
// -------------------------------------------------------------------------------------------
// Nothing in this file reaches past FAssetRegistryModule / FARFilter / USoundWave / FJsonObject,
// all present unchanged on UE 5.3-5.8, so it owes no row in docs/engine-version-support.md. The
// LUFS figures it forwards are measured by PwAnalyzeBuffer, which owns whatever version note that
// back-end needs.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/PathUtils.h"

#include "AudioGen/PwAudioAnalysis.h"
#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioCompare.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwAudioDecompose.h"
#include "AudioGen/PwAudioPlot.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthRecipe.h"
#include "State/PluginState.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two anonymous
// namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwAudioAnalysisHandlerInternal
{
    // ---------------------------------------------------------------------------------------
    // Caps. Every one of them is REPORTED when it bites - a list of 8 deviations returned for a
    // comparison that produced 57 reads as "these two differ in 8 ways", which is a measurement
    // nobody took (rpc-design.md §1).
    // ---------------------------------------------------------------------------------------

    /**
     * Deviations carried inline by the compare summary. Sized against the ~4,250-character
     * wrapped ceiling with the pretty writer: one deviation is six fields on six lines, about
     * 190 characters, and the rest of the summary (two source blocks, six paired scalars, the
     * mode counts, the diagnosis histogram and the truncation block) is about 2,000.
     */
    constexpr int32 MaxSummaryDeviations = 8;

    /**
     * The two lists in a to_recipe draft whose length follows the analysed audio rather than the
     * schema: the modal bank (up to the schema's 32 modes) and the residual contour (up to its 64
     * envelope points). Uncapped, the 250 ms fixture in TestAudioAnalysisHandler.cpp serialized to
     * 8,595 characters against the ~4,250 ceiling, and both lists grow with the reference's
     * duration - so the draft spilled to a file exactly when it got interesting.
     *
     * Asymmetric because the two rows cost differently under the pretty writer: a mode is three
     * numbers on three lines (~63 characters), a contour point is a three-field object (~108). A
     * draft is a starting point to edit, so the loudest modes and the contour's coarse shape are
     * what carry it; audio.analysis.decompose reports every mode and every residual band for a
     * caller who needs the whole fit. Both caps are REPORTED when they bite, in `note` and in
     * `truncated` - a ten-mode bank returned for a thirty-mode fit otherwise reads as the fit.
     */
    constexpr int32 MaxDraftModes = 10;
    constexpr int32 MaxDraftContourPoints = 8;

    /**
     * Decimals kept in the draft's fitted quantities. UE's JSON writer prints every double with
     * "%.17g", so an unrounded fitted frequency spends about 18 characters stating a value the fit
     * is good to two decimals of. The contour's normalized value keeps four: two would quantize a
     * fade into 1% steps, and it is the one quantity here whose whole range is 0..1.
     */
    constexpr int32 DraftDecimals = 2;
    constexpr int32 DraftContourValueDecimals = 4;

    /**
     * Flagged assets named inline by audit_folder, and unauditable ones. Small because a row is
     * an asset path plus its findings, and the path alone is often 60 characters. A caller that
     * needs every flagged path pages with a small `limit`; a caller surveying a large folder
     * reads the counts and the histogram, which cover the WHOLE page rather than these rows.
     */
    constexpr int32 MaxReportedAuditAssets = 6;
    constexpr int32 MaxReportedNotAudited = 4;

    /**
     * audit_folder's page size - the number of assets one call LOADS AND DECODES. The default is
     * a real batch (§8: the caller must not have to loop), and the ceiling exists because each
     * asset costs a package load plus a blocking bulk-data payload read.
     */
    constexpr int32 DefaultAuditLimit = 50;
    constexpr int32 MaxAuditLimit = 200;

    /** Longest per-row error text carried in a response. Enough for a code plus a field path. */
    constexpr int32 MaxErrorChars = 200;

    /** Longest generated plot-file stem before it is hashed. */
    constexpr int32 MaxPlotStemChars = 96;

    // ---------------------------------------------------------------------------------------
    // audit_folder thresholds. Published in the response rather than only spelled here, because
    // a flag is only comparable across runs if the reader can see the floor it cleared.
    // ---------------------------------------------------------------------------------------

    /**
     * Absolute mean sample value at or beyond which a DC offset is a defect, linear full scale.
     * 0.01 is 1% of full scale, about -40 dBFS: a bias that costs a percent of headroom, biases
     * every downstream sum, and clicks at the start and end of playback. Reported SIGNED (§6):
     * +0.02 and -0.02 are different faults with different causes.
     */
    constexpr double DcOffsetThreshold = 0.01;

    /**
     * Distance from the page's MEDIAN integrated loudness at which an asset is an outlier, LU.
     * 6 LU is six times the 1 LU delivery tolerance PwCompareLimits::LoudnessToleranceLu is
     * written to - well past the point where one asset jumps out of a mix, and far enough out
     * that a deliberately quiet sound in a folder of loud ones is not flagged for being quiet.
     * Median rather than mean so a handful of broken assets cannot move the reference they are
     * being measured against.
     */
    constexpr double LoudnessOutlierLu = 6.0;

    // ---------------------------------------------------------------------------------------
    // The closed audit-finding vocabulary. Closed rather than free prose so the calling model
    // branches on the token instead of parsing sentences, and paired with PwCompareDiagnosis'
    // reasoning: a check that can fail in two directions gets two tokens.
    // ---------------------------------------------------------------------------------------
    namespace AuditFinding
    {
        /** The buffer holds NaN or infinity. Checked FIRST - see the ordering note on AuditOneAsset. */
        inline constexpr TCHAR NonFiniteSamples[]  = TEXT("non_finite_samples");
        /** Peak sample below the silence floor: the asset carries no signal at all. */
        inline constexpr TCHAR DigitalSilence[]    = TEXT("digital_silence");
        /** Samples at or beyond full scale, counted per channel-sample. */
        inline constexpr TCHAR Clipping[]          = TEXT("clipping");
        /** Signed mean sample value past DcOffsetThreshold. */
        inline constexpr TCHAR DcOffset[]          = TEXT("dc_offset");
        /** Integrated loudness more than LoudnessOutlierLu from the page median. */
        inline constexpr TCHAR LoudnessOutlier[]   = TEXT("loudness_outlier");
        /** Sample rate differs from the page's most common one. */
        inline constexpr TCHAR SampleRateOutlier[] = TEXT("sample_rate_outlier");
        /** Channel count differs from the page's most common one. */
        inline constexpr TCHAR ChannelOutlier[]    = TEXT("channel_count_outlier");
    }

    /** Every token audit_folder can emit, in a stable order. The histogram publishes all seven. */
    const TArray<const TCHAR*>& AuditFindingVocabulary()
    {
        static const TArray<const TCHAR*> Tokens = {
            AuditFinding::NonFiniteSamples,
            AuditFinding::DigitalSilence,
            AuditFinding::Clipping,
            AuditFinding::DcOffset,
            AuditFinding::LoudnessOutlier,
            AuditFinding::SampleRateOutlier,
            AuditFinding::ChannelOutlier,
        };
        return Tokens;
    }

    // ---------------------------------------------------------------------------------------
    // Small JSON helpers
    // ---------------------------------------------------------------------------------------

    /**
     * Rounds before emission. Two reasons, both real: the report must not publish sixteen digits
     * of a centroid estimate that is good to three, and UE's JSON writer prints doubles with
     * "%.17g", so one unrounded value can cost twenty characters of a budget measured in
     * thousands.
     */
    double RoundTo(double Value, int32 Decimals)
    {
        if (!FMath::IsFinite(Value))
        {
            return 0.0;
        }
        const double Scale = FMath::Pow(10.0, static_cast<double>(Decimals));
        return FMath::RoundToDouble(Value * Scale) / Scale;
    }

    void SetRounded(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double Value,
                    int32 Decimals)
    {
        Object->SetNumberField(Key, RoundTo(Value, Decimals));
    }

    FString Truncate(const FString& In, int32 MaxChars)
    {
        return (In.Len() <= MaxChars) ? In : (In.Left(MaxChars) + TEXT("..."));
    }

    /**
     * The object behind a field, or null. Reached through TryGetObjectField rather than through
     * FJsonObject::Values, whose key type is UE::FSharedString from UE 5.8 and FString before it -
     * TryGetObjectField takes an FStringView on every version and is the one spelling that
     * compiles across the range. The handle is const; the object it points at is not, which is
     * what lets the caller strip fields out of it.
     */
    TSharedPtr<FJsonObject> MutableObjectField(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key)
    {
        if (!Root.IsValid())
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Found = nullptr;
        if (Root->TryGetObjectField(Key, Found) && Found && Found->IsValid())
        {
            return *Found;
        }
        return nullptr;
    }

    /** One `{returned, total}` row of a truncation report. Emitted only when the cap bit. */
    void AddTruncation(const TSharedPtr<FJsonObject>& Truncated, const TCHAR* Key,
                       int32 Returned, int32 Total)
    {
        if (Returned >= Total)
        {
            return;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("returned"), Returned);
        Entry->SetNumberField(TEXT("total"), Total);
        Truncated->SetObjectField(Key, Entry);
    }

    // ---------------------------------------------------------------------------------------
    // Detail level
    // ---------------------------------------------------------------------------------------

    /**
     * Reads `detail`, a CLOSED set. An unrecognised value is an error naming the set rather than
     * a silent fall back to the summary (§3) - a caller who asked for full detail and silently
     * got the summary would conclude the series they wanted does not exist.
     */
    bool ReadDetail(FHandlerContext& Ctx, bool& bOutFullDetail)
    {
        bOutFullDetail = false;

        FString Detail = Ctx.GetString(TEXT("detail"));
        Detail.TrimStartAndEndInline();
        if (Detail.IsEmpty())
        {
            return true;
        }

        const FString Lower = Detail.ToLower();
        if (Lower == TEXT("summary"))
        {
            return true;
        }
        if (Lower == TEXT("full"))
        {
            bOutFullDetail = true;
            return true;
        }

        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'detail' is '%s', which is not a detail level. Valid levels are ")
                            TEXT("'summary' (the default - scalars only, sized to fit the response ")
                            TEXT("ceiling) and 'full' (adds the per-frame series and the uncapped ")
                            TEXT("arrays, and may spill to a file)."), *Detail));
        return false;
    }

    // ---------------------------------------------------------------------------------------
    // Source resolution
    // ---------------------------------------------------------------------------------------

    enum class ESourceKind : uint8
    {
        // Zero is a failure, so a default-constructed source can never be mistaken for a
        // resolved one (rpc-design.md §2).
        None = 0,
        Candidate,
        SoundWave,
    };

    struct FResolvedSource
    {
        ESourceKind    Kind = ESourceKind::None;
        FString        CandidateId;
        FString        AssetPath;
        FPwAudioBuffer Buffer;

        bool IsResolved() const { return Kind != ESourceKind::None && Buffer.NumFrames() > 0; }
    };

    /** The wire keys one side of a verb accepts. Aliases follow the plugin's camel/snake rule. */
    struct FSourceKeys
    {
        /** Human name of the side, used verbatim in every message ("source", "reference", ...). */
        const TCHAR*    Label = TEXT("source");
        TArray<FString> CandidateKeys;
        TArray<FString> AssetKeys;
    };

    FSourceKeys MakeSingleSourceKeys()
    {
        FSourceKeys Keys;
        Keys.Label = TEXT("source");
        Keys.CandidateKeys = {TEXT("candidateId"), TEXT("candidate_id")};
        Keys.AssetKeys = {TEXT("assetPath"), TEXT("asset_path"), TEXT("soundWavePath")};
        return Keys;
    }

    FSourceKeys MakeReferenceKeys()
    {
        FSourceKeys Keys;
        Keys.Label = TEXT("reference");
        Keys.CandidateKeys = {TEXT("referenceCandidateId"), TEXT("reference_candidate_id")};
        Keys.AssetKeys = {TEXT("referenceAssetPath"), TEXT("reference_asset_path")};
        return Keys;
    }

    FSourceKeys MakeCandidateSideKeys()
    {
        FSourceKeys Keys;
        Keys.Label = TEXT("candidate");
        Keys.CandidateKeys = {TEXT("candidateId"), TEXT("candidate_id")};
        Keys.AssetKeys = {TEXT("candidateAssetPath"), TEXT("candidate_asset_path")};
        return Keys;
    }

    /** Keys that name a file on disk. Present -> the import remedy, never a silent ignore. */
    const TArray<FString>& FilePathKeys()
    {
        static const TArray<FString> Keys = {
            TEXT("filePath"), TEXT("file_path"), TEXT("file"),
            TEXT("wavPath"), TEXT("wav_path"), TEXT("audioFile")
        };
        return Keys;
    }

    /** True when a value is a filesystem path rather than a mounted asset path. */
    bool LooksLikeFilesystemPath(const FString& In)
    {
        if (In.Contains(TEXT("\\")) || In.Contains(TEXT(":")))
        {
            return true;
        }
        static const TCHAR* const Extensions[] = {
            TEXT(".wav"), TEXT(".ogg"), TEXT(".mp3"), TEXT(".flac"), TEXT(".aif"), TEXT(".aiff")
        };
        for (const TCHAR* Extension : Extensions)
        {
            if (In.EndsWith(Extension, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    /**
     * The way out of every "I only have a file" dead end (§7). Named once so every call site hands
     * back the same instruction and the same parameter name.
     */
    FString ImportRemedy(const FString& What, const TCHAR* AssetKey)
    {
        return FString::Printf(
            TEXT("'%s' names a file on disk, and there is no file-reading path here by design - ")
            TEXT("everything measured is a project asset, so what was measured and what the ")
            TEXT("project ships are the same bytes. Import the file as a SoundWave first (Content ")
            TEXT("Browser import, or audio.authoring.create_sound_wave_from_pcm for raw PCM), then ")
            TEXT("pass its asset path as '%s'."), *Truncate(What, MaxErrorChars), AssetKey);
    }

    /** `/Game/Audio/SW_Hit` -> `/Game/Audio/SW_Hit.SW_Hit`; an already-qualified path is kept. */
    FString ToObjectPath(const FString& PackagePath)
    {
        int32 DotIndex = INDEX_NONE;
        if (PackagePath.FindLastChar(TEXT('.'), DotIndex))
        {
            int32 SlashIndex = INDEX_NONE;
            PackagePath.FindLastChar(TEXT('/'), SlashIndex);
            if (DotIndex > SlashIndex)
            {
                return PackagePath;
            }
        }
        return PackagePath + TEXT(".") + FPaths::GetCleanFilename(PackagePath);
    }

    /** Compact `{kind, id-or-path, frames, sampleRate, durationSeconds}` block. */
    TSharedPtr<FJsonObject> SerializeSource(const FResolvedSource& Source)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        if (Source.Kind == ESourceKind::Candidate)
        {
            Object->SetStringField(TEXT("kind"), TEXT("candidate"));
            Object->SetStringField(TEXT("candidateId"), Source.CandidateId);
        }
        else
        {
            Object->SetStringField(TEXT("kind"), TEXT("soundWave"));
            Object->SetStringField(TEXT("assetPath"), Source.AssetPath);
        }
        Object->SetNumberField(TEXT("frames"), Source.Buffer.NumFrames());
        Object->SetNumberField(TEXT("sampleRate"), Source.Buffer.SampleRate);
        SetRounded(Object, TEXT("durationSeconds"),
                   static_cast<double>(Source.Buffer.DurationSeconds()), 4);
        return Object;
    }

    /**
     * Resolves one side's audio, sending the error itself and returning false on every failure.
     *
     * Order is fixed and each case is ANSWERED where it is detected (§7): a file key first (the
     * caller has a file, not an asset, and needs the import instruction rather than "missing
     * parameter"), then neither key, then both keys, then the kind-specific lookup. Answering the
     * file case first is what stops it falling through into a generic "no source supplied", which
     * would point the caller at the parameter they already tried to use.
     */
    bool ResolveSource(FHandlerContext& Ctx, FPwCandidateRegistry& Registry,
                       const FSourceKeys& Keys, FResolvedSource& Out)
    {
        Out = FResolvedSource();

        const FString CanonicalCandidateKey = Keys.CandidateKeys[0];
        const FString CanonicalAssetKey = Keys.AssetKeys[0];

        const FString FileValue = Ctx.GetStringFirstOf(FilePathKeys());
        if (!FileValue.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                ImportRemedy(FileValue, *CanonicalAssetKey));
            return false;
        }

        FString CandidateId = Ctx.GetStringFirstOf(Keys.CandidateKeys);
        CandidateId.TrimStartAndEndInline();
        FString RawAssetPath = Ctx.GetStringFirstOf(Keys.AssetKeys);
        RawAssetPath.TrimStartAndEndInline();

        if (CandidateId.IsEmpty() && RawAssetPath.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(
                    TEXT("No %s supplied: pass either '%s' (a candidate from audio.synth.generate) ")
                    TEXT("or '%s' (a USoundWave asset path). There is no default - guessing which ")
                    TEXT("sound you meant would report measurements of audio you never asked about."),
                    Keys.Label, *CanonicalCandidateKey, *CanonicalAssetKey));
            return false;
        }

        if (!CandidateId.IsEmpty() && !RawAssetPath.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(
                    TEXT("Two %s sources supplied ('%s' = '%s' and '%s' = '%s'). They name ")
                    TEXT("different audio; pass exactly one."),
                    Keys.Label, *CanonicalCandidateKey, *CandidateId,
                    *CanonicalAssetKey, *RawAssetPath));
            return false;
        }

        if (!CandidateId.IsEmpty())
        {
            const FPwCandidateLookupResult Lookup = Registry.Get(CandidateId);
            if (!Lookup.IsHit())
            {
                // The code first: it is what error handling branches on, and evicted /
                // never-generated / unknown-id have divergent remedies. The structured payload
                // carries the removal record and the live budget, which is what survives the
                // oversize-spill rewrite.
                Ctx.SendError(FPwCandidateRegistry::MissErrorCode(Lookup.Status),
                              FPwCandidateRegistry::MakeMissMessage(CandidateId, Lookup),
                              Registry.BuildMissPayload(CandidateId, Lookup));
                return false;
            }
            Out.Kind = ESourceKind::Candidate;
            Out.CandidateId = CandidateId;
            Out.Buffer = Lookup.Candidate->Buffer;
            return true;
        }

        if (LooksLikeFilesystemPath(RawAssetPath))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                ImportRemedy(RawAssetPath, *CanonicalAssetKey));
            return false;
        }

        const FString Sanitized = SanitizeProjectRelativePath(RawAssetPath);
        if (Sanitized.IsEmpty() || !IsValidAssetPath(Sanitized))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(
                    TEXT("'%s' is not a valid asset path for %s: '%s'. Expected a mounted content ")
                    TEXT("path such as /Game/Audio/SW_Hit."),
                    *CanonicalAssetKey, Keys.Label, *Truncate(RawAssetPath, MaxErrorChars)));
            return false;
        }

        const FString ObjectPath = ToObjectPath(Sanitized);
        USoundWave* Wave =
            Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *ObjectPath));
        if (!Wave)
        {
            // Distinguish "nothing there" from "something else there": the first is an import or
            // a typo, the second is the wrong asset entirely, and they are different fixes (§7).
            if (UObject* Other = FindObject<UObject>(nullptr, *ObjectPath))
            {
                Ctx.SendError(ErrorCodes::ERR_ASSET_CLASS_MISMATCH,
                    FString::Printf(
                        TEXT("'%s' is a %s, not a USoundWave. This namespace measures sample data, ")
                        TEXT("which only a SoundWave carries; for a SoundCue or MetaSound, ")
                        TEXT("reach the wave it plays."),
                        *ObjectPath, *Other->GetClass()->GetName()));
                return false;
            }
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(
                    TEXT("No USoundWave at '%s'. If the audio is a file on disk, import it as a ")
                    TEXT("SoundWave first (Content Browser import, or ")
                    TEXT("audio.authoring.create_sound_wave_from_pcm for raw PCM) and pass the ")
                    TEXT("resulting asset path as '%s'."), *ObjectPath, *CanonicalAssetKey));
            return false;
        }

        FString DecodeCode;
        FString DecodeError;
        if (!PwDecodeSoundWaveWithCode(Wave, Out.Buffer, DecodeCode, DecodeError))
        {
            // The decoder's own code, forwarded unmodified: AUDIO_PROCEDURAL_UNSUPPORTED,
            // AUDIO_MULTICHANNEL_UNSUPPORTED, AUDIO_EMPTY_BUFFER and DECODE_FAILED point at four
            // different remedies, and flattening them to one would delete the sentence that says
            // what to do.
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetStringField(TEXT("assetPath"), Wave->GetPathName());
            Data->SetStringField(TEXT("side"), Keys.Label);
            Ctx.SendError(DecodeCode, DecodeError, Data);
            return false;
        }

        Out.Kind = ESourceKind::SoundWave;
        Out.AssetPath = Wave->GetPathName();
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Plots. Same directory and the same deterministic file naming as audio.synth.generate, so
    // a retry rewrites the file rather than adding a second one (§8).
    // ---------------------------------------------------------------------------------------

    enum class EPlotView : uint8
    {
        Waveform,
        Spectrogram,
        ConstantQ
    };

    const TCHAR* LexPlotView(EPlotView View)
    {
        switch (View)
        {
        case EPlotView::Waveform:    return TEXT("waveform");
        case EPlotView::Spectrogram: return TEXT("spectrogram");
        case EPlotView::ConstantQ:   return TEXT("constantq");
        default:                     return TEXT("unknown");
        }
    }

    bool PlotViewFromString(const FString& In, EPlotView& Out)
    {
        const FString Lower = In.ToLower();
        if (Lower == TEXT("waveform"))    { Out = EPlotView::Waveform;    return true; }
        if (Lower == TEXT("spectrogram")) { Out = EPlotView::Spectrogram; return true; }
        if (Lower == TEXT("constantq") || Lower == TEXT("constant_q"))
        {
            Out = EPlotView::ConstantQ;
            return true;
        }
        return false;
    }

    /**
     * Reads `plots`, which accepts one view name or an array of them. An unrecognised view is an
     * ERROR naming the closed set (§3): dropping it silently leaves the caller waiting for an
     * image that was never going to arrive.
     */
    bool ReadPlotViews(FHandlerContext& Ctx, TArray<EPlotView>& Out)
    {
        Out.Reset();

        const auto AddOne = [&Out](const FString& Name, FString& OutBad)
        {
            EPlotView View = EPlotView::Waveform;
            if (!PlotViewFromString(Name, View))
            {
                OutBad = Name;
                return false;
            }
            Out.AddUnique(View);
            return true;
        };

        FString Bad;
        if (const TArray<TSharedPtr<FJsonValue>>* Array = Ctx.GetArray(TEXT("plots")))
        {
            for (const TSharedPtr<FJsonValue>& Value : *Array)
            {
                if (!Value.IsValid() || Value->Type != EJson::String)
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                        TEXT("'plots' holds a non-string element. Every entry names a view: "
                             "waveform, spectrogram or constantq."));
                    return false;
                }
                if (!AddOne(Value->AsString(), Bad))
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                        FString::Printf(
                            TEXT("'plots' holds '%s', which is not a view. Valid views are ")
                            TEXT("waveform, spectrogram, constantq."), *Bad));
                    return false;
                }
            }
            return true;
        }

        const FString Single = Ctx.GetString(TEXT("plots"));
        if (Single.IsEmpty())
        {
            return true;
        }
        if (!AddOne(Single, Bad))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("'plots' is '%s', which is not a view. Valid views are ")
                                TEXT("waveform, spectrogram, constantq."), *Bad));
            return false;
        }
        return true;
    }

    /** Absolute directory the analyzer PNGs land in - shared with audio.synth.generate. */
    FString PlotDirectory()
    {
        return FPaths::ConvertRelativePathToFull(
            FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("audio-plots"));
    }

    /**
     * Deterministic file stem for a source. A candidate uses its id; a SoundWave uses its asset
     * path flattened, so two waves of the same name in different folders cannot collide. A path
     * longer than the cap keeps its tail (the asset name, which is what a human reads) plus a
     * hash of the WHOLE path, so the disambiguation survives the truncation.
     */
    FString PlotStem(const FResolvedSource& Source)
    {
        if (Source.Kind == ESourceKind::Candidate)
        {
            return Source.CandidateId;
        }
        FString Stem = Source.AssetPath;
        Stem.ReplaceInline(TEXT("/"), TEXT("_"));
        Stem.ReplaceInline(TEXT("."), TEXT("_"));
        Stem.RemoveFromStart(TEXT("_"));
        if (Stem.Len() > MaxPlotStemChars)
        {
            Stem = FString::Printf(TEXT("%s_%08x"), *Stem.Right(MaxPlotStemChars),
                                   GetTypeHash(Source.AssetPath));
        }
        return Stem;
    }

    /**
     * Renders the requested views and reports every one of them - a produced image under
     * `images`, a failed one under `imageFailures`.
     *
     * A plot failure never fails the verb: the analysis happened, so reporting a missing image is
     * the truth, while erroring would throw away work the caller can still use.
     */
    void RenderPlots(const FResolvedSource& Source, const TArray<EPlotView>& Views,
                     FPwCandidateRegistry& Registry, const TSharedPtr<FJsonObject>& Result)
    {
        if (Views.Num() == 0)
        {
            return;
        }

        const FPwAudioBuffer& Buffer = Source.Buffer;
        const FString Directory = PlotDirectory();
        IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);
        const FString Stem = PlotStem(Source);

        // Only the spectrogram view needs an STFT; computed once and only when asked for.
        FPwStftResult Stft;
        bool bStftAttempted = false;
        FPwStftError StftError;

        TArray<TSharedPtr<FJsonValue>> Images;
        TArray<TSharedPtr<FJsonValue>> Failures;

        for (const EPlotView View : Views)
        {
            const FString Path =
                Directory / FString::Printf(TEXT("%s_%s.png"), *Stem, LexPlotView(View));

            FString PlotError;
            bool bWrote = false;
            switch (View)
            {
            case EPlotView::Waveform:
                bWrote = PwPlotWaveform(Buffer, Path, PlotError);
                break;
            case EPlotView::ConstantQ:
                bWrote = PwPlotConstantQ(Buffer, Path, PlotError);
                break;
            case EPlotView::Spectrogram:
            {
                if (!bStftAttempted)
                {
                    bStftAttempted = true;
                    TArray<float> Mono;
                    Mono.SetNumUninitialized(Buffer.NumFrames());
                    for (int32 Frame = 0; Frame < Buffer.NumFrames(); ++Frame)
                    {
                        Mono[Frame] = 0.5f * (Buffer.Left[Frame] + Buffer.Right[Frame]);
                    }
                    PwComputeStft(Mono, Buffer.SampleRate, FPwStftSettings(), Stft, &StftError);
                }
                if (!Stft.IsValid())
                {
                    PlotError = StftError.Message.IsEmpty()
                        ? TEXT("the STFT could not be computed for this buffer.")
                        : StftError.Message;
                    break;
                }
                bWrote = PwPlotSpectrogram(Stft, Buffer.SampleRate, /*bLogFrequency=*/true,
                                           Path, PlotError);
                break;
            }
            default:
                PlotError = TEXT("unhandled view.");
                break;
            }

            if (!bWrote)
            {
                TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
                Failure->SetStringField(TEXT("view"), LexPlotView(View));
                Failure->SetStringField(TEXT("error"), Truncate(PlotError, MaxErrorChars));
                Failures.Add(MakeShared<FJsonValueObject>(Failure));
                continue;
            }

            // sizeBytes is read off the file system, not off the encoder - the one number here
            // the writer cannot fake (§4). A zero-byte PNG is a failed write that returned true.
            const int64 SizeBytes = IFileManager::Get().FileSize(*Path);
            TSharedPtr<FJsonObject> Image = MakeShared<FJsonObject>();
            Image->SetStringField(TEXT("view"), LexPlotView(View));
            Image->SetStringField(TEXT("path"), Path);
            Image->SetNumberField(TEXT("sizeBytes"), static_cast<double>(SizeBytes));
            Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
            Images.Add(MakeShared<FJsonValueObject>(Image));

            if (Source.Kind == ESourceKind::Candidate)
            {
                Registry.AddImagePath(Source.CandidateId, Path);
            }
        }

        if (Images.Num() > 0)
        {
            Result->SetArrayField(TEXT("images"), Images);
        }
        if (Failures.Num() > 0)
        {
            Result->SetArrayField(TEXT("imageFailures"), Failures);
        }
    }

    // ---------------------------------------------------------------------------------------
    // Job plumbing shared by decompose / compare / to_recipe / audit_folder
    // ---------------------------------------------------------------------------------------

    /**
     * The started payload every job here sends.
     *
     * `cancellable` is structurally true, not asserted: nothing in this file calls
     * FJobRegistry::SetCancelCallback, which is the only thing Cancel() reads to tell a
     * cancellable verb from an uncancellable one, so the flag cannot drift away from the
     * behaviour it describes.
     */
    TSharedPtr<FJsonObject> MakeStartedPayload(const TCHAR* Work)
    {
        TSharedPtr<FJsonObject> Started = MakeShared<FJsonObject>();
        Started->SetBoolField(TEXT("cancellable"), false);
        Started->SetStringField(TEXT("message"), FString::Printf(
            TEXT("%s. This job registers no cancel callback - the analysis pipeline cannot be ")
            TEXT("stopped mid-flight - so system.job_cancel reports %s rather than a ")
            TEXT("cancellation that did not happen. Poll system.job_status with this ticket_id."),
            Work, ErrorCodes::ERR_JOB_CANCEL_UNSUPPORTED));
        return Started;
    }

    /** A failed job carries only an error STRING on the wire, so the code rides in the result. */
    void FailJob(const FJobOnComplete& OnComplete, const TSharedPtr<FJsonObject>& Result,
                 const FString& Code, const FString& Message)
    {
        Result->SetStringField(TEXT("errorCode"),
            Code.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_PARAMS) : Code);
        OnComplete(false, Result, Message);
    }

    // ---------------------------------------------------------------------------------------
    // compare: summary trimming
    // ---------------------------------------------------------------------------------------

    /**
     * Cuts the full compare report down to the summary form and REPORTS every cut.
     *
     * What is removed and why:
     *   modes.matched   - up to 64 rows of 11 numbers. Every matched mode that actually deviates
     *                     is already in `deviations`, with the same reference/candidate/delta
     *                     triple and a diagnosis token; the array adds only the modes that agree.
     *   modes.missing   - identically duplicated by the missing_mode / extra_mode deviations.
     *   modes.extra
     *   residual.bands  - duplicated by the residual_excess / residual_deficit deviations.
     * Each is REMOVED rather than emptied: an empty `matched` array beside 41 matched modes reads
     * as "nothing matched", which is a measurement nobody took, while an absent field plus a
     * `truncated` entry saying `returned: 0, total: 41` cannot be misread (rpc-design.md §1).
     *
     * `deviations` is capped in its published order (onset, attack, decay, tail, loudness,
     * centroid, matched modes, missing, extra, residual) - deterministic, so two runs diff
     * cleanly. `diagnosisCounts` is computed over ALL of them, so the tail that did not fit is
     * still visible as a count rather than silently gone.
     */
    void TrimCompareReportToSummary(const TSharedPtr<FJsonObject>& Report,
                                    const FPwCompareResult& Source)
    {
        if (!Report.IsValid() || !Source.bMeasured)
        {
            return;
        }

        TSharedPtr<FJsonObject> Truncated = MakeShared<FJsonObject>();

        if (const TSharedPtr<FJsonObject> Modes = MutableObjectField(Report, TEXT("modes")))
        {
            Modes->RemoveField(TEXT("matched"));
            Modes->RemoveField(TEXT("missing"));
            Modes->RemoveField(TEXT("extra"));
            Modes->SetNumberField(TEXT("matchedCount"), Source.ModePairs.Num());
            Modes->SetNumberField(TEXT("missingCount"), Source.MissingModes.Num());
            Modes->SetNumberField(TEXT("extraCount"), Source.ExtraModes.Num());
            AddTruncation(Truncated, TEXT("modesMatched"), 0, Source.ModePairs.Num());
            AddTruncation(Truncated, TEXT("modesMissing"), 0, Source.MissingModes.Num());
            AddTruncation(Truncated, TEXT("modesExtra"), 0, Source.ExtraModes.Num());
        }

        if (const TSharedPtr<FJsonObject> Residual = MutableObjectField(Report, TEXT("residual")))
        {
            Residual->RemoveField(TEXT("bands"));
            Residual->SetNumberField(TEXT("bandCount"), Source.ResidualBands.Num());
            AddTruncation(Truncated, TEXT("residualBands"), 0, Source.ResidualBands.Num());
        }

        const int32 TotalDeviations = Source.Deviations.Num();
        if (TotalDeviations > MaxSummaryDeviations)
        {
            const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
            if (Report->TryGetArrayField(TEXT("deviations"), Existing) && Existing)
            {
                TArray<TSharedPtr<FJsonValue>> Kept;
                Kept.Reserve(MaxSummaryDeviations);
                for (int32 Index = 0; Index < MaxSummaryDeviations && Index < Existing->Num(); ++Index)
                {
                    Kept.Add((*Existing)[Index]);
                }
                Report->SetArrayField(TEXT("deviations"), Kept);
            }
            AddTruncation(Truncated, TEXT("deviations"), MaxSummaryDeviations, TotalDeviations);
        }

        // The histogram is over every deviation, capped or not, so "you have 30 extra modes"
        // survives a trim that kept 8 rows.
        TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
        for (const FPwCompareDeviation& Deviation : Source.Deviations)
        {
            const double Previous = Counts->HasField(Deviation.Diagnosis)
                ? Counts->GetNumberField(Deviation.Diagnosis) : 0.0;
            Counts->SetNumberField(Deviation.Diagnosis, Previous + 1.0);
        }
        Report->SetObjectField(TEXT("diagnosisCounts"), Counts);

        if (Truncated->Values.Num() > 0)
        {
            Report->SetObjectField(TEXT("truncated"), Truncated);
        }
    }

    // ---------------------------------------------------------------------------------------
    // to_recipe: draft trimming
    // ---------------------------------------------------------------------------------------

    /**
     * The first Keep entries of a mode array, rounded. Order is the mapper's: loudest first.
     *
     * A non-numeric entry becomes 0, which is below every mode range the schema accepts, so the
     * re-validation refuses the draft rather than shipping it - dropping the entry instead would
     * leave the three mode arrays at different lengths, which is the one shape the bank's own
     * length check cannot recover from.
     */
    TArray<TSharedPtr<FJsonValue>> CapModeArray(const TArray<TSharedPtr<FJsonValue>>& In, int32 Keep)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        const int32 Count = FMath::Min(Keep, In.Num());
        Out.Reserve(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out.Add(MakeShared<FJsonValueNumber>(
                RoundTo(In[Index].IsValid() ? In[Index]->AsNumber() : 0.0, DraftDecimals)));
        }
        return Out;
    }

    /**
     * The contour resampled to at most Keep points, rounded, endpoints included - the same
     * even-spacing decimation PwDecompositionToRecipe already applies at the schema's 64-point
     * ceiling, so the draft's shape starts and ends where the measured one did.
     *
     * Times are CLAMPED to the layer's span after rounding rather than left as rounded: the last
     * measured point sits exactly at the end of the clip, and a duration that is not a whole
     * hundredth of a millisecond would round that point PAST the end, which the schema rejects.
     */
    TArray<TSharedPtr<FJsonValue>> CapContour(const TArray<TSharedPtr<FJsonValue>>& In, int32 Keep,
                                              double LayerSpanMs)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        const int32 Count = FMath::Min(Keep, In.Num());
        Out.Reserve(Count);
        for (int32 Step = 0; Step < Count; ++Step)
        {
            const int32 Index = (Count < 2) ? 0
                : FMath::Min(In.Num() - 1, FMath::FloorToInt32(
                    static_cast<double>(Step) * static_cast<double>(In.Num() - 1)
                        / static_cast<double>(Count - 1)));

            const TSharedPtr<FJsonObject> Point = In[Index].IsValid()
                ? In[Index]->AsObject() : nullptr;
            double TimeMs = 0.0;
            double Value = 0.0;
            if (!Point.IsValid() || !Point->TryGetNumberField(TEXT("timeMs"), TimeMs)
                || !Point->TryGetNumberField(TEXT("value"), Value))
            {
                // Dropped rather than defaulted: a point invented at 0 would be a measurement
                // nobody took, and the dropped count is already reported by the caller (§1).
                continue;
            }

            TSharedPtr<FJsonObject> Kept = MakeShared<FJsonObject>();
            Kept->SetNumberField(TEXT("timeMs"),
                FMath::Min(RoundTo(TimeMs, DraftDecimals), LayerSpanMs));
            Kept->SetNumberField(TEXT("value"), RoundTo(Value, DraftContourValueDecimals));
            FString Curve;
            if (Point->TryGetStringField(TEXT("curve"), Curve))
            {
                Kept->SetStringField(TEXT("curve"), Curve);
            }
            Out.Add(MakeShared<FJsonValueObject>(Kept));
        }
        return Out;
    }

    /**
     * Cuts the fitted draft down to what answers inline, and REPORTS every cut.
     *
     * Fills `Truncated` with the `{returned, total}` rows and returns the note fragment naming
     * what was capped - empty when nothing was, because a cap that did not bite is not news. A
     * silent trim is the §1 defect in its purest form here: the recipe IS the answer, so ten
     * modes handed back for a thirty-mode fit would read as the fit rather than as a draft.
     *
     * The three mode arrays are capped to the same count on purpose - the schema's own
     * ValidateModalModes rejects a bank whose arrays disagree in length, so an off-by-one here
     * would produce a draft that errors on first use.
     */
    FString TrimRecipeToDraft(const TSharedPtr<FJsonObject>& RecipeJson,
                              const TSharedPtr<FJsonObject>& Truncated)
    {
        const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
        if (!RecipeJson.IsValid() || !RecipeJson->TryGetArrayField(TEXT("layers"), Layers) || !Layers)
        {
            return FString();
        }

        double DurationMs = 0.0;
        RecipeJson->TryGetNumberField(TEXT("durationMs"), DurationMs);

        static const TCHAR* const ModeArrayKeys[] = {
            TEXT("modeFreqsHz"), TEXT("modeDecaysMs"), TEXT("modeGainsDb") };

        int32 KeptModes = 0;
        int32 TotalModes = 0;
        int32 KeptPoints = 0;
        int32 TotalPoints = 0;

        for (const TSharedPtr<FJsonValue>& LayerValue : *Layers)
        {
            const TSharedPtr<FJsonObject> Layer =
                LayerValue.IsValid() ? LayerValue->AsObject() : nullptr;
            if (!Layer.IsValid())
            {
                continue;
            }

            if (const TSharedPtr<FJsonObject> Generator = MutableObjectField(Layer, TEXT("generator")))
            {
                if (const TSharedPtr<FJsonObject> Params = MutableObjectField(Generator, TEXT("params")))
                {
                    const int32 KeyCount = static_cast<int32>(UE_ARRAY_COUNT(ModeArrayKeys));
                    for (int32 Which = 0; Which < KeyCount; ++Which)
                    {
                        const TArray<TSharedPtr<FJsonValue>>* Modes = nullptr;
                        if (!Params->TryGetArrayField(ModeArrayKeys[Which], Modes) || !Modes)
                        {
                            continue;
                        }
                        const int32 Total = Modes->Num();
                        const TArray<TSharedPtr<FJsonValue>> Capped =
                            CapModeArray(*Modes, MaxDraftModes);
                        const int32 Kept = Capped.Num();
                        Params->SetArrayField(ModeArrayKeys[Which], Capped);

                        // Counted once for the bank, not once per array: the three describe the
                        // same modes, so summing all three would report the bank three times over.
                        if (Which == 0)
                        {
                            TotalModes += Total;
                            KeptModes += Kept;
                        }
                    }
                }
            }

            const TArray<TSharedPtr<FJsonValue>>* Contour = nullptr;
            if (Layer->TryGetArrayField(TEXT("ampEnvelope"), Contour) && Contour)
            {
                double StartMs = 0.0;
                Layer->TryGetNumberField(TEXT("startMs"), StartMs);

                TotalPoints += Contour->Num();
                const TArray<TSharedPtr<FJsonValue>> Capped =
                    CapContour(*Contour, MaxDraftContourPoints, DurationMs - StartMs);
                KeptPoints += Capped.Num();
                Layer->SetArrayField(TEXT("ampEnvelope"), Capped);
            }
        }

        AddTruncation(Truncated, TEXT("modes"), KeptModes, TotalModes);
        AddTruncation(Truncated, TEXT("residualContour"), KeptPoints, TotalPoints);

        TArray<FString> Parts;
        if (KeptModes < TotalModes)
        {
            Parts.Add(FString::Printf(
                TEXT("the modal bank carries the %d loudest of %d fitted modes"),
                KeptModes, TotalModes));
        }
        if (KeptPoints < TotalPoints)
        {
            Parts.Add(FString::Printf(
                TEXT("the residual contour is resampled to %d of %d measured points"),
                KeptPoints, TotalPoints));
        }
        if (Parts.Num() == 0)
        {
            return FString();
        }
        return FString::Printf(
            TEXT("%s, so the draft answers inline rather than spilling to a file - call ")
            TEXT("audio.analysis.decompose for every mode and residual band"),
            *FString::Join(Parts, TEXT(" and ")));
    }

    // ---------------------------------------------------------------------------------------
    // audit_folder
    // ---------------------------------------------------------------------------------------

    /**
     * One audited asset's measurements.
     *
     * Deliberately holds only what CROSSES the audit's two passes: the context-free checks
     * (clipping, DC offset) consume their measurement where they take it, while the format and
     * loudness outlier checks cannot be decided until the whole page has been measured.
     */
    struct FAuditRow
    {
        FString AssetPath;

        /** True when the analysis named the asset digitally silent - the level checks then have
            nothing to measure and are SKIPPED rather than reported as passing. */
        bool    bSilent = false;

        int32   SampleRate = 0;
        int32   Channels = 0;

        /** BS.1770 needs roughly 500 ms of signal; a shorter asset has no integrated loudness,
            and is therefore neither an outlier nor proven fine (§1). */
        bool    bLoudnessMeasured = false;
        double  IntegratedLufs = 0.0;

        TArray<TSharedPtr<FJsonObject>> Findings;
    };

    TSharedPtr<FJsonObject> MakeFinding(const TCHAR* Check, double Measured, int32 Decimals)
    {
        TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
        Finding->SetStringField(TEXT("check"), Check);
        Finding->SetNumberField(TEXT("measured"), RoundTo(Measured, Decimals));
        return Finding;
    }

    /**
     * A finding whose magnitude genuinely was not measured - the check tripped, but the quantity
     * behind it is unavailable. `measured` is OMITTED rather than filled with a plausible 1: a
     * fabricated count is exactly what §1 forbids, and the reader must be able to tell "the check
     * fired" from "the check fired this many times".
     */
    TSharedPtr<FJsonObject> MakeUnquantifiedFinding(const TCHAR* Check)
    {
        TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
        Finding->SetStringField(TEXT("check"), Check);
        return Finding;
    }

    /** Most common value in a sample, ties broken toward the smaller value for determinism. */
    bool ModalValue(const TArray<int32>& Values, int32& OutModal, int32& OutDistinct)
    {
        OutModal = 0;
        OutDistinct = 0;
        if (Values.Num() == 0)
        {
            return false;
        }

        TMap<int32, int32> Counts;
        for (const int32 Value : Values)
        {
            ++Counts.FindOrAdd(Value);
        }
        OutDistinct = Counts.Num();

        int32 BestCount = -1;
        for (const TPair<int32, int32>& Pair : Counts)
        {
            if (Pair.Value > BestCount || (Pair.Value == BestCount && Pair.Key < OutModal))
            {
                BestCount = Pair.Value;
                OutModal = Pair.Key;
            }
        }
        return true;
    }

    /** Median of a sample. Sorted copy rather than an in-place sort of the caller's array. */
    bool MedianValue(TArray<double> Values, double& OutMedian)
    {
        OutMedian = 0.0;
        if (Values.Num() == 0)
        {
            return false;
        }
        Values.Sort();
        const int32 Middle = Values.Num() / 2;
        OutMedian = (Values.Num() % 2 == 1)
            ? Values[Middle]
            : 0.5 * (Values[Middle - 1] + Values[Middle]);
        return true;
    }

    /**
     * Loads, decodes and analyses one asset, filling Row's measurements and its per-asset
     * findings.
     *
     * CHECK ORDER IS LOAD-BEARING (rpc-design.md §7). PwAnalyzeBuffer rejects a non-finite buffer
     * BEFORE it tests for silence, because NaN compares false against every threshold, so
     * FMath::Max(0.0, NaN) is 0 and a NaN-filled asset scanned for its peak first measures as
     * digital silence. That ordering is inherited here rather than re-implemented: a non-finite
     * asset arrives as an analysis FAILURE carrying AUDIO_NON_FINITE_SAMPLES and is flagged as
     * that, and only an asset the analyzer accepted can reach the silence branch. Undoing this by
     * testing silence at the handler layer would send the caller to look at their gain staging
     * instead of at whatever produced the NaN.
     *
     * Silence then short-circuits the level checks: clipping, DC offset and loudness have nothing
     * to measure on a buffer with no signal, and reporting "no clipping" for silence is a pass
     * awarded for an absence.
     *
     * @return false when the asset could not be measured at all; OutErrorCode then names why.
     */
    bool AuditOneAsset(const FAssetData& Data, FAuditRow& Row, FString& OutErrorCode)
    {
        Row = FAuditRow();
        Row.AssetPath = Data.GetObjectPathString();
        OutErrorCode.Reset();

        USoundWave* Wave = Cast<USoundWave>(Data.GetAsset());
        if (!Wave)
        {
            OutErrorCode = ErrorCodes::ERR_ASSET_LOAD_FAILED;
            return false;
        }
        Row.AssetPath = Wave->GetPathName();

        // Cheap fields first, off the loaded object: the channel count is a UPROPERTY and the
        // deinterleaved buffer cannot carry it (mono duplicates into both sides).
        Row.Channels = Wave->NumChannels;

        // PwDecodeSoundWaveWithCode rejects a procedural wave and a >2-channel layout BEFORE it
        // touches the payload, so "only fully decode what you must" is already satisfied by the
        // decoder rather than by a duplicated pre-check here.
        FPwAudioBuffer Buffer;
        FString DecodeError;
        if (!PwDecodeSoundWaveWithCode(Wave, Buffer, OutErrorCode, DecodeError))
        {
            return false;
        }
        Row.SampleRate = Buffer.SampleRate;

        FPwAudioAnalysis Analysis;
        FString AnalysisError;
        if (!PwAnalyzeBuffer(Buffer, Analysis, OutErrorCode, AnalysisError))
        {
            if (OutErrorCode == ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES)
            {
                // A defect the audit exists to find, not an asset it failed to read. The count is
                // unavailable (the analyzer refuses before it fills the technical family), so the
                // finding carries the check alone rather than a fabricated magnitude.
                Row.Findings.Add(MakeUnquantifiedFinding(AuditFinding::NonFiniteSamples));
                return true;
            }
            return false;
        }

        Row.bSilent = Analysis.Technical.bDigitalSilence;

        if (Row.bSilent)
        {
            Row.Findings.Add(MakeFinding(AuditFinding::DigitalSilence,
                                         Analysis.Technical.PeakLinear, 12));
            return true;
        }

        if (Analysis.Technical.ClippedSamples > 0)
        {
            Row.Findings.Add(MakeFinding(AuditFinding::Clipping,
                                         static_cast<double>(Analysis.Technical.ClippedSamples), 0));
        }

        if (FMath::Abs(Analysis.Technical.DcOffset) > DcOffsetThreshold)
        {
            // Signed, not absolute (§6): a positive and a negative bias are different faults.
            Row.Findings.Add(MakeFinding(AuditFinding::DcOffset, Analysis.Technical.DcOffset, 4));
        }

        Row.bLoudnessMeasured = Analysis.Loudness.State.bMeasured;
        Row.IntegratedLufs = Analysis.Loudness.IntegratedLufs;
        return true;
    }
}

// =================================================================================================
// audio.analysis.analyze
// =================================================================================================

REGISTER_RPC_HANDLER("audio.analysis.analyze", "audio.analysis",
    "Measure one sound - a session candidate or any USoundWave asset - and report the descriptor "
    "families: technical (duration, rate, peak/RMS dB, clipped samples, signed DC offset, start/end "
    "discontinuities), envelope (onset, attack, decay, tail, crest, transient count), gated BS.1770 "
    "loudness, spectral (centroid, rolloff, flatness, six-band split, peaks), pitch and stereo "
    "image. A family that could not be measured is OMITTED and its reason listed under `unmeasured`, "
    "never filled with zeros. Digital silence is a success reporting only the technical family. "
    "Optional analyzer PNGs leave as file paths. There is no file-reading path: import a loose audio "
    "file as a SoundWave first.",
    RPC_PARAMS(
        RPC_PARAM_OPT("candidateId", "string",
            "Session candidate from audio.synth.generate. Pass this OR assetPath, never both."),
        RPC_PARAM_OPT("assetPath", "path",
            "USoundWave asset path, e.g. /Game/Audio/SW_Hit. Pass this OR candidateId, never both."),
        RPC_PARAM_DEF("detail", "string",
            "'summary' (scalars only, sized to fit the response ceiling) or 'full' (adds the onset "
            "list, pitch track, flux series and analysis parameters; may spill to a file).",
            "summary"),
        RPC_PARAM_OPT("plots", "array|string",
            "Analyzer PNGs to render: waveform, spectrogram, constantq. Returned as file paths with "
            "sizeBytes and mimeType - never inline.")
    ))
{
    using namespace PwAudioAnalysisHandlerInternal;

    bool bFullDetail = false;
    if (!ReadDetail(Ctx, bFullDetail)) return true;

    TArray<EPlotView> Views;
    if (!ReadPlotViews(Ctx, Views)) return true;

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    FResolvedSource Source;
    if (!ResolveSource(Ctx, Registry, MakeSingleSourceKeys(), Source)) return true;

    FPwAudioAnalysis Analysis;
    FString AnalysisCode;
    FString AnalysisError;
    if (!PwAnalyzeBuffer(Source.Buffer, Analysis, AnalysisCode, AnalysisError))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetObjectField(TEXT("source"), SerializeSource(Source));
        Ctx.SendError(AnalysisCode, AnalysisError, Data);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetObjectField(TEXT("source"), SerializeSource(Source));
    Result->SetStringField(TEXT("detail"), bFullDetail ? TEXT("full") : TEXT("summary"));
    Result->SetObjectField(TEXT("analysis"), SerializeAudioAnalysis(Analysis, bFullDetail));

    if (Source.Kind == ESourceKind::Candidate)
    {
        // Always the summary form, matching audio.synth.generate, so `hasAnalysis` in
        // list_candidates means the same thing whichever verb attached it.
        Registry.SetAnalysis(Source.CandidateId,
                             SerializeAudioAnalysis(Analysis, /*bFullDetail=*/false));
    }

    RenderPlots(Source, Views, Registry, Result);

    Ctx.SendSuccess(FString::Printf(
        TEXT("Analysed %d frames at %d Hz (%s detail)."),
        Source.Buffer.NumFrames(), Source.Buffer.SampleRate,
        bFullDetail ? TEXT("full") : TEXT("summary")), Result);
    return true;
}

// =================================================================================================
// audio.analysis.decompose
// =================================================================================================

REGISTER_RPC_HANDLER("audio.analysis.decompose", "audio.analysis",
    "Take a sound apart into the three layers the synthesizer can put back together: characterised "
    "transients, tracked sinusoidal partials with a fitted exponential mode each, and the residual "
    "the partials did not explain, summarised as log-spaced band envelopes - plus harmonic / "
    "percussive / residual energy ratios, stereo behaviour and the symbolic report. Returns a JOB "
    "TICKET: the pipeline is a multi-resolution STFT plus median-filter separation plus peak "
    "tracking plus per-track fitting. The job cannot be cancelled and says so. A track whose "
    "amplitude never decays reports no fit rather than an invented decay time.",
    RPC_PARAMS(
        RPC_PARAM_OPT("candidateId", "string",
            "Session candidate from audio.synth.generate. Pass this OR assetPath, never both."),
        RPC_PARAM_OPT("assetPath", "path",
            "USoundWave asset path. Pass this OR candidateId, never both."),
        RPC_PARAM_DEF("detail", "string",
            "'summary' (capped arrays, every cap reported) or 'full' (the per-frame point arrays; "
            "may spill to a file).", "summary")
    ))
{
    using namespace PwAudioAnalysisHandlerInternal;

    bool bFullDetail = false;
    if (!ReadDetail(Ctx, bFullDetail)) return true;

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    // Resolved synchronously so a miss comes back as CANDIDATE_EVICTED / NO_CANDIDATES /
    // ASSET_NOT_FOUND / AUDIO_PROCEDURAL_UNSUPPORTED rather than as a job's bare error string.
    FResolvedSource Source;
    if (!ResolveSource(Ctx, Registry, MakeSingleSourceKeys(), Source)) return true;

    const TSharedPtr<FJsonObject> SourceJson = SerializeSource(Source);

    TSharedPtr<FJsonObject> Started = MakeStartedPayload(TEXT("Decomposing"));
    Started->SetObjectField(TEXT("source"), SourceJson);

    FJobBindArgs Args;
    Args.Method = TEXT("audio.analysis.decompose");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate =
        [Buffer = MoveTemp(Source.Buffer), SourceJson, bFullDetail](FJobOnComplete OnComplete)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetObjectField(TEXT("source"), SourceJson);

        const FPwDecomposeSettings Settings;
        FPwDecomposition Decomposition;
        FString Code;
        FString Error;
        if (!PwDecomposeBuffer(Buffer, Settings, Decomposition, Code, Error)
            || !Decomposition.bMeasured)
        {
            FailJob(OnComplete, Result, Code,
                Error.IsEmpty() ? Decomposition.UnmeasuredReason : Error);
            return;
        }

        Result->SetStringField(TEXT("detail"), bFullDetail ? TEXT("full") : TEXT("summary"));
        Result->SetObjectField(TEXT("decomposition"),
                               SerializeDecomposition(Decomposition, bFullDetail));
        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}

// =================================================================================================
// audio.analysis.compare
// =================================================================================================

REGISTER_RPC_HANDLER("audio.analysis.compare", "audio.analysis",
    "Measure what a candidate render differs from a reference recording BY, and in which direction. "
    "Every difference is paired and signed - reference value, candidate value and the signed delta - "
    "with a closed-vocabulary diagnosis token (attack_too_sharp / attack_too_soft, missing_mode / "
    "extra_mode, ...) so the caller branches instead of parsing prose. Frequencies are compared in "
    "cents and decays as ratios, because +50 Hz is an octave in one register and inaudible in "
    "another. Both sides of the mode matching are reported: reference modes with no counterpart AND "
    "candidate modes with no counterpart. Returns a JOB TICKET (it decomposes both sides) and cannot "
    "be cancelled. Silence is an error here: a comparison against silence produces no deviations, "
    "which would read as a match.",
    RPC_PARAMS(
        RPC_PARAM_OPT("referenceCandidateId", "string",
            "Reference side as a session candidate. Pass this OR referenceAssetPath."),
        RPC_PARAM_OPT("referenceAssetPath", "path",
            "Reference side as a USoundWave asset path. Pass this OR referenceCandidateId."),
        RPC_PARAM_OPT("candidateId", "string",
            "Candidate side as a session candidate. Pass this OR candidateAssetPath."),
        RPC_PARAM_OPT("candidateAssetPath", "path",
            "Candidate side as a USoundWave asset path. Pass this OR candidateId."),
        RPC_PARAM_DEF("detail", "string",
            "'summary' (paired scalars, mode counts, the capped deviation list and a full diagnosis "
            "histogram) or 'full' (every matched / missing / extra mode and every residual band; "
            "may spill to a file).", "summary")
    ))
{
    using namespace PwAudioAnalysisHandlerInternal;

    bool bFullDetail = false;
    if (!ReadDetail(Ctx, bFullDetail)) return true;

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    FResolvedSource Reference;
    if (!ResolveSource(Ctx, Registry, MakeReferenceKeys(), Reference)) return true;

    FResolvedSource Candidate;
    if (!ResolveSource(Ctx, Registry, MakeCandidateSideKeys(), Candidate)) return true;

    const TSharedPtr<FJsonObject> ReferenceJson = SerializeSource(Reference);
    const TSharedPtr<FJsonObject> CandidateJson = SerializeSource(Candidate);

    TSharedPtr<FJsonObject> Started = MakeStartedPayload(TEXT("Comparing"));
    Started->SetObjectField(TEXT("reference"), ReferenceJson);
    Started->SetObjectField(TEXT("candidate"), CandidateJson);

    FJobBindArgs Args;
    Args.Method = TEXT("audio.analysis.compare");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate =
        [ReferenceBuffer = MoveTemp(Reference.Buffer), CandidateBuffer = MoveTemp(Candidate.Buffer),
         ReferenceJson, CandidateJson, bFullDetail](FJobOnComplete OnComplete)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetObjectField(TEXT("reference"), ReferenceJson);
        Result->SetObjectField(TEXT("candidate"), CandidateJson);

        FPwCompareResult Compare;
        FString Code;
        FString Error;
        if (!PwCompareAudio(ReferenceBuffer, CandidateBuffer, Compare, Code, Error))
        {
            FailJob(OnComplete, Result, Code,
                Error.IsEmpty() ? Compare.UnmeasuredReason : Error);
            return;
        }

        const TSharedPtr<FJsonObject> Report = SerializeCompareResult(Compare);
        if (!bFullDetail)
        {
            TrimCompareReportToSummary(Report, Compare);
        }

        Result->SetStringField(TEXT("detail"), bFullDetail ? TEXT("full") : TEXT("summary"));
        // Derived from the deviation list rather than stored beside it, so the verdict cannot
        // disagree with what it summarises (§2).
        Result->SetBoolField(TEXT("match"), Compare.IsMatch());
        Result->SetNumberField(TEXT("deviationCount"), Compare.Deviations.Num());
        Result->SetObjectField(TEXT("comparison"), Report);
        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}

// =================================================================================================
// audio.analysis.to_recipe
// =================================================================================================

REGISTER_RPC_HANDLER("audio.analysis.to_recipe", "audio.analysis",
    "Analysis by resynthesis: decompose a reference sound and emit a DRAFT audio.synth recipe "
    "fitted to it - a modal layer from the fitted modes, an exciter chosen from the measured "
    "transient shape, and a filtered-noise layer from the residual's band limits, tilt and contour. "
    "With no preset library in this system this is the cold-start path: the user's own audio is the "
    "template. The returned `note` is load-bearing - it records what the mapping DROPPED, CAPPED or "
    "ESTIMATED (notably that the noise layer's absolute gain is a first estimate, closed in one "
    "iteration by audio.analysis.compare's loudness delta), so it is how the caller knows what to "
    "trust. The recipe is round-tripped through the schema parser before it is returned, so a draft "
    "that would error on first use is refused rather than handed over. The draft's two lists that "
    "scale with the reference - the modal bank and the residual contour - are CAPPED so the whole "
    "recipe answers inline instead of spilling to a file, and both caps are reported, in `note` and "
    "as `{returned, total}` rows under `truncated`; audio.analysis.decompose reports every mode and "
    "every residual band. Returns a JOB TICKET and cannot be cancelled.",
    RPC_PARAMS(
        RPC_PARAM_OPT("candidateId", "string",
            "Reference as a session candidate. Pass this OR assetPath, never both."),
        RPC_PARAM_OPT("assetPath", "path",
            "Reference as a USoundWave asset path. Pass this OR candidateId, never both.")
    ))
{
    using namespace PwAudioAnalysisHandlerInternal;

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    FResolvedSource Source;
    if (!ResolveSource(Ctx, Registry, MakeSingleSourceKeys(), Source)) return true;

    const TSharedPtr<FJsonObject> SourceJson = SerializeSource(Source);

    TSharedPtr<FJsonObject> Started = MakeStartedPayload(TEXT("Fitting a draft recipe"));
    Started->SetObjectField(TEXT("source"), SourceJson);

    FJobBindArgs Args;
    Args.Method = TEXT("audio.analysis.to_recipe");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate =
        [Buffer = MoveTemp(Source.Buffer), SourceJson](FJobOnComplete OnComplete)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetObjectField(TEXT("source"), SourceJson);

        const FPwDecomposeSettings Settings;
        FPwDecomposition Decomposition;
        FString Code;
        FString Error;
        if (!PwDecomposeBuffer(Buffer, Settings, Decomposition, Code, Error)
            || !Decomposition.bMeasured)
        {
            FailJob(OnComplete, Result, Code,
                Error.IsEmpty() ? Decomposition.UnmeasuredReason : Error);
            return;
        }

        FPwSynthRecipe Recipe;
        FString RecipeCode;
        FString RecipeNote;
        if (!PwDecompositionToRecipe(Decomposition, Recipe, RecipeCode, RecipeNote))
        {
            FailJob(OnComplete, Result, RecipeCode, RecipeNote);
            return;
        }

        const TSharedPtr<FJsonObject> RecipeJson = SerializeSynthRecipe(Recipe);
        if (!RecipeJson.IsValid())
        {
            FailJob(OnComplete, Result, ErrorCodes::ERR_INVALID_RECIPE,
                TEXT("The fitted recipe did not serialize, so there is no draft to hand back."));
            return;
        }

        // Trimmed to what answers inline, then re-validated - in that order, because the schema
        // parser is the only check the trim cannot fake (§4). The mapper validated what it fitted;
        // the caller receives what came out of THIS pass, so this pass owes its own round trip.
        TSharedPtr<FJsonObject> Truncated = MakeShared<FJsonObject>();
        const FString DraftNote = TrimRecipeToDraft(RecipeJson, Truncated);

        FPwSynthRecipe Trimmed;
        FPwSynthRecipeError TrimError;
        if (!ParseSynthRecipe(RecipeJson, Trimmed, TrimError))
        {
            FailJob(OnComplete, Result,
                TrimError.Code.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_RECIPE) : TrimError.Code,
                FString::Printf(TEXT("The fitted recipe no longer validates once trimmed to the ")
                    TEXT("response budget, so it is refused rather than returned. %s"),
                    *TrimError.ToString()));
            return;
        }

        // NOTES ARE NOT FAILURES: a true return with an EMPTY code and a non-empty message is the
        // back-end's note convention. Always emitted, empty included - an absent field would be
        // indistinguishable from a mapping that reported nothing because it never looked, and an
        // empty string is the measurement "nothing was dropped, capped or estimated". The trim's
        // own note joins the mapping's with the same "; " the mapper joins its notes with, so a
        // caller splitting on it still reads one list.
        Result->SetStringField(TEXT("note"), (RecipeNote.IsEmpty() || DraftNote.IsEmpty())
            ? RecipeNote + DraftNote
            : RecipeNote + TEXT("; ") + DraftNote);
        Result->SetObjectField(TEXT("recipe"), RecipeJson);
        if (Truncated->Values.Num() > 0)
        {
            Result->SetObjectField(TEXT("truncated"), Truncated);
        }

        // What the draft was fitted FROM, so the caller can see how much of the reference the
        // mapping actually had to work with before trusting the result.
        TSharedPtr<FJsonObject> FittedFrom = MakeShared<FJsonObject>();
        FittedFrom->SetNumberField(TEXT("transients"), Decomposition.Transients.Num());
        FittedFrom->SetNumberField(TEXT("partials"), Decomposition.Partials.Num());
        FittedFrom->SetNumberField(TEXT("modes"), Decomposition.Modes.Num());
        FittedFrom->SetNumberField(TEXT("residualBands"), Decomposition.Residual.Num());
        SetRounded(FittedFrom, TEXT("durationMs"), Decomposition.DurationMs, 2);
        Result->SetObjectField(TEXT("fittedFrom"), FittedFrom);
        // Counted off the recipe that is actually being returned, not off the one before the trim.
        Result->SetNumberField(TEXT("layers"), Trimmed.Layers.Num());

        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}

// =================================================================================================
// audio.analysis.audit_folder
// =================================================================================================

REGISTER_RPC_HANDLER("audio.analysis.audit_folder", "audio.analysis",
    "Sweep a content folder of USoundWaves in ONE call and flag technical defects: non-finite "
    "samples, digital silence, clipping, DC offset past the published threshold, integrated-loudness "
    "outliers against the page median, and sample-rate / channel-count outliers against the page's "
    "most common format. Read-only - nothing is loaded for writing and nothing is saved. Returns a "
    "JOB TICKET (each asset costs a package load plus a blocking bulk-data read) and cannot be "
    "cancelled. An empty match set is an ERROR, not a zero-item success: a typo in a folder path "
    "otherwise looks exactly like a clean sweep. The response carries counts, a diagnosis histogram "
    "and the format distribution for the WHOLE page, plus the first few flagged assets by name; page "
    "with a small `limit` to enumerate every flagged asset, or a large one to survey a big folder "
    "from the counts.",
    RPC_PARAMS(
        RPC_PARAM_REQ("folder", "path",
            "Content folder to sweep, e.g. /Game/Audio. No default - a folder guessed for the "
            "caller would audit assets they did not ask about."),
        RPC_PARAM_DEF("recursive", "boolean", "Include sub-folders.", "true"),
        RPC_PARAM_DEF("limit", "integer",
            "Assets this call loads and decodes (1-200). Also bounds the work, since every audited "
            "asset costs a blocking payload read.", "50"),
        RPC_PARAM_DEF("offset", "integer",
            "Assets to skip before the page. Ordering is by asset path, so paging is stable and a "
            "retry converges.", "0")
    ))
{
    using namespace PwAudioAnalysisHandlerInternal;

    FString RawFolder;
    if (!Ctx.RequireString(TEXT("folder"), RawFolder)) return true;
    RawFolder.TrimStartAndEndInline();

    if (LooksLikeFilesystemPath(RawFolder))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(
                TEXT("'folder' is '%s', which is a filesystem path. This verb sweeps mounted ")
                TEXT("content, so pass a package path such as /Game/Audio; import loose files as ")
                TEXT("SoundWaves first."), *Truncate(RawFolder, MaxErrorChars)));
        return true;
    }

    FString Folder = SanitizeProjectRelativePath(RawFolder);
    while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/")))
    {
        Folder.LeftChopInline(1);
    }
    if (Folder.IsEmpty() || !IsValidAssetPath(Folder) || Folder.Contains(TEXT(".")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(
                TEXT("'folder' is '%s', which is not a content folder path. Expected a mounted ")
                TEXT("package path with no object suffix, e.g. /Game/Audio."),
                *Truncate(RawFolder, MaxErrorChars)));
        return true;
    }

    const bool bRecursive = Ctx.GetBool(TEXT("recursive"), true);
    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), DefaultAuditLimit), 1, MaxAuditLimit);
    const int32 Offset = FMath::Max(0, Ctx.GetInt(TEXT("offset"), 0));

    FAssetRegistryModule& RegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = RegistryModule.Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(USoundWave::StaticClass()->GetClassPathName());
    // Recursive classes so a USoundWave SUBCLASS is enumerated rather than silently skipped: a
    // skipped asset is invisible, whereas one the decoder rejects is reported with its own code.
    Filter.bRecursiveClasses = true;
    Filter.bRecursivePaths = bRecursive;
    Filter.PackagePaths.Add(FName(*Folder));

    TArray<FAssetData> Matched;
    AssetRegistry.GetAssets(Filter, Matched);
    // Sorted by object path so `offset` addresses the same asset on every call - the property
    // that makes paging stable and a retried page converge (§8).
    Matched.Sort([](const FAssetData& A, const FAssetData& B)
    {
        // FString carries no operator<, so the ordering goes through Compare rather than through
        // a relational operator that does not exist.
        return A.GetObjectPathString().Compare(B.GetObjectPathString(),
                                               ESearchCase::CaseSensitive) < 0;
    });

    const bool bScanning = AssetRegistry.IsLoadingAssets();
    const int32 Total = Matched.Num();

    if (Total == 0)
    {
        // Zero is not a small number (§7): a folder that matched nothing and a folder full of
        // clean audio must not produce the same-shaped answer.
        Ctx.SendError(ErrorCodes::ERR_NO_ASSETS_MATCHED,
            FString::Printf(
                TEXT("No USoundWave assets under '%s'%s. Check the folder path and 'recursive'%s."),
                *Folder,
                bRecursive ? TEXT(" (recursive)") : TEXT(" (this folder only)"),
                bScanning
                    ? TEXT("; the asset registry is still scanning, so the answer may be incomplete "
                           "until the scan finishes")
                    : TEXT("")));
        return true;
    }

    if (Offset >= Total)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(
                TEXT("'offset' is %d but '%s' holds only %d SoundWave(s), so the page would audit ")
                TEXT("nothing. An empty page reported as a clean sweep is the one answer this verb ")
                TEXT("must not give; pass an offset below %d."), Offset, *Folder, Total, Total));
        return true;
    }

    const int32 PageSize = FMath::Min(Limit, Total - Offset);
    TArray<FAssetData> Page;
    Page.Reserve(PageSize);
    for (int32 Index = 0; Index < PageSize; ++Index)
    {
        Page.Add(Matched[Offset + Index]);
    }

    TSharedPtr<FJsonObject> Started = MakeStartedPayload(TEXT("Auditing"));
    Started->SetStringField(TEXT("folder"), Folder);
    Started->SetNumberField(TEXT("total"), Total);
    Started->SetNumberField(TEXT("examining"), PageSize);

    FJobBindArgs Args;
    Args.Method = TEXT("audio.analysis.audit_folder");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate =
        [Page = MoveTemp(Page), Folder, bRecursive, bScanning, Total, Offset, Limit]
        (FJobOnComplete OnComplete)
    {
        // ---- pass 1: per-asset measurements and the context-free checks --------------------
        TArray<FAuditRow> Rows;
        Rows.Reserve(Page.Num());
        TArray<TSharedPtr<FJsonValue>> NotAudited;
        int32 NotAuditedTotal = 0;

        for (const FAssetData& Data : Page)
        {
            FAuditRow Row;
            FString Code;
            if (!AuditOneAsset(Data, Row, Code))
            {
                ++NotAuditedTotal;
                if (NotAudited.Num() < MaxReportedNotAudited)
                {
                    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("assetPath"), Row.AssetPath);
                    Entry->SetStringField(TEXT("errorCode"), Code);
                    NotAudited.Add(MakeShared<FJsonValueObject>(Entry));
                }
                continue;
            }
            Rows.Add(MoveTemp(Row));
        }

        // ---- pass 2: the checks that need the page as context ------------------------------
        // Format and loudness outliers are relative to the page, so they cannot be decided until
        // every asset in it has been measured.
        TArray<int32> SampleRates;
        TArray<int32> ChannelCounts;
        TArray<double> LoudnessValues;
        int32 LevelChecked = 0;
        for (const FAuditRow& Row : Rows)
        {
            if (Row.bSilent)
            {
                continue;
            }
            ++LevelChecked;
            if (Row.SampleRate > 0)
            {
                SampleRates.Add(Row.SampleRate);
            }
            if (Row.Channels > 0)
            {
                ChannelCounts.Add(Row.Channels);
            }
            if (Row.bLoudnessMeasured)
            {
                LoudnessValues.Add(Row.IntegratedLufs);
            }
        }

        int32 ModalSampleRate = 0;
        int32 DistinctSampleRates = 0;
        const bool bHaveModalRate = ModalValue(SampleRates, ModalSampleRate, DistinctSampleRates);

        int32 ModalChannels = 0;
        int32 DistinctChannels = 0;
        const bool bHaveModalChannels = ModalValue(ChannelCounts, ModalChannels, DistinctChannels);

        double MedianLufs = 0.0;
        const bool bHaveMedianLufs = MedianValue(LoudnessValues, MedianLufs);

        for (FAuditRow& Row : Rows)
        {
            if (Row.bSilent)
            {
                continue;
            }
            if (bHaveModalRate && Row.SampleRate > 0 && Row.SampleRate != ModalSampleRate)
            {
                Row.Findings.Add(MakeFinding(AuditFinding::SampleRateOutlier,
                                             static_cast<double>(Row.SampleRate), 0));
            }
            if (bHaveModalChannels && Row.Channels > 0 && Row.Channels != ModalChannels)
            {
                Row.Findings.Add(MakeFinding(AuditFinding::ChannelOutlier,
                                             static_cast<double>(Row.Channels), 0));
            }
            // An asset whose loudness family was not measured is neither an outlier nor proven
            // fine; `loudness.measured` publishes that coverage so a zero outlier count cannot be
            // read as "every asset was checked" (§1).
            if (bHaveMedianLufs && Row.bLoudnessMeasured
                && FMath::Abs(Row.IntegratedLufs - MedianLufs) > LoudnessOutlierLu)
            {
                Row.Findings.Add(MakeFinding(AuditFinding::LoudnessOutlier, Row.IntegratedLufs, 2));
            }
        }

        // ---- assemble ----------------------------------------------------------------------
        TMap<FString, int32> FindingCounts;
        int32 Flagged = 0;
        TArray<TSharedPtr<FJsonValue>> FlaggedRows;
        for (const FAuditRow& Row : Rows)
        {
            if (Row.Findings.Num() == 0)
            {
                continue;
            }
            ++Flagged;
            for (const TSharedPtr<FJsonObject>& Finding : Row.Findings)
            {
                ++FindingCounts.FindOrAdd(Finding->GetStringField(TEXT("check")));
            }
            if (FlaggedRows.Num() < MaxReportedAuditAssets)
            {
                TArray<TSharedPtr<FJsonValue>> Findings;
                Findings.Reserve(Row.Findings.Num());
                for (const TSharedPtr<FJsonObject>& Finding : Row.Findings)
                {
                    Findings.Add(MakeShared<FJsonValueObject>(Finding));
                }
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("assetPath"), Row.AssetPath);
                Entry->SetArrayField(TEXT("findings"), Findings);
                FlaggedRows.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("folder"), Folder);
        Result->SetBoolField(TEXT("recursive"), bRecursive);
        Result->SetNumberField(TEXT("total"), Total);
        Result->SetNumberField(TEXT("offset"), Offset);
        Result->SetNumberField(TEXT("limit"), Limit);
        Result->SetNumberField(TEXT("examined"), Page.Num());
        Result->SetNumberField(TEXT("audited"), Rows.Num());
        Result->SetNumberField(TEXT("notAudited"), NotAuditedTotal);
        Result->SetNumberField(TEXT("flagged"), Flagged);
        Result->SetNumberField(TEXT("clean"), Rows.Num() - Flagged);
        // Two denominators, because the checks do not all cover the same set: a digitally silent
        // asset never reaches the level checks, and a sub-500 ms asset has no integrated loudness.
        Result->SetNumberField(TEXT("levelChecked"), LevelChecked);
        if (bScanning)
        {
            // Reported, not repaired: a total measured mid-scan can grow, and a caller reading
            // "0 flagged of 12" must be able to tell that from "0 flagged of 412".
            Result->SetBoolField(TEXT("registryScanInProgress"), true);
        }
        if (Offset + Page.Num() < Total)
        {
            Result->SetNumberField(TEXT("nextOffset"), Offset + Page.Num());
        }

        // Every token, including the zeros: a zero here is the measurement "this check ran over
        // the page and found nothing", which an omitted key could not say.
        TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
        for (const TCHAR* Token : AuditFindingVocabulary())
        {
            const int32* Found = FindingCounts.Find(FString(Token));
            Counts->SetNumberField(Token, Found ? *Found : 0);
        }
        Result->SetObjectField(TEXT("findingCounts"), Counts);

        TSharedPtr<FJsonObject> Format = MakeShared<FJsonObject>();
        if (bHaveModalRate)
        {
            Format->SetNumberField(TEXT("modalSampleRate"), ModalSampleRate);
            Format->SetNumberField(TEXT("distinctSampleRates"), DistinctSampleRates);
        }
        if (bHaveModalChannels)
        {
            Format->SetNumberField(TEXT("modalChannels"), ModalChannels);
            Format->SetNumberField(TEXT("distinctChannelCounts"), DistinctChannels);
        }
        Result->SetObjectField(TEXT("format"), Format);

        TSharedPtr<FJsonObject> Loudness = MakeShared<FJsonObject>();
        Loudness->SetNumberField(TEXT("measured"), LoudnessValues.Num());
        Loudness->SetNumberField(TEXT("outlierToleranceLu"), LoudnessOutlierLu);
        if (bHaveMedianLufs)
        {
            SetRounded(Loudness, TEXT("medianLufs"), MedianLufs, 2);
        }
        Result->SetObjectField(TEXT("loudness"), Loudness);

        // Published once rather than repeated on every finding: the floors are what make two
        // sweeps comparable, and repeating them per row would cost the response its budget.
        TSharedPtr<FJsonObject> Thresholds = MakeShared<FJsonObject>();
        Thresholds->SetNumberField(TEXT("clippedSamples"), 0);
        Thresholds->SetNumberField(TEXT("dcOffsetLinear"), DcOffsetThreshold);
        Thresholds->SetNumberField(TEXT("loudnessOutlierLu"), LoudnessOutlierLu);
        Thresholds->SetNumberField(TEXT("silencePeakLinear"), PwAudioAnalysisLimits::SilencePeak);
        Result->SetObjectField(TEXT("thresholds"), Thresholds);

        if (FlaggedRows.Num() > 0)
        {
            Result->SetArrayField(TEXT("assets"), FlaggedRows);
        }
        if (NotAudited.Num() > 0)
        {
            Result->SetArrayField(TEXT("notAuditedAssets"), NotAudited);
        }

        TSharedPtr<FJsonObject> Truncated = MakeShared<FJsonObject>();
        AddTruncation(Truncated, TEXT("assets"), FlaggedRows.Num(), Flagged);
        AddTruncation(Truncated, TEXT("notAuditedAssets"), NotAudited.Num(), NotAuditedTotal);
        if (Truncated->Values.Num() > 0)
        {
            Result->SetObjectField(TEXT("truncated"), Truncated);
        }

        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}
