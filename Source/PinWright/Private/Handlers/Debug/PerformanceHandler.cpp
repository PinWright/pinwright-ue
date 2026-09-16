// Copyright (c) 2026 Alexander Penkin. MIT License.

// PerformanceHandler.cpp - Migrated from PinWright_PerformanceHandlers.cpp
// Performance profiling, benchmarking, and rendering optimization handlers

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/CVarPriorityPreservingSet.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Containers/Ticker.h"
#include "ShaderCompiler.h"
#include "Engine/Engine.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
// FCommandStatsFile::Get().LastFileSaved — the absolute path the stats system records for the
// finalized 'stat stopfile' capture; used to resolve the written .uestats deterministically.
// Everything it declares is guarded by #if STATS, so all uses below are STATS-guarded too.
#include "Stats/StatsFile.h"
// UE 5.8 deprecated the whole legacy stat-file capture: StatsFile.h defines
// UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8 (default 0) and compiles FCommandStatsFile — and the
// 'stat startfile' / 'stat stopfile' command parsing in StatsCommand.cpp — out unless the engine
// is rebuilt with that macro on. Pre-5.8 StatsFile.h never defines the macro, so its presence is
// the feature signal: undefined => capture always available under STATS; defined => available
// only if the engine opted back in.
#if defined(UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8)
#define PINWRIGHT_HAS_STATS_FILE_CAPTURE (STATS && UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8)
#else
#define PINWRIGHT_HAS_STATS_FILE_CAPTURE STATS
#endif

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/StaticMeshComponent.h"
#include "ContentStreaming.h"
#include "Editor/UnrealEd/Public/Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "LevelEditor.h"
// FMeshMergingSettings moved from Engine/MeshMerging.h (UE <= 5.4) to the split
// MeshMerge/MeshMergingSettings.h (UE 5.5+); pick whichever exists on this engine.
#if __has_include("MeshMerge/MeshMergingSettings.h")
#include "MeshMerge/MeshMergingSettings.h"
#else
#include "Engine/MeshMerging.h"
#endif
#include "MeshMergeModule.h"
#include "ProfilingDebugging/ScopedTimers.h"
#include "Subsystems/EditorActorSubsystem.h"
// EmitStringArray — the shared TArray<FString> -> JSON string array emitter used for the
// `warnings` convention (run_benchmark's requested-vs-measured and stat-file notes).
#include "Utils/JsonUtils.h"

// merge_actors headless path: drive IMeshMergeUtilities::MergeComponentsToStaticMesh
// directly with an explicit output package, bypassing the Merge Actors tool's modal
// CreateModalSaveAssetDialog (which would hang a headless MCP call).
#include "MeshMergeModule.h"
#include "IMeshMergeUtilities.h"
// FMeshMergingSettings concrete definition. NOTE: the Engine/MeshMerging.h umbrella only
// pulls these settings headers in under UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_5 (off in
// 5.7's modern include order), so include the concrete header directly — IMeshMergeUtilities.h
// only forward-declares FMeshMergingSettings. The header split out of Engine/MeshMerging.h
// in UE 5.5; on 5.3/5.4 the concrete struct still lives in the Engine umbrella header.
#if __has_include("MeshMerge/MeshMergingSettings.h")
#include "MeshMerge/MeshMergingSettings.h"
#else
#include "Engine/MeshMerging.h"
#endif
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Modules/ModuleManager.h"

