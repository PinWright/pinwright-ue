// Copyright (c) 2026 Alexander Penkin. MIT License.

// Movie Render Queue (MRQ) handlers: queue, inspect, select, remove and clear jobs, run the queue
// (long-running), and enumerate primary-config presets. Gated on __has_include so the plugin still
// links when the engine's MovieRenderPipeline plugin is disabled in a target.
#include "CoreMinimal.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/MRQ/MRQArtifactReport.h"
#include "Handlers/MRQ/MRQHandlerTestHooks.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "LevelSequence.h"
#include "Utils/AssetUtils.h"
#include "Engine/World.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if __has_include("MoviePipelineQueueSubsystem.h") && \
    __has_include("MoviePipelinePIEExecutor.h") && \
    __has_include("MoviePipelineQueue.h") && \
    __has_include("MoviePipelinePrimaryConfig.h")
    #include "MoviePipelineQueueSubsystem.h"
    #include "MoviePipelinePIEExecutor.h"
    #include "MoviePipelineQueue.h"
    #include "MoviePipelinePrimaryConfig.h"
    #include "MoviePipelineExecutor.h"
    // UMoviePipeline::GetCurrentJob, so an executor error can name the job it happened on rather
    // than arriving as a bare string with no owner.
    #include "MoviePipeline.h"
    // Artifact readback for mrq.run_jobs: the output data the pipeline publishes, the output
    // setting that carries the resolution, and the video output base every encoder derives from.
    #include "MovieRenderPipelineDataTypes.h"
    #include "MoviePipelineOutputSetting.h"
    #include "MoviePipelineVideoOutputBase.h"
    // Pre-flight disclosure for mrq.create_job needs the base every file writer derives from, so
    // a config that writes an image sequence is disclosed as plainly as one that writes a movie.
    #include "MoviePipelineOutputBase.h"
    #define MCP_HAS_MRQ 1
#else
    #define MCP_HAS_MRQ 0
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMRQHandler, Log, All);

#if MCP_HAS_MRQ
namespace
{
    bool ValidateMRQAssetPath(const FString& RequestedPath, const TCHAR* FieldName,
        FString& OutError)
    {
        const FString Path = RequestedPath.TrimStartAndEnd();
        const FString PackagePath = FPackageName::ObjectPathToPackageName(Path);
        FText ValidationReason;
        const bool bValidObjectPath = !Path.IsEmpty() &&
            FPackageName::IsValidObjectPath(Path, &ValidationReason);
        const bool bMounted = !PackagePath.IsEmpty() &&
            !FPackageName::GetPackageMountPoint(PackagePath).IsNone();
        const bool bValidLongPackageName = !PackagePath.IsEmpty() &&
            FPackageName::IsValidLongPackageName(PackagePath, true, &ValidationReason);
        if (!bValidObjectPath || !bMounted || !bValidLongPackageName)
        {
            OutError = FString::Printf(
                TEXT("%s '%s' must be a valid long package/object path on a mounted root"),
                FieldName, *Path);
            return false;
        }
        return true;
    }

    bool HasMRQTraversalComponent(const FString& Path)
    {
        int32 ComponentStart = 0;
        while (ComponentStart < Path.Len())
        {
            int32 ComponentEnd = ComponentStart;
            while (ComponentEnd < Path.Len() && Path[ComponentEnd] != TEXT('/') &&
                Path[ComponentEnd] != TEXT('\\'))
            {
                ++ComponentEnd;
            }
            if (Path.Mid(ComponentStart, ComponentEnd - ComponentStart).Equals(
                    TEXT(".."), ESearchCase::CaseSensitive))
            {
                return true;
            }
            ComponentStart = ComponentEnd + 1;
        }
        return false;
    }

