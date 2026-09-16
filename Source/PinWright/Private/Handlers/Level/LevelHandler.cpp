// Copyright (c) 2026 Alexander Penkin. MIT License.

// LevelHandler.cpp - Migrated from PinWright_LevelHandlers.cpp
// Level loading, saving, streaming, creation, deletion, export/import,
// sublevel management, visibility, locking, info queries, and build operations

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Level/LevelBuildBinds.h"
#include "Handlers/Level/LevelLoadDiagnostics.h"
#include "Handlers/Level/LevelNameParamUtils.h"
#include "Handlers/Level/MapSwapGuardRefusal.h"
#include "Dispatch/SafePoint.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Dom/JsonObject.h"

#include "ActorEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Brush.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "LevelEditor.h"
#include "LightingBuildOptions.h"
#include "Misc/ConfigCacheIni.h"
#include "RenderingThread.h"
#include "GameFramework/WorldSettings.h"
#include "Utils/ActorUtils.h"
#include "Utils/AssetDeletePolicy.h"
#include "Utils/AssetDumpSuggestion.h"
#include "Utils/MapSwapDirtyWorldGuard.h"
#include "Utils/PieState.h"
#include "Engine/LevelBounds.h"
#include "LevelUtils.h"
#include "EditorBuildUtils.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"

#if defined(__has_include)
#if __has_include("Subsystems/LevelEditorSubsystem.h")
#include "Subsystems/LevelEditorSubsystem.h"
#define MCP_LEVEL_HAS_LEVELEDITOR_SUBSYSTEM 1
#elif __has_include("LevelEditorSubsystem.h")
#include "LevelEditorSubsystem.h"
#define MCP_LEVEL_HAS_LEVELEDITOR_SUBSYSTEM 1
#else
#define MCP_LEVEL_HAS_LEVELEDITOR_SUBSYSTEM 0
#endif
#else
#define MCP_LEVEL_HAS_LEVELEDITOR_SUBSYSTEM 0
#endif

// Dedicated category for this handler's save-persistence diagnostics, matching the
// pattern create_level uses (LogMcpLevelStructureHandler) instead of the generic LogTemp.
DEFINE_LOG_CATEGORY_STATIC(LogMcpLevelHandler, Log, All);

// Helper: get all levels from a world (persistent + streaming)
static TArray<ULevel*> GetAllLevelsFromWorldLevel(UWorld* World)
{
    TArray<ULevel*> Levels;
    if (!World) return Levels;
    if (World->PersistentLevel)
    {
        Levels.Add(World->PersistentLevel);
    }
    for (const ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
    {
        if (StreamingLevel)
        {
            ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();
            if (LoadedLevel)
            {
                Levels.Add(LoadedLevel);
            }
        }
    }
    return Levels;
}

// Helper: find a level by path in the world
static ULevel* FindLevelByPathLevel(UWorld* World, const FString& LevelPath)
{
    if (LevelPath.IsEmpty())
        return World ? World->GetCurrentLevel() : nullptr;

    TArray<ULevel*> Levels = GetAllLevelsFromWorldLevel(World);
    for (ULevel* Level : Levels)
    {
        if (Level && Level->GetOutermost() && Level->GetOutermost()->GetName() == LevelPath)
        {
            return Level;
        }
    }
    return nullptr;
}

// Helper: dispatch to another registered handler
static bool CrossDispatchLevel(FHandlerContext& Ctx, const FString& MethodName, const TSharedPtr<FJsonObject>& Payload)
{
    auto* Sub = Ctx.GetSubsystem();
    if (Sub)
    {
        return Sub->DispatchMethod(MethodName, Ctx.GetRequestId(), Payload);
    }
    Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_NOT_FOUND, TEXT("Subsystem not available for dispatch"));
    return true;
}

// Helper: ~10s heartbeat progress for the async editor-build jobs (lighting /
// navigation / build_all), so a streaming client sees liveness between the
// job's start and its completion delegate. The completion binds live in
// LevelBuildBinds.h and are shared / not job-aware, so the heartbeat lives here
// beside the StartJob call and polls the same public engine flags those
// watchdogs use. Captures only copyable state (strings + start time) and
// re-resolves the editor world each tick. RecordProgress returns false once the
// ticket leaves "running" (completed / failed / cancelled), which self-removes
// the ticker on every terminal path including cancellation.
static void AddLevelBuildHeartbeat(const FString& TicketId, const FString& Label)
{
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [TicketId, Label, StartSeconds = FPlatformTime::Seconds()](float) -> bool
    {
        const int32 Elapsed = FMath::RoundToInt(FPlatformTime::Seconds() - StartSeconds);
        auto Progress = MakeShared<FJsonObject>();
        Progress->SetNumberField(TEXT("elapsedSeconds"), Elapsed);
        Progress->SetBoolField(TEXT("buildRunning"),
            FEditorBuildUtils::IsBuildCurrentlyRunning());
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (World)
        {
            if (UNavigationSystemV1* NavSys = UNavigationSystemV1::GetNavigationSystem(World))
            {
                Progress->SetBoolField(TEXT("navBuildRunning"),
                    NavSys->IsNavigationBuildInProgress());
            }
        }
        return FPluginState::Get().GetJobRegistry().RecordProgress(
            TicketId,
            FString::Printf(TEXT("%s running, %ds elapsed"), *Label, Elapsed),
            Progress, /*bBypassRateLimit=*/true);
    }), 10.0f);
}

