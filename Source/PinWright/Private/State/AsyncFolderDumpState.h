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
