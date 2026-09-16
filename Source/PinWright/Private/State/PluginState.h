// Copyright (c) 2026 Alexander Penkin. MIT License.

// Singleton that owns all shared mutable state for PinWright
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "State/BlueprintTracker.h"
#include "State/SaveThrottler.h"
#include "State/AsyncFolderDumpState.h"
#include "Utils/JobMonitorLog.h"
#include "State/JobRegistry.h"

enum class EReloadCompleteReason;
// Forward-declared rather than included: PluginState.h is pulled into most of the plugin,
// and the candidate registry drags in the AudioGen buffer/recipe headers. TUniquePtr only
// needs the complete type where it is destroyed, which is PluginState.cpp.
class FPwCandidateRegistry;

class PINWRIGHT_API FPluginState
{
public:
    static FPluginState& Get();

    // Last in-process Live Coding compile result (system.live_coding_compile ->
    // system.live_coding_status readback). Stored as the string form of
    // ELiveCodingCompileResult ("Success"/"NoChanges"/"Failure"/..., "None" until a
    // compile runs) so this header stays decoupled from the Windows-only
    // ILiveCodingModule header. Game-thread only, like the other simple fields.
    FString& LiveCodingLastCompileResult() { return LiveCodingLastCompileResultValue; }

    FBlueprintTracker& Blueprints() { return BlueprintTrackerInstance; }
    FSaveThrottler& SaveThrottle() { return SaveThrottlerInstance; }

    TMap<FString, TSharedPtr<FJsonObject>>& SequenceRegistry() { return SequenceRegistryMap; }
    FString& CurrentSequencePath() { return CurrentSequencePathValue; }
    TMap<FString, TSharedPtr<FJsonObject>>& NiagaraRegistry() { return NiagaraRegistryMap; }

    // Actor transform snapshot cache (used by actor.create_snapshot / actor.restore_snapshot)
    // Capped at 500 entries to prevent unbounded growth in long sessions.
    TMap<FString, FTransform>& CachedActorSnapshots()
    {
        if (CachedActorSnapshotsMap.Num() > 500)
        {
            CachedActorSnapshotsMap.Empty();
        }
        return CachedActorSnapshotsMap;
    }

    // Node creation stack for compiler undo support (one entry per Compile() call)
    TArray<TArray<FGuid>>& NodeCreationStack() { return NodeCreationStackData; }

    // Parallel stack to NodeCreationStack: true when the matching compile entry ran
    // Phase 0 deletion sweeps. Used by blueprint.undo_last_bpir to refuse rollback
    // when forward Phase 0 sweeps cannot be reversed by un-creating nodes alone.
    // Must be pushed in lock-step with NodeCreationStack and popped together.
    TArray<bool>& Phase0RanStack() { return Phase0RanStackData; }

    FAsyncFolderDumpState& GetFolderDump() { return FolderDumpInstance; }

    FJobMonitorLog& GetJobMonitorLog();
    FJobRegistry&   GetJobRegistry();

    // Session-scoped store of rendered audio-synth candidates (Private/AudioGen). Bounded by
    // bytes and count with LRU eviction, because a candidate carries its rendered audio.
    FPwCandidateRegistry& GetCandidateRegistry();

    // Returns all loaded UBlueprintFunctionLibrary subclasses, keyed by class name.
    // The scan is re-run after hot-reload / Live Coding; otherwise cached.
    const TMap<FString, UClass*>& GetScannedFunctionLibraries();

    // Clears the cached function library scan so the next call re-scans.
    void InvalidateFunctionLibraryCache();

#if WITH_DEV_AUTOMATION_TESTS
    // Resets all internal state to defaults. Used between test runs to prevent
    // state leakage across tests.
    void ResetForTesting();
#endif

private:
    FPluginState();
    ~FPluginState();

    void OnReloadComplete(EReloadCompleteReason Reason);
    FDelegateHandle ReloadCompleteDelegateHandle;

    FBlueprintTracker BlueprintTrackerInstance;
    FSaveThrottler SaveThrottlerInstance;

    // Backing store for LiveCodingLastCompileResult(); see accessor comment above.
    FString LiveCodingLastCompileResultValue = TEXT("None");

    TMap<FString, TSharedPtr<FJsonObject>> SequenceRegistryMap;
    FString CurrentSequencePathValue;
    TMap<FString, TSharedPtr<FJsonObject>> NiagaraRegistryMap;

    TMap<FString, FTransform> CachedActorSnapshotsMap;
    TArray<TArray<FGuid>> NodeCreationStackData;
    TArray<bool> Phase0RanStackData;

    FAsyncFolderDumpState FolderDumpInstance;

    TUniquePtr<FJobMonitorLog> JobMonitorLogInstance;
    TUniquePtr<FJobRegistry>   JobRegistryInstance;
    TUniquePtr<FPwCandidateRegistry> CandidateRegistryInstance;

    // Cached result of the TObjectIterator<UClass> function library scan.
    TMap<FString, UClass*> ScannedFunctionLibraries;
    bool bFunctionLibrariesScanned = false;
};