// ---- level.load ----
REGISTER_RPC_HANDLER("level.load", "level", "Load a level package and make it the active editor world. Synchronous: the call returns once the map is fully loaded (editor.open_level is the editor-domain alias). Pass a /Game/-prefixed package path or a bare short name. REFUSES with DIRTY_WORLD_BLOCKS_MAP_SWAP, changing nothing, when the requested map is ALREADY loaded in this editor with unsaved changes: the engine cannot unload a dirty package and its Map_Load then hits an unconditional Fatal that kills the whole editor process. The refusal names the blocking package; save it (saveDirtyTargetWorld:true, or editor.save_all) or discard its edits, then retry.",
    RPC_PARAMS(
        RPC_PARAM_REQ("levelPath", "path", "Package path (e.g. /Game/Maps/MyLevel) or bare short name. Bare names are resolved under /Game/."),
        RPC_PARAM_DEF("saveDirtyTargetWorld", "boolean", "When the requested map is already resident with unsaved changes, write that ONE package to disk first and then load it, instead of refusing with DIRTY_WORLD_BLOCKS_MAP_SWAP. Keeps the unsaved edits (they are saved, then re-read), and is narrower than editor.save_all, which also writes every other dirty package. Off by default because in a shared editor the dirty map is often another caller's work in progress. The response reports savedDirtyTargetWorld/savedPackage when it fired; if the save does not actually clear the block the call still refuses.", "false")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString LevelPath;
    Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
    if (LevelPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("levelPath required"));
        return true;
    }

    // Auto-resolve short names
    if (!LevelPath.StartsWith(TEXT("/")) && !FPaths::FileExists(LevelPath))
    {
        FString TryPath = FString::Printf(TEXT("/Game/Maps/%s"), *LevelPath);
        if (FPackageName::DoesPackageExist(TryPath))
        {
            LevelPath = TryPath;
        }
    }

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    if (UWorld* CurrentWorld = GEditor->GetEditorWorldContext().World())
    {
        const FString CurrentPackageName = CurrentWorld->GetOutermost()
            ? CurrentWorld->GetOutermost()->GetName()
            : FString();
        if (DoesRequestedLevelMatchCurrentWorld(LevelPath, CurrentPackageName, CurrentWorld->GetMapName()))
        {
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("alreadyLoaded"), true);
            Resp->SetStringField(TEXT("levelPath"), CurrentPackageName);
            VerifyAssetExists(Resp, CurrentPackageName);
            Ctx.SendSuccess(Resp);
            return true;
        }
    }

    // Try to resolve package path to filename
    FString Filename;
    bool bGotFilename = false;
    if (FPackageName::IsPackageFilename(LevelPath))
    {
        Filename = LevelPath;
        bGotFilename = true;
    }
    else
    {
        if (FPackageName::TryConvertLongPackageNameToFilename(
                LevelPath, Filename, FPackageName::GetMapPackageExtension()))
        {
            bGotFilename = true;
        }
    }

    const FString FileToLoad = bGotFilename ? Filename : LevelPath;

    // Verify a .umap exists on disk via the shared mount-aware probe (the same
    // helper the read-only getters classify a miss with).
    if (!DoesLevelMapExistOnDisk(LevelPath))
    {
        // No .umap on disk. Distinguish a genuinely-missing path from a world that
        // is registered / live in memory but never persisted — the latter cannot be
        // loaded from disk, and a flat FILE_NOT_FOUND (a path-resolution-flavored
        // error) drives the caller into path-form trial-and-error instead of telling
        // them to save or discard the orphan. Mirrors the asset.dump split of
        // ASSET_FILE_MISSING vs ASSET_LOAD_FAILED. Shared with editor.open_level.
        return SendLevelNotLoadableError(Ctx, LevelPath, LevelPath);
    }

    // FEditorFileUtils::LoadMap is a synchronous, game-thread-bound call: by the
    // time it returns the map is fully loaded and actors are ready. The response is
    // still emitted from the same place it always was — immediately after LoadMap
    // returns — so this method keeps its documented "Synchronous" contract and does
    // NOT reintroduce the leaked-job bug the old Ctx.StartJob + OnMapOpened wrapper
    // caused (B-level-load-no-completion-signal): completion is never tied to the
    // OnMapOpened delegate, which does not fire on several LoadMap early-out paths.
    //
    // What DID change: LoadMap may not run on an arbitrary game-thread stack.
    // Requests arrive on the socket I/O thread and are marshalled with
    // AsyncTask(ENamedThreads::GameThread, ...) (RpcDispatcher.cpp:377-385), and the
    // game thread drains that queue from inside UWorld::Tick while it waits on tick
    // groups. LoadMap tears the outgoing world down synchronously and GCs its ULevel,
    // which is illegal mid-frame — the tick-task manager still lists that level, and
    // ~ULevel trips `check(!LevelList.Contains(TickTaskLevel))`, killing the editor.
    // The swap therefore runs inline only when this stack is already outside a world
    // tick, and otherwise hops to the next core-ticker pass, which the engine loop
    // pumps after the world tick has ended. Full evidence, with engine file:line
    // citations, is in Dispatch/SafePoint.h.
    //
    // editor.open_level and editor.open_asset (World) both cross-dispatch here, so
    // this single call site covers all three verbs. That cross-dispatch is also why
    // level.load gates IN THE HANDLER rather than through the dispatcher's
    // tick-unsafe method table: FRpcDispatcher::DispatchMethod bypasses
    // ProcessRequest, so a table entry would not fire for the forwarded calls.
    auto PerformSwapAndBuildResponse = [LevelPath, FileToLoad](bool bDeferred)
    {
        FlushRenderingCommands();
        FEditorFileUtils::LoadMap(FileToLoad);

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("levelPath"), LevelPath);
        if (UWorld* LoadedWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
        {
            const FString LoadedPackage = LoadedWorld->GetOutermost()
                ? LoadedWorld->GetOutermost()->GetName()
                : FString();
            Resp->SetBoolField(TEXT("loaded"),
                DoesRequestedLevelMatchCurrentWorld(LevelPath, LoadedPackage, LoadedWorld->GetMapName()));
            Resp->SetStringField(TEXT("activeLevelPath"), LoadedPackage);
        }
        // Observable proof of which branch ran, so a caller (or a later runtime
        // verification pass) can tell a safe-point hop from an inline load.
        Resp->SetBoolField(TEXT("deferredToSafePoint"), bDeferred);
        VerifyAssetExists(Resp, LevelPath);
        return Resp;
    };

    const bool bSaveDirtyTargetWorld = Ctx.GetBool(TEXT("saveDirtyTargetWorld"), false);

    return PinWrightSafePoint::RunAtSafePoint(Ctx,
        TEXT("level.load: FEditorFileUtils::LoadMap tears the outgoing world down and GCs its ULevel"),
        [PerformSwapAndBuildResponse, LevelPath, FileToLoad, bSaveDirtyTargetWorld]
        (const PinWrightSafePoint::FSafePointResponder& Responder)
    {
        // The dirty-target-world precondition. Deferring to a safe point fixed the
        // tick-reentrancy assertion but not this: Map_Load cannot unload a DIRTY resident
        // copy of the map it is being asked to open, and appErrors when it fails to
        // ("World Memory Leaks"), killing the process and every other agent's session with
        // it. Evidence and the exact reachability are in Utils/MapSwapDirtyWorldGuard.h.
        //
        // Probed HERE rather than before RunAtSafePoint on purpose: on the deferred path a
        // pre-check would be a tick old, and in a shared editor another stream can dirty the
        // target in that window. This is the last instruction before LoadMap.
        PinWrightMapSwapGuard::FTargetWorldState TargetState =
            PinWrightMapSwapGuard::ProbeTargetWorld(FileToLoad);
        bool bSavedBlockingWorld = false;
        if (PinWrightMapSwapGuard::WouldMapLoadFatal(TargetState))
        {
            FString SaveFailure;
            bSavedBlockingWorld = bSaveDirtyTargetWorld
                && PinWrightMapSwapGuard::SaveBlockingWorldPackage(TargetState, SaveFailure);
            if (!bSavedBlockingWorld)
            {
                TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
                ErrorData->SetStringField(TEXT("levelPath"), LevelPath);
                ErrorData->SetStringField(TEXT("blockingPackage"), TargetState.PackageName);
                ErrorData->SetBoolField(TEXT("packageDirty"), TargetState.bPackageDirty);
                ErrorData->SetBoolField(TEXT("worldFound"), TargetState.bWorldFound);
                ErrorData->SetBoolField(TEXT("worldSurvivesGarbageCollect"),
                    TargetState.bWorldSurvivesEditorCollect);
                ErrorData->SetBoolField(TEXT("worldEverInitialized"),
                    TargetState.bWorldEverInitialized);
                ErrorData->SetBoolField(TEXT("saveAttempted"), bSaveDirtyTargetWorld);
                if (!SaveFailure.IsEmpty())
                {
                    ErrorData->SetStringField(TEXT("saveFailure"), SaveFailure);
                }
                Responder.SendError(ErrorCodes::ERR_DIRTY_WORLD_BLOCKS_MAP_SWAP,
                    PinWrightMapSwapGuard::DescribeRefusal(TargetState, SaveFailure), ErrorData);
                return;
            }
        }

        // The SECOND fatal on this path, and the one the guard above cannot see: Map_Load
        // tears the outgoing world down, collects, and then walks EVERY resident UWorld
        // (CheckForWorldGCLeaks) — anything still there that belongs to no world context
        // and is not one of the types the editor keeps is logged Fatal by default
        // (Editor.CheckForWorldGCLeaksAreFatal). That check reads no dirty flag, so the
        // probe purges the dead worlds' external references (the Python wrapper registry is
        // the observed holder), runs the same collect the load would run, and refuses only
        // on what is actually still standing. True for the transaction-buffer argument:
        // Map_Load resets the undo buffer before it destroys the world, so a world only that
        // buffer holds is released in time and must not be refused.
        const PinWrightMapSwapGuard::FWorldSurvivorProbeResult SurvivorProbe =
            PinWrightMapSwapGuard::ProbeResidentWorldSurvivors(
                TargetState.PackageName, /*bTransactionBufferWillBeCleared=*/true);
        if (SurvivorProbe.bProbeUnavailable || SurvivorProbe.IsBlocked())
        {
            TSharedPtr<FJsonObject> ErrorData =
                PinWrightMapSwapGuard::BuildSurvivorErrorData(LevelPath, SurvivorProbe);
            if (bSavedBlockingWorld)
            {
                // A save already happened; the caller has to know that before retrying.
                ErrorData->SetBoolField(TEXT("savedDirtyTargetWorld"), true);
                ErrorData->SetStringField(TEXT("savedPackage"), TargetState.PackageName);
            }
            if (SurvivorProbe.bProbeUnavailable)
            {
                Responder.SendError(ErrorCodes::ERR_EDITOR_NOT_READY,
                    FString::Printf(
                        TEXT("Cannot verify the map swap is survivable right now: %s. Nothing "
                             "was loaded; retry when the editor is idle."),
                        *SurvivorProbe.UnavailableReason),
                    ErrorData);
            }
            else
            {
                Responder.SendError(ErrorCodes::ERR_DIRTY_WORLD_BLOCKS_MAP_SWAP,
                    PinWrightMapSwapGuard::DescribeSurvivorRefusal(SurvivorProbe), ErrorData);
            }
            return;
        }

        // Responder is the caller's own FHandlerContext on the inline path and an
        // FAsyncResponseToken on the deferred one, so both branches keep the exact
        // response route they had before the helper was extracted.
        TSharedPtr<FJsonObject> Resp = PerformSwapAndBuildResponse(Responder.IsDeferred());
        if (bSavedBlockingWorld)
        {
            // The opted-in write is a side effect the caller must be able to see happened.
            Resp->SetBoolField(TEXT("savedDirtyTargetWorld"), true);
            Resp->SetStringField(TEXT("savedPackage"), TargetState.PackageName);
        }
        Responder.SendSuccess(Resp);
    });
}

