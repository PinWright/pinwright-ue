// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintIndexHandler.cpp - Blueprint search using UE's Find-in-Blueprints system
// blueprint.search: keyword search across all indexed Blueprint assets
//
// LIVENESS CONTRACT. Nothing in this file may hold the game thread while the
// Find-in-Blueprints index builds or while the search worker walks the corpus. Both
// are unbounded in the size of the content tree, and a first-of-session call on a
// large project measured 136 s with no frame drawn in between before the editor died
// on a VirtualTextureProducer assert. Three things used to block; none of them do now.
//
//   1. The auto-index pump ran `while (IsCacheInProgress()) { FiB.Tick(0); Sleep(); }`
//      on the RPC stack. FFindInBlueprintSearchManager is an FTickableEditorObject
//      (FindInBlueprintManager.h:528, :538) whose caching operation finalizes one
//      asset per tick, so that loop was the editor's own frame loop with the frame
//      taken out. Worse, the pump's own timeout was unenforceable: elapsed time was
//      only sampled BETWEEN Tick() calls and a single Tick() can load a level. It is
//      now a single Tick() kick, issued from the core ticker, that hands deferred
//      assets to a caching operation; after that the EDITOR advances it and this
//      handler only observes IsCacheInProgress().
//   2. The search wait was `while (!Search->IsComplete()) Sleep(0.01f)` for up to
//      timeoutSeconds. Bounded, but a bound of two minutes of dead editor is not a
//      fix; and holding the game thread across it also denies the FiB caching
//      operation the ticks it needs and denies FiB the pause it takes for garbage
//      collection and package saves.
//   3. Result resolution called FFindInBlueprintsResult::GetParentBlueprint(), which
//      is LoadObject under the hood (FindInBlueprints.cpp:301-329) and was called
//      once per matched root with nothing bounding it - not even `limit`, which is
//      applied afterwards. That is what mass-loaded levels, registered landscapes and
//      built a multi-GB static mesh in the reported incident. It was never needed:
//      a FiB root result's display string IS the indexed asset's object path
//      (FImaginaryBlueprint::CreateSearchResult_Internal returns the FiB_Path tag,
//      ImaginaryBlueprintData.cpp:634-644, fed from FSearchData::AssetPath at
//      FindInBlueprintManager.cpp:2298, :2329), so every field the response carries
//      comes from AssetRegistry rows this handler already gathered. Nothing is loaded.
//
// The run is therefore driven from FTSTicker::GetCoreTicker() and answers through an
// FAsyncResponseToken. The request/response shape is UNCHANGED - deliberately no job
// ticket, because turning a synchronous verb into a ticket for every caller is a
// breaking change (docs/rpc-design.md 9a). A caller receives exactly the body it
// always did, just without the editor having stopped to produce it.

#include "Handlers/Blueprint/BlueprintIndexHandler.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Dispatch/ScopedUnattendedRpc.h"

#include "FindInBlueprintManager.h"
#include "ImaginaryBlueprintData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Containers/Ticker.h"

bool PinWright::BlueprintIndexHandler::StopAndJoinStreamSearch(
    const TSharedRef<FStreamSearch>& Search, double TimeoutSeconds)
{
    // FStreamSearch::Run always registers the query with BeginSearchQuery before
    // ContinueSearchQuery observes Stop(). Waiting for IsComplete therefore sets
    // the FiB first-search flag and unregisters the query without recursively
    // pumping the game-thread task queue.
    Search->Stop();

    const double StartSeconds = FPlatformTime::Seconds();
    while (!Search->IsComplete())
    {
        if (FPlatformTime::Seconds() - StartSeconds > TimeoutSeconds)
        {
            // The worker has not reached its WasStopped() check - almost certainly
            // parked in BlockSearchQueryIfPaused (see the header). Waiting longer
            // spends game-thread time on a lock only the game thread can release.
            UE_LOG(LogTemp, Warning,
                TEXT("blueprint.search: the Find-in-Blueprints worker did not acknowledge Stop() "
                     "within %.1fs; detaching it to the core ticker instead of holding the game "
                     "thread on it."),
                TimeoutSeconds);
            DetachStreamSearchToCoreTicker(Search);
            return false;
        }
        FPlatformProcess::Sleep(0.001f);
    }

    // IsComplete is set immediately before Run returns. EnsureCompletion can now
    // join the native thread without entering its ProcessThreadUntilIdle loop.
    Search->EnsureCompletion();
    return true;
}