    bool ValidateMRQOutputDirectory(const FString& RequestedPath, FString& OutError)
    {
        FString ExpandedPath = RequestedPath.TrimStartAndEnd();
        if (ExpandedPath.IsEmpty())
        {
            OutError = TEXT("MRQ output directory must not be empty");
            return false;
        }

        ExpandedPath.ReplaceInline(TEXT("{project_dir}"), *FPaths::ProjectDir(),
            ESearchCase::IgnoreCase);
        if (HasMRQTraversalComponent(ExpandedPath))
        {
            OutError = FString::Printf(TEXT("MRQ output directory '%s' may not contain '..'"),
                *RequestedPath);
            return false;
        }

        FString FullPath = FPaths::ConvertRelativePathToFull(ExpandedPath);
        FPaths::NormalizeDirectoryName(FullPath);
        if (FullPath.IsEmpty())
        {
            OutError = FString::Printf(TEXT("MRQ output directory '%s' is not a valid path"),
                *RequestedPath);
            return false;
        }

        IFileManager& FileManager = IFileManager::Get();
        if (FileManager.FileExists(*FullPath))
        {
            OutError = FString::Printf(TEXT("MRQ output directory '%s' names a file"),
                *RequestedPath);
            return false;
        }

        FString ProbeDirectory = FullPath;
        while (!FileManager.DirectoryExists(*ProbeDirectory))
        {
            if (FileManager.FileExists(*ProbeDirectory))
            {
                OutError = FString::Printf(
                    TEXT("MRQ output directory '%s' has a file-valued ancestor"),
                    *RequestedPath);
                return false;
            }

            const FString ParentDirectory = FPaths::GetPath(ProbeDirectory);
            if (ParentDirectory.IsEmpty() ||
                ParentDirectory.Equals(ProbeDirectory, ESearchCase::IgnoreCase))
            {
                break;
            }
            ProbeDirectory = ParentDirectory;
        }
        if (!FileManager.DirectoryExists(*ProbeDirectory))
        {
            OutError = FString::Printf(
                TEXT("MRQ output directory '%s' has no existing writable ancestor"),
                *RequestedPath);
            return false;
        }

        const FString ProbeFilename = FPaths::Combine(ProbeDirectory, FString::Printf(
            TEXT(".PinWrightMRQProbe_%s.tmp"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        FArchive* Probe = FileManager.CreateFileWriter(*ProbeFilename);
        if (!Probe)
        {
            OutError = FString::Printf(TEXT("MRQ output directory '%s' is not writable"),
                *RequestedPath);
            return false;
        }
        delete Probe;
        if (!FileManager.Delete(*ProbeFilename, true, false, true))
        {
            OutError = FString::Printf(
                TEXT("MRQ output directory '%s' could not remove its write probe"),
                *RequestedPath);
            return false;
        }
        return true;
    }

    // Turn one finished job's output data into the artifact block mrq.run_jobs publishes.
    //
    // This is the extraction half; the measuring half is PinWrightMRQ::BuildArtifactReport, which
    // stats every path rather than assuming it is there. Everything gathered here is either a
    // path the pipeline says it wrote, or a value the pipeline cached while rendering — nothing
    // is echoed from what the caller asked for. See Handlers/MRQ/MRQArtifactReport.h for why the
    // paths are safe to stat at this point in the render's lifetime.
    TSharedPtr<FJsonObject> MakeJobArtifactReport(const FMoviePipelineOutputData& OutputData)
    {
        TArray<PinWrightMRQ::FRenderedFile> Files;
        PinWrightMRQ::FEncodeContext Context;

        int32 TotalFrames = 0;
        bool bAnyFrameCount = false;
        // Per-shot pipeline STATE, alongside the timing. A shot that is still Rendering or
        // CoolingDown when its work-finished callback fires did not produce every frame it was
        // asked for, and today that reads as an unqualified success on a short render
        // (B-mrq-run-jobs-succeeds-on-unrenderable-frames). Reported rather than judged: the
        // enum is published verbatim and only a non-Finished state raises a warning.
        TArray<TSharedPtr<FJsonValue>> ShotEntries;
        int32 UnfinishedShots = 0;
        // WorkMetrics/CachedFrameRate are what the pipeline actually rendered at — the shot
        // caches them during initialization — rather than what the sequence asset declares.
        // TotalOutputFrameCount is -1 until CalculateWorkMetrics has run, so a non-positive
        // value leaves the frame count unset rather than contributing a zero.
        auto AccumulateShotTiming = [&TotalFrames, &bAnyFrameCount, &Context, &ShotEntries,
            &UnfinishedShots](const UMoviePipelineExecutorShot* Shot)
        {
            if (!Shot)
            {
                return;
            }
            if (Shot->ShotInfo.WorkMetrics.TotalOutputFrameCount > 0)
            {
                TotalFrames += Shot->ShotInfo.WorkMetrics.TotalOutputFrameCount;
                bAnyFrameCount = true;
            }
            if (!Context.FrameRate.IsSet() && Shot->ShotInfo.CachedFrameRate.IsValid())
            {
                Context.FrameRate = Shot->ShotInfo.CachedFrameRate.AsDecimal();
            }

            TSharedPtr<FJsonObject> ShotEntry = MakeShared<FJsonObject>();
            const FString ShotName = Shot->InnerName.IsEmpty() ? Shot->OuterName : Shot->InnerName;
            if (!ShotName.IsEmpty())
            {
                ShotEntry->SetStringField(TEXT("name"), ShotName);
            }
            const UEnum* StateEnum = StaticEnum<EMovieRenderShotState>();
            const FString StateName = StateEnum
                ? StateEnum->GetNameStringByValue(static_cast<int64>(Shot->ShotInfo.State))
                : FString();
            if (!StateName.IsEmpty())
            {
                ShotEntry->SetStringField(TEXT("state"), StateName);
            }
            if (Shot->ShotInfo.State != EMovieRenderShotState::Finished)
            {
                ++UnfinishedShots;
            }
            ShotEntries.Add(MakeShared<FJsonValueObject>(ShotEntry));
        };

        for (const FMoviePipelineShotOutputData& ShotData : OutputData.ShotData)
        {
            AccumulateShotTiming(ShotData.Shot.Get());
            for (const TPair<FMoviePipelinePassIdentifier, FMoviePipelineRenderPassOutputData>& Pass
                : ShotData.RenderPassData)
            {
                for (const FString& Path : Pass.Value.FilePaths)
                {
                    Files.Add(PinWrightMRQ::FRenderedFile{ Path, Pass.Key.Name });
                }
            }
        }
        // Movie Render Graph jobs publish the same files through GraphData instead; ShotData is
        // empty for them, so reading only one of the two would report a graph render as having
        // written nothing. FMoviePipelineOutputData::GraphData arrived in UE 5.4 — on 5.3 the
        // struct carries ShotData alone, so a graph job has nothing to read here.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        for (const FMovieGraphRenderOutputData& GraphData : OutputData.GraphData)
        {
            AccumulateShotTiming(GraphData.Shot.Get());
            for (const TPair<FMovieGraphRenderDataIdentifier, FMovieGraphRenderLayerOutputData>& Layer
                : GraphData.RenderLayerData)
            {
                for (const FString& Path : Layer.Value.FilePaths)
                {
                    Files.Add(PinWrightMRQ::FRenderedFile{ Path, Layer.Key.LayerName });
                }
            }
        }
#endif

        if (bAnyFrameCount)
        {
            Context.FrameCount = TotalFrames;
        }

        if (const UMoviePipelineExecutorJob* Job = OutputData.Job.Get())
        {
            if (const UMoviePipelinePrimaryConfig* Config = Job->GetConfiguration())
            {
                if (const UMoviePipelineOutputSetting* OutputSetting =
                    Config->FindSetting<UMoviePipelineOutputSetting>())
                {
                    Context.Width = OutputSetting->OutputResolution.X;
                    Context.Height = OutputSetting->OutputResolution.Y;
                }
                // The resolved video output, whichever encoder class the preset carries — the
                // encoder settings are read off it rather than accepted from the caller, since
                // mrq.create_job takes no encoder params at all.
                const TArray<UMoviePipelineSetting*> VideoOutputs =
                    Config->FindSettingsByClass(UMoviePipelineVideoOutputBase::StaticClass());
                if (VideoOutputs.Num() > 0)
                {
                    Context.RequestedEncoder =
                        PinWrightMRQ::ReadRequestedEncoderSettings(VideoOutputs[0]);
                }
            }
        }

        TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
        if (const UMoviePipelineExecutorJob* NamedJob = OutputData.Job.Get())
        {
            Report->SetStringField(TEXT("jobName"), NamedJob->JobName);
        }
        // NAMED, NOT A VERDICT ON THE PICTURE. `bSuccess` is the pipeline's own answer to "did I
        // run to completion and write my files"; it says nothing about what is in them, which is
        // what `framesSuspect` and the per-frame `suspect` flags on `outputFiles[]` are for.
        Report->SetBoolField(TEXT("jobSucceeded"), OutputData.bSuccess);
        if (ShotEntries.Num() > 0)
        {
            Report->SetArrayField(TEXT("shots"), ShotEntries);
        }
        if (UnfinishedShots > 0)
        {
            // Appended to whatever BuildArtifactReport already found, so the caller reads one
            // warnings array per job rather than two.
            TArray<TSharedPtr<FJsonValue>> Warnings;
            const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
            if (Report->TryGetArrayField(TEXT("warnings"), Existing) && Existing)
            {
                Warnings = *Existing;
            }
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("%d of %d shots were not in the Finished state when this job reported its ")
                TEXT("work as complete (see `shots[].state`). A shot still in WarmingUp, ")
                TEXT("MotionBlur, Rendering or CoolingDown did not produce every frame it was ")
                TEXT("asked for, so the frame count, the file list and `jobSucceeded` above all ")
                TEXT("describe a partial render."),
                UnfinishedShots, ShotEntries.Num())));
            Report->SetArrayField(TEXT("warnings"), Warnings);
        }
        return Report;
    }