// ---- performance.generate_memory_report ----
REGISTER_RPC_HANDLER("performance.generate_memory_report", "performance", "Run UE's 'memreport' console command and write the breakdown (or 'memreport -full' for the detailed variant) to Saved/Profiling/MemReports. The absolute path of the written .memreport file is resolved and returned in the response. Useful for diagnosing memory leaks and large texture/mesh hogs.",
    RPC_PARAMS(
        RPC_PARAM_OPT("detailed", "boolean", "When true, runs 'memreport -full' for a deeper per-class / per-package breakdown; defaults to false."),
        RPC_PARAM_OPT("outputPath", "string", "Echoed back in the response under requestedOutputPath for client convenience; the authoritative on-disk location is determined by UE and returned as the absolute 'path' field (Saved/Profiling/MemReports/<session>/<name>.memreport).")
    ))
{
    bool bDetailed = Ctx.GetBool(TEXT("detailed"), false);

    if (!GEditor)
    {
        Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor not available"));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();

    // Resolve the .memreport path deterministically instead of forcing callers to
    // glob/mtime-sort Saved/Profiling/MemReports (the off-MCP fallback the outputPath
    // param doc promised should be unnecessary).
    //
    // The bare 'memreport' console command DEFERS its work: UEngine::HandleMemReportCommand
    // only queues "MemReportDeferred ..." onto GEngine->DeferredCommands, which runs on a
    // LATER tick (after a forced full-purge GC). So immediately after Exec("memreport") the
    // file does not yet exist, and "newest file now" would resolve a stale prior report.
    //
    // Instead we issue the deferred command form directly — "MemReportDeferred -NAME=<token>"
    // routes straight to UEngine::HandleMemReportDeferredCommand in the SAME Exec call (it is
    // the deferred body, not a re-defer), so the file is written synchronously before Exec
    // returns. The explicit -NAME= gives the leaf a unique token (UE still prefixes a Pid on
    // server/editor builds), so we resolve the exact file by that token — race-free even if
    // another capture is in flight. -FULL selects the detailed profile (same as 'memreport -full').
    const FString Token = FString::Printf(TEXT("PinWrightMemReport_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    FString Cmd = FString::Printf(TEXT("MemReportDeferred -NAME=%s"), *Token);
    if (bDetailed)
    {
        Cmd += TEXT(" -FULL");
    }

    GEngine->Exec(World, *Cmd);

    // The report lands at <ProfilingDir>/MemReports/<session-folder>/[Pid<N>_]<token>.memreport.
    // The session folder (ProfilingHelpers' ProfilingSessionFolderName) is a process-lifetime
    // static fixed for the whole editor run, so the write always targets the SAME — and, since
    // it is created on first report, the NEWEST — immediate child of MemReports/. Resolve that
    // one folder and scan it non-recursively, instead of FindFilesRecursive walking every prior
    // session folder plus each detailed-report side-subdir (which grows unboundedly with report
    // history). The unique token still pins the exact file; bounding the scope only cuts cost.
    const FString MemReportsDir = FPaths::ProfilingDir() / TEXT("MemReports");

    TArray<FString> SessionDirs;
    IFileManager::Get().FindFiles(SessionDirs, *(MemReportsDir / TEXT("*")),
        /*Files*/ false, /*Directories*/ true);

    FString SessionDir = MemReportsDir;
    if (SessionDirs.Num() > 0)
    {
        // Pick the most-recently-modified immediate child — the current run's session folder.
        const FString* Newest = nullptr;
        FDateTime NewestTime = FDateTime::MinValue();
        for (const FString& Dir : SessionDirs)
        {
            const FDateTime ModTime = IFileManager::Get().GetTimeStamp(*(MemReportsDir / Dir));
            if (!Newest || ModTime > NewestTime)
            {
                Newest = &Dir;
                NewestTime = ModTime;
            }
        }
        SessionDir = MemReportsDir / *Newest;
    }

    TArray<FString> Found;
    IFileManager::Get().FindFiles(Found,
        *(SessionDir / FString::Printf(TEXT("*%s*.memreport"), *Token)),
        /*Files*/ true, /*Directories*/ false);

    FString ResolvedPath;
    if (Found.Num() > 0)
    {
        // FindFiles yields leaf names only; rejoin onto SessionDir before absolutizing.
        ResolvedPath = FPaths::ConvertRelativePathToFull(SessionDir / Found[0]);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("message"), TEXT("Memory report generated"));
    Resp->SetBoolField(TEXT("detailed"), bDetailed);
    if (!ResolvedPath.IsEmpty())
    {
        Resp->SetStringField(TEXT("path"), ResolvedPath);
    }
    else
    {
        // The exec ran but the file could not be located (ALLOW_DEBUG_FILES disabled, or a
        // platform that suppresses the write). Report the directory UE writes to so the
        // caller still has somewhere to look, and flag that the precise path is unresolved.
        Resp->SetStringField(TEXT("reportsDir"), FPaths::ConvertRelativePathToFull(MemReportsDir));
        Resp->SetBoolField(TEXT("pathResolved"), false);
    }

    // Echo a caller-supplied outputPath verbatim (the param's documented "client convenience"
    // value). The authoritative location is always the resolved 'path' above.
    const FString RequestedOutputPath = Ctx.GetString(TEXT("outputPath"));
    if (!RequestedOutputPath.IsEmpty())
    {
        Resp->SetStringField(TEXT("requestedOutputPath"), RequestedOutputPath);
    }

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- .uestats stat-file path resolution (shared by stop_profiling / run_benchmark) ----
//
// The stat-file capture ('stat startfile' / 'stat stopfile') is finalized by the engine on the
// stats pipe. GEngine->Exec(World, TEXT("stat stopfile")) routes through
// UE::Stats::DirectStatsCommand with bBlockForCompletion=true (FStatCmdCore::Exec_Runtime,
// StatsCommand.cpp:2268 -> Task.Wait() at :2452), so the Exec BLOCKS until IStatsWriteFile::Stop()
// has renamed the temp capture to its final <ProfilingDir>/UnrealStats/<name>.uestats and recorded
// that absolute path in FCommandStatsFile::Get().LastFileSaved (StatsFile.cpp:276). Reading that
// member straight after the Exec is therefore deterministic and race-free — the stat capture is a
// singleton (FCommandStatsFile::Start() calls Stop() first), so there is never a concurrent writer,
// and we do NOT mtime-sort the directory (the racy approach the sibling result-path fixes rejected).
// Returns the absolute path, or empty when STATS is compiled out or no capture had been active.
static FString ResolveFinalizedStatsFilePath()
{
#if PINWRIGHT_HAS_STATS_FILE_CAPTURE
    const FString& Last = FCommandStatsFile::Get().LastFileSaved;
    if (!Last.IsEmpty())
    {
        return FPaths::ConvertRelativePathToFull(Last);
    }
#endif
    return FString();
}

// The directory stat-file captures are written to: <ProfilingDir>/UnrealStats. Matches the path
// IStatsWriteFile::Start() builds (FPaths::ProfilingDir() + "UnrealStats/").
static FString GetUnrealStatsDir()
{
    return FPaths::ConvertRelativePathToFull(FPaths::ProfilingDir() / TEXT("UnrealStats"));
}

// True iff a stat-file capture is currently active (a start_profiling with no matching stop yet).
// Read BEFORE issuing 'stat stopfile' so stop_profiling only trusts LastFileSaved when this stop
// actually finalizes a live capture (otherwise LastFileSaved holds a stale prior path).
static bool IsStatsCaptureActive()
{
#if PINWRIGHT_HAS_STATS_FILE_CAPTURE
    return FCommandStatsFile::Get().IsStatFileActive();
#else
    return false;
#endif
}

// Finalize any in-flight stat-file capture and resolve the written .uestats path in one step,
// owning the ordering the resolution depends on: read IsStatsCaptureActive() BEFORE issuing
// 'stat stopfile' (the Exec clears the active flag), then trust LastFileSaved only when a capture
// was actually live — otherwise it holds a stale prior path. 'stat stopfile' blocks on the stats
// pipe (DirectStatsCommand bBlockForCompletion=true), so LastFileSaved is already the finalized
// path when Exec returns. Returns the absolute path, or empty when no capture was active / STATS is
// compiled out. World routes the Exec (editor world for stop_profiling, nullptr for the benchmark
// ticker); GEngine is null-guarded so a torn-down engine can't crash the stop path.
static FString StopStatFileCaptureAndResolvePath(UWorld* World)
{
    const bool bWasActive = IsStatsCaptureActive();
    if (GEngine)
    {
        GEngine->Exec(World, TEXT("stat stopfile"));
    }
    return bWasActive ? ResolveFinalizedStatsFilePath() : FString();
}

// ---- performance.start_profiling ----
REGISTER_RPC_HANDLER("performance.start_profiling", "performance", "Begin a stat-file capture by issuing 'stat startfile'. Pair with performance.stop_profiling, which finalizes the capture and returns the resolved absolute .uestats path; the file lands under Saved/Profiling/UnrealStats/ (reported here as statsDir) and loads in the Profiler tool. For Insights traces use insights.start_session.",
    RPC_NO_PARAMS)
{
#if !PINWRIGHT_HAS_STATS_FILE_CAPTURE
    // UE 5.8 compiled the legacy stat-file capture out (UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8
    // defaults to 0): 'stat startfile' no longer parses and no .uestats can be produced, so a
    // "Profiling started" success here would be fabricated. Fail loud and steer to Insights.
    Ctx.SendError(TEXT("NOT_SUPPORTED"),
        TEXT("Stat-file capture ('stat startfile') is compiled out on this engine (deprecated in UE 5.8); use insights.start_session for profiling traces."));
    return true;
#else
    if (!GEditor)
    {
        Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor not available"));
        return true;
    }

    GEngine->Exec(GEditor->GetEditorWorldContext().World(), TEXT("stat startfile"));

    // The engine picks a timestamped .uestats name that is not knowable on the game thread until
    // stop_profiling finalizes the capture (it is recorded in LastFileSaved only at Stop), so
    // report the directory the capture will land in; stop_profiling returns the exact file path.
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("message"), TEXT("Profiling started"));
    Resp->SetStringField(TEXT("statsDir"), GetUnrealStatsDir());
    Ctx.SendSuccess(Resp);
    return true;
#endif
}

// ---- performance.stop_profiling ----
REGISTER_RPC_HANDLER("performance.stop_profiling", "performance", "Finalize the stat-file capture started by performance.start_profiling (issues 'stat stopfile') and return the resolved absolute path of the written .uestats file as statFilePath — read deterministically from the engine's FCommandStatsFile::LastFileSaved, not a directory scan. The file lands under Saved/Profiling/UnrealStats/. If no capture was active, reports pathResolved:false and statsDir instead of a fabricated path.",
    RPC_NO_PARAMS)
{
#if !PINWRIGHT_HAS_STATS_FILE_CAPTURE
    // Mirror start_profiling: with the capture compiled out there is never anything to finalize,
    // so "Profiling stopped" would be a fabricated success.
    Ctx.SendError(TEXT("NOT_SUPPORTED"),
        TEXT("Stat-file capture ('stat stopfile') is compiled out on this engine (deprecated in UE 5.8); use insights.stop_session for profiling traces."));
    return true;
#else
    if (!GEditor)
    {
        Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor not available"));
        return true;
    }

    // Finalize the capture and resolve its written .uestats path (the helper owns the
    // read-active-before-stopfile ordering and the blocking-Exec rationale).
    const FString StatFilePath = StopStatFileCaptureAndResolvePath(GEditor->GetEditorWorldContext().World());

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("message"), TEXT("Profiling stopped"));
    if (!StatFilePath.IsEmpty())
    {
        Resp->SetStringField(TEXT("statFilePath"), StatFilePath);
    }
    else
    {
        // No active capture to finalize (a stop without a prior start), or STATS compiled out.
        // Report where captures land and flag the path unresolved so the response never fabricates
        // a path (mirrors performance.generate_memory_report's pathResolved:false fallback).
        Resp->SetStringField(TEXT("statsDir"), GetUnrealStatsDir());
        Resp->SetBoolField(TEXT("pathResolved"), false);
    }
    Ctx.SendSuccess(Resp);
    return true;
#endif
}

// ---- performance.show_fps ----
REGISTER_RPC_HANDLER("performance.show_fps", "performance", "Toggle the on-screen FPS overlay (issues 'stat fps'). For broader stat categories use performance.show_stats.",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Enable or disable FPS display (toggles)")
    ))
{
    if (!GEditor)
    {
        Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor not available"));
        return true;
    }

    GEngine->Exec(GEditor->GetEditorWorldContext().World(), TEXT("stat fps"));
    Ctx.SendSuccess(TEXT("FPS stat toggled"));
    return true;
}

// ---- performance.show_stats ----
REGISTER_RPC_HANDLER("performance.show_stats", "performance", "Toggle an on-screen stat category overlay by issuing 'stat <category>'. Same as the in-engine 'stat ...' console command. Category is sanitized to alphanumerics + underscore to prevent command injection.",
    RPC_PARAMS(
        RPC_PARAM_REQ("category", "string", "Stat group name (e.g. 'unit', 'fps', 'scenerendering', 'gpu'); restricted to [A-Za-z0-9_].")
    ))
{
    FString Category = Ctx.GetString(TEXT("category"));
    if (Category.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Category required"));
        return true;
    }

    if (!GEditor)
    {
        Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor not available"));
        return true;
    }

    // Sanitize category to prevent console command injection
    bool bIsValidCategory = true;
    for (int32 i = 0; i < Category.Len(); ++i)
    {
        TCHAR C = Category[i];
        if (!FChar::IsAlnum(C) && C != TEXT('_'))
        {
            bIsValidCategory = false;
            break;
        }
    }

    if (!bIsValidCategory)
    {
        Ctx.SendError(TEXT("INVALID_CATEGORY"),
            TEXT("Invalid stat category name. Only alphanumeric characters and underscores allowed."));
        return true;
    }

    GEngine->Exec(GEditor->GetEditorWorldContext().World(),
        *FString::Printf(TEXT("stat %s"), *Category));

    Ctx.SendSuccess(FString::Printf(TEXT("Stat '%s' toggled"), *Category));
    return true;
}

// Builds the {cvar, value} response entry shared by performance.set_scalability's sg.* group
// read-back and performance.apply_baseline_settings' applied-CVar report, so the per-CVar
// report shape lives in exactly one place.
static TSharedPtr<FJsonValue> MakePerformanceCVarEntry(const TCHAR* Name, int32 Value)
{
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("cvar"), Name);
    Entry->SetNumberField(TEXT("value"), Value);
    return MakeShared<FJsonValueObject>(Entry);
}