// ---- level.save ----
REGISTER_RPC_MUTATING_HANDLER("level.save", "level", "Save the active editor world's persistent level package. To save with a new name, use level.save_as.",
    RPC_NO_PARAMS)
{
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No world loaded"));
        return true;
    }

    ULevel* PersistentLevel = World->PersistentLevel;
    const FString PackageName = World->GetOutermost()->GetName();

    FJobBindArgs Args;
    Args.Method = TEXT("level.save");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("packageName"), PackageName);

    // Filled with the ticket id after Ctx.StartJob returns; the safe-point
    // continuation below is always deferred, so it observes the filled value.
    TSharedRef<FString> JobTicket = MakeShared<FString>();

    Args.BindNativeDelegate =
        [Ctx, WeakLevel = TWeakObjectPtr<ULevel>(PersistentLevel), PackageName, JobTicket](FJobOnComplete OnComplete)
    {
        PinWrightSafePoint::DeferJobToSafePoint(Ctx, TEXT("level.save"),
            [WeakLevel, PackageName, JobTicket, OnComplete]() mutable
            {
                ULevel* Live = WeakLevel.Get();
                if (!Live)
                {
                    OnComplete(false, nullptr, TEXT("LEVEL_GONE"));
                    return;
                }
                // The save itself is synchronous on the game thread (no ticker can
                // observe it mid-flight), so the one honest mid-job signal is this
                // milestone marking the moment the save phase actually starts.
                if (!JobTicket->IsEmpty())
                {
                    auto Progress = MakeShared<FJsonObject>();
                    Progress->SetStringField(TEXT("packageName"), PackageName);
                    FPluginState::Get().GetJobRegistry().RecordProgress(
                        *JobTicket,
                        FString::Printf(TEXT("saving %s"), *PackageName),
                        Progress, /*bBypassRateLimit=*/true);
                }
                const bool bSaveReported = McpSafeLevelSave(Live, PackageName, 5);

                // McpSafeLevelSave's shared ShouldTreatLevelSaveAsSuccess OR-policy
                // accepts a clean package / registry asset as success even when no
                // .umap landed on disk — for a freshly-created in-memory-only world
                // (CreatePackage'd but never written) that yields a false saved:true.
                // level.save promises on-disk persistence of the active world, so the
                // only honest signal is the file on disk. Re-gate saved:true through the
                // shared VerifyLevelSavedToDisk helper (mount-aware resolve + FileExists
                // probe + the stricter ShouldTreatCreateLevelSaveAsSuccess predicate that
                // create_level uses), leaving the lenient OR-policy and its package/mount
                // tests intact (B-level-save-saved-true-in-memory-no-umap).
                FString LevelFilename;
                FString ErrorCode;
                const bool bOk = VerifyLevelSavedToDisk(PackageName, bSaveReported, LevelFilename, ErrorCode);
                if (bSaveReported && !bOk)
                {
                    UE_LOG(LogMcpLevelHandler, Error,
                        TEXT("level.save: save reported success but no .umap on disk: %s (file=%s)"),
                        *PackageName, *LevelFilename);
                }

                auto R = MakeShared<FJsonObject>();
                R->SetBoolField(TEXT("saved"), bOk);
                R->SetStringField(TEXT("levelPath"), PackageName);
                if (bSaveReported && !bOk)
                {
                    R->SetStringField(TEXT("persistenceNote"),
                        TEXT("Save reported success but no .umap was written to disk — the active world is an in-memory-only level that was never persisted. Use level.save_as to write it to a /Game/ path."));
                }
                OnComplete(bOk, R, ErrorCode);
            },
            [OnComplete]() mutable
            {
                OnComplete(false, nullptr, FString::Printf(
                    TEXT("%s: The dispatcher/request context ended before the deferred "
                         "level.save operation began."),
                    ErrorCodes::ERR_SAVE_FAILED));
            });
    };
    *JobTicket = Ctx.StartJob(Args);
    return true;
}