    bool HasMRQRenderedOutput(const TArray<TSharedPtr<FJsonValue>>& JobReports)
    {
        for (const TSharedPtr<FJsonValue>& Value : JobReports)
        {
            const TSharedPtr<FJsonObject>* Report = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Report) && Report && (*Report).IsValid() &&
                (*Report)->GetNumberField(TEXT("measuredFileCount")) > 0)
            {
                return true;
            }
        }
        return false;
    }

    bool HasMRQUnrenderableJob(const TArray<TSharedPtr<FJsonValue>>& JobReports)
    {
        for (const TSharedPtr<FJsonValue>& Value : JobReports)
        {
            const TSharedPtr<FJsonObject>* Report = nullptr;
            bool bAllAnalyzedFramesUnrenderable = false;
            if (Value.IsValid() && Value->TryGetObject(Report) && Report && (*Report).IsValid()
                && (*Report)->TryGetBoolField(
                    TEXT("allAnalyzedFramesUnrenderable"), bAllAnalyzedFramesUnrenderable)
                && bAllAnalyzedFramesUnrenderable)
            {
                return true;
            }
        }
        return false;
    }

    // Read the queued job's RESOLVED configuration into the pre-flight context mrq.create_job
    // discloses. Everything here comes off the config the job now carries — after
    // SetConfiguration copied the preset into it — never off the request, so the response stops
    // depending on `presetPath` being a true claim about what will render.
    //
    // This is the extraction half; PinWrightMRQ::BuildPreflightReport is the reporting half, and
    // the encoder read-back is the same PinWrightMRQ::ReadRequestedEncoderSettings that
    // mrq.run_jobs uses on the finished render.
    PinWrightMRQ::FPreflightContext ReadPreflightContext(const UMoviePipelineExecutorJob* Job)
    {
        PinWrightMRQ::FPreflightContext Context;
        const UMoviePipelinePrimaryConfig* Config = Job ? Job->GetConfiguration() : nullptr;
        if (!Config)
        {
            return Context;
        }

        // Always present: UMoviePipelinePrimaryConfig owns an OutputSetting subobject and
        // GetUserSettings() appends it, so resolution and the path shape are disclosable even for
        // a job queued with no preset at all.
        if (const UMoviePipelineOutputSetting* OutputSetting =
            Config->FindSetting<UMoviePipelineOutputSetting>())
        {
            Context.Width = OutputSetting->OutputResolution.X;
            Context.Height = OutputSetting->OutputResolution.Y;
            Context.OutputDirectory = OutputSetting->OutputDirectory.Path;
            Context.FileNameFormat = OutputSetting->FileNameFormat;
            // Only when the config overrides it. Without the override the rate comes from the
            // sequence, which is not loaded here — so nothing is published rather than a guess.
            if (OutputSetting->bUseCustomFrameRate && OutputSetting->OutputFrameRate.IsValid())
            {
                Context.FrameRateOverride = OutputSetting->OutputFrameRate.AsDecimal();
            }
        }

        // Enabled file writers only: FindSettingsByClass drops disabled settings, and a disabled
        // output writes nothing, so listing it would overstate what this job produces.
        for (const UMoviePipelineSetting* Output
            : Config->FindSettingsByClass(UMoviePipelineOutputBase::StaticClass()))
        {
            if (Output)
            {
                Context.OutputClassPaths.Add(Output->GetClass()->GetPathName());
            }
        }

        const TArray<UMoviePipelineSetting*> VideoOutputs =
            Config->FindSettingsByClass(UMoviePipelineVideoOutputBase::StaticClass());
        if (VideoOutputs.Num() > 0)
        {
            Context.RequestedEncoder = PinWrightMRQ::ReadRequestedEncoderSettings(VideoOutputs[0]);
        }
        return Context;
    }

    TSharedPtr<FJsonObject> MakeQueuedJobDisclosure(int32 Index,
        const UMoviePipelineExecutorJob* Job)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), Index);
        Entry->SetStringField(TEXT("jobName"), Job ? Job->JobName : FString());
        Entry->SetStringField(TEXT("sequencePath"), Job ? Job->Sequence.ToString() : FString());
        return Entry;
    }

    // One queue entry in the list disclosure. The aliases are deliberate: create_job historically
    // calls the map field `levelPath`, while the queue API and list contract call it a map. Keeping
    // both lets a caller correlate a list entry with the create response without making either
    // spelling ambiguous.
    TSharedPtr<FJsonObject> MakeQueueJobEntry(int32 Index, const UMoviePipelineExecutorJob* Job)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), Index);
        Entry->SetStringField(TEXT("jobName"), Job ? Job->JobName : FString());
        Entry->SetBoolField(TEXT("enabled"), Job && Job->IsEnabled());

        const FString SequencePath = Job ? Job->Sequence.ToString() : FString();
        const FString MapPath = Job ? Job->Map.ToString() : FString();
        Entry->SetStringField(TEXT("sequencePath"), SequencePath);
        Entry->SetStringField(TEXT("mapPath"), MapPath);
        Entry->SetStringField(TEXT("levelPath"), MapPath);

        const UMoviePipelinePrimaryConfig* PresetOrigin = Job ? Job->GetPresetOrigin() : nullptr;
        const UMoviePipelinePrimaryConfig* Configuration = Job ? Job->GetConfiguration() : nullptr;
        Entry->SetStringField(TEXT("presetPath"),
            PresetOrigin ? PresetOrigin->GetPathName() : FString());
        Entry->SetStringField(TEXT("configurationPath"),
            Configuration ? Configuration->GetPathName() : FString());

        const PinWrightMRQ::FPreflightContext PreflightContext = ReadPreflightContext(Job);
        TArray<FString> UnusedWarnings;
        Entry->SetObjectField(TEXT("preflight"),
            PinWrightMRQ::BuildPreflightReport(PreflightContext, UnusedWarnings));
        Entry->SetStringField(TEXT("outputDirectory"), PreflightContext.OutputDirectory);
        Entry->SetStringField(TEXT("fileNameFormat"), PreflightContext.FileNameFormat);
        return Entry;
    }

    struct FScopedJobEnabledState
    {
        TWeakObjectPtr<UMoviePipelineExecutorJob> Job;
        bool bWasEnabled = false;
    };

    void RestoreScopedJobEnabledStates(
        const TSharedRef<TArray<FScopedJobEnabledState>>& States,
        const TSharedRef<bool>& bRestored)
    {
        if (*bRestored)
        {
            return;
        }
        *bRestored = true;
        for (const FScopedJobEnabledState& State : *States)
        {
            if (State.Job.IsValid())
            {
                State.Job->SetIsEnabled(State.bWasEnabled);
            }
        }
    }
}
#endif // MCP_HAS_MRQ