// ---- performance.set_scalability ----
REGISTER_RPC_HANDLER("performance.set_scalability", "performance", "Set the engine scalability quality level. Drives the canonical sg.* group CVars (sg.ViewDistanceQuality, sg.ShadowQuality, ...) via Scalability::SetQualityLevels, which writes them at ECVF_SetByScalability — the LOWEST settable CVar priority. A group already pinned higher (by a device profile, a config, or a prior console 'sg.<Group> N' / 'scalability N' at ECVF_SetByConsole) is therefore NOT overwritten and keeps its pinned value. The response reads the sg.* groups back and reports their effective per-group values plus whether every group reached the requested level, so a success can never silently mean 'nothing changed'.",
    RPC_PARAMS(
        RPC_PARAM_OPT("level", "integer", "Quality level (0=Low, 1=Medium, 2=High, 3=Epic, 4=Cinematic)")
    ))
{
    int32 Level = Ctx.GetInt(TEXT("level"), 3);

    Scalability::FQualityLevels Quals;
    Quals.SetFromSingleQualityLevel(Level);
    Scalability::SetQualityLevels(Quals);
    Scalability::SaveState(GEditorIni);

    // Read the canonical sg.* group CVars back after the apply and report their EFFECTIVE
    // values (IConsoleVariable::GetInt returns the current highest-priority value). Because
    // SetQualityLevels writes at ECVF_SetByScalability (the lowest settable priority), a group
    // already pinned higher keeps its pinned value and the requested level is silently dropped
    // for that group. Returning the read-back (instead of a bare "Scalability set") lets a
    // caller see exactly which groups landed and which were shadowed. ResolutionQuality is
    // intentionally excluded: it is a screen-percentage, not a 0..4 quality level, so it would
    // never equal Level and does not belong in the requestedLevelApplied comparison.
    static const TCHAR* const LevelGroups[] = {
        TEXT("sg.ViewDistanceQuality"),
        TEXT("sg.AntiAliasingQuality"),
        TEXT("sg.ShadowQuality"),
        TEXT("sg.GlobalIlluminationQuality"),
        TEXT("sg.ReflectionQuality"),
        TEXT("sg.PostProcessQuality"),
        TEXT("sg.TextureQuality"),
        TEXT("sg.EffectsQuality"),
        TEXT("sg.FoliageQuality"),
        TEXT("sg.ShadingQuality"),
        TEXT("sg.LandscapeQuality"),
    };

    TArray<TSharedPtr<FJsonValue>> AppliedGroups;
    bool bAllAtRequestedLevel = true;
    for (const TCHAR* GroupName : LevelGroups)
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(GroupName);
        if (!CVar)
        {
            continue;
        }
        const int32 Effective = CVar->GetInt();
        AppliedGroups.Add(MakePerformanceCVarEntry(GroupName, Effective));
        if (Effective != Level)
        {
            bAllAtRequestedLevel = false;
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("requestedLevel"), Level);
    Resp->SetArrayField(TEXT("appliedGroups"), AppliedGroups);
    // Machine-checkable signal: false means at least one sg.* group did not reach the
    // requested level (a higher-priority pin shadowed the scalability write).
    Resp->SetBoolField(TEXT("requestedLevelApplied"), bAllAtRequestedLevel);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- performance.set_resolution_scale ----
REGISTER_RPC_HANDLER("performance.set_resolution_scale", "performance", "Set screen percentage / resolution scale",
    RPC_PARAMS(
        RPC_PARAM_REQ("scale", "number", "Screen percentage (e.g. 100.0 for native)")
    ))
{
    double Scale = Ctx.GetNumber(TEXT("scale"), -1.0);
    if (Scale < 0.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Scale required"));
        return true;
    }

    // r.ScreenPercentage is the ResolutionQuality scalability member (Scalability.cpp:551 writes
    // it at ECVF_SetByScalability, with no BaseScalability.ini row), so a bare Set() here would
    // pin it above the user's Scalability panel for the session. See
    // Handlers/CVarPriorityPreservingSet.h.
    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
    CVarPriorityPreservingSet::SetPreservingPriority(CVar, (float)Scale);

    Ctx.SendSuccess(TEXT("Resolution scale set"));
    return true;
}

// ---- performance.set_vsync ----
REGISTER_RPC_HANDLER("performance.set_vsync", "performance", "Enable or disable VSync",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Enable VSync (default true)")
    ))
{
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);

    // r.VSync carries ECVF_Scalability (ConsoleManager.cpp:4330, flags :4334) and has no
    // BaseScalability.ini row, so an ini scan misses it while the panel still owns it. Written at
    // its existing priority; see Handlers/CVarPriorityPreservingSet.h.
    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));
    CVarPriorityPreservingSet::SetPreservingPriority(CVar, bEnabled ? 1 : 0);

    Ctx.SendSuccess(TEXT("VSync configured"));
    return true;
}