// ---- level.save_as ----
REGISTER_RPC_MUTATING_HANDLER("level.save_as", "level", "Save the active editor world's persistent level under a new package path (Save As). The original package is left untouched.",
    RPC_PARAMS(
        RPC_PARAM_REQ("savePath", "path", "Destination package path for the saved level, e.g. /Game/Maps/MyLevel_Copy.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString SavePath;
    Payload->TryGetStringField(TEXT("savePath"), SavePath);
    if (SavePath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("savePath required for save_level_as"));
        return true;
    }

#if MCP_LEVEL_HAS_LEVELEDITOR_SUBSYSTEM
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    if (ULevelEditorSubsystem* LevelEditorSS = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
    {
        FJobBindArgs Args;
        Args.Method = TEXT("level.save_as");
        Args.StartedPayload = MakeShared<FJsonObject>();
        Args.StartedPayload->SetStringField(TEXT("savePath"), SavePath);

        // Filled with the ticket id after Ctx.StartJob returns; the safe-point
        // continuation below is always deferred, so it observes the filled value.
        TSharedRef<FString> JobTicket = MakeShared<FString>();

        Args.BindNativeDelegate =
            [Ctx, SavePath, JobTicket](FJobOnComplete OnComplete)
        {
            PinWrightSafePoint::DeferJobToSafePoint(Ctx, TEXT("level.save_as"),
                [SavePath, JobTicket, OnComplete]() mutable
                {
                    // Synchronous game-thread save — the milestone marking the start
                    // of the save phase is the only observable mid-job signal.
                    if (!JobTicket->IsEmpty())
                    {
                        auto Progress = MakeShared<FJsonObject>();
                        Progress->SetStringField(TEXT("savePath"), SavePath);
                        FPluginState::Get().GetJobRegistry().RecordProgress(
                            *JobTicket,
                            FString::Printf(TEXT("saving %s"), *SavePath),
                            Progress, /*bBypassRateLimit=*/true);
                    }
                    bool bSaveReported = false;
#if __has_include("FileHelpers.h")
                    if (GEditor)
                    {
                        if (UWorld* World = GEditor->GetEditorWorldContext().World())
                        {
                            GEditor->ForceGarbageCollection(true);
                            bSaveReported = McpSafeLevelSave(World->PersistentLevel, SavePath, 5);
                        }
                    }
#endif
                    // Save As writes the active world to a new SavePath; like level.save
                    // the only honest persistence signal is the .umap on disk. The shared
                    // ShouldTreatLevelSaveAsSuccess OR-policy can report success for an
                    // in-memory-only world via the clean-package fallback with no file
                    // written, so re-gate saved:true through the shared VerifyLevelSavedToDisk
                    // helper (mount-aware resolve + FileExists probe + the stricter
                    // disk-presence predicate level.save / create_level use)
                    // (B-level-save-saved-true-in-memory-no-umap).
                    FString SavedFilename;
                    FString ErrorCode;
                    const bool bOk = VerifyLevelSavedToDisk(SavePath, bSaveReported, SavedFilename, ErrorCode);
                    if (bSaveReported && !bOk)
                    {
                        UE_LOG(LogMcpLevelHandler, Error,
                            TEXT("level.save_as: save reported success but no .umap on disk: %s (file=%s)"),
                            *SavePath, *SavedFilename);
                    }
                    // bOk implies the .umap is on disk, which implies the resolve filled
                    // SavedFilename; rescan the registry off that already-validated path.
                    if (bOk)
                    {
                        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
                        TArray<FString> FilesToScan;
                        FilesToScan.Add(SavedFilename);
                        AssetRegistry.ScanFilesSynchronous(FilesToScan, true);
                    }
                    auto R = MakeShared<FJsonObject>();
                    R->SetBoolField(TEXT("saved"), bOk);
                    R->SetStringField(TEXT("levelPath"), SavePath);
                    if (bSaveReported && !bOk)
                    {
                        R->SetStringField(TEXT("persistenceNote"),
                            TEXT("Save reported success but no .umap was written to disk — the active world could not be persisted to the requested path."));
                    }
                    OnComplete(bOk, R, ErrorCode);
                },
                [OnComplete]() mutable
                {
                    OnComplete(false, nullptr, FString::Printf(
                        TEXT("%s: The dispatcher/request context ended before the deferred "
                             "level.save_as operation began."),
                        ErrorCodes::ERR_SAVE_FAILED));
                });
        };
        *JobTicket = Ctx.StartJob(Args);
        return true;
    }
#endif
    Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_MISSING, TEXT("LevelEditorSubsystem not available"));
    return true;
}

// ---- level.create ----
REGISTER_RPC_HANDLER("level.create", "level", "Create a new empty (non-World-Partition) level asset. For World-Partition levels use level.structure.create_level instead. Specify either a full package path or a short name (resolved under /Game/Maps/).",
    RPC_PARAMS(
        LevelNameParamUtils::CreateNameParam(TEXT("Short name (accepts the 'name' alias); combined with /Game/Maps/ to form the package path. A value starting with '/' is used verbatim as the package path."), /*bRequired=*/false),
        RPC_PARAM_OPT("levelPath", "path", "Full destination package path; takes precedence over levelName when both are present.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();

    // Read the leaf/short name from whichever accepted key the caller used
    // ({levelName, name}) so the `name` alias resolves end-to-end.
    FString LevelName = LevelNameParamUtils::ResolveCreateName(Ctx);
    FString LevelPath;
    if (Payload)
    {
        Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
    }

    FString SavePath = LevelPath;
    if (SavePath.IsEmpty() && !LevelName.IsEmpty())
    {
        if (LevelName.StartsWith(TEXT("/")))
            SavePath = LevelName;
        else
            SavePath = FString::Printf(TEXT("/Game/Maps/%s"), *LevelName);
    }

    if (SavePath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("levelName or levelPath required for create_level"));
        return true;
    }

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    // Check if map already exists - if so, open it
    if (FPackageName::DoesPackageExist(SavePath))
    {
        // `Open <path>` is UEditorEngine::Map_Load by another spelling, so it reaches the
        // same post-cleanse leak check as the NewMap branch below and needs the same
        // precondition. It bypasses level.load, so it gets neither the dirty-target guard
        // nor the safe-point gate — this is the console route, and only the survivor probe
        // is portable to it.
        if (RefuseIfWorldsSurviveMapSwap(Ctx, SavePath, /*bTransactionBufferWillBeCleared=*/true))
        {
            return true;
        }
        const FString Cmd = FString::Printf(TEXT("Open %s"), *SavePath);
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("command"), Cmd);
        return CrossDispatchLevel(Ctx, TEXT("system.console_command"), P);
    }

#if MCP_LEVEL_HAS_LEVELEDITOR_SUBSYSTEM && __has_include("FileHelpers.h")
    if (GEditor->IsPlaySessionInProgress())
    {
        GEditor->RequestEndPlayMap();
        Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE, TEXT("Cannot create level while Play In Editor is active."));
        return true;
    }

    FlushRenderingCommands();
    // Safe by itself: ForceGarbageCollection only raises a flag, and the collect it
    // schedules runs from GEngine->ConditionalCollectGarbage() at LevelTick.cpp:1970,
    // AFTER `bInTick = false` (:1965) and after EndFrame (:1886).
    GEditor->ForceGarbageCollection(true);
    FlushRenderingCommands();

    // UEditorEngine::NewMap reaches EditorDestroyWorld -> Cleanse ->
    // CheckForWorldGCLeaks exactly as Map_Load does, so a dead world some referencer is
    // still holding kills the editor here too — a level.create issued in that state is the
    // shipped case on the board. The deferred collect above does not answer the question:
    // it only raises a flag. False for the transaction-buffer argument: NewMap resets the
    // undo buffer only after its leak check has run.
    if (RefuseIfWorldsSurviveMapSwap(Ctx, SavePath, /*bTransactionBufferWillBeCleared=*/false))
    {
        return true;
    }

    // NOT safe by itself, which is why `level.create` is listed in the tick-unsafe
    // method table in Dispatch/SafePoint.cpp: UEditorEngine::NewMap
    // (EditorServer.cpp:2187) calls EditorDestroyWorld (:2206) -> Cleanse (:2080) ->
    // CollectGarbage (EditorEngine.cpp:2859) -> ~ULevel -> FreeTickTaskLevel, which
    // asserts !LevelList.Contains(TickTaskLevel) if this runs inside UWorld::Tick.
    // The dispatcher re-queues the whole request onto the core ticker instead; do
    // not add a second gate here.
    //
    // bIsPartitionedWorld=false: level.create is documented and named as the
    // non-World-Partition verb (use level.structure.create_level for WP). Passing
    // true here built a WP world (external actors, no self-contained persistent
    // .umap), breaking the streaming-sublevel workflow its own docs promise.
    if (UWorld* NewWorld = GEditor->NewMap(false))
    {
        GEditor->GetEditorWorldContext().SetCurrentWorld(NewWorld);

        // A path that resolves to no mount root cannot be written at all, so a failed
        // conversion has to fail the request instead of being dropped on the floor.
        FString Filename;
        if (!FPackageName::TryConvertLongPackageNameToFilename(
                SavePath, Filename, FPackageName::GetMapPackageExtension()))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("Cannot resolve '%s' to a filename — the mount root is not registered."), *SavePath));
            return true;
        }
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);

        // SaveMap takes a FILESYSTEM filename. Passing the package path made the
        // leading '/' read as rooted, so Windows resolved it against the current
        // drive and wrote the .umap outside the project while still reporting
        // success (B-level-save-package-path-as-filename).
        const bool bSaveReported = FEditorFileUtils::SaveMap(NewWorld, Filename);

        // level.create was the only save verb with no on-disk gate — it answered
        // success on SaveMap's word alone. Route it through the same shared
        // disk-presence check level.save / level.save_as use
        // (B-level-save-verify-existence-not-freshness).
        FString SavedFilename;
        FString ErrorCode;
        const bool bOk = VerifyLevelSavedToDisk(SavePath, bSaveReported, SavedFilename, ErrorCode);
        if (bOk)
        {
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetStringField(TEXT("levelPath"), SavePath);
            Resp->SetStringField(TEXT("packagePath"), SavePath);
            Resp->SetStringField(TEXT("objectPath"),
                SavePath + TEXT(".") + FPaths::GetBaseFilename(SavePath));
            Ctx.SendSuccess(Resp);
        }
        else
        {
            UE_LOG(LogMcpLevelHandler, Error,
                TEXT("level.create: no .umap on disk after save (saveReported=%s): %s (file=%s)"),
                bSaveReported ? TEXT("true") : TEXT("false"), *SavePath, *Filename);
            Ctx.SendError(ErrorCode, TEXT("Failed to save new level"));
        }
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED, TEXT("Failed to create new map"));
    }
    return true;
#else
    const FString Cmd = FString::Printf(TEXT("Open %s"), *SavePath);
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("command"), Cmd);
    return CrossDispatchLevel(Ctx, TEXT("system.console_command"), P);
#endif
}

// ---- level.stream ----
REGISTER_RPC_HANDLER("level.stream", "level", "Toggle a streaming sublevel's loaded and visible state independently. Runtime/PIE-only: wraps the StreamLevel console command, which has no editor-time consumer and returns RUNTIME_ONLY_COMMAND at editor authoring time. For editor-time work use level.set_visibility (visible flag) and level.add_sublevel / level.remove_from_world / level.structure.configure_level_streaming (loaded flag).",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("levelName", "string", "Sublevel name or package path to stream.", "levelPath"),
        RPC_PARAM_OPT("shouldBeLoaded", "boolean", "Whether the sublevel should be loaded into memory. Defaults to true."),
        RPC_PARAM_OPT("shouldBeVisible", "boolean", "Whether the sublevel should be visible (rendered) once loaded. Defaults to true.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString LevelName;
    bool bLoad = true;
    bool bVis = true;
    Payload->TryGetStringField(TEXT("levelName"), LevelName);
    Payload->TryGetBoolField(TEXT("shouldBeLoaded"), bLoad);
    Payload->TryGetBoolField(TEXT("shouldBeVisible"), bVis);
    if (LevelName.IsEmpty())
        Payload->TryGetStringField(TEXT("levelPath"), LevelName);

    if (LevelName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("stream_level requires levelName or levelPath"));
        return true;
    }

    // level.stream cross-dispatches the StreamLevel console command, which only
    // has a consumer in a running game / PIE world's streaming subsystem. At
    // editor authoring time GEditor->Exec finds no handler, so it fails with a
    // bare [EXEC_FAILED] Command not executed that names neither this method nor
    // the runtime-only restriction. Short-circuit with an actionable redirect to
    // the editor-time verbs instead of letting the cryptic exec failure surface.
    const bool bPieActive = (GEditor && GEditor->PlayWorld != nullptr);
    if (!bPieActive)
    {
        Ctx.SendError(ErrorCodes::ERR_RUNTIME_ONLY_COMMAND,
            TEXT("level.stream wraps the runtime-only StreamLevel console command, which has no "
                 "editor-time consumer and fails at editor authoring time. Start a play session "
                 "(PIE) to use it, or use the editor-time verbs: level.set_visibility toggles the "
                 "visible flag; level.add_sublevel / level.remove_from_world and "
                 "level.structure.configure_level_streaming control the loaded flag."));
        return true;
    }

    const FString Cmd = FString::Printf(TEXT("StreamLevel %s %s %s"), *LevelName,
        bLoad ? TEXT("Load") : TEXT("Unload"),
        bVis ? TEXT("Show") : TEXT("Hide"));
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("command"), Cmd);
    return CrossDispatchLevel(Ctx, TEXT("system.console_command"), P);
}