// ---- mrq.create_job ----
REGISTER_RPC_HANDLER("mrq.create_job", "mrq",
    "Queue a MoviePipeline render job (sequence + level + optional preset). Caller subsequently "
    "invokes mrq.run_jobs to start the executor. A `presetPath` that will not load is REFUSED "
    "with [MRQ_PRESET_NOT_LOADABLE] and queues nothing — it used to queue an unconfigured job and "
    "still echo the preset, so the render silently ran on engine defaults. The response carries a "
    "`preflight` block read off the configuration the queued job actually holds — `resolution`, "
    "`outputDirectory` and `fileNameFormat` (format strings the pipeline expands at render time, "
    "not resolved paths), `frameRateOverride` when the config overrides the sequence rate, "
    "`outputs` (the class of every enabled file writer; empty means this job writes nothing) and "
    "`encoderRequested` when a video output is configured. `warnings` (present only when "
    "non-empty) names a job that will write no files and a rate control that is unbounded below "
    "(`Quality`/`ConstantQP`), so the encode can be fixed before the render is spent rather than "
    "after.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Level sequence asset path to render"),
        RPC_PARAM_REQ("levelPath", "path", "Map asset path to load"),
        RPC_PARAM_OPT("presetPath", "path", "UMoviePipelinePrimaryConfig asset path (preset)"),
        RPC_PARAM_OPT("jobName", "string", "Display name for the job entry")
    ))
{
#if MCP_HAS_MRQ
    FString SequencePath;
    if (!Ctx.RequireString(TEXT("sequencePath"), SequencePath)) { return true; }
    FString LevelPath;
    if (!Ctx.RequireString(TEXT("levelPath"), LevelPath)) { return true; }
    FString PresetPath = Ctx.GetString(TEXT("presetPath"));
    const FString JobName    = Ctx.GetString(TEXT("jobName"));

    FString PathError;
    if (!ValidateMRQAssetPath(SequencePath, TEXT("sequencePath"), PathError) ||
        !ValidateMRQAssetPath(LevelPath, TEXT("levelPath"), PathError) ||
        (!PresetPath.IsEmpty() &&
            !ValidateMRQAssetPath(PresetPath, TEXT("presetPath"), PathError)))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
        return true;
    }
    SequencePath.TrimStartAndEndInline();
    LevelPath.TrimStartAndEndInline();
    PresetPath.TrimStartAndEndInline();

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("GEditor not available"));
        return true;
    }
    UMoviePipelineQueueSubsystem* QSS = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
    if (!QSS)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_SUBSYSTEM_UNAVAILABLE,
            TEXT("UMoviePipelineQueueSubsystem unavailable"));
        return true;
    }
    UMoviePipelineQueue* Queue = QSS->GetQueue();
    if (!Queue)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_QUEUE_NULL, TEXT("MoviePipeline queue is null"));
        return true;
    }
    if (QSS->IsRendering())
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_RENDER_IN_PROGRESS,
            TEXT("MoviePipeline queue is already being rendered; wait for the active executor "
                 "to finish before appending a job"));
        return true;
    }

    // Resolved BEFORE the job is allocated, so a preset that will not load leaves the queue
    // exactly as it was. This used to be a soft failure: the job was queued with no
    // configuration, a UE_LOG warning went to the editor log that no caller ever sees, and the
    // response below echoed `presetPath` byte-identically to the success case — so a caller who
    // typo'd a path was told their preset applied and then spent minutes rendering at the
    // engine's CDO defaults (Quality/CRF 20) into a directory they did not choose
    // (B-mrq-render-result-omits-bitrate-and-size).
    //
    // Refused rather than warned, for three reasons a reviewer can check. (1) The same handler
    // already hard-errors CLASS_NOT_FOUND for an unloadable `executorClass` in mrq.run_jobs;
    // treating an unloadable `presetPath` as a warning is internally inconsistent. (2) An
    // unconfigured job is a supported state ONLY when the caller asked for it by omitting
    // presetPath — docs/rpc-design.md §1: "an unresolvable target is an error, and the empty
    // result is reserved for a question that was actually asked". (3) The cost is asymmetric and
    // one-directional: refusing costs a corrected path now, proceeding costs minutes of render
    // and a wrong deliverable that leaves the tool, and the misconfigured job would sit in the
    // shared queue as a trap for anyone's later mrq.run_jobs.
    UMoviePipelinePrimaryConfig* Preset = nullptr;
    if (!PresetPath.IsEmpty())
    {
        // LoadObject is sync — acceptable here because handlers run on the game thread
        // and clients explicitly opted into a preset asset they expect to exist.
        Preset = LoadObject<UMoviePipelinePrimaryConfig>(nullptr, *PresetPath);
        if (!Preset)
        {
            UE_LOG(LogMRQHandler, Warning, TEXT("mrq.create_job: preset '%s' not loadable; nothing queued"), *PresetPath);
            Ctx.SendError(ErrorCodes::ERR_MRQ_PRESET_NOT_LOADABLE, FString::Printf(
                TEXT("presetPath '%s' did not load as a UMoviePipelinePrimaryConfig, so NOTHING ")
                TEXT("was queued. The job was refused rather than queued unconfigured: it would ")
                TEXT("have rendered at the engine's CDO defaults into a directory you did not ")
                TEXT("choose, for minutes, while this response named your preset. Call ")
                TEXT("mrq.list_presets to see the preset asset paths that exist, or omit ")
                TEXT("presetPath to queue deliberately unconfigured."),
                *PresetPath));
            return true;
        }
    }

    const UMoviePipelineOutputSetting* OutputSetting = Preset
        ? Preset->FindSetting<UMoviePipelineOutputSetting>()
        : UMoviePipelineOutputSetting::StaticClass()->GetDefaultObject<UMoviePipelineOutputSetting>();
    if (!OutputSetting ||
        !ValidateMRQOutputDirectory(OutputSetting->OutputDirectory.Path, PathError))
    {
        if (!OutputSetting)
        {
            PathError = TEXT("MRQ job has no output directory setting");
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
        return true;
    }

    const FResolvedAsset SequenceAsset = ResolveAsset(SequencePath, /*bLoadObject=*/true);
    if (!SequenceAsset.Object)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("sequencePath '%s' was not found"), *SequencePath));
        return true;
    }
    if (!SequenceAsset.Object->IsA(ULevelSequence::StaticClass()))
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_WRONG_TYPE, FString::Printf(
            TEXT("sequencePath '%s' resolved as '%s', expected ULevelSequence"),
            *SequencePath, *SequenceAsset.Object->GetClass()->GetName()));
        return true;
    }
    const ULevelSequence* Sequence = Cast<ULevelSequence>(SequenceAsset.Object);
    if (!Sequence->GetMovieScene())
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_INVALID,
            FString::Printf(TEXT("sequencePath '%s' has no MovieScene"), *SequencePath));
        return true;
    }

    const FResolvedAsset MapAsset = ResolveAsset(LevelPath, /*bLoadObject=*/true);
    if (!MapAsset.Object)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("levelPath '%s' was not found"), *LevelPath));
        return true;
    }
    if (!MapAsset.Object->IsA(UWorld::StaticClass()))
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_WRONG_TYPE, FString::Printf(
            TEXT("levelPath '%s' resolved as '%s', expected UWorld/map asset"),
            *LevelPath, *MapAsset.Object->GetClass()->GetName()));
        return true;
    }
    UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!Job)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_JOB_ALLOCATION_FAILED,
            TEXT("AllocateNewJob returned null"));
        return true;
    }

    Job->SetSequence(SequenceAsset.ObjectPath);
    Job->Map = MapAsset.ObjectPath;
    if (!JobName.IsEmpty())
    {
        Job->JobName = JobName;
    }
    if (Preset)
    {
        Job->SetConfiguration(Preset);
    }

    const TArray<UMoviePipelineExecutorJob*>& Jobs = Queue->GetJobs();
    const int32 JobIndex = Jobs.Num() - 1;
    const int32 QueueSize = Jobs.Num();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("jobIndex"), JobIndex);
    Resp->SetStringField(TEXT("sequencePath"), SequencePath);
    Resp->SetStringField(TEXT("levelPath"), LevelPath);
    Resp->SetStringField(TEXT("presetPath"), PresetPath);
    Resp->SetStringField(TEXT("jobName"), Job->JobName);
    Resp->SetNumberField(TEXT("queueSize"), QueueSize);

    TArray<TSharedPtr<FJsonValue>> QueuedJobs;
    QueuedJobs.Reserve(JobIndex);
    for (int32 Index = 0; Index < JobIndex; ++Index)
    {
        QueuedJobs.Add(MakeShared<FJsonValueObject>(MakeQueuedJobDisclosure(Index, Jobs[Index])));
    }
    Resp->SetArrayField(TEXT("queuedJobs"), QueuedJobs);

    // Pre-flight: what the queued job will actually produce, read off the configuration it now
    // carries. Published here because being wrong is cheapest before the render is spent — every
    // number that decides the encode is knowable at queue time, and mrq.run_jobs only gets to
    // report it once the deliverable already exists.
    TArray<FString> PreflightWarnings;
    Resp->SetObjectField(TEXT("preflight"),
        PinWrightMRQ::BuildPreflightReport(ReadPreflightContext(Job), PreflightWarnings));
    if (QueuedJobs.Num() > 0)
    {
        PreflightWarnings.Add(FString::Printf(
            TEXT("The shared MoviePipeline queue already contained %d job(s), which are listed in "
                 "`queuedJobs`; any entries that are still enabled may also be rendered by "
                 "`mrq.run_jobs` unless its `jobs` selection is supplied."),
            QueuedJobs.Num()));
    }
    if (PreflightWarnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarningValues;
        WarningValues.Reserve(PreflightWarnings.Num());
        for (const FString& Warning : PreflightWarnings)
        {
            WarningValues.Add(MakeShared<FJsonValueString>(Warning));
        }
        Resp->SetArrayField(TEXT("warnings"), WarningValues);
    }
    Ctx.SendSuccess(Resp);