// ---- performance.set_frame_rate_limit ----
REGISTER_RPC_HANDLER("performance.set_frame_rate_limit", "performance", "Set maximum FPS limit",
    RPC_PARAMS(
        RPC_PARAM_REQ("maxFPS", "number", "Maximum frames per second (0 = unlimited)")
    ))
{
    double Limit = Ctx.GetNumber(TEXT("maxFPS"), -1.0);
    if (Limit < 0.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("maxFPS required"));
        return true;
    }

    GEngine->SetMaxFPS((float)Limit);
    Ctx.SendSuccess(TEXT("Max FPS set"));
    return true;
}

// ---- performance.configure_nanite ----
REGISTER_RPC_HANDLER("performance.configure_nanite", "performance", "Enable or disable Nanite rendering",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Enable Nanite (default true)")
    ))
{
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);

    // r.Nanite carries ECVF_Scalability (StaticMeshSceneProxy.cpp:110, flags :117). Written at
    // its existing priority; see Handlers/CVarPriorityPreservingSet.h.
    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite"));
    CVarPriorityPreservingSet::SetPreservingPriority(CVar, bEnabled ? 1 : 0);

    Ctx.SendSuccess(TEXT("Nanite configured"));
    return true;
}

// ---- performance.configure_lod ----
REGISTER_RPC_HANDLER("performance.configure_lod", "performance", "Configure LOD bias and forced LOD level",
    RPC_PARAMS(
        RPC_PARAM_OPT("lodBias", "number", "MipMap LOD bias value"),
        RPC_PARAM_OPT("forceLOD", "integer", "Force a specific LOD level (-1 = auto)")
    ))
{
    double LODBias = 0.0;
    bool bHasLODBias = Ctx.GetRawPayload()->TryGetNumberField(TEXT("lodBias"), LODBias);
    if (bHasLODBias)
    {
        // r.MipMapLODBias carries ECVF_Scalability (Texture2D.cpp:81, flags :84). Written at its
        // existing priority; see Handlers/CVarPriorityPreservingSet.h.
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MipMapLODBias"));
        CVarPriorityPreservingSet::SetPreservingPriority(CVar, (float)LODBias);
    }

    double ForceLOD = -1.0;
    bool bHasForceLOD = Ctx.GetRawPayload()->TryGetNumberField(TEXT("forceLOD"), ForceLOD);
    if (bHasForceLOD)
    {
        // r.ForceLOD carries ECVF_Scalability (RenderCore.cpp:223, flags :226). Written at its
        // existing priority; see Handlers/CVarPriorityPreservingSet.h.
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ForceLOD"));
        CVarPriorityPreservingSet::SetPreservingPriority(CVar, (int32)ForceLOD);
    }

    Ctx.SendSuccess(TEXT("LOD settings configured"));
    return true;
}

// ---- performance.configure_texture_streaming ----
REGISTER_RPC_HANDLER("performance.configure_texture_streaming", "performance", "Configure texture streaming settings. Writes r.Streaming.PoolSize (an ECVF_Scalability TextureQuality member) and r.TextureStreaming at each CVar's EXISTING SetBy priority, never at ECVF_SetByCode, so the call cannot pin them above the ECVF_SetByScalability priority the editor's own Settings > Engine Scalability Settings panel writes at.",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Enable texture streaming"),
        RPC_PARAM_OPT("poolSize", "number", "Streaming pool size in MB"),
        RPC_PARAM_OPT("boostPlayerLocation", "boolean", "Boost streaming around player camera")
    ))
{
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);

    double PoolSize = 0;
    if (Ctx.GetRawPayload()->TryGetNumberField(TEXT("poolSize"), PoolSize))
    {
        // r.Streaming.PoolSize carries ECVF_Scalability (TextureStreamingHelpers.cpp:120, flags
        // :123) and is a TextureQuality member (BaseScalability.ini:749,760,771,782,793). Written
        // at its existing priority; see Handlers/CVarPriorityPreservingSet.h.
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize"));
        CVarPriorityPreservingSet::SetPreservingPriority(CVar, (float)PoolSize);
    }

    bool bBoost = Ctx.GetBool(TEXT("boostPlayerLocation"), false);
    if (bBoost)
    {
        if (GEditor && GEditor->GetEditorWorldContext().World())
        {
            APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(
                GEditor->GetEditorWorldContext().World(), 0);
            if (Cam)
            {
                IStreamingManager::Get().AddViewLocation(Cam->GetCameraLocation());
            }
        }
    }

    // r.TextureStreaming is NOT scalability-flagged (TextureStreamingHelpers.cpp:105, flags :110
    // are ECVF_Default | ECVF_RenderThreadSafe), but it goes through the same helper so this verb
    // has one write rule rather than two — and so a future ECVF_Scalability reclassification
    // upstream cannot silently reopen the pin here.
    IConsoleVariable* CVarStream = IConsoleManager::Get().FindConsoleVariable(TEXT("r.TextureStreaming"));
    CVarPriorityPreservingSet::SetPreservingPriority(CVarStream, bEnabled ? 1 : 0);

    Ctx.SendSuccess(TEXT("Texture streaming configured"));
    return true;
}