// ---- level.build_lighting ----
REGISTER_RPC_HANDLER("level.build_lighting", "level", "Build static lighting / lightmaps for the active level at the requested quality. Long-running; runs as a job.",
    RPC_PARAMS(
        RPC_PARAM_OPT("quality", "string", "Lighting build quality: 'preview' (fast), 'medium', 'high', or 'production' (slowest, best). Defaults to engine setting.")
    ))
{
    FString Quality = Ctx.GetString(TEXT("quality"));
    // -1 = caller sent no quality: keep the editor's configured level (read from the ini below).
    int32 RequestedQuality = -1;
    FString QualityName;
    if (!Quality.IsEmpty())
    {
        const FString LowerQuality = Quality.ToLower();
        if (LowerQuality == TEXT("preview") || LowerQuality == TEXT("0"))
        {
            RequestedQuality = Quality_Preview;
            QualityName = TEXT("Preview");
        }
        else if (LowerQuality == TEXT("medium") || LowerQuality == TEXT("1"))
        {
            RequestedQuality = Quality_Medium;
            QualityName = TEXT("Medium");
        }
        else if (LowerQuality == TEXT("high") || LowerQuality == TEXT("2"))
        {
            RequestedQuality = Quality_High;
            QualityName = TEXT("High");
        }
        else if (LowerQuality == TEXT("production") || LowerQuality == TEXT("3"))
        {
            RequestedQuality = Quality_Production;
            QualityName = TEXT("Production");
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_UNKNOWN_QUALITY,
                FString::Printf(TEXT("Unknown lighting quality: %s. Valid: preview/0, medium/1, high/2, production/3"), *Quality));
            return true;
        }
    }

    FJobBindArgs Args;
    Args.Method = TEXT("level.build_lighting");
    Args.StartedPayload = MakeShared<FJsonObject>();
    if (!QualityName.IsEmpty())
        Args.StartedPayload->SetStringField(TEXT("quality"), QualityName);

    Args.BindNativeDelegate =
        [RequestedQuality](FJobOnComplete OnComplete)
    {
        BindLightingBuildCompletion()(OnComplete);
        if (!GEditor)
        {
            return;
        }

        UWorld* World = GEditor->GetEditorWorldContext().World();

        // Neither route into FEditorBuildUtils can carry a quality: the BuildLighting exec
        // discards its argument (UUnrealEdEngine::HandleBuildLightingCommand ignores Str), and
        // EditorBuild rebuilds FLightingBuildOptions from GEditorPerProjectIni itself. Load the
        // same ini block EditorBuild loads, override QualityLevel, and drive the build through
        // UEditorEngine::BuildLighting — the only entry point that takes the options struct.
        FLightingBuildOptions LightingBuildOptions;
        GConfig->GetBool(TEXT("LightingBuildOptions"), TEXT("OnlyBuildSelected"), LightingBuildOptions.bOnlyBuildSelected, GEditorPerProjectIni);
        GConfig->GetBool(TEXT("LightingBuildOptions"), TEXT("OnlyBuildCurrentLevel"), LightingBuildOptions.bOnlyBuildCurrentLevel, GEditorPerProjectIni);
        GConfig->GetBool(TEXT("LightingBuildOptions"), TEXT("OnlyBuildSelectedLevels"), LightingBuildOptions.bOnlyBuildSelectedLevels, GEditorPerProjectIni);
        GConfig->GetBool(TEXT("LightingBuildOptions"), TEXT("OnlyBuildVisibility"), LightingBuildOptions.bOnlyBuildVisibility, GEditorPerProjectIni);
        GConfig->GetBool(TEXT("LightingBuildOptions"), TEXT("UseErrorColoring"), LightingBuildOptions.bUseErrorColoring, GEditorPerProjectIni);
        GConfig->GetBool(TEXT("LightingBuildOptions"), TEXT("ShowLightingBuildInfo"), LightingBuildOptions.bShowLightingBuildInfo, GEditorPerProjectIni);

        int32 QualityLevel = Quality_Preview;
        GConfig->GetInt(TEXT("LightingBuildOptions"), TEXT("QualityLevel"), QualityLevel, GEditorPerProjectIni);
        if (RequestedQuality >= 0)
        {
            QualityLevel = RequestedQuality;
        }
        LightingBuildOptions.QualityLevel = static_cast<ELightingBuildQuality>(
            FMath::Clamp<int32>(QualityLevel, Quality_Preview, Quality_Production));

        // Lightmass exports BSP from the current model, so keep EditorBuild's pre-pass: rebuild
        // geometry first when the map has real (non-volume, non-builder) brushes.
        if (World)
        {
            for (TActorIterator<ABrush> BrushIt(World); BrushIt; ++BrushIt)
            {
                ABrush* Brush = *BrushIt;
                if (Brush && !Brush->IsVolumeBrush() && !Brush->IsBrushShape() && !FActorEditorUtils::IsABuilderBrush(Brush))
                {
                    GEditor->Exec(World, TEXT("MAP REBUILD ALLVISIBLE"));
                    break;
                }
            }
        }

        GEditor->BuildLighting(LightingBuildOptions);
    };
    AddLevelBuildHeartbeat(Ctx.StartJob(Args), TEXT("lighting build"));
    return true;
}