void PinWright::BlueprintIndexHandler::DetachStreamSearchToCoreTicker(
    const TSharedRef<FStreamSearch>& Search)
{
    // Stop() only increments FStreamSearch::StopTaskCounter
    // (FindInBlueprintManager.cpp:225-228). The worker observes it at the top of the
    // next ContinueSearchQuery, leaves its Run() loop, calls EnsureSearchQueryEnds
    // (:215) and sets bThreadCompleted (:217) - so the search deregisters itself
    // from the FiB manager even though nobody is waiting on it.
    Search->Stop();

    // Poll off the RPC stack. The delegate holds the only remaining strong
    // reference, so nothing here can be destroyed on the game thread's critical
    // path; when the lambda finally returns false the ticker drops the delegate,
    // ~FStreamSearch runs, and its FRunnableThread::Kill(true) returns immediately
    // because the thread has already exited.
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([Search](float) -> bool
        {
            if (!Search->IsComplete())
            {
                return true;  // keep polling
            }

            // Same ordering rationale as StopAndJoinStreamSearch above: waiting for
            // IsComplete first keeps EnsureCompletion out of its own
            // ProcessThreadUntilIdle loop (FindInBlueprintManager.cpp:246-254).
            Search->EnsureCompletion();
            return false;  // one-shot; releases the search
        }),
        0.05f);
}

namespace
{
    /** Map a user-facing filter string to the engine's ESearchQueryFilter enum */
    ESearchQueryFilter ParseFilterString(const FString& InFilter)
    {
        if (InFilter.Equals(TEXT("Nodes"),       ESearchCase::IgnoreCase)) return ESearchQueryFilter::NodesFilter;
        if (InFilter.Equals(TEXT("Pins"),        ESearchCase::IgnoreCase)) return ESearchQueryFilter::PinsFilter;
        if (InFilter.Equals(TEXT("Properties"),  ESearchCase::IgnoreCase)) return ESearchQueryFilter::PropertiesFilter;
        if (InFilter.Equals(TEXT("Variables"),   ESearchCase::IgnoreCase)) return ESearchQueryFilter::VariablesFilter;
        if (InFilter.Equals(TEXT("Components"),  ESearchCase::IgnoreCase)) return ESearchQueryFilter::ComponentsFilter;
        if (InFilter.Equals(TEXT("Functions"),   ESearchCase::IgnoreCase)) return ESearchQueryFilter::FunctionsFilter;
        if (InFilter.Equals(TEXT("Macros"),      ESearchCase::IgnoreCase)) return ESearchQueryFilter::MacrosFilter;
        if (InFilter.Equals(TEXT("Graphs"),      ESearchCase::IgnoreCase)) return ESearchQueryFilter::GraphsFilter;
        return ESearchQueryFilter::AllFilter;
    }

    /** Count match items in an FFindInBlueprintsResult tree (children only, not the root) */
    int32 CountMatchItems(const TSharedPtr<FFindInBlueprintsResult>& Node)
    {
        if (!Node.IsValid()) return 0;
        int32 Count = 0;
        for (const auto& Child : Node->Children)
        {
            Count++; // Count this child as a match
            Count += CountMatchItems(Child); // Plus its descendants
        }
        return Count;
    }

    /** Collect match details from an FFindInBlueprintsResult tree (flattened, capped) */
    void CollectMatches(
        const TSharedPtr<FFindInBlueprintsResult>& Node,
        TArray<TPair<FString, FString>>& OutMatches,
        int32 MaxMatches)
    {
        if (!Node.IsValid()) return;
        for (const auto& Child : Node->Children)
        {
            if (OutMatches.Num() >= MaxMatches) return;

            FString Category = Child->GetCategory().ToString();
            FString Text = Child->GetDisplayString().ToString();
            if (!Text.IsEmpty())
            {
                OutMatches.Add({Category, Text});
            }

            CollectMatches(Child, OutMatches, MaxMatches);
        }
    }

