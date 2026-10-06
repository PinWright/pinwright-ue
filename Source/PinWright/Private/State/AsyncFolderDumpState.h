// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/TopLevelAssetPath.h"

class SNotificationItem;
class FAssetDumpNotificationWidget;

enum class EAsyncAssetDumpKind : uint8
{
    None,
    Folder,
    SingleAsset
};

// One queued dump work item. Carries the full object path to load (multi-asset
// packages must load a specific inner object — the package tail may name no
// object at all) plus the package identity and registry facts the skip-stub
// branch needs when the load fails.
struct FPendingDumpEntry
{
    FString ObjectPath;
    FString PackageName;
    FTopLevelAssetPath ClassPath;
    bool bIsMapOrWorld = false;
};

struct FAsyncDumpSkip
{
    FString AssetPath;
    FString Code;
    FString Message;
};

// An asset whose synchronous dump held the game thread past the slow-asset threshold.
struct FAsyncDumpSlowAsset
{
    FString AssetPath;
    double  ElapsedSeconds = 0.0;
    FString Phase;
    double  PhaseSeconds = 0.0;
};

// One LoadPackageAsync request issued before an asset's synchronous dump. Shared with the
// completion delegate, so a sweep that ends or is cancelled while the load still runs
// leaves the delegate a live object to write to.
struct FAsyncDumpLoad
{
    FString ObjectPath;
    bool    bDone = false;
    // Time this sweep spent pumping the loader for it, and the longest single pump. The
    // timeout counts the first, so a throttled editor ticking rarely does not time out a
    // load that got little loading time.
    double  PumpedSeconds = 0.0;
    double  LongestPumpSeconds = 0.0;
    // Wall-clock start, for the backstop that still ends a load nothing can pump (async
    // loading suspended).
    double  StartedSeconds = 0.0;
};

struct FAsyncFolderDumpState
{
    EAsyncAssetDumpKind Kind = EAsyncAssetDumpKind::None;
    FString   RootDir;
    FString   FolderPath;
    FString   AssetPath;
    FString   OutRoot;
    FDateTime StartedAt;
    bool bInProgress = false;
    bool bDiff = false;
    bool bIncludeWidgetScreenshot = false;
    bool bRecursive = true;
    bool bIncludeLevels = false;
    bool bForce = false;
    bool bPreflightPending = false;
    bool bRegistryScanComplete = false;
    int32 TotalAssetCount = 0;
    int32 PreflightProcessedCount = 0;
    int32 QueuedCount = 0;
    int32 DumpedCount = 0;
    int32 UnchangedCount = 0;
    int32 SkipCount = 0;
    int32 CompileDeferralCount = 0;
    int32 CompileTimeoutCount = 0;
    TArray<FPendingDumpEntry> PendingAssets;
    TArray<FPendingDumpEntry> PreflightCandidates;
    // First observation time for assets whose platform data is still compiling.
    // The ticker requeues these assets instead of synchronously forcing compilation.
    TMap<FString, double> CompileWaitStartedSeconds;
    TMap<FString, double> CompileWaitLastProgressSeconds;
    // Last observed FAssetCompilingManager backlog. A drop means something finished
    // compiling, which restarts every pending wait: the timeout is a no-progress cap,
    // not a deadline an asset can hit just because the sweep is long.
    int32 LastRemainingCompileCount = 0;
    TSet<FString>   LiveDumpDirs;
    // Package names from the dump root's dump-stalled.txt; folder sweeps skip them.
    TSet<FString>   StalledPackages;
    // The entry whose package is loading asynchronously; the sweep yields its ticks until
    // the load completes or times out.
    FPendingDumpEntry LoadingEntry;
    TSharedPtr<FAsyncDumpLoad> ActiveLoad;
    // Timed-out loads still running in the loader. No release step runs while one is in
    // flight: its FlushAsyncLoading would block on the load the timeout gave up on.
    TArray<TSharedPtr<FAsyncDumpLoad>> AbandonedLoads;
    int32 LoadTimeoutCount = 0;
    TArray<FAsyncDumpSlowAsset> SlowAssets;
    // Set once the end-of-sweep (Final) release step has run; the sweep finalizes only
    // after it, so its last loads are not left resident.
    bool  bFinalReleaseDone = false;
    // Packages this sweep brought into memory itself (absent from memory when the
    // sweep reached them). The release step unloads only these, so packages the user,
    // the editor world or an open asset editor already had resident are never touched.
    // FNames survive GC across ticks; raw UPackage* would not.
    TSet<FName>     SweepLoadedPackages;
    // Assets processed since the last release step; drives the count trigger.
    int32 AssetsSinceRelease = 0;
    // Release-step bookkeeping, published in the progress payload.
    int32 ReleaseStepCount = 0;
    int32 ReleasedPackageCount = 0;
    // Set between requesting a collect and observing that it ran. The sweep loads no
    // further assets while this is true, so the collect lands before the resident set
    // starts growing again.
    bool  bAwaitingReleaseGc = false;
    int32 ReleaseGcBaseline = 0;
    double ReleaseGcRequestedSeconds = 0.0;
    uint64 ReleaseWorkingSetBeforeBytes = 0;
    // Working set measured AFTER the last release collect (seeded from the first tick
    // that could release, so the first interval is bounded too). The growth trigger
    // measures against this, not against the pre-collect figure: growth is what the
    // sweep has accumulated since the last time memory was actually returned.
    uint64 ReleaseWorkingSetBaselineBytes = 0;
    // Packages already dirty when the dump started; anything dirtied by the dump
    // itself is cleared each tick so the editor never prompts to save mid-sweep.
    TSet<FName>     BaselineDirty;
    FTSTicker::FDelegateHandle TickerHandle;
    FString         JobTicketId;
    FString         CurrentAsset;
    FString         CurrentPhase;
    FDateTime       CurrentPhaseStartedAt;
    double          CurrentPhaseStartedSeconds = 0.0;
    double          WorkStartedSeconds = 0.0;
    double          LastAssetElapsedSeconds = 0.0;
    // Editor-visible progress for MCP-triggered folder dumps. The notification
    // manager owns the item; state only retains a weak handle for updates and
    // terminal fade-out.
    TWeakPtr<SNotificationItem> ProgressNotification;
    TSharedPtr<FAssetDumpNotificationWidget> ProgressNotificationWidget;
    bool            bCancelRequested = false;
    bool            bTickActive = false;
    TArray<FString> WrittenPaths;
    TArray<TPair<FString, FString>> FileErrors;
    TArray<FAsyncDumpSkip> AssetSkips;
    FString DumpDir;
    FString ErrorCode;
    FString ErrorMessage;
    FString Mode;
};