// ---- performance.merge_actors ----
// Drives the merge headlessly via IMeshMergeUtilities::MergeComponentsToStaticMesh
// with a computed output package, NOT the interactive Merge Actors tool
// (IMergeActorsTool::RunMergeFromSelection). The tool path routes through
// GetPackageNameForMergeAction -> CreateModalSaveAssetDialog, a modal dialog that
// cannot be answered in a headless MCP context (board
// B-merge-actors-rejects-valid-selection). Driving the utilities API directly with
// an explicit, non-empty package name bypasses the dialog and the editor-selection
// gate entirely.
REGISTER_RPC_HANDLER("performance.merge_actors", "performance", "Combine 2+ static-mesh actors into a single merged StaticMeshActor (UE's mesh-merge utilities) — the standard workflow for reducing draw calls on grouped static geometry. Runs headless (no save dialog). Optionally deletes the source actors and replaces them with the merge result.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actors", "array", "Array of actor display labels or paths; minimum 2 entries. Only actors carrying a UStaticMeshComponent with a non-null static mesh are merged."),
        RPC_PARAM_OPT("toolName", "string", "Merge tool variant; defaults to 'Merge' (UE's mesh-merging tool, GetToolNameText()==\"Merge\"). The legacy alias 'MeshMerging' is accepted and mapped to 'Merge'. Only the mesh-merging variant is performed — other tool names are rejected."),
        RPC_PARAM_OPT("outputPackage", "string", "Explicit long package name for the merged static mesh asset (e.g. '/Game/Merged/MyMerge'); defaults to a generated, unique '/Game/Merged/MERGED_<FirstActor>' path."),
        RPC_PARAM_OPT("replaceSourceActors", "boolean", "When true, deletes the source actors after merging; defaults to false (kept alongside the merged result).")
    ))
{
    const TArray<TSharedPtr<FJsonValue>>* NamesArray = Ctx.GetArray(TEXT("actors"));
    if (!NamesArray || NamesArray->Num() < 2)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("merge_actors requires an 'actors' array with at least 2 entries"));
        return true;
    }

    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available for merge_actors"));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    TArray<AActor*> ActorsToMerge;

    auto ResolveActorByName = [World](const FString& Name) -> AActor*
    {
        if (Name.IsEmpty()) return nullptr;

        if (AActor* ByPath = FindObject<AActor>(nullptr, *Name))
            return ByPath;

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor) continue;

            const FString Label = Actor->GetActorLabel();
            const FString ObjName = Actor->GetName();
            if (Label.Equals(Name, ESearchCase::IgnoreCase) ||
                ObjName.Equals(Name, ESearchCase::IgnoreCase))
            {
                return Actor;
            }
        }
        return nullptr;
    };

    for (const TSharedPtr<FJsonValue>& Val : *NamesArray)
    {
        if (!Val.IsValid() || Val->Type != EJson::String) continue;

        const FString RawName = Val->AsString().TrimStartAndEnd();
        if (RawName.IsEmpty()) continue;

        if (AActor* Resolved = ResolveActorByName(RawName))
        {
            ActorsToMerge.AddUnique(Resolved);
        }
    }

    if (ActorsToMerge.Num() < 2)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("merge_actors resolved fewer than 2 valid actors"));
        return true;
    }

    // Validate the toolName param against the only variant this handler can honor.
    // The merge does NOT go through any IMergeActorsTool's RunMergeFromSelection():
    // that path calls GetPackageNameForMergeAction(GetDefaultPackageName(), ...) ->
    // ContentBrowserModule.CreateModalSaveAssetDialog(), a blocking modal "Create
    // Merged Actor" dialog that hangs a headless MCP call. Instead we drive
    // IMeshMergeUtilities::MergeComponentsToStaticMesh directly with an explicit
    // output package (the same engine API FMeshMergingTool::RunMerge uses, minus the
    // dialog). Because the merge is hardwired to mesh-merging, the only accepted
    // toolName values are "Merge" (UE 5.7's mesh-merging tool reports
    // GetToolNameText()=="Merge") and the historical alias "MeshMerging" — any other
    // registered tool (MeshProxy, Batch, ...) is rejected rather than silently
    // running a mesh merge under a name the handler does not actually perform.
    FString RequestedToolName = Ctx.GetString(TEXT("toolName"));
    if (!RequestedToolName.IsEmpty()
        && !RequestedToolName.Equals(TEXT("Merge"), ESearchCase::IgnoreCase)
        && !RequestedToolName.Equals(TEXT("MeshMerging"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(TEXT("MERGE_TOOL_UNAVAILABLE"),
            FString::Printf(TEXT("merge_actors only performs the mesh-merging variant; toolName '%s' is not honored (use 'Merge')"),
                *RequestedToolName));
        return true;
    }

    // Collect the static-mesh primitive components to merge directly from the resolved
    // actors — no dependency on the editor selection set or the interactive tool's
    // CanMergeFromSelection() gate. This is exactly the includability rule the engine
    // applies (UStaticMeshComponent with a non-null GetStaticMesh()). UniqueLevels is
    // tracked so replaceSourceActors can require a single shared level and so the merged
    // actor is spawned into the source level.
    TArray<UPrimitiveComponent*> ComponentsToMerge;
    TArray<ULevel*> UniqueLevels;
    for (AActor* Actor : ActorsToMerge)
    {
        if (!Actor) continue;
        TArray<UStaticMeshComponent*> MeshComponents;
        Actor->GetComponents(MeshComponents);
        for (UStaticMeshComponent* MeshComponent : MeshComponents)
        {
            if (MeshComponent && MeshComponent->GetStaticMesh() != nullptr)
            {
                ComponentsToMerge.Add(MeshComponent);
            }
        }
        UniqueLevels.AddUnique(Actor->GetLevel());
    }

    if (ComponentsToMerge.Num() == 0)
    {
        Ctx.SendError(TEXT("MERGE_NOT_POSSIBLE"),
            TEXT("None of the resolved actors carry a UStaticMeshComponent with an assigned static mesh"));
        return true;
    }

    const bool bReplaceSources = Ctx.GetBool(TEXT("replaceSourceActors"), false);
    if (bReplaceSources && UniqueLevels.Num() > 1)
    {
        Ctx.SendError(TEXT("MERGE_NOT_POSSIBLE"),
            TEXT("replaceSourceActors requires all source actors to live in the same level"));
        return true;
    }

    // Compute an explicit, non-empty output package so the utilities path never opens
    // a modal save dialog. A caller-supplied outputPackage wins; otherwise default to a
    // generated, unique /Game/Merged/MERGED_<FirstActor> path.
    FString OutputPackage = Ctx.GetString(TEXT("outputPackage")).TrimStartAndEnd();
    if (OutputPackage.IsEmpty())
    {
        // Sanitize the actor label into a valid asset-name segment (labels routinely
        // carry spaces/dots/slashes that are illegal in a long package name), then run
        // it through CreateUniqueAssetName so a repeat merge yields MERGED_<Actor>_1,
        // _2, ... instead of silently overwriting a prior merged mesh.
        const FString SafeSegment = SanitizeAssetName(
            FString::Printf(TEXT("MERGED_%s"), *ActorsToMerge[0]->GetActorLabel()));
        const FString DesiredPackage = FString::Printf(TEXT("/Game/Merged/%s"), *SafeSegment);

        const FAssetToolsModule& AssetToolsModule =
            FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        FString UniqueAssetName;
        AssetToolsModule.Get().CreateUniqueAssetName(DesiredPackage, TEXT(""), OutputPackage, UniqueAssetName);
    }

    // Validate the final long package name so an invalid caller string or sanitized
    // segment fails with the project's standard INVALID_PATH error instead of reaching
    // low-level package code with a malformed path.
    if (!IsValidMountPoint(OutputPackage) || OutputPackage.Contains(TEXT(" ")))
    {
        Ctx.SendError(TEXT("INVALID_PATH"),
            FString::Printf(TEXT("Invalid output package path '%s'"), *OutputPackage));
        return true;
    }

    IMeshMergeUtilities& MeshUtilities =
        FModuleManager::Get().LoadModuleChecked<IMeshMergeModule>("MeshMergeUtilities").GetUtilities();

    FMeshMergingSettings MergeSettings;
    TArray<UObject*> AssetsToSync;
    FVector MergedActorLocation = FVector::ZeroVector;
    const float ScreenAreaSize = TNumericLimits<float>::Max();

    MeshUtilities.MergeComponentsToStaticMesh(
        ComponentsToMerge, World, MergeSettings, /*InBaseMaterial*/ nullptr, /*InOuter*/ nullptr,
        OutputPackage, AssetsToSync, MergedActorLocation, ScreenAreaSize, /*bSilent*/ true);

    // Single pass over the produced assets: register each so it appears in the
    // Content Browser, and capture the first UStaticMesh as the merge result.
    UStaticMesh* MergedMesh = nullptr;
    for (UObject* Asset : AssetsToSync)
    {
        if (!Asset) continue;
        FAssetRegistryModule::AssetCreated(Asset);
        if (!MergedMesh)
        {
            MergedMesh = Cast<UStaticMesh>(Asset);
        }
    }

    if (!MergedMesh)
    {
        Ctx.SendError(TEXT("MERGE_FAILED"), TEXT("Mesh merge produced no static mesh asset"));
        return true;
    }

    // Spawn the merged actor at the computed merge location and assign the merged mesh.
    // ComponentsToMerge is non-empty here (the gate above returned otherwise), so the
    // same loop ran UniqueLevels.AddUnique() at least once — UniqueLevels[0] always exists.
    // ToRawPtr(World->PersistentLevel): PersistentLevel is a TObjectPtr<ULevel> while
    // UniqueLevels[0] is a raw ULevel* — without the explicit raw conversion the ternary
    // has ambiguous common type (C2445) between ULevel* and TObjectPtr<ULevel>.
    ULevel* SpawnLevel = UniqueLevels[0] ? UniqueLevels[0] : ToRawPtr(World->PersistentLevel);
    FActorSpawnParameters SpawnParams;
    SpawnParams.OverrideLevel = SpawnLevel;
    AStaticMeshActor* MergedActor = World->SpawnActor<AStaticMeshActor>(
        MergedActorLocation, FRotator::ZeroRotator, SpawnParams);

    if (!MergedActor)
    {
        Ctx.SendError(TEXT("MERGE_FAILED"), TEXT("Failed to spawn merged StaticMeshActor"));
        return true;
    }

    if (UStaticMeshComponent* MergedComponent = MergedActor->GetStaticMeshComponent())
    {
        MergedComponent->SetStaticMesh(MergedMesh);
        MergedComponent->MarkRenderStateDirty();
    }
    MergedActor->SetActorLabel(MergedMesh->GetName());

    if (bReplaceSources)
    {
        // Delete via UEditorActorSubsystem so the removal is undoable and keeps the
        // editor selection/outliner consistent — matching the plugin's actor-deletion
        // convention (LifecycleHandler, EffectHandler, etc.). Fall back to
        // World->EditorDestroyActor only if the subsystem is unavailable.
        UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
        for (AActor* Actor : ActorsToMerge)
        {
            if (!Actor) continue;
            if (ActorSS)
            {
                ActorSS->DestroyActor(Actor);
            }
            else
            {
                World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
            }
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("mergedActorCount"), ActorsToMerge.Num());
    Resp->SetNumberField(TEXT("sourceActorCount"), ActorsToMerge.Num());
    Resp->SetNumberField(TEXT("mergedComponentCount"), ComponentsToMerge.Num());
    Resp->SetBoolField(TEXT("replaceSourceActors"), bReplaceSources);
    // mergedPackageName / outputPackage are the same explicit, non-modal output package;
    // both are reported (mergedPackageName for callers/tests that assert the headless
    // path was taken, outputPackage as the human-facing name).
    Resp->SetStringField(TEXT("mergedPackageName"), OutputPackage);
    Resp->SetStringField(TEXT("outputPackage"), OutputPackage);
    Resp->SetStringField(TEXT("mergedAssetPath"), MergedMesh->GetPathName());
    Resp->SetStringField(TEXT("mergedMesh"), MergedMesh->GetPathName());
    AddActorVerification(Resp, MergedActor);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- performance.run_benchmark ----
//
// WHY THIS VERB MEASURES FRAME DELTAS AND NOT THE STATS SYSTEM (board
// B-performance-run-benchmark-measures-nothing). It used to issue 'stat startfile', wait out
// `duration`, and complete a SUCCESSFUL job whose entire payload was `{captured:false}` — a
// benchmark that reported no performance quantity on any code path, on any engine. The 5.8 half
// of that is unfixable through the path it used: UE 5.8 gates FCommandStatsFile and the
// 'stat startfile'/'stat stopfile' parsing behind UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8
// (StatsFile.h:8-9 default 0; StatsCommand.cpp:2049-2092), so on a stock 5.8 editor the command
// does not parse and no .uestats can exist. But the legacy capture was never the measurement —
// it only produced a FILE for the standalone Profiler GUI — so refusing would have shipped a
// dead verb over a fixable one.
//
// The measurement runs on an entry point 5.8 still has: FEngineLoop::Tick calls
// FTSTicker::GetCoreTicker().Tick(FApp::GetDeltaTime()) once per frame
// (LaunchEngineLoop.cpp:6103) and FTSTicker::Tick hands that same delta to every element due
// this frame (Ticker.cpp:121), so a zero-delay ticker element IS a per-frame frame-time sampler.
// No stats thread, no engine rebuild, no version gate.
//
// The stat-file capture is kept as a SECONDARY artefact where the engine still compiles it in,
// reported as statFileCaptured/statFilePath, and its absence is a warning rather than the whole
// answer.

// Per-frame sampling state for one run_benchmark measurement window. Game-thread only: created by
// the job bind and shared into the core-ticker delegate, which appends one frame per fire.
struct FPinWrightBenchmarkFrameWindow
{
    // One entry per sampled frame, in observation order, in milliseconds.
    TArray<double> FrameTimesMs;
    // Sum of the sampled deltas — the span the samples actually cover, published as
    // measuredDurationSeconds. Deliberately NOT wall clock: keeping the window and the samples on
    // the same clock is what makes avgFps == 1000/frameTimeMs.mean instead of two numbers that
    // can disagree.
    double MeasuredSeconds = 0.0;
    // Wall clock at window open, used only by the backstop below.
    double StartWallSeconds = 0.0;
};

// Reduce one window's per-frame samples to the published `frameTimeMs` block. Takes the array by
// value and sorts it so the percentile reads are index lookups. Returns null when there is nothing
// to summarize, so the caller OMITS the block rather than publishing a zeroed one.
static TSharedPtr<FJsonObject> PinWrightSummarizeBenchmarkFrameTimesMs(TArray<double> SamplesMs)
{
    if (SamplesMs.Num() == 0)
    {
        return nullptr;
    }
    SamplesMs.Sort();

    double TotalMs = 0.0;
    for (const double SampleMs : SamplesMs)
    {
        TotalMs += SampleMs;
    }

    // Nearest-rank percentile (index = ceil(F * N) - 1, clamped), no interpolation: every number
    // published here is the duration of a frame that was actually observed. p50/p95 are carried
    // alongside the mean because a stalling frame can only ADD time, so a mean alone hides the
    // spikes that are the reason to run a benchmark at all.
    auto PercentileMs = [&SamplesMs](double Fraction) -> double
    {
        const int32 Index = FMath::Clamp(
            (int32)FMath::CeilToDouble(Fraction * (double)SamplesMs.Num()) - 1,
            0, SamplesMs.Num() - 1);
        return SamplesMs[Index];
    };

    TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
    Block->SetNumberField(TEXT("min"), SamplesMs[0]);
    Block->SetNumberField(TEXT("p50"), PercentileMs(0.50));
    Block->SetNumberField(TEXT("p95"), PercentileMs(0.95));
    Block->SetNumberField(TEXT("max"), SamplesMs.Last());
    Block->SetNumberField(TEXT("mean"), TotalMs / (double)SamplesMs.Num());
    return Block;
}

REGISTER_RPC_HANDLER("performance.run_benchmark", "performance", "Measure editor frame time over a window: samples every frame for `duration` seconds, then completes the job with the MEASURED frameCount, measuredDurationSeconds, avgFps and frameTimeMs {min, p50, p95, max, mean}. Async — the call returns a ticket; poll system.job_status for the measurement. Where the engine still compiles the legacy stat-file capture in, a .uestats is taken alongside and reported as statFileCaptured/statFilePath; UE 5.8 deprecated that capture out, so statFileCaptured is false there and a warning says so (the frame measurement is unaffected — use insights.start_session for a full trace). If no frame time could be observed the job FAILS with FRAME_TIME_NOT_MEASURED; it never completes successfully without numbers.",
    RPC_PARAMS(
        RPC_PARAM_OPT("duration", "number", "Length of the measurement window in seconds (default 5)")
    ))
{
    // `type` used to be declared here and read by nothing — an unvalidated, inert parameter that
    // advertised benchmark modes this verb has never had. Dropped rather than invented: the
    // dispatcher's UNKNOWN_PARAMS gate now rejects it loudly instead of accepting it silently.
    const double DurationSeconds = Ctx.GetNumber(TEXT("duration"), 5.0);
    if (!(DurationSeconds > 0.0))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("duration must be greater than 0 seconds (got %f)"), DurationSeconds));
        return true;
    }

    FJobBindArgs Args;
    Args.Method = TEXT("performance.run_benchmark");
    Args.StartedPayload = MakeShared<FJsonObject>();
    // Requested, never conflated with measured: the result publishes measuredDurationSeconds
    // beside this, and warns when the two disagree.
    Args.StartedPayload->SetNumberField(TEXT("requestedDurationSeconds"), DurationSeconds);

    Args.BindNativeDelegate =
        [DurationSeconds](FJobOnComplete OnComplete)
    {
#if PINWRIGHT_HAS_STATS_FILE_CAPTURE
        if (GEngine)
        {
            GEngine->Exec(nullptr, TEXT("stat startfile"));
        }
#endif
        TSharedRef<FPinWrightBenchmarkFrameWindow> Window =
            MakeShared<FPinWrightBenchmarkFrameWindow>();
        Window->StartWallSeconds = FPlatformTime::Seconds();

        // Delay 0.0f: the core ticker fires this element on EVERY frame and passes that frame's
        // delta (see the block comment above), which is the sample.
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [OnComplete, Window, DurationSeconds](float DeltaTime) mutable -> bool
        {
            Window->FrameTimesMs.Add((double)DeltaTime * 1000.0);
            Window->MeasuredSeconds += (double)DeltaTime;

            // The window closes on ACCUMULATED frame time so that the samples and
            // measuredDurationSeconds describe the same span. The wall-clock backstop exists
            // because a ticker driven with a zero delta would never accumulate and would leave
            // this element alive forever in a long-running editor; whichever condition trips
            // first closes the window, and the requested-vs-measured warning below is what tells
            // the caller the window was cut short.
            const double WallSeconds = FPlatformTime::Seconds() - Window->StartWallSeconds;
            if (Window->MeasuredSeconds < DurationSeconds && WallSeconds < DurationSeconds + 5.0)
            {
                return true;
            }

            FString StatFilePath;
#if PINWRIGHT_HAS_STATS_FILE_CAPTURE
            // Finalize the capture and resolve its .uestats path (same deterministic
            // finalize-and-resolve as stop_profiling — ordering owned by the helper), so a
            // stats-capturing benchmark says WHERE its stat file went. Not attempted at all when
            // the capture is compiled out: 'stat stopfile' does not parse there.
            StatFilePath = StopStatFileCaptureAndResolvePath(nullptr);
#endif

            const TSharedPtr<FJsonObject> FrameTimeMs =
                PinWrightSummarizeBenchmarkFrameTimesMs(Window->FrameTimesMs);
            if (!FrameTimeMs.IsValid() || Window->MeasuredSeconds <= 0.0)
            {
                // The exact shape this verb was fixed for: a caller builds on a completed job, so
                // when nothing was observed the job FAILS rather than publishing a zeroed or empty
                // measurement. Refusals log at Warning, never Error.
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("performance.run_benchmark: no frame time observed over %d sampled frame(s) (%f s accumulated); failing the job instead of completing it without a measurement."),
                    Window->FrameTimesMs.Num(), Window->MeasuredSeconds);
                OnComplete(false, nullptr, TEXT("FRAME_TIME_NOT_MEASURED"));
                return false;
            }

            auto R = MakeShared<FJsonObject>();
            R->SetNumberField(TEXT("requestedDurationSeconds"), DurationSeconds);
            R->SetNumberField(TEXT("measuredDurationSeconds"), Window->MeasuredSeconds);
            R->SetNumberField(TEXT("frameCount"), Window->FrameTimesMs.Num());
            R->SetObjectField(TEXT("frameTimeMs"), FrameTimeMs);
            R->SetNumberField(TEXT("avgFps"),
                (double)Window->FrameTimesMs.Num() / Window->MeasuredSeconds);
            // Whether a .uestats was actually produced — a fact about the stat file, kept plainly
            // separate from whether the benchmark measured anything.
            R->SetBoolField(TEXT("statFileCaptured"), !StatFilePath.IsEmpty());
            if (!StatFilePath.IsEmpty())
            {
                R->SetStringField(TEXT("statFilePath"), StatFilePath);
            }

            TArray<FString> Warnings;
            // Whole frames are sampled, so the window overruns `duration` by up to one frame; the
            // backstop can also close it early. Warn only when the gap is material.
            if (FMath::Abs(Window->MeasuredSeconds - DurationSeconds) > 0.1 * DurationSeconds)
            {
                Warnings.Add(FString::Printf(
                    TEXT("Measured window %.3fs differs from the requested %.3fs; frameCount, avgFps and frameTimeMs describe the MEASURED window."),
                    Window->MeasuredSeconds, DurationSeconds));
            }
#if !PINWRIGHT_HAS_STATS_FILE_CAPTURE
            Warnings.Add(FString(
                TEXT("No .uestats file: the legacy stat-file capture ('stat startfile') is compiled out on this engine (deprecated in UE 5.8). The frame measurement above is unaffected; use insights.start_session for a full trace.")));
#endif
            if (Warnings.Num() > 0)
            {
                R->SetArrayField(TEXT("warnings"), EmitStringArray(Warnings));
            }

            OnComplete(true, R, FString());
            return false;
        }), 0.0f);
    };
    Ctx.StartJob(Args);
    return true;
}