    /**
     * Make sure the Find-in-Blueprints index is MOVING. Does not wait for it.
     *
     * FFindInBlueprintSearchManager defers a full indexing pass until the first
     * search has occurred (bHasFirstSearchOccurred, set by BeginSearchQuery) and
     * reports itself tickable only while a caching operation, a pending first-index
     * transfer, an open FiB window or an active query exists
     * (FindInBlueprintManager.cpp:3964-3971). A cold editor therefore sits on a pile
     * of deferred assets that nothing advances. One empty stream search sets the
     * flag; one Tick() moves the deferred set into PendingAssets and hands it to
     * CacheAllAssets, after which IsCacheInProgress() keeps the manager tickable and
     * the editor advances it on its own.
     *
     * Both steps are O(1) in the size of the corpus: CacheAllAssets only builds the
     * caching object and calls Start(), which spawns the async index-builder runnable
     * (FindInBlueprintManager.cpp:3298-3396, :1545+ Start). No asset is indexed on
     * this stack. The one wait is the trigger search's join, which is bounded by
     * StopAndJoinStreamSearch - and this is called from the core ticker, never from an
     * RPC stack, so even that bound is not spent on a caller's request.
     */
    void KickFiBIndexing()
    {
        static bool bFirstKickDone = false;
        FFindInBlueprintSearchManager& FiBManager = FFindInBlueprintSearchManager::Get();

        if (!bFirstKickDone)
        {
            // Trigger the first-search flag so deferred assets get moved to pending.
            // BeginSearchQuery() sets bHasFirstSearchOccurred = true.
            FStreamSearchOptions TriggerOpts;
            TSharedRef<FStreamSearch> TriggerSearch = MakeShared<FStreamSearch>(TEXT(""), TriggerOpts);
            PinWright::BlueprintIndexHandler::StopAndJoinStreamSearch(TriggerSearch);
            bFirstKickDone = true;
        }

        if (!FiBManager.IsCacheInProgress())
        {
            // Kick off the transfer: deferred -> pending -> caching.
            FiBManager.Tick(0.0f);
        }
    }

    /**
     * Everything one blueprint.search run needs after the RPC stack is gone.
     * Heap-owned by the core-ticker delegate that drives it.
     */
    struct FSearchRun
    {
        explicit FSearchRun(const TSharedRef<FAsyncResponseToken>& InToken)
            : Token(InToken)
        {
        }

        FString Query;
        FString Path;
        ESearchQueryFilter Filter = ESearchQueryFilter::AllFilter;
        int32 Limit = 50;
        bool bAutoIndex = true;
        double TimeoutSeconds = 0.0;
        double StartedSeconds = 0.0;
        double DeadlineSeconds = 0.0;

        // AssetRegistry rows under `path`, plus the two keys FiB results arrive
        // under. Indices rather than pointers: these maps outlive the stack frame
        // that built the array, so a pointer into it is a latent dangle.
        TArray<FAssetData> AssetsInScope;
        TMap<FString, int32> ObjectPathToAsset;  // lowercased object path
        TMap<FString, int32> NameToAsset;        // lowercased asset name

        TSharedPtr<FStreamSearch> Search;
        double SearchStartedSeconds = 0.0;
        bool bIndexKicked = false;
        bool bIndexWaited = false;
        bool bIndexTimedOut = false;
        double IndexWaitSeconds = 0.0;
        float IndexProgress = 0.0f;

        TSharedRef<FAsyncResponseToken> Token;
    };

