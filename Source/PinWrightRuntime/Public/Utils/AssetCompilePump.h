// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "AssetCompilingManager.h"
#include "Engine/StaticMesh.h"

// The one game-thread pump every bounded compile wait in the plugin drives.
//
// It exists because a wait that merely holds the game thread and sleeps STARVES the very
// work it is waiting on, and therefore reports "still compiling" forever. Asset compilation
// is only partly off-thread: worker threads produce results, but every step that CONSUMES
// them runs on the game thread out of FAssetCompilingManager::ProcessAsyncTasks, which the
// editor calls once per tick. FShaderCompilingManager is an IAssetCompilingManager
// registered there and its ProcessAsyncTasks is exactly ProcessAsyncResults(...) - the pass
// that finalises a finished shader map, writes its HLSL errors back into
// FMaterial::CompileErrors and installs the game-thread shader map. FNiagaraSystemCompilingManager
// registers there too and runs the compile task's queued game-thread functions.
//
// Measured evidence (board B-niagara-compile-wait-does-not-wait): a Niagara system that
// compiles in 0.104775 s when the editor drives it sat undrained for the entire 90 s ceiling
// of an unpumped wait and landed the instant the loop gave up - the engine logged
// "took 90.024727 sec (time since issued)", matching the wait to 30 ms. The engine's own
// game-thread waits do the same pump: FNiagaraSystemCompilationTask::WaitTillCompileCompletion
// pokes FNiagaraSystemCompilingManager::AdvanceAsyncTasks between event waits, and
// AsyncCompilationHelpers.cpp calls ProcessAsyncResults "to avoid starvation while we wait for
// other async tasks to finish".
//
// First proved out in Handlers/Niagara/NiagaraCompileWait.cpp; shared from here so a second
// bounded wait (Handlers/Material/MaterialCompileErrorCollector.h) reuses the pump instead of
// re-deriving it.
namespace PinWright::AssetCompile
{
#if WITH_DEV_AUTOMATION_TESTS
    inline bool& CompilePendingOverride()
    {
        static bool bForcePending = false;
        return bForcePending;
    }

    class FScopedCompilePendingOverride
    {
    public:
        explicit FScopedCompilePendingOverride(const bool bForcePending)
            : bPrevious(CompilePendingOverride())
        {
            CompilePendingOverride() = bForcePending;
        }

        ~FScopedCompilePendingOverride()
        {
            CompilePendingOverride() = bPrevious;
        }

        FScopedCompilePendingOverride(const FScopedCompilePendingOverride&) = delete;
        FScopedCompilePendingOverride& operator=(const FScopedCompilePendingOverride&) = delete;

    private:
        bool bPrevious;
    };
#endif

    // The override is test-only and is consulted only after the production build trigger. The
    // shipping path always observes the mesh's real compile state.
    inline bool IsCompilingForBoundedWait(const UStaticMesh* Mesh,
                                          const bool bAllowTestOverride)
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (bAllowTestOverride && CompilePendingOverride())
        {
            return true;
        }
#else
        (void)bAllowTestOverride;
#endif
        return Mesh && Mesh->IsCompiling();
    }

    // bLimitExecutionTime=true keeps one iteration inside a frame's processing budget, so the
    // wait finalises what is ready rather than draining every unrelated texture or mesh compile
    // the editor happens to have queued. Game thread only.
    inline void AdvanceOnGameThread()
    {
        FAssetCompilingManager::Get().ProcessAsyncTasks(/*bLimitExecutionTime=*/true);
    }
}