#else
    Ctx.SendError(ErrorCodes::ERR_MRQ_NOT_AVAILABLE, TEXT("MovieRenderPipeline plugin not enabled in this build"));
#endif
    return true;
}

// ---- mrq.run_jobs (long-running) ----
REGISTER_RPC_HANDLER("mrq.run_jobs", "mrq",
    "Execute the current MoviePipeline queue using the requested executor (default: "
    "UMoviePipelinePIEExecutor). A supplied executorClass must resolve to a concrete "
    "UMoviePipelineExecutorBase subclass; invalid or abstract classes are refused before a job "
    "ticket is started. Long-running — returns a job ticket; poll via system.job_status. "
    "The terminal result carries a `jobs` array with the facts about each artifact: `outputFiles` "
    "(the paths the pipeline reported writing, each stat'd for `exists` and `fileSizeBytes`), "
    "`totalFileSizeBytes`, `frameCount`, `frameRate`, `durationSeconds`, `resolution`, "
    "`overallBitrateBps`, `bitsPerPixel` and `shots` (each shot's pipeline `state`). Size and "
    "bitrate are MEASURED off the finished files; the encoder read-back is published separately as "
    "`encoderRequested` because a quality-targeted encode places no lower bound on the bitrate it "
    "achieves. `jobSucceeded` means only THE PIPELINE RAN TO COMPLETION AND WROTE ITS FILES — it "
    "is not a verdict on the pixels. For that, a bounded sample of the written frames is DECODED "
    "and measured: each still-image entry of `outputFiles` carries `imageAnalyzed` and, when true, "
    "`imageStats` (luminance mean/variance/min/max, lit-pixel share, `toneLevelsUsed`, and the "
    "`flatRegionFraction` / `flatRegionBounds` / `flatRegionLevel` that catch a frame whose lower "
    "half is a low-variance void — the failure every file-size field is blind to), the `blank` / "
    "`crushed` / `blownOut` verdicts, per-channel mean/variance, and a `suspect` flag with "
    "`suspectReasons`. `framesAnalyzed`, `framesSuspect` and `framesUnrenderable` roll that up per "
    "job. Partial flat regions warn, but when every decoded sample for any job is one near-uniform "
    "colour the terminal ticket fails with RENDER_UNRENDERABLE_FRAMES. Pass "
    "allowUnrenderableFrames:true only when uniform output is intentional. Anything that could "
    "not be measured is OMITTED rather than "
    "reported as 0, and `warnings` names each omission, any encode below the 0.04 bits/pixel "
    "plausibility floor, any suspect frame (with where in the render the suspect frames fall), and "
    "any shot that was not Finished. `executorErrors` lists what the executor itself reported when "
    "that delegate is available. A failed executor completion or per-job `bSuccess:false` is "
    "terminal and reports `MRQ_EXECUTOR_FAILED`. `jobs` is absent entirely — with `artifactWarning` "
    "in its place — when a non-PIE executorClass was requested, since only the PIE executor publishes per-job "
    "output data. By default every enabled job in the shared queue runs; pass `jobs` as queue "
    "indices to render only those entries. Selection uses a transient copy containing only the "
    "requested jobs, preserving the shared queue and restoring temporary copy state when the "
    "executor finishes or fails to start.",
    RPC_PARAMS(
        RPC_PARAM_OPT("executorClass", "classref", "Concrete UMoviePipelineExecutorBase subclass path (default: UMoviePipelinePIEExecutor)"),
        RPC_PARAM_OPT("jobs", "array", "Queue indices to render; omit to render every enabled queue job"),
        RPC_PARAM_OPT("allowUnrenderableFrames", "bool", "Allow a near-uniform decoded sample to complete successfully (default false)")
    ))
{
#if MCP_HAS_MRQ
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("GEditor not available"));
        return true;
    }
    UMoviePipelineQueueSubsystem* QSS = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
    if (!QSS)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_SUBSYSTEM_UNAVAILABLE, TEXT("UMoviePipelineQueueSubsystem unavailable"));
        return true;
    }
    UMoviePipelineQueue* Queue = QSS->GetQueue();
    if (!Queue)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_QUEUE_NULL, TEXT("MoviePipeline queue is null"));
        return true;
    }
    const TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
    if (Jobs.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_QUEUE_EMPTY, TEXT("MoviePipeline queue has no jobs to run"));
        return true;
    }

    const bool bHasJobSelection = Ctx.GetRawPayload().IsValid()
        && Ctx.GetRawPayload()->HasField(TEXT("jobs"));
    TArray<int32> SelectedJobIndices;
    TSet<int32> SelectedJobSet;
    if (bHasJobSelection)
    {
        const TArray<TSharedPtr<FJsonValue>>* RequestedJobs = nullptr;
        if (!Ctx.RequireArray(TEXT("jobs"), RequestedJobs) || !RequestedJobs)
        {
            return true;
        }
        if (RequestedJobs->Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("'jobs' must contain at least one queue index when supplied"));
            return true;
        }
        SelectedJobIndices.Reserve(RequestedJobs->Num());
        for (int32 ArrayIndex = 0; ArrayIndex < RequestedJobs->Num(); ++ArrayIndex)
        {
            const TSharedPtr<FJsonValue>& Value = (*RequestedJobs)[ArrayIndex];
            double Number = 0.0;
            if (!Value.IsValid() || !Value->TryGetNumber(Number)
                || !FMath::IsFinite(Number)
                || !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
                || Number < 0.0 || Number > static_cast<double>(MAX_int32))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("'jobs[%d]' must be a non-negative integer queue index"), ArrayIndex));
                return true;
            }

            const int32 JobIndex = FMath::RoundToInt(Number);
            if (!Jobs.IsValidIndex(JobIndex))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, FString::Printf(
                    TEXT("Queue job index %d is outside 0..%d"), JobIndex, Jobs.Num() - 1));
                return true;
            }
            if (SelectedJobSet.Contains(JobIndex))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("'jobs' contains duplicate queue index %d"), JobIndex));
                return true;
            }
            SelectedJobIndices.Add(JobIndex);
            SelectedJobSet.Add(JobIndex);
        }
    }

    UClass* ResolvedExecutorClass = UMoviePipelinePIEExecutor::StaticClass();
    const FString ExecutorClassPath = Ctx.GetString(TEXT("executorClass"));
    if (!ExecutorClassPath.IsEmpty())
    {
        ResolvedExecutorClass = LoadClass<UObject>(nullptr, *ExecutorClassPath);
        if (!ResolvedExecutorClass)
        {
            Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND, FString::Printf(TEXT("Executor class '%s' not loadable"), *ExecutorClassPath));
            return true;
        }
    }
    if (!ResolvedExecutorClass->IsChildOf(UMoviePipelineExecutorBase::StaticClass())
        || ResolvedExecutorClass->HasAnyClassFlags(CLASS_Abstract))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_EXECUTOR_CLASS, FString::Printf(
            TEXT("Executor class '%s' must be a concrete UMoviePipelineExecutorBase subclass"),
            *ResolvedExecutorClass->GetPathName()));
        return true;
    }
    TSubclassOf<UMoviePipelineExecutorBase> ExecutorClass = ResolvedExecutorClass;

    if (QSS->IsRendering())
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_RENDER_IN_PROGRESS,
            TEXT("MoviePipeline queue is already being rendered; wait for the active executor "
                 "to finish before starting another mrq.run_jobs call"));
        return true;
    }

    FJobBindArgs Args;
    Args.Method = Ctx.GetMethod();
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetNumberField(TEXT("queueSize"), Jobs.Num());
    Args.StartedPayload->SetStringField(TEXT("executorClass"), ExecutorClass->GetPathName());
    if (bHasJobSelection)
    {
        TArray<TSharedPtr<FJsonValue>> SelectionValues;
        SelectionValues.Reserve(SelectedJobIndices.Num());
        for (const int32 JobIndex : SelectedJobIndices)
        {
            SelectionValues.Add(MakeShared<FJsonValueNumber>(JobIndex));
        }
        Args.StartedPayload->SetArrayField(TEXT("jobs"), SelectionValues);
    }
    // Subsystem captured weakly: long-running render could outlive a GC pass.
    TWeakObjectPtr<UMoviePipelineQueueSubsystem> WeakQSS(QSS);
    TSubclassOf<UMoviePipelineExecutorBase> ExecutorClassCopy = ExecutorClass;
    const bool bAllowUnrenderableFrames = Ctx.GetBool(TEXT("allowUnrenderableFrames"), false);
    Args.BindNativeDelegate =
        [WeakQSS, ExecutorClassCopy, bHasJobSelection, SelectedJobSet, SelectedJobIndices,
            bAllowUnrenderableFrames]
        (FJobOnComplete OnComplete) mutable
    {
        UMoviePipelineQueueSubsystem* LiveQSS = WeakQSS.Get();
        if (!LiveQSS)
        {
            OnComplete(false, nullptr, TEXT("Queue subsystem was garbage collected before render started"));
            return;
        }

        UMoviePipelineQueue* LiveQueue = LiveQSS->GetQueue();
        if (!LiveQueue)
        {
            OnComplete(false, nullptr, TEXT("MoviePipeline queue was unavailable when render started"));
            return;
        }
        if (LiveQSS->IsRendering())
        {
            OnComplete(false, nullptr, TEXT("MoviePipeline queue is already being rendered"));
            return;
        }

        const TArray<UMoviePipelineExecutorJob*> LiveJobs = LiveQueue->GetJobs();
        TSharedRef<TArray<FScopedJobEnabledState>> EnabledStates =
            MakeShared<TArray<FScopedJobEnabledState>>();
        TSharedRef<bool> bRestored = MakeShared<bool>(false);
        const auto RestoreEnabledStates = [EnabledStates, bRestored]()
        {
            RestoreScopedJobEnabledStates(EnabledStates, bRestored);
        };

        // PIE validates every job in the queue it receives, including disabled jobs. A selected
        // render therefore needs a transient queue containing only the requested jobs; toggling
        // enabled state on the shared queue alone still lets an unrelated invalid map abort the
        // render before the selected job starts. Duplicate the complete queue first so custom
        // executor state is preserved, then remove unselected copies in reverse index order.
        UMoviePipelineQueue* QueueToRender = LiveQueue;
        TSharedPtr<TStrongObjectPtr<UMoviePipelineQueue>> SelectedQueueHolder;
        if (bHasJobSelection)
        {
            for (const int32 JobIndex : SelectedJobIndices)
            {
                if (!LiveJobs.IsValidIndex(JobIndex) || !LiveJobs[JobIndex])
                {
                    OnComplete(false, nullptr,
                        TEXT("A selected queue job changed before render started"));
                    return;
                }
            }

            UMoviePipelineQueue* SelectedQueue = DuplicateObject<UMoviePipelineQueue>(
                LiveQueue, LiveQSS);
            if (!SelectedQueue)
            {
                OnComplete(false, nullptr, TEXT("Could not allocate the selected MRQ queue"));
                return;
            }
            SelectedQueueHolder = MakeShared<TStrongObjectPtr<UMoviePipelineQueue>>(SelectedQueue);
            for (const int32 JobIndex : SelectedJobIndices)
            {
                TArray<UMoviePipelineExecutorJob*> SelectedJobs = SelectedQueue->GetJobs();
                if (!SelectedJobs.IsValidIndex(JobIndex) || !SelectedJobs[JobIndex])
                {
                    RestoreEnabledStates();
                    OnComplete(false, nullptr,
                        TEXT("A selected queue copy changed before render started"));
                    return;
                }
                UMoviePipelineExecutorJob* SelectedJob = SelectedJobs[JobIndex];
                if (!SelectedJob->GetConfiguration())
                {
                    RestoreEnabledStates();
                    OnComplete(false, nullptr,
                        TEXT("A selected queue job has no MoviePipeline configuration"));
                    return;
                }
                FScopedJobEnabledState& State = EnabledStates->AddDefaulted_GetRef();
                State.Job = SelectedJob;
                State.bWasEnabled = SelectedJob->IsEnabled();
                SelectedJob->SetIsEnabled(true);
            }
            TArray<UMoviePipelineExecutorJob*> SelectedJobs = SelectedQueue->GetJobs();
            for (int32 CopyIndex = SelectedJobs.Num() - 1; CopyIndex >= 0; --CopyIndex)
            {
                if (!SelectedJobSet.Contains(CopyIndex))
                {
                    SelectedQueue->DeleteJob(SelectedJobs[CopyIndex]);
                }
            }
            QueueToRender = SelectedQueue;
        }

        // Create and bind the executor before starting it. RenderQueueInstanceWithExecutorInstance
        // starts the executor before returning, so an invalid sequence/map can finish synchronously
        // and otherwise bypass the completion delegate that restores a scoped job selection.
        UMoviePipelineExecutorBase* Exec = NewObject<UMoviePipelineExecutorBase>(
            LiveQSS, ExecutorClassCopy);
        if (!Exec)
        {
            RestoreEnabledStates();
            OnComplete(false, nullptr, TEXT("Could not allocate the requested MRQ executor"));
            return;
        }
        // Per-job artifact facts, collected as each job finishes rather than at the end: the
        // executor's own OnExecutorFinished carries no output data, and the graph path nulls the
        // job pointer immediately after this broadcast.
        TSharedRef<TArray<TSharedPtr<FJsonValue>>> JobReports =
            MakeShared<TArray<TSharedPtr<FJsonValue>>>();

        // Best-effort detail from the executor error delegate. UE 5.8's base implementation routes
        // OnExecutorErroredImpl through OnExecutorFinished, so the terminal decision below also
        // consumes the finished bSuccess value and per-job output bSuccess directly.
        TSharedRef<TArray<TSharedPtr<FJsonValue>>> ExecutorErrors =
            MakeShared<TArray<TSharedPtr<FJsonValue>>>();
        Exec->OnExecutorErrored().AddLambda(
            [ExecutorErrors](UMoviePipelineExecutorBase* /*InExec*/, UMoviePipeline* Pipeline,
                bool bIsFatal, FText ErrorText)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetBoolField(TEXT("fatal"), bIsFatal);
            Entry->SetStringField(TEXT("message"), ErrorText.ToString());
            if (Pipeline)
            {
                if (const UMoviePipelineExecutorJob* ErroredJob = Pipeline->GetCurrentJob())
                {
                    Entry->SetStringField(TEXT("jobName"), ErroredJob->JobName);
                }
            }
            ExecutorErrors->Add(MakeShared<FJsonValueObject>(Entry));
        });
        // OnIndividualJobWorkFinished is declared on UMoviePipelinePIEExecutor, not on the base,
        // so a caller-supplied executor of another class produces no per-job data. That is
        // reported below rather than silently returning an empty jobs array.
        TSharedRef<bool> bAnyJobFailed = MakeShared<bool>(false);
        TSharedRef<FString> JobFailureMessage = MakeShared<FString>();
        UMoviePipelinePIEExecutor* PIEExec = Cast<UMoviePipelinePIEExecutor>(Exec);
        if (PIEExec)
        {
            PIEExec->OnIndividualJobWorkFinished().AddLambda(
                [JobReports, bAnyJobFailed, JobFailureMessage](FMoviePipelineOutputData OutputData)
            {
                if (!OutputData.bSuccess)
                {
                    *bAnyJobFailed = true;
                    if (JobFailureMessage->IsEmpty())
                    {
                        *JobFailureMessage = OutputData.Job && !OutputData.Job->JobName.IsEmpty()
                            ? FString::Printf(TEXT("MRQ job '%s' reported failure"),
                                *OutputData.Job->JobName)
                            : TEXT("MRQ job reported failure");
                    }
                }
                JobReports->Add(MakeShared<FJsonValueObject>(MakeJobArtifactReport(OutputData)));
            });
        }
        bool bCollectsArtifacts = PIEExec != nullptr;