    /**
     * Build the response for a finished (or given-up-on) run and answer the caller.
     *
     * bTimedOut means "this answer is incomplete because the budget ran out" — it is
     * true both when the worker was cut short AND when the index wait consumed the
     * whole budget so no worker ever started. A caller must be able to read one field
     * to know whether it is holding a complete answer, and 0 results with
     * timedOut=false would be a claim that the corpus was searched and held nothing.
     */
    void FinishSearchRun(const TSharedRef<FSearchRun>& Run, bool bTimedOut)
    {
        FFindInBlueprintSearchManager& FiBManager = FFindInBlueprintSearchManager::Get();

        TArray<TSharedPtr<FFindInBlueprintsResult>> Items;
        float SearchPercentComplete = 0.0f;
        int32 UnindexedCount = 0;
        double SearchElapsedSeconds = 0.0;
        const bool bSearchRan = Run->Search.IsValid();

        if (bSearchRan)
        {
            SearchElapsedSeconds = FPlatformTime::Seconds() - Run->SearchStartedSeconds;
            SearchPercentComplete = Run->Search->GetPercentComplete();
            UnindexedCount = Run->Search->GetOutOfDateCount();

            // Drains ItemsFound. Safe to call while the worker is still running:
            // GetFilteredItems takes the same SearchCriticalSection the worker appends
            // results under (FindInBlueprintManager.cpp:176-179 vs :270-275), so a
            // timed-out search still hands back everything matched so far.
            Run->Search->GetFilteredItems(Items);

            if (bTimedOut)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("blueprint.search: query '%s' exceeded timeoutSeconds=%.0f (%.0f%% of the "
                         "corpus scanned); returning %d partial result tree(s) and detaching the "
                         "worker."),
                    *Run->Query, Run->TimeoutSeconds, SearchPercentComplete * 100.0f, Items.Num());

                // Neither EnsureCompletion() nor letting the shared pointer die here is
                // allowed: EnsureCompletion spins until the worker exits AND pumps the
                // game-thread task queue while waiting (FindInBlueprintManager.cpp:246-254),
                // and ~FStreamSearch destroys its FRunnableThread, whose destructor calls
                // Kill(true) -> WaitForSingleObject(INFINITE) (MicrosoftRunnableThread.h:51-57,
                // :90-101). Either one restores the unbounded wait this file exists to avoid.
                PinWright::BlueprintIndexHandler::DetachStreamSearchToCoreTicker(
                    Run->Search.ToSharedRef());
            }
            else
            {
                // Completed naturally. IsComplete() is set immediately before Run() returns,
                // so EnsureCompletion joins the native thread without entering its
                // ProcessThreadUntilIdle loop.
                Run->Search->EnsureCompletion();
            }