// ---- performance.enable_gpu_timing ----
REGISTER_RPC_HANDLER("performance.enable_gpu_timing", "performance", "Enable or disable GPU timing stats",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Enable GPU timing (default true)")
    ))
{
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);

    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUStatsEnabled"));
    CVarPriorityPreservingSet::SetPreservingPriority(CVar, bEnabled ? 1 : 0);

    if (bEnabled)
    {
        if (!GEditor)
        {
            Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor not available"));
            return true;
        }
        GEngine->Exec(GEditor->GetEditorWorldContext().World(), TEXT("stat gpu"));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("enabled"), bEnabled);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- performance.apply_baseline_settings ----
REGISTER_RPC_HANDLER("performance.apply_baseline_settings", "performance", "Set a curated bundle of r.* render CVars (VSync, HDR, motion blur, DoF, bloom, shadow, anisotropy) tuned for performance, quality, or balanced. Does NOT change the sg.* scalability groups — use performance.set_scalability for those. Six of the seven CVars nevertheless carry ECVF_Scalability and belong to the PostProcessQuality, ShadowQuality and TextureQuality groups, so every write goes in at the CVar's EXISTING SetBy priority rather than the default ECVF_SetByCode: the call cannot pin them above the ECVF_SetByScalability priority the editor's own Settings > Engine Scalability Settings panel writes at, which the response reports as scalabilityCVarsPinned:false. The response reports the exact CVar names and their values RE-READ after the write, so a value the engine clamped or a write a higher priority discarded is reported as it ended up, not as it was requested.",
    RPC_PARAMS(
        RPC_PARAM_OPT("profile", "string", "'performance' (lower quality, higher fps), 'quality' (cinematic), or 'balanced' (default sensible mid).")
    ))
{
    FString Profile = Ctx.GetString(TEXT("profile"), TEXT("balanced"));

    // Collects each CVar this profile actually applies so the response can report
    // exactly what changed (instead of a bare echo of the requested profile).
    //
    // Two properties of this lambda are load-bearing, both from board
    // B-performance-typed-verbs-pin-scalability-cvars:
    //
    //  1. The write goes in at the CVar's EXISTING SetBy priority, not the ECVF_SetByCode a bare
    //     Set() would use. Six of the seven CVars below carry ECVF_Scalability — r.VSync
    //     (ConsoleManager.cpp:4330/:4334), r.MotionBlurQuality (:4126/:4130),
    //     r.DepthOfFieldQuality (:4173/:4181), r.BloomQuality (:4082/:4091), r.ShadowQuality
    //     (:4119/:4123), r.MaxAnisotropy (:4361/:4364) — spanning PostProcessQuality,
    //     ShadowQuality and TextureQuality, so a Code-priority write here took the user's
    //     Scalability panel away for those three groups for the rest of the session. See
    //     Handlers/CVarPriorityPreservingSet.h for why the existing priority and not a fixed
    //     ECVF_SetByScalability.
    //  2. The reported value is RE-READ from the CVar after the write, not the requested value.
    //     They differ whenever the engine clamps, or whenever CanChange discarded the write
    //     because something already sits higher; echoing the request would report a write that
    //     never landed as applied.
    TArray<TSharedPtr<FJsonValue>> Applied;
    auto SetCVar = [&Applied](const TCHAR* Name, int32 Value)
    {
        if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            CVarPriorityPreservingSet::SetPreservingPriority(CVar, Value);
            Applied.Add(MakePerformanceCVarEntry(Name, CVar->GetInt()));
        }
    };

    if (Profile.Equals(TEXT("performance"), ESearchCase::IgnoreCase))
    {
        SetCVar(TEXT("r.VSync"), 0);
        SetCVar(TEXT("r.AllowHDR"), 0);
        SetCVar(TEXT("r.MotionBlurQuality"), 0);
        SetCVar(TEXT("r.DepthOfFieldQuality"), 0);
        SetCVar(TEXT("r.BloomQuality"), 0);
        SetCVar(TEXT("r.ShadowQuality"), 1);
        SetCVar(TEXT("r.MaxAnisotropy"), 4);
    }
    else if (Profile.Equals(TEXT("quality"), ESearchCase::IgnoreCase))
    {
        SetCVar(TEXT("r.VSync"), 1);
        SetCVar(TEXT("r.AllowHDR"), 1);
        SetCVar(TEXT("r.MotionBlurQuality"), 4);
        SetCVar(TEXT("r.DepthOfFieldQuality"), 2);
        SetCVar(TEXT("r.BloomQuality"), 5);
        SetCVar(TEXT("r.ShadowQuality"), 5);
        SetCVar(TEXT("r.MaxAnisotropy"), 16);
    }
    else
    {
        SetCVar(TEXT("r.VSync"), 1);
        SetCVar(TEXT("r.AllowHDR"), 1);
        SetCVar(TEXT("r.MotionBlurQuality"), 2);
        SetCVar(TEXT("r.DepthOfFieldQuality"), 1);
        SetCVar(TEXT("r.BloomQuality"), 3);
        SetCVar(TEXT("r.ShadowQuality"), 3);
        SetCVar(TEXT("r.MaxAnisotropy"), 8);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("profile"), Profile);
    Resp->SetArrayField(TEXT("appliedCVars"), Applied);
    // Make the no-op on scalability groups explicit (machine-checkable) so a caller
    // cannot read a success here as "the sg.* groups were reset". The corrected
    // description and the appliedCVars report already carry the r.*-only scope in
    // prose, so no redundant prose note is added here.
    Resp->SetBoolField(TEXT("scalabilityGroupsChanged"), false);
    // scalabilityGroupsChanged:false was literally true and materially misleading on its own: no
    // sg.* value moved, and yet six ECVF_Scalability CVars across three groups came out of this
    // call pinned at ECVF_SetByCode, above the priority the user's Scalability panel writes at.
    // A caller reading the first field for reassurance got the reassurance and the damage. This
    // second field is the one that answers the question they were actually asking, and it is
    // false because SetPreservingPriority above cannot raise a CVar's priority.
    Resp->SetBoolField(TEXT("scalabilityCVarsPinned"), false);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- performance.optimize_draw_calls ----