#if WITH_DEV_AUTOMATION_TESTS
        if (const PinWrightMRQHandlerTestHooks::FInjectedExecutorResult* Injected =
            PinWrightMRQHandlerTestHooks::ActiveExecutorResult())
        {
            TSharedPtr<FJsonObject> InjectedReport = PinWrightMRQ::BuildArtifactReport(
                Injected->Files, Injected->EncodeContext);
            InjectedReport->SetBoolField(TEXT("jobSucceeded"), Injected->bJobSucceeded);
            if (!Injected->JobName.IsEmpty())
            {
                InjectedReport->SetStringField(TEXT("jobName"), Injected->JobName);
            }
            JobReports->Add(MakeShared<FJsonValueObject>(InjectedReport));
            if (!Injected->bJobSucceeded)
            {
                *bAnyJobFailed = true;
                if (JobFailureMessage->IsEmpty())
                {
                    *JobFailureMessage = Injected->JobName.IsEmpty()
                        ? TEXT("MRQ job reported failure")
                        : FString::Printf(TEXT("MRQ job '%s' reported failure"), *Injected->JobName);
                }
            }
            bCollectsArtifacts = true;
        }
#endif
        Exec->OnExecutorFinished().AddLambda(
            [OnComplete = MoveTemp(OnComplete), JobReports, ExecutorErrors, bCollectsArtifacts,
                bAllowUnrenderableFrames, bAnyJobFailed, JobFailureMessage, RestoreEnabledStates,
                SelectedQueueHolder]
            (UMoviePipelineExecutorBase* InExec, bool bSuccess) mutable
        {
            RestoreEnabledStates();
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            const bool bExecutorFailed = !bSuccess || *bAnyJobFailed;
            const bool bNoRenderedOutput = bCollectsArtifacts && bSuccess &&
                !HasMRQRenderedOutput(*JobReports);
            const bool bUnrenderableFrames = bCollectsArtifacts && bSuccess && !bNoRenderedOutput
                && HasMRQUnrenderableJob(*JobReports);
            const bool bUnrenderableFramesRefused =
                bUnrenderableFrames && !bAllowUnrenderableFrames;
            const bool bReportedSuccess =
                !bExecutorFailed && !bNoRenderedOutput && !bUnrenderableFramesRefused;
            Result->SetBoolField(TEXT("success"), bReportedSuccess);
            if (bCollectsArtifacts)
            {
                Result->SetBoolField(TEXT("unrenderableFramesDetected"), bUnrenderableFrames);
            }
            if (bNoRenderedOutput)
            {
                Result->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_RENDER_NO_OUTPUT);
                Result->SetStringField(TEXT("error"),
                    TEXT("MRQ executor reported success but produced no rendered output"));
            }
            else if (bUnrenderableFramesRefused)
            {
                Result->SetStringField(
                    TEXT("errorCode"), ErrorCodes::ERR_RENDER_UNRENDERABLE_FRAMES);
                Result->SetStringField(TEXT("error"),
                    TEXT("Every decoded sample from at least one MRQ job was a near-uniform "
                         "colour; inspect jobs[].outputFiles[].imageStats for measured channel "
                         "means and variances"));
            }
            else if (bUnrenderableFrames)
            {
                Result->SetStringField(TEXT("unrenderableFramesWarning"),
                    TEXT("Every decoded sample from at least one MRQ job was a near-uniform "
                         "colour, but allowUnrenderableFrames:true explicitly allowed it"));
            }
            else if (bExecutorFailed)
            {
                FString FailureMessage = InExec ? InExec->GetStatusMessage() : FString();
                if (FailureMessage.IsEmpty())
                {
                    FailureMessage = *JobFailureMessage;
                }
                if (FailureMessage.IsEmpty())
                {
                    FailureMessage = TEXT("MRQ executor reported failure");
                }
                Result->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_MRQ_EXECUTOR_FAILED);
                Result->SetStringField(TEXT("error"), FailureMessage);
            }
            if (ExecutorErrors->Num() > 0)
            {
                Result->SetArrayField(TEXT("executorErrors"), *ExecutorErrors);
                if (bReportedSuccess)
                {
                    // The combination the engine makes possible and nothing used to surface: the
                    // executor errored non-fatally and still finished "successfully".
                    Result->SetStringField(TEXT("executorWarning"), FString::Printf(
                        TEXT("The executor reported %d error(s) and still finished with ")
                        TEXT("success:true, because UMoviePipelineExecutorBase only clears its ")
                        TEXT("success flag for FATAL errors. Read `executorErrors` before "
                             "treating this render as clean."),
                        ExecutorErrors->Num()));
                }
            }
            if (bCollectsArtifacts)
            {
                Result->SetArrayField(TEXT("jobs"), *JobReports);
            }
            else
            {
                // Omitted rather than emitted empty: an empty jobs array would read as "the
                // render wrote nothing", which is a different claim from "nothing was measured".
                Result->SetStringField(TEXT("artifactWarning"),
                    TEXT("The requested executor is not a UMoviePipelinePIEExecutor, which is the ")
                    TEXT("only executor that broadcasts per-job output data, so NOTHING about the ")
                    TEXT("files this render wrote was measured. `jobs` is omitted rather than ")
                    TEXT("returned empty. Re-run without executorClass to get output paths, ")
                    TEXT("sizes and the achieved bitrate."));
            }
            OnComplete(bReportedSuccess, Result,
                bNoRenderedOutput ? FString(ErrorCodes::ERR_RENDER_NO_OUTPUT) :
                (bUnrenderableFramesRefused
                    ? FString(ErrorCodes::ERR_RENDER_UNRENDERABLE_FRAMES) :
                    (bExecutorFailed ? FString(ErrorCodes::ERR_MRQ_EXECUTOR_FAILED) : FString())));
        });
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        LiveQSS->RenderQueueInstanceWithExecutorInstance(QueueToRender, Exec);
#else
        // UMoviePipelineQueueSubsystem gained the queue-instance overload in UE 5.6; before that
        // RenderQueueWithExecutorInstance can only render the subsystem's own current queue - it
        // IS the 5.6 overload called with GetQueue(), so the unselected render is identical.
        if (QueueToRender == LiveQueue)
        {
            LiveQSS->RenderQueueWithExecutorInstance(Exec);
        }
        else
        {
            // A selected sub-queue is a transient queue the subsystem has never heard of, and no
            // pre-5.6 API renders one. Run the same three steps the subsystem performs - hook the
            // finished delegate, execute - minus its ActiveExecutor bookkeeping, which is what
            // would otherwise keep the executor referenced, so root it for the render's duration.
            // Consequence on these engines only: the subsystem reports IsRendering() == false for
            // a selected render, so a concurrent mrq.run_jobs is not refused by that check.
            TSharedRef<TStrongObjectPtr<UMoviePipelineExecutorBase>> ExecutorHolder =
                MakeShared<TStrongObjectPtr<UMoviePipelineExecutorBase>>(Exec);
            Exec->OnExecutorFinished().AddLambda(
                [ExecutorHolder](UMoviePipelineExecutorBase* /*InExec*/, bool /*bSuccess*/)
            {
                ExecutorHolder->Reset();
            });
            Exec->Execute(QueueToRender);
        }