            Run->Search.Reset();
        }

        constexpr int32 MaxMatchesPerBlueprint = 10;

        struct FScoredResult
        {
            FString AssetPath;
            FString Name;
            FString ParentClass;
            int32 MatchCount = 0;
            TArray<TPair<FString, FString>> Matches; // category, text
        };

        TArray<FScoredResult> ScoredResults;

        for (const TSharedPtr<FFindInBlueprintsResult>& RootItem : Items)
        {
            if (!RootItem.IsValid()) continue;

            // Resolve the blueprint WITHOUT loading it. GetParentBlueprint() would
            // LoadObject every match here; the object path is already in the result's
            // display string, so the whole resolution is two hash lookups against the
            // AssetRegistry rows gathered for `path`. A result matching neither key is
            // outside the requested scope and is dropped - which is also what makes
            // `path` a real filter rather than a suggestion.
            const FString Key = RootItem->GetDisplayString().ToString().ToLower();
            const int32* FoundIndex = Run->ObjectPathToAsset.Find(Key);
            if (!FoundIndex)
            {
                // Legacy index entries can carry the bare asset name instead.
                FoundIndex = Run->NameToAsset.Find(Key);
            }
            if (!FoundIndex) continue;

            const FAssetData& AssetData = Run->AssetsInScope[*FoundIndex];

            FScoredResult Result;
            Result.AssetPath = AssetData.GetObjectPathString();
            Result.Name = AssetData.AssetName.ToString();
            Result.ParentClass =
                FiBManager.GetSearchDataForAssetPath(AssetData.GetSoftObjectPath()).ParentClass;
            Result.MatchCount = FMath::Max(CountMatchItems(RootItem), 1);
            CollectMatches(RootItem, Result.Matches, MaxMatchesPerBlueprint);

            ScoredResults.Add(MoveTemp(Result));
        }

        // Sort by matchCount descending
        ScoredResults.Sort([](const FScoredResult& A, const FScoredResult& B)
        {
            return A.MatchCount > B.MatchCount;
        });

        const int32 ResultsShown = FMath::Min(ScoredResults.Num(), Run->Limit);

        TArray<TSharedPtr<FJsonValue>> ResultsArray;
        for (int32 i = 0; i < ResultsShown; ++i)
        {
            const FScoredResult& Scored = ScoredResults[i];

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("path"), Scored.AssetPath);
            Entry->SetStringField(TEXT("name"), Scored.Name);
            if (!Scored.ParentClass.IsEmpty())
            {
                Entry->SetStringField(TEXT("parentClass"), Scored.ParentClass);
            }
            Entry->SetNumberField(TEXT("matchCount"), Scored.MatchCount);

            TArray<TSharedPtr<FJsonValue>> MatchesArray;
            for (const TPair<FString, FString>& Match : Scored.Matches)
            {
                TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
                MatchObj->SetStringField(TEXT("category"), Match.Key);
                MatchObj->SetStringField(TEXT("text"), Match.Value);
                MatchesArray.Add(MakeShared<FJsonValueObject>(MatchObj));
            }
            Entry->SetArrayField(TEXT("matches"), MatchesArray);

            ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
        }

        const FString UnindexedNote = UnindexedCount > 0
            ? FString::Printf(TEXT(" (%d unindexed blueprints)"), UnindexedCount)
            : TEXT("");

        const bool bIndexStillBuilding = FiBManager.IsCacheInProgress();
        const int32 UnindexedAssets = FiBManager.GetNumberUnindexedAssets();

        TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
        ResponseObj->SetArrayField(TEXT("results"), ResultsArray);
        ResponseObj->SetNumberField(TEXT("totalMatches"), ScoredResults.Num());
        ResponseObj->SetNumberField(TEXT("resultsShown"), ResultsShown);
        ResponseObj->SetNumberField(TEXT("unindexedCount"), UnindexedCount);
        ResponseObj->SetNumberField(TEXT("unindexedAssets"), UnindexedAssets);
        // Always present, so a caller can branch on it without a HasField probe, and so
        // "few results" is never silently indistinguishable from "gave up early".
        ResponseObj->SetBoolField(TEXT("timedOut"), bTimedOut);
        ResponseObj->SetNumberField(TEXT("searchElapsedSeconds"), SearchElapsedSeconds);
        // Distinguishes "the worker was cut short" from "the worker never started
        // because the index wait used the budget" — the two produce very different
        // follow-up actions and both otherwise present as timedOut with few results.
        ResponseObj->SetBoolField(TEXT("searchRan"), bSearchRan);
        // Unconditional for the same reason: a caller that got 3 results needs to know
        // whether the corpus it searched was complete, and that is a different question
        // from whether the search itself finished.
        ResponseObj->SetBoolField(TEXT("indexInProgress"), bIndexStillBuilding);
        if (bIndexStillBuilding)
        {
            ResponseObj->SetNumberField(TEXT("indexProgress"), FiBManager.GetCacheProgress());
        }
        if (Run->bIndexWaited)
        {
            ResponseObj->SetNumberField(TEXT("indexWaitSeconds"), Run->IndexWaitSeconds);
            ResponseObj->SetBoolField(TEXT("indexTimedOut"), Run->bIndexTimedOut);
        }
        if (bTimedOut)
        {
            ResponseObj->SetNumberField(TEXT("searchTimeoutSeconds"), Run->TimeoutSeconds);
            if (bSearchRan)
            {
                // Omitted rather than zeroed when no worker ran: 0% "scanned" reads as a
                // measurement of a corpus walk that never happened.
                ResponseObj->SetNumberField(TEXT("searchPercentComplete"), SearchPercentComplete);
            }
        }

        FString Message;
        if (Run->bIndexTimedOut)
        {
            Message = FString::Printf(
                TEXT("PARTIAL: the Find-in-Blueprints index was still building when the %.0fs "
                     "budget ran out (%.0f%% indexed), so no search was run. The index keeps "
                     "building on the editor's own tick - retry, or raise timeoutSeconds. "
                     "Searched nothing under %s."),
                Run->TimeoutSeconds, Run->IndexProgress * 100.0f, *Run->Path);
        }
        else if (bTimedOut)
        {
            Message = FString::Printf(
                TEXT("PARTIAL: Find-in-Blueprints did not finish within %.0fs (%.0f%% scanned). "
                     "Found %d blueprints matching '%s' under %s so far%s. Narrow the query or "
                     "raise timeoutSeconds for a complete answer."),
                Run->TimeoutSeconds, SearchPercentComplete * 100.0f,
                ScoredResults.Num(), *Run->Query, *Run->Path, *UnindexedNote);
        }
        else
        {
            Message = FString::Printf(TEXT("Found %d blueprints matching '%s' under %s%s"),
                ScoredResults.Num(), *Run->Query, *Run->Path, *UnindexedNote);
            if (bIndexStillBuilding)
            {
                Message += FString::Printf(
                    TEXT(". NOTE: the Find-in-Blueprints index is still building (%.0f%%), so "
                         "assets indexed after this call could not match - re-run for a complete "
                         "answer."),
                    FiBManager.GetCacheProgress() * 100.0f);
            }
            else if (!Run->bAutoIndex && UnindexedAssets > 0)
            {
                // autoIndex=false never kicks the index, so IsCacheInProgress() stays false
                // and the NOTE above cannot fire. Without this clause the cheap path reports
                // "Found 0 blueprints" in exactly the words a fully-indexed corpus uses for a
                // genuine absence, and the caller has no way to tell which one they got. The
                // count is the manager's, not the worker's: it is the figure that is
                // meaningful on a cold index the search never observed.
                Message += FString::Printf(
                    TEXT(". NOTE: autoIndex was off and %d asset(s) are not in the "
                         "Find-in-Blueprints index, so nothing inside them could match - pass "
                         "autoIndex:true to index them first."),
                    UnindexedAssets);
            }
        }
        ResponseObj->SetStringField(TEXT("message"), Message);

        Run->Token->SendSuccess(ResponseObj);
    }

    /**
     * One core-ticker pass of a run. Returns true to keep ticking.
     *
     * Phase 1 (only when autoIndex): kick the index once, then OBSERVE the FiB
     * caching operation. The editor's own FTickableEditorObject pump advances it, so
     * every later pass reads two flags and a clock - the editor keeps drawing frames,
     * other RPCs keep being serviced, and a garbage collection or package save can
     * take the FiB pause it needs.
     * Phase 2: start the stream search and observe it the same way.
     *
     * The timeout is enforceable here in a way it never was inside the old pump: one
     * pass does no engine work, so elapsed time is sampled once per frame instead of
     * around calls that can each load a level.
     */
    bool TickSearchRun(const TSharedRef<FSearchRun>& Run)
    {
        FScopedUnattendedRpc UnattendedScope;

        FFindInBlueprintSearchManager& FiBManager = FFindInBlueprintSearchManager::Get();
        const double Now = FPlatformTime::Seconds();
        const bool bOutOfTime = Now >= Run->DeadlineSeconds;

        if (!Run->Search.IsValid())
        {
            if (Run->bAutoIndex && !Run->bIndexKicked)
            {
                // Deliberately here and not on the RPC stack: the kick's one-shot
                // trigger search carries a bounded join, and even a bounded join is
                // game-thread time the caller's request should not be holding.
                Run->bIndexKicked = true;
                KickFiBIndexing();
            }

            if (Run->bAutoIndex && FiBManager.IsCacheInProgress())
            {
                Run->bIndexWaited = true;
                Run->IndexWaitSeconds = Now - Run->StartedSeconds;
                Run->IndexProgress = FiBManager.GetCacheProgress();

                if (!bOutOfTime)
                {
                    return true;
                }

                // The whole budget went to indexing, so there is no time left to search
                // with. Say that, rather than spawning a worker whose first pass would
                // immediately be detached and reported as a zero-result search.
                Run->bIndexTimedOut = true;
                FinishSearchRun(Run, /*bTimedOut=*/true);
                return false;
            }

            FStreamSearchOptions Options;
            Options.ImaginaryDataFilter = Run->Filter;
            Run->Search = MakeShared<FStreamSearch>(Run->Query, Options);
            Run->SearchStartedSeconds = Now;
            return true;
        }

        if (Run->Search->IsComplete())
        {
            FinishSearchRun(Run, /*bTimedOut=*/false);
            return false;
        }

        if (bOutOfTime)
        {
            FinishSearchRun(Run, /*bTimedOut=*/true);
            return false;
        }

        return true;
    }
}

