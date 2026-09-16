// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Templates/SharedPointer.h"

class FStreamSearch;

namespace PinWright::BlueprintIndexHandler
{
    /**
     * Stop a stream search and join it without pumping the current task-graph queue,
     * giving up after TimeoutSeconds.
     *
     * FStreamSearch::EnsureCompletion pumps that queue while waiting, which is
     * re-entrant when an RPC is already executing from the game-thread queue.
     *
     * The wait is BOUNDED because the worker's acknowledgement of Stop() is not
     * guaranteed to be prompt: FFindInBlueprintSearchManager::ContinueSearchQuery
     * calls BlockSearchQueryIfPaused() BEFORE it observes WasStopped()
     * (FindInBlueprintManager.cpp:2912-2920), so a FiB pause taken for garbage
     * collection or a package save parks the worker on PauseThreadsCriticalSection
     * with the stop request still unread. An unbounded spin here would hand the game
     * thread to that lock for as long as the pause lasts.
     *
     * Returns true when the worker was joined on this stack. Returns false when the
     * bound expired, in which case the search has been handed to
     * DetachStreamSearchToCoreTicker and the caller must not touch it again.
     */
    bool StopAndJoinStreamSearch(const TSharedRef<FStreamSearch>& Search,
                                 double TimeoutSeconds = 5.0);

    /**
     * Abandon a still-running stream search WITHOUT blocking the caller.
     *
     * Signals Stop() and then parks the last reference on the core ticker, which
     * polls IsComplete() and calls EnsureCompletion() once the worker has exited.
     *
     * Needed because the two obvious ways to end a search both block the game
     * thread for an unbounded time: EnsureCompletion() spins until the worker
     * finishes (and pumps the game-thread task queue while doing so, which is
     * re-entrant from inside an RPC), and simply dropping the TSharedRef runs
     * ~FRunnableThread -> Kill(true) -> WaitForSingleObject(INFINITE). Use this
     * whenever a search is given up on rather than waited out.
     */
    void DetachStreamSearchToCoreTicker(const TSharedRef<FStreamSearch>& Search);
}