#endif
    };
    Ctx.StartJob(Args);
#else
    Ctx.SendError(ErrorCodes::ERR_MRQ_NOT_AVAILABLE, TEXT("MovieRenderPipeline plugin not enabled in this build"));
#endif
    return true;
}

// ---- mrq.list_jobs ----
REGISTER_RPC_HANDLER("mrq.list_jobs", "mrq",
    "Enumerate the editor-global MoviePipeline queue, including each job's identity, resolved "
    "configuration and output path shape. The returned indices are the positional values accepted "
    "by mrq.run_jobs.jobs and mrq.remove_job; they can change after queue edits.",
    RPC_NO_PARAMS)
{
#if MCP_HAS_MRQ
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("GEditor not available"));
        return true;
    }
    UMoviePipelineQueueSubsystem* QSS = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
    if (!QSS)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_SUBSYSTEM_UNAVAILABLE,
            TEXT("UMoviePipelineQueueSubsystem unavailable"));
        return true;
    }
    UMoviePipelineQueue* Queue = QSS->GetQueue();
    if (!Queue)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_QUEUE_NULL, TEXT("MoviePipeline queue is null"));
        return true;
    }

    const TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
    TArray<TSharedPtr<FJsonValue>> JobValues;
    JobValues.Reserve(Jobs.Num());
    for (int32 Index = 0; Index < Jobs.Num(); ++Index)
    {
        JobValues.Add(MakeShared<FJsonValueObject>(MakeQueueJobEntry(Index, Jobs[Index])));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("jobs"), JobValues);
    Resp->SetNumberField(TEXT("count"), Jobs.Num());
    Resp->SetNumberField(TEXT("queueSize"), Jobs.Num());
    Ctx.SendSuccess(Resp);