// ---- level.list ----
REGISTER_RPC_HANDLER("level.list", "level", "List the persistent level + every streaming sublevel in the active editor world, plus every UMap asset known to the asset registry. Useful as a discovery call before level.load / level.add_sublevel.",
    RPC_NO_PARAMS)
{
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> LevelsArray;

    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (World)
    {
        TSharedPtr<FJsonObject> CurrentLevel = MakeShared<FJsonObject>();
        CurrentLevel->SetStringField(TEXT("name"), World->GetMapName());
        CurrentLevel->SetStringField(TEXT("path"), World->GetOutermost()->GetName());
        CurrentLevel->SetBoolField(TEXT("isPersistent"), true);
        CurrentLevel->SetBoolField(TEXT("isLoaded"), true);
        CurrentLevel->SetBoolField(TEXT("isVisible"), true);
        LevelsArray.Add(MakeShared<FJsonValueObject>(CurrentLevel));

        for (const ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
        {
            if (!StreamingLevel) continue;
            TSharedPtr<FJsonObject> LevelEntry = MakeShared<FJsonObject>();
            LevelEntry->SetStringField(TEXT("name"), StreamingLevel->GetWorldAssetPackageName());
            LevelEntry->SetStringField(TEXT("path"), StreamingLevel->GetWorldAssetPackageFName().ToString());
            LevelEntry->SetBoolField(TEXT("isPersistent"), false);
            LevelEntry->SetBoolField(TEXT("isLoaded"), StreamingLevel->IsLevelLoaded());
            LevelEntry->SetBoolField(TEXT("isVisible"), StreamingLevel->IsLevelVisible());
            LevelEntry->SetStringField(TEXT("streamingState"),
                StreamingLevel->IsStreamingStatePending() ? TEXT("Pending")
                : StreamingLevel->IsLevelLoaded() ? TEXT("Loaded")
                : TEXT("Unloaded"));
            LevelsArray.Add(MakeShared<FJsonValueObject>(LevelEntry));
        }
    }

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
    TArray<FAssetData> MapAssets;
    AssetRegistry.GetAssetsByClass(
        FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("World")), MapAssets, false);

    TArray<TSharedPtr<FJsonValue>> AllMapsArray;
    for (const FAssetData& MapAsset : MapAssets)
    {
        TSharedPtr<FJsonObject> MapEntry = MakeShared<FJsonObject>();
        MapEntry->SetStringField(TEXT("name"), MapAsset.AssetName.ToString());
        MapEntry->SetStringField(TEXT("path"), MapAsset.PackageName.ToString());
        MapEntry->SetStringField(TEXT("objectPath"), MapAsset.GetObjectPathString());
        AllMapsArray.Add(MakeShared<FJsonValueObject>(MapEntry));
    }

    Resp->SetArrayField(TEXT("currentWorldLevels"), LevelsArray);
    Resp->SetNumberField(TEXT("currentWorldLevelCount"), LevelsArray.Num());
    Resp->SetArrayField(TEXT("allMaps"), AllMapsArray);
    Resp->SetNumberField(TEXT("allMapsCount"), AllMapsArray.Num());

    if (World)
    {
        Resp->SetStringField(TEXT("currentMap"), World->GetMapName());
        Resp->SetStringField(TEXT("currentMapPath"), World->GetOutermost()->GetName());
    }

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- level.export ----
REGISTER_RPC_HANDLER("level.export", "level", "Export a level to a text-format .t3d file on disk. Useful for diffing or copy-pasting actors across projects.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("exportPath", "filepath", "Filesystem path (project-relative or absolute) to write the .t3d file.", "destinationPath"),
        RPC_PARAM_OPT("levelPath", "path", "Package path of the level to export. Defaults to the active editor level.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString LevelPath;
    Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
    FString ExportPath;
    Payload->TryGetStringField(TEXT("exportPath"), ExportPath);
    if (ExportPath.IsEmpty())
        Payload->TryGetStringField(TEXT("destinationPath"), ExportPath);

    if (ExportPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("exportPath required"));
        return true;
    }

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    UWorld* WorldToExport = nullptr;
    if (!LevelPath.IsEmpty())
    {
        UWorld* Current = GEditor->GetEditorWorldContext().World();
        if (Current && (Current->GetOutermost()->GetName() == LevelPath ||
                        Current->GetPathName() == LevelPath))
        {
            WorldToExport = Current;
        }
    }
    if (!WorldToExport)
        WorldToExport = GEditor->GetEditorWorldContext().World();

    if (!WorldToExport)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No world loaded"));
        return true;
    }

    // Resolve a project-relative exportPath to an absolute path against the
    // project dir (matching AssetWorkflowHandler / the other output handlers).
    // ExportMap and IFileManager resolve relative paths against the engine CWD,
    // not FPaths::ProjectDir(), so without this the FileExists guard below and the
    // echoed exportPath could disagree with where the file actually lands.
    if (FPaths::IsRelative(ExportPath))
    {
        ExportPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), ExportPath);
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExportPath), true);

    // Export the world as human-readable T3D text (the documented contract:
    // a diffable / copy-paste-able .t3d file). GEditor->ExportMap drives the
    // engine's own asset-export task with Exporter=NULL, which selects the text
    // ULevelExporterT3D by the file extension and writes the "Begin Map { ... }"
    // text format -- NOT a binary .umap package (the prior FEditorFileUtils::SaveMap
    // behavior). bExportSelectedActorsOnly=false exports the whole level.
    GEditor->ExportMap(WorldToExport, *ExportPath, /*bExportSelectedActorsOnly=*/false);

    // ExportMap returns void, so confirm the artifact actually landed on disk
    // rather than blindly returning success (e.g. an unsupported extension yields
    // no exporter and writes nothing).
    if (!IFileManager::Get().FileExists(*ExportPath))
    {
        Ctx.SendError(ErrorCodes::ERR_EXPORT_FAILED,
            FString::Printf(TEXT("T3D export produced no file at '%s' (expected a .t3d text export)."), *ExportPath));
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("exportPath"), ExportPath);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- level.add_sublevel ----
REGISTER_RPC_HANDLER("level.add_sublevel", "level", "Attach a level package to the active world as a streaming sublevel using LevelStreamingDynamic/LevelStreamingAlwaysLoaded.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("subLevelPath", "path", "Package path of the sublevel to attach, e.g. /Game/Maps/Sub.", "levelPath"),
        RPC_PARAM_OPT("streamingMethod", "string", "Streaming class: 'Blueprint' (LevelStreamingDynamic) or 'AlwaysLoaded'. Defaults to 'Blueprint'.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString SubLevelPath;
    Payload->TryGetStringField(TEXT("subLevelPath"), SubLevelPath);
    if (SubLevelPath.IsEmpty())
        Payload->TryGetStringField(TEXT("levelPath"), SubLevelPath);

    if (SubLevelPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("subLevelPath required"));
        return true;
    }

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_EDITOR, TEXT("Editor unavailable"));
        return true;
    }

    GEditor->ForceGarbageCollection(true);

    // Verify file existence
    FString Filename;
    bool bFileFound = false;
    if (FPackageName::TryConvertLongPackageNameToFilename(
            SubLevelPath, Filename, FPackageName::GetMapPackageExtension()))
    {
        if (IFileManager::Get().FileExists(*Filename))
            bFileFound = true;
    }
    if (!bFileFound && IFileManager::Get().FileExists(*SubLevelPath))
        bFileFound = true;
    if (!bFileFound && !FPackageName::DoesPackageExist(SubLevelPath))
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_NOT_FOUND,
            FString::Printf(TEXT("Level file not found: %s"), *SubLevelPath));
        return true;
    }

    FString StreamingMethod = TEXT("Blueprint");
    Payload->TryGetStringField(TEXT("streamingMethod"), StreamingMethod);

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No world loaded"));
        return true;
    }

    UClass* StreamingClass = ULevelStreamingDynamic::StaticClass();
    if (StreamingMethod.Equals(TEXT("AlwaysLoaded"), ESearchCase::IgnoreCase))
    {
        StreamingClass = ULevelStreamingAlwaysLoaded::StaticClass();
    }

    ULevelStreaming* NewLevel = UEditorLevelUtils::AddLevelToWorld(World, *SubLevelPath, StreamingClass);
    if (NewLevel)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("sublevelPath"), SubLevelPath);
        Result->SetStringField(TEXT("world"), World->GetName());
        Result->SetStringField(TEXT("streamingMethod"), StreamingMethod);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_ADD_FAILED,
            FString::Printf(TEXT("Failed to add sublevel %s (Check logs)"), *SubLevelPath));
    }
    return true;
}

// ---- level.delete ----
REGISTER_RPC_HANDLER("level.delete", "level", "Delete a level package from the content browser. Errors with ASSET_IN_USE if the level is currently loaded or referenced by other assets, having changed nothing; pass force:true to delete anyway and read that parameter's description first.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("levelPath", "path", "Package path of the level asset to delete, e.g. /Game/Maps/Old.", "path"),
        RPC_PARAM_OPT("force", "boolean", "Delete even when something still references the level; defaults to false, which refuses with ASSET_IN_USE and changes nothing. force:true runs the engine's Force Delete: it replaces EVERY in-memory pointer to the level with null editor-wide and marks each of those packages dirty BEFORE the engine decides whether the .umap may go — irreversibly (no transaction, no undo), and the file can still survive, leaving the referencers broken. Packages damaged that way are listed in referencesNulled; do not save them, reload them with asset.reload.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString LevelPath;
    Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
    if (LevelPath.IsEmpty())
        Payload->TryGetStringField(TEXT("path"), LevelPath);

    if (LevelPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("levelPath required for delete_level"));
        return true;
    }

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(
            ErrorCodes::ERR_PIE_ACTIVE,
            TEXT("level.delete cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    // The summary has always claimed this verb "errors if the level is ... referenced by
    // other assets". UEditorAssetLibrary::DeleteAsset never implemented that guard - it
    // funnels into ObjectTools::ForceDeleteObjects, which nulls every in-memory pointer to
    // the level and dirties those packages before it knows whether the .umap may go. The
    // policy is where that guard now lives; see Utils/AssetDeletePolicy.h.
    const bool bForce = Ctx.GetBool(TEXT("force"), false);
    const AssetDeletePolicy::FResult DeleteResult =
        AssetDeletePolicy::DeleteAsset(LevelPath, bForce);

    if (DeleteResult.bDeleted)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("levelPath"), LevelPath);
        Result->SetBoolField(TEXT("deleted"), true);
        if (DeleteResult.bForced)
        {
            // ForceDeleteObjects discards its own FForceReplaceInfo, so this diff is the
            // only way a caller learns what it damaged.
            Result->SetBoolField(TEXT("forced"), true);
            TArray<TSharedPtr<FJsonValue>> Dirtied;
            for (const FString& Package : DeleteResult.DirtiedPackages)
            {
                Dirtied.Add(MakeShared<FJsonValueString>(Package));
            }
            Result->SetArrayField(TEXT("referencesNulled"), Dirtied);
        }
        Ctx.SendSuccess(Result);
    }
    else if (DeleteResult.bRefused)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_IN_USE, DeleteResult.Message);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_DELETE_FAILED,
            FString::Printf(TEXT("Failed to delete level: %s"), *LevelPath));
    }
    return true;
}