REGISTER_RPC_HANDLER("performance.optimize_draw_calls", "performance", "Configure draw call optimization CVars",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Enable cached draw commands (default true)"),
        RPC_PARAM_OPT("instancing", "boolean", "Enable dynamic instancing (default true)")
    ))
{
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);
    bool bInstancing = Ctx.GetBool(TEXT("instancing"), true);

    // Neither of these two carries ECVF_Scalability, but the write rule is the file's rule: a
    // bare Set() stamps ECVF_SetByCode, which outranks anything below it for the session. See
    // Handlers/CVarPriorityPreservingSet.h.
    auto SetCVar = [](const TCHAR* Name, int32 Value)
    {
        CVarPriorityPreservingSet::SetPreservingPriority(
            IConsoleManager::Get().FindConsoleVariable(Name), Value);
    };

    SetCVar(TEXT("r.MeshDrawCommands.DynamicInstancing"), bInstancing ? 1 : 0);
    SetCVar(TEXT("r.MeshDrawCommands.UseCachedCommands"), bEnabled ? 1 : 0);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("optimized"), bEnabled);
    Resp->SetBoolField(TEXT("instancing"), bInstancing);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- performance.configure_occlusion_culling ----
REGISTER_RPC_HANDLER("performance.configure_occlusion_culling", "performance", "Configure occlusion culling settings",
    RPC_PARAMS(
        RPC_PARAM_OPT("enabled", "boolean", "Enable occlusion queries (default true)"),
        RPC_PARAM_OPT("slop", "number", "Occlusion slop value"),
        RPC_PARAM_OPT("minScreenRadius", "number", "Minimum screen radius for occlusion")
    ))
{
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);

    double OcclusionSlop = 0.0;
    bool bHasSlop = Ctx.GetRawPayload()->TryGetNumberField(TEXT("slop"), OcclusionSlop);

    double MinScreenRadiusForOcclusion = 0.0;
    bool bHasMinRadius = Ctx.GetRawPayload()->TryGetNumberField(TEXT("minScreenRadius"), MinScreenRadiusForOcclusion);

    // Same write rule as the rest of the file: at the CVar's existing SetBy priority, never at
    // the default ECVF_SetByCode. See Handlers/CVarPriorityPreservingSet.h.
    auto SetCVar = [](const TCHAR* Name, int32 Value)
    {
        CVarPriorityPreservingSet::SetPreservingPriority(
            IConsoleManager::Get().FindConsoleVariable(Name), Value);
    };

    auto SetCVarFloat = [](const TCHAR* Name, float Value)
    {
        CVarPriorityPreservingSet::SetPreservingPriority(
            IConsoleManager::Get().FindConsoleVariable(Name), Value);
    };

    SetCVar(TEXT("r.AllowOcclusionQueries"), bEnabled ? 1 : 0);

    if (bHasSlop)
        SetCVarFloat(TEXT("r.OcclusionSlop"), (float)OcclusionSlop);

    if (bHasMinRadius)
        SetCVarFloat(TEXT("r.OcclusionCullMinScreenRadius"), (float)MinScreenRadiusForOcclusion);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("enabled"), bEnabled);
    if (bHasSlop)
        Resp->SetNumberField(TEXT("slop"), OcclusionSlop);
    if (bHasMinRadius)
        Resp->SetNumberField(TEXT("minScreenRadius"), MinScreenRadiusForOcclusion);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- performance.optimize_shaders ----