#else
    Ctx.SendError(ErrorCodes::ERR_MRQ_NOT_AVAILABLE,
        TEXT("MovieRenderPipeline plugin not enabled in this build"));
#endif
    return true;
}

// ---- mrq.remove_job ----
REGISTER_RPC_HANDLER("mrq.remove_job", "mrq",
    "Remove one job from the editor-global MoviePipeline queue by its current positional index. "
    "Use mrq.list_jobs immediately before removal because indices change after queue edits.",
    RPC_PARAMS(
        RPC_PARAM_REQ("jobIndex", "integer", "Current positional index returned by mrq.list_jobs")
    ))
{
#if MCP_HAS_MRQ
    int32 JobIndex = -1;
    if (!Ctx.RequireInt(TEXT("jobIndex"), JobIndex))
    {
        return true;
    }
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("GEditor not available"));
        return true;
    }
    UMoviePipelineQueueSubsystem* QSS = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
    if (!QSS)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_SUBSYSTEM_UNAVAILABLE,
            TEXT("UMoviePipelineQueueSubsystem unavailable"));
        return true;
    }
    UMoviePipelineQueue* Queue = QSS->GetQueue();
    if (!Queue)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_QUEUE_NULL, TEXT("MoviePipeline queue is null"));
        return true;
    }
    if (QSS->IsRendering())
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_RENDER_IN_PROGRESS,
            TEXT("MoviePipeline queue is being rendered; wait for the active executor before "
                 "removing a job"));
        return true;
    }

    const TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
    if (!Jobs.IsValidIndex(JobIndex) || !Jobs[JobIndex])
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, FString::Printf(
            TEXT("Queue job index %d is outside 0..%d"), JobIndex, Jobs.Num() - 1));
        return true;
    }
    UMoviePipelineExecutorJob* Job = Jobs[JobIndex];
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("removedIndex"), JobIndex);
    Resp->SetStringField(TEXT("jobName"), Job->JobName);
    Resp->SetStringField(TEXT("sequencePath"), Job->Sequence.ToString());
    Resp->SetStringField(TEXT("mapPath"), Job->Map.ToString());
    Queue->DeleteJob(Job);
    Resp->SetNumberField(TEXT("queueSize"), Queue->GetJobs().Num());
    Ctx.SendSuccess(Resp);
#else
    Ctx.SendError(ErrorCodes::ERR_MRQ_NOT_AVAILABLE,
        TEXT("MovieRenderPipeline plugin not enabled in this build"));
#endif
    return true;
}

// ---- mrq.clear_queue ----
REGISTER_RPC_HANDLER("mrq.clear_queue", "mrq",
    "Remove every job from the editor-global MoviePipeline queue. Refused while an executor is "
    "rendering because the queue is shared editor state.",
    RPC_NO_PARAMS)
{
#if MCP_HAS_MRQ
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("GEditor not available"));
        return true;
    }
    UMoviePipelineQueueSubsystem* QSS = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
    if (!QSS)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_SUBSYSTEM_UNAVAILABLE, TEXT("UMoviePipelineQueueSubsystem unavailable"));
        return true;
    }
    UMoviePipelineQueue* Queue = QSS->GetQueue();
    if (!Queue)
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_QUEUE_NULL, TEXT("MoviePipeline queue is null"));
        return true;
    }
    if (QSS->IsRendering())
    {
        Ctx.SendError(ErrorCodes::ERR_MRQ_RENDER_IN_PROGRESS,
            TEXT("MoviePipeline queue is being rendered; wait for the active executor before "
                 "clearing it"));
        return true;
    }

    const int32 RemovedCount = Queue->GetJobs().Num();
    Queue->DeleteAllJobs();
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("removedCount"), RemovedCount);
    Resp->SetNumberField(TEXT("queueSize"), Queue->GetJobs().Num());
    Ctx.SendSuccess(Resp);
#else
    Ctx.SendError(ErrorCodes::ERR_MRQ_NOT_AVAILABLE, TEXT("MovieRenderPipeline plugin not enabled in this build"));
#endif
    return true;
}

// ---- mrq.list_presets ----
REGISTER_RPC_HANDLER("mrq.list_presets", "mrq",
    "Enumerate UMoviePipelinePrimaryConfig preset assets in the project for client discovery.",
    RPC_NO_PARAMS)
{
#if MCP_HAS_MRQ
    IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
    if (!AssetRegistry)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_REGISTRY_UNAVAILABLE, TEXT("IAssetRegistry::Get() returned null"));
        return true;
    }
    TArray<FAssetData> AssetData;
    AssetRegistry->GetAssetsByClass(UMoviePipelinePrimaryConfig::StaticClass()->GetClassPathName(), AssetData, /*bSearchSubClasses*/ false);

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Reserve(AssetData.Num());
    for (const FAssetData& Asset : AssetData)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("assetPath"), Asset.GetSoftObjectPath().ToString());
        Entry->SetStringField(TEXT("packagePath"), Asset.PackagePath.ToString());
        Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("presets"), Arr);
    Resp->SetNumberField(TEXT("count"), Arr.Num());
    Ctx.SendSuccess(Resp);
#else
    Ctx.SendError(ErrorCodes::ERR_MRQ_NOT_AVAILABLE, TEXT("MovieRenderPipeline plugin not enabled in this build"));
#endif
    return true;
}