// ---- level.rename ----
REGISTER_RPC_HANDLER("level.rename", "level", "Rename / move a level package, fixing up redirectors so existing references continue to resolve.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("levelPath", "path", "Source level package path.", "sourcePath"),
        RPC_PARAM_REQ("destinationPath", "path", "New package path the level should be moved to.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString SourcePath;
    Payload->TryGetStringField(TEXT("levelPath"), SourcePath);
    if (SourcePath.IsEmpty())
        Payload->TryGetStringField(TEXT("sourcePath"), SourcePath);
    FString DestinationPath;
    Payload->TryGetStringField(TEXT("destinationPath"), DestinationPath);

    if (SourcePath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("levelPath or sourcePath required"));
        return true;
    }
    if (DestinationPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("destinationPath required"));
        return true;
    }

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(
            ErrorCodes::ERR_PIE_ACTIVE,
            TEXT("level.rename cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    bool bRenamed = UEditorAssetLibrary::RenameAsset(SourcePath, DestinationPath);
    if (bRenamed)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("sourcePath"), SourcePath);
        Result->SetStringField(TEXT("destinationPath"), DestinationPath);
        Result->SetBoolField(TEXT("renamed"), true);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_RENAME_FAILED,
            FString::Printf(TEXT("Failed to rename level: %s"), *SourcePath));
    }
    return true;
}

// ---- level.duplicate ----
REGISTER_RPC_HANDLER("level.duplicate", "level", "Duplicate an existing level package to a new location without modifying the source.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("sourcePath", "path", "Source level package path.", "levelPath"),
        RPC_PARAM_REQ("destinationPath", "path", "Package path for the duplicate.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString SourcePath;
    Payload->TryGetStringField(TEXT("sourcePath"), SourcePath);
    if (SourcePath.IsEmpty())
        Payload->TryGetStringField(TEXT("levelPath"), SourcePath);
    FString DestinationPath;
    Payload->TryGetStringField(TEXT("destinationPath"), DestinationPath);

    if (SourcePath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("sourcePath or levelPath required"));
        return true;
    }
    if (DestinationPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("destinationPath required"));
        return true;
    }

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(
            ErrorCodes::ERR_PIE_ACTIVE,
            TEXT("level.duplicate cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    UObject* DuplicatedAsset = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestinationPath);
    if (DuplicatedAsset != nullptr)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("sourcePath"), SourcePath);
        Result->SetStringField(TEXT("destinationPath"), DestinationPath);
        Result->SetBoolField(TEXT("duplicated"), true);
        // UEditorAssetLibrary::DuplicateAsset creates the copy in memory only (the
        // package is marked dirty but never written) — no .umap lands on disk. Make
        // that explicit so a caller does not read duplicated:true as "load-ready on
        // disk" and hit LEVEL_NOT_PERSISTED on an immediate level.load. The duplicate
        // must be saved (level.save_as / editor.save_all) before it can be loaded from
        // disk. These are additive fields; duplicated:true keeps its meaning. The
        // signal is emitted under the same saved:false + persistenceNote pair that the
        // sibling level.structure.create_level uses for this exact in-memory-only
        // condition (LevelStructureHandler.cpp), so a caller can key off one dedicated
        // field for "is this on disk?" — the generic `note` key is overloaded with
        // unrelated hints elsewhere in the level cluster, so it is deliberately avoided.
        Result->SetBoolField(TEXT("saved"), false);
        Result->SetStringField(TEXT("persistenceNote"),
            TEXT("Duplicate exists in memory only (not yet on disk). Save it "
                 "(level.save_as / editor.save_all) before level.load can open it; "
                 "an immediate load fails LEVEL_NOT_PERSISTED."));
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_DUPLICATE_FAILED,
            FString::Printf(TEXT("Failed to duplicate level: %s"), *SourcePath));
    }
    return true;
}

// ---- level.get_info ----
REGISTER_RPC_HANDLER("level.get_info", "level", "Return descriptive metadata about a level: name, package path, owning world, and actor count. Reads from the loaded world when no path is provided.",
    RPC_PARAMS(
        RPC_PARAM_OPT_ALIAS("levelPath", "path", "Level package path to inspect. Must already be loaded into the active world (the persistent level or a loaded sublevel) — this getter reads only loaded levels; an on-disk-but-unloaded map errors LEVEL_NOT_LOADED (level.load it first). Defaults to the active editor level.", "level_path")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    FString LevelPath;
    if (Payload)
    {
        Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
        if (LevelPath.IsEmpty()) Payload->TryGetStringField(TEXT("level_path"), LevelPath);
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    ULevel* TargetLevel = FindLevelByPathLevel(World, LevelPath);
    if (!TargetLevel)
    {
        return SendLevelInspectMissError(Ctx, LevelPath);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("levelPath"), TargetLevel->GetOutermost() ? TargetLevel->GetOutermost()->GetName() : TEXT(""));
    Result->SetStringField(TEXT("levelName"), TargetLevel->GetName());
    Result->SetNumberField(TEXT("actorCount"), TargetLevel->Actors.Num());
    Ctx.SendSuccess(Result);
    return true;
}

// ---- level.remove_from_world ----
REGISTER_RPC_HANDLER("level.remove_from_world", "level", "Detach a streaming sublevel from the active world. The level package itself is not deleted; use level.delete for that.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("levelPath", "path", "Package path of the streaming sublevel to detach.", "level_path")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString LevelPath;
    Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
    if (LevelPath.IsEmpty()) Payload->TryGetStringField(TEXT("level_path"), LevelPath);

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    ULevel* TargetLevel = FindLevelByPathLevel(World, LevelPath);
    if (TargetLevel)
    {
        // `level.remove_from_world` is listed in the tick-unsafe method table in
        // Dispatch/SafePoint.cpp: RemoveLevelFromWorld (EditorLevelUtils.cpp:973) ->
        // RemoveLevelsFromWorld (:844) -> GEditor->Cleanse (:933) -> CollectGarbage
        // (EditorEngine.cpp:2859) -> ~ULevel -> FreeTickTaskLevel, which asserts
        // !LevelList.Contains(TickTaskLevel) if this runs inside UWorld::Tick. The
        // dispatcher re-queues the whole request onto the core ticker; do not add a
        // second gate here.
        bool bRemoved = UEditorLevelUtils::RemoveLevelFromWorld(TargetLevel);
        if (bRemoved)
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("levelPath"), LevelPath);
            Result->SetBoolField(TEXT("removed"), true);
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_REMOVE_FAILED, TEXT("Failed to remove level"));
        }
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_LEVEL_NOT_FOUND,
            FString::Printf(TEXT("Level not found: %s"), *LevelPath));
    }
    return true;
}

// ---- level.set_visibility ----
REGISTER_RPC_HANDLER("level.set_visibility", "level", "Toggle the visibility flag on a streaming sublevel without unloading it. Hides actors from rendering while keeping them ticking.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("levelPath", "path", "Package path of the streaming sublevel.", "level_path"),
        RPC_PARAM_OPT("visible", "boolean", "True to make the sublevel visible, false to hide. Defaults to true.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString LevelPath;
    Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
    if (LevelPath.IsEmpty()) Payload->TryGetStringField(TEXT("level_path"), LevelPath);
    bool bVisible = true;
    Payload->TryGetBoolField(TEXT("visible"), bVisible);

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    ULevel* TargetLevel = FindLevelByPathLevel(World, LevelPath);
    if (TargetLevel)
    {
        UEditorLevelUtils::SetLevelVisibility(TargetLevel, bVisible, true);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("levelPath"), LevelPath);
        Result->SetBoolField(TEXT("visible"), bVisible);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_LEVEL_NOT_FOUND,
            FString::Printf(TEXT("Level not found: %s"), *LevelPath));
    }
    return true;
}

// ---- level.set_locked ----
REGISTER_RPC_HANDLER("level.set_locked", "level", "Lock or unlock a level for editing. Locked levels cannot be modified through the editor; useful when collaborating to prevent accidental writes.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("levelPath", "path", "Package path of the level to lock or unlock.", "level_path"),
        RPC_PARAM_OPT("locked", "boolean", "True to lock (read-only), false to unlock. Defaults to true.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("Payload missing"));
        return true;
    }

    FString LevelPath;
    Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
    if (LevelPath.IsEmpty()) Payload->TryGetStringField(TEXT("level_path"), LevelPath);
    bool bLocked = true;
    Payload->TryGetBoolField(TEXT("locked"), bLocked);

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    ULevel* TargetLevel = FindLevelByPathLevel(World, LevelPath);
    if (TargetLevel)
    {
        if (bLocked != FLevelUtils::IsLevelLocked(TargetLevel))
        {
            FLevelUtils::ToggleLevelLock(TargetLevel);
        }
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("levelPath"), LevelPath);
        Result->SetBoolField(TEXT("locked"), FLevelUtils::IsLevelLocked(TargetLevel));
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_LEVEL_NOT_FOUND,
            FString::Printf(TEXT("Level not found: %s"), *LevelPath));
    }
    return true;
}