// ---- blueprint.search ----
REGISTER_RPC_HANDLER("blueprint.search", "blueprint",
    "Search all indexed Blueprint assets using UE's Find-in-Blueprints system",
    RPC_PARAMS(
        RPC_PARAM_REQ("query", "string", "Search query — simple keywords or FiB expression syntax (e.g. Nodes(\"SetColor\"), Variables(\"Speed\"))"),
        RPC_PARAM_DEF("path", "path", "Result filter, not a cost control: Find-in-Blueprints always searches its whole index and matches outside this path are dropped afterwards, so narrowing it returns fewer results but does not make the call cheaper or the autoIndex wait shorter. The one exception is a path containing no Blueprints at all, which is answered immediately without touching the index.", "/Game"),
        RPC_PARAM_DEF("filter", "string", "Result type filter: All, Nodes, Pins, Properties, Variables, Components, Functions, Macros, Graphs", "All"),
        RPC_PARAM_DEF("limit", "number", "Maximum number of blueprint results to return", "50"),
        RPC_PARAM_DEF("autoIndex", "boolean", "Wait for the Find-in-Blueprints index to finish building before searching. The wait is passive — the editor keeps ticking — and is bounded by timeoutSeconds.", "true"),
        RPC_PARAM_DEF("timeoutSeconds", "number", "Total seconds the call may spend waiting, covering the autoIndex wait and the Find-in-Blueprints worker together. On expiry it returns whatever was matched so far with timedOut=true (or indexTimedOut=true when the index consumed the budget). Clamped to 1-600; the default leaves headroom under the transport's 120s response deadline.", "100")
    ))
{
    FString Query;
    if (!Ctx.RequireString(TEXT("query"), Query)) return true;

    const FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game"));
    const FString FilterStr = Ctx.GetString(TEXT("filter"), TEXT("All"));
    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), 50), 1, 500);
    const bool bAutoIndex = Ctx.GetBool(TEXT("autoIndex"), true);
    const double TimeoutSeconds =
        FMath::Clamp(Ctx.GetNumber(TEXT("timeoutSeconds"), 100.0), 1.0, 600.0);

    // Build the set of asset paths under the requested scope. FStreamSearch searches
    // ALL blueprints — the scope is applied to its results afterwards.
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FARFilter ARFilter;
    ARFilter.PackagePaths.Add(FName(*Path));
    ARFilter.bRecursivePaths = true;
    ARFilter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")));

    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssets(ARFilter, AssetDataList);

    // No blueprints under the requested path — return empty results without invoking
    // FStreamSearch, and without kicking the index. Answering here also keeps this
    // branch synchronous, which is the only branch that can be driven end-to-end from
    // an automation test without a real corpus.
    if (AssetDataList.Num() == 0)
    {
        TSharedPtr<FJsonObject> EmptyResponse = MakeShared<FJsonObject>();
        EmptyResponse->SetArrayField(TEXT("results"), {});
        EmptyResponse->SetNumberField(TEXT("totalMatches"), 0);
        EmptyResponse->SetNumberField(TEXT("resultsShown"), 0);
        EmptyResponse->SetNumberField(TEXT("unindexedCount"), 0);
        EmptyResponse->SetNumberField(TEXT("unindexedAssets"),
            FFindInBlueprintSearchManager::Get().GetNumberUnindexedAssets());
        // Same shape as the searched path: `timedOut` and `indexInProgress` are
        // unconditional so callers never have to distinguish "field absent" from
        // "field false".
        EmptyResponse->SetBoolField(TEXT("timedOut"), false);
        EmptyResponse->SetBoolField(TEXT("indexInProgress"),
            FFindInBlueprintSearchManager::Get().IsCacheInProgress());
        EmptyResponse->SetStringField(TEXT("message"),
            FString::Printf(TEXT("No blueprints found under %s"), *Path));
        Ctx.SendSuccess(EmptyResponse);
        return true;
    }

    const TSharedRef<FSearchRun> Run = MakeShared<FSearchRun>(Ctx.MakeAsyncToken());
    Run->Query = Query;
    Run->Path = Path;
    Run->Filter = ParseFilterString(FilterStr);
    Run->Limit = Limit;
    Run->bAutoIndex = bAutoIndex;
    Run->TimeoutSeconds = TimeoutSeconds;
    Run->StartedSeconds = FPlatformTime::Seconds();
    Run->DeadlineSeconds = Run->StartedSeconds + TimeoutSeconds;

    Run->AssetsInScope = MoveTemp(AssetDataList);
    Run->ObjectPathToAsset.Reserve(Run->AssetsInScope.Num());
    Run->NameToAsset.Reserve(Run->AssetsInScope.Num());
    for (int32 Index = 0; Index < Run->AssetsInScope.Num(); ++Index)
    {
        const FAssetData& AssetData = Run->AssetsInScope[Index];
        Run->ObjectPathToAsset.Add(AssetData.GetObjectPathString().ToLower(), Index);
        Run->NameToAsset.Add(AssetData.AssetName.ToString().ToLower(), Index);
    }

    // Every remaining step runs from the core ticker. Nothing below this line executes
    // on the RPC stack, so the game thread returns to its frame loop immediately and
    // the FiB caching operation — which needs those frames — can actually progress.
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([Run](float) -> bool
        {
            return TickSearchRun(Run);
        }),
        0.0f);

    return true;
}