REGISTER_RPC_HANDLER("performance.optimize_shaders", "performance", "Recompile shaders by mode",
    RPC_PARAMS(
        RPC_PARAM_OPT("mode", "string", "Recompile mode: 'changed', 'material', 'global'"),
        RPC_PARAM_OPT("forceRecompile", "boolean", "Force recompile all shaders")
    ))
{
    FString Mode = Ctx.GetString(TEXT("mode"), TEXT("changed"));
    bool bForceRecompile = Ctx.GetBool(TEXT("forceRecompile"), false);

    FString Cmd;
    if (bForceRecompile)
        Cmd = TEXT("all");
    else if (Mode.Equals(TEXT("material"), ESearchCase::IgnoreCase))
        Cmd = TEXT("material");
    else if (Mode.Equals(TEXT("global"), ESearchCase::IgnoreCase))
        Cmd = TEXT("global");
    else
        Cmd = TEXT("changed");

    FJobBindArgs Args;
    Args.Method = TEXT("performance.optimize_shaders");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("mode"), Cmd);

    Args.BindNativeDelegate =
        [Cmd](FJobOnComplete OnComplete)
    {
        if (GEngine)
        {
            GEngine->Exec(nullptr, *FString::Printf(TEXT("recompileshaders %s"), *Cmd));
        }
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [OnComplete](float) mutable -> bool
        {
            if (!GShaderCompilingManager || !GShaderCompilingManager->IsCompiling())
            {
                auto R = MakeShared<FJsonObject>();
                R->SetBoolField(TEXT("compiled"), true);
                OnComplete(true, R, FString());
                return false;
            }
            return true;
        }), 0.5f);
    };
    Ctx.StartJob(Args);
    return true;
}