// ---- level.get_actors ----
REGISTER_RPC_HANDLER("level.get_actors", "level", "List every actor that belongs to the named level (not the entire world). Useful for inspecting a specific sublevel's contents.",
    RPC_PARAMS(
        RPC_PARAM_OPT_ALIAS("levelPath", "path", "Package path of the level whose actors should be listed. Must already be loaded into the active world (the persistent level or a loaded sublevel) — this getter reads only loaded levels; an on-disk-but-unloaded map errors LEVEL_NOT_LOADED (level.load it first). Defaults to the active editor level.", "level_path")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    FString LevelPath;
    if (Payload)
    {
        Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
        if (LevelPath.IsEmpty()) Payload->TryGetStringField(TEXT("level_path"), LevelPath);
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    ULevel* TargetLevel = FindLevelByPathLevel(World, LevelPath);
    if (!TargetLevel)
    {
        return SendLevelInspectMissError(Ctx, LevelPath);
    }

    TArray<TSharedPtr<FJsonValue>> ActorsArray;
    for (AActor* Actor : TargetLevel->Actors)
    {
        if (Actor)
        {
            ActorsArray.Add(MakeShared<FJsonValueString>(Actor->GetName()));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("levelPath"), TargetLevel->GetOutermost() ? TargetLevel->GetOutermost()->GetName() : TEXT(""));
    Result->SetNumberField(TEXT("count"), ActorsArray.Num());
    Result->SetArrayField(TEXT("actors"), ActorsArray);

    if (UPackage* LevelPackage = TargetLevel->GetOutermost())
    {
        const FString LevelPackageName = LevelPackage->GetName();
        if (FString DumpHint = AssetDumpSuggestion::BuildDumpSuggestionHint(LevelPackageName, AssetDumpSuggestion::EDumpSubjectKind::Level); !DumpHint.IsEmpty())
        {
            Result->SetStringField(TEXT("hint"), DumpHint);
        }
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ---- level.get_bounds ----
REGISTER_RPC_HANDLER("level.get_bounds", "level", "Return the world-space axis-aligned bounding box that encloses every actor in the level (origin + extent). Uses the level's ALevelBounds actor when present (hasLevelBounds=true), otherwise sums each actor's component bounding box. isValid is false (and min/max are the degenerate origin box) only when no actor has finite renderable bounds.",
    RPC_PARAMS(
        RPC_PARAM_OPT_ALIAS("levelPath", "path", "Package path of the level to measure. Must already be loaded into the active world (the persistent level or a loaded sublevel) — this getter reads only loaded levels; an on-disk-but-unloaded map errors LEVEL_NOT_LOADED (level.load it first). Defaults to the active editor level.", "level_path")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    FString LevelPath;
    if (Payload)
    {
        Payload->TryGetStringField(TEXT("levelPath"), LevelPath);
        if (LevelPath.IsEmpty()) Payload->TryGetStringField(TEXT("level_path"), LevelPath);
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    ULevel* TargetLevel = FindLevelByPathLevel(World, LevelPath);
    if (!TargetLevel)
    {
        return SendLevelInspectMissError(Ctx, LevelPath);
    }

    // Prefer the level's ALevelBounds actor when present and valid. Most levels
    // (every freshly created one) have no ALevelBounds actor, so fall back to
    // summing each actor's component bounding box over TargetLevel->Actors —
    // the behavior the documented contract ("encloses every actor in the level")
    // promises. McpActorUtils::SumActorBounds holds the shared union rule (seed
    // ForceInit, skip ALevelScriptActor, union only IsValid boxes); the same
    // helper backs the auto-calculate path in LevelStructureHandler.
    const bool bHasLevelBounds = TargetLevel->LevelBoundsActor.IsValid();
    FBox LevelBounds(ForceInit);
    if (bHasLevelBounds)
    {
        LevelBounds = TargetLevel->LevelBoundsActor->GetComponentsBoundingBox();
    }

    if (!LevelBounds.IsValid)
    {
        LevelBounds = McpActorUtils::SumActorBounds(TargetLevel->Actors);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("levelPath"), TargetLevel->GetOutermost() ? TargetLevel->GetOutermost()->GetName() : TEXT(""));
    // hasLevelBounds tells callers whether the box came from a dedicated
    // ALevelBounds actor (true) or was computed by enclosing the level's actors
    // (false); isValid is false only when the level holds no actor with finite
    // renderable bounds (then min/max stay at the degenerate origin box).
    Result->SetBoolField(TEXT("hasLevelBounds"), bHasLevelBounds);
    Result->SetBoolField(TEXT("isValid"), LevelBounds.IsValid != 0);
    Result->SetStringField(TEXT("min"), FString::Printf(TEXT("X=%f Y=%f Z=%f"), LevelBounds.Min.X, LevelBounds.Min.Y, LevelBounds.Min.Z));
    Result->SetStringField(TEXT("max"), FString::Printf(TEXT("X=%f Y=%f Z=%f"), LevelBounds.Max.X, LevelBounds.Max.Y, LevelBounds.Max.Z));
    Ctx.SendSuccess(Result);
    return true;
}

// ---- level.get_lighting_scenarios ----
REGISTER_RPC_HANDLER("level.get_lighting_scenarios", "level", "List every streaming sublevel in the active world that is flagged as a lighting scenario. Lighting scenarios let you bake distinct lighting setups (day / night / etc.) and switch between them.",
    RPC_NO_PARAMS)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No editor world available"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Scenarios;
    TArray<ULevel*> Levels = GetAllLevelsFromWorldLevel(World);
    for (ULevel* Level : Levels)
    {
        if (Level && Level->bIsLightingScenario)
        {
            TSharedPtr<FJsonObject> ScenarioInfo = MakeShared<FJsonObject>();
            ScenarioInfo->SetStringField(TEXT("levelPath"), Level->GetOutermost() ? Level->GetOutermost()->GetName() : TEXT(""));
            ScenarioInfo->SetStringField(TEXT("levelName"), Level->GetName());
            Scenarios.Add(MakeShared<FJsonValueObject>(ScenarioInfo));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("scenarios"), Scenarios);
    Result->SetNumberField(TEXT("count"), Scenarios.Num());
    Ctx.SendSuccess(Result);
    return true;
}

// ---- level.build_navigation ----
REGISTER_RPC_HANDLER("level.build_navigation", "level", "Build the navigation mesh (recast) for the active world. Long-running; runs as a job. Required after substantial level geometry changes for AI to navigate.",
    RPC_NO_PARAMS)
{
    FJobBindArgs Args;
    Args.Method = TEXT("level.build_navigation");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.BindNativeDelegate =
        [](FJobOnComplete OnComplete)
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        BindNavigationBuildCompletion(World)(OnComplete);
        FEditorBuildUtils::EditorBuild(World, FBuildOptions::BuildAIPaths);
    };
    AddLevelBuildHeartbeat(Ctx.StartJob(Args), TEXT("navigation build"));
    return true;
}

// ---- level.build_all ----
REGISTER_RPC_HANDLER("level.build_all", "level", "Run the editor's full Build All sequence: geometry, navigation, lighting, and reflection captures. Long-running; runs as a job.",
    RPC_NO_PARAMS)
{
    FJobBindArgs Args;
    Args.Method = TEXT("level.build_all");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.BindNativeDelegate =
        [](FJobOnComplete OnComplete)
    {
        TSharedRef<int32, ESPMode::ThreadSafe> Pending
            = MakeShared<int32, ESPMode::ThreadSafe>(2);
        TSharedRef<bool, ESPMode::ThreadSafe> AllSuccess
            = MakeShared<bool, ESPMode::ThreadSafe>(true);

        auto OneDone = [OnComplete, Pending, AllSuccess]
            (bool bSuccess, TSharedPtr<FJsonObject>, FString) mutable
        {
            if (!bSuccess) *AllSuccess = false;
            if (--*Pending == 0)
            {
                auto R = MakeShared<FJsonObject>();
                OnComplete(*AllSuccess, R,
                    *AllSuccess ? FString() : TEXT("partial_failure"));
            }
        };

        // Two completion arms over one BuildAll. The lighting watchdog polls the
        // GLOBAL FEditorBuildUtils::IsBuildCurrentlyRunning() flag, which covers the
        // whole BuildAll sequence (geometry + nav + lighting + reflection), not just
        // the lighting phase — so its reconcile is keyed to the entire build
        // finishing. The navigation arm polls the phase-specific
        // IsNavigationBuildInProgress(). Pending=2 above waits for both arms.
        BindLightingBuildCompletion()(OneDone);
        BindNavigationBuildCompletion(
            GEditor->GetEditorWorldContext().World())(OneDone);

        FEditorBuildUtils::EditorBuild(
            GEditor->GetEditorWorldContext().World(),
            FBuildOptions::BuildAll);
    };
    AddLevelBuildHeartbeat(Ctx.StartJob(Args), TEXT("build all"));
    return true;
}
