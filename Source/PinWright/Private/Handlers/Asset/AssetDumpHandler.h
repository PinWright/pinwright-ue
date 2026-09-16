// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Utils/AssetDumpWriter.h"

struct FPendingDumpEntry;

namespace DumpFileNames
{
    inline constexpr const TCHAR* Meta          = TEXT("meta.json");
    inline constexpr const TCHAR* Properties    = TEXT("properties.json");
    inline constexpr const TCHAR* Mgir          = TEXT("mgir.txt");
    inline constexpr const TCHAR* TreeXml       = TEXT("tree.xml");
    inline constexpr const TCHAR* WidgetAnimations = TEXT("widget_animations.json");
    inline constexpr const TCHAR* BpirTxt       = TEXT("bpir.txt");
    inline constexpr const TCHAR* Scs           = TEXT("scs.json");
    inline constexpr const TCHAR* ScsTxt        = TEXT("scs.txt");
    inline constexpr const TCHAR* WorldSettings = TEXT("world_settings.json");
    inline constexpr const TCHAR* LevelBp       = TEXT("level_bp.txt");
    inline constexpr const TCHAR* Sublevels     = TEXT("sublevels.json");
    inline constexpr const TCHAR* ActorsManifest = TEXT("actors/manifest.json");
    inline constexpr const TCHAR* NiagaraParameters = TEXT("niagara_parameters.json");
    inline constexpr const TCHAR* NiagaraStack = TEXT("niagara_stack.json");
    inline constexpr const TCHAR* NiagaraGraphs = TEXT("niagara_graphs.json");
    inline constexpr const TCHAR* NiagaraCompile = TEXT("niagara_compile.json");
    inline constexpr const TCHAR* Cascade = TEXT("cascade.json");
    inline constexpr const TCHAR* AnimGraph = TEXT("anim_graph.json");
    inline constexpr const TCHAR* Agir       = TEXT("agir.txt");
    inline constexpr const TCHAR* PcgIr      = TEXT("pcgir.txt");
    inline constexpr const TCHAR* DataTable = TEXT("data_table.json");
    inline constexpr const TCHAR* StaticMesh = TEXT("static_mesh.json");
    inline constexpr const TCHAR* StaticMeshTxt = TEXT("static_mesh.txt");
    inline constexpr const TCHAR* SkeletalMesh = TEXT("skeletal_mesh.json");
    inline constexpr const TCHAR* PhysicsAsset = TEXT("physics_asset.json");
    inline constexpr const TCHAR* Skeleton = TEXT("skeleton.json");
    inline constexpr const TCHAR* MaterialInstance = TEXT("material_instance.json");
    inline constexpr const TCHAR* Texture = TEXT("texture.json");
    inline constexpr const TCHAR* TextureTxt = TEXT("texture.txt");
    inline constexpr const TCHAR* SoundWave = TEXT("sound_wave.json");
    inline constexpr const TCHAR* SoundCue = TEXT("sound_cue.json");
    inline constexpr const TCHAR* Scir = TEXT("scir.txt");
    inline constexpr const TCHAR* Btir = TEXT("btir.txt");
    inline constexpr const TCHAR* Crir = TEXT("crir.txt");
    inline constexpr const TCHAR* Nir = TEXT("nir.txt");
    inline constexpr const TCHAR* MetaSound = TEXT("metasound.json");
    inline constexpr const TCHAR* Msir = TEXT("msir.txt");
    inline constexpr const TCHAR* LevelSequence = TEXT("level_sequence.json");
    inline constexpr const TCHAR* UserDefinedStruct = TEXT("user_defined_struct.json");
    inline constexpr const TCHAR* StateTree = TEXT("state_tree.json");
    inline constexpr const TCHAR* EnvQuery = TEXT("env_query.json");
    inline constexpr const TCHAR* AnimSequence = TEXT("anim_sequence.json");
    inline constexpr const TCHAR* AnimMontage = TEXT("anim_montage.json");
    inline constexpr const TCHAR* BlendSpace = TEXT("blend_space.json");
    inline constexpr const TCHAR* LandscapeGrassType = TEXT("landscape_grass_type.json");
    inline constexpr const TCHAR* SubsurfaceProfile = TEXT("subsurface_profile.json");
    inline constexpr const TCHAR* MapReferences = TEXT("map_references.json");
    inline constexpr const TCHAR* WidgetPreviewPng = TEXT("preview.png");
}

// Centralized error codes emitted by DumpSingleAsset and consumed by TickFolderDump's
// skip-stub branch. Keeping them in a single place avoids string-literal drift between
// emit sites and comparison sites (e.g. the PATH_TOO_LONG short-circuit).
namespace AssetDumpErrorCodes
{
    // AssetFileMissing is emitted when DoesPackageExist fails the pre-LoadObject check —
    // distinguishes orphan/baker residue (file truly absent) from genuine load failures.
    inline constexpr const TCHAR* AssetFileMissing = TEXT("ASSET_FILE_MISSING");
    inline constexpr const TCHAR* AssetLoadFailed  = TEXT("ASSET_LOAD_FAILED");
    inline constexpr const TCHAR* PathTooLong      = TEXT("PATH_TOO_LONG");
    inline constexpr const TCHAR* DumpWriteFailed  = TEXT("DUMP_WRITE_FAILED");
    inline constexpr const TCHAR* AssetNoBaseline  = TEXT("ASSET_NO_BASELINE");
    inline constexpr const TCHAR* AssetCompileTimeout = TEXT("ASSET_COMPILE_TIMEOUT");
    inline constexpr const TCHAR* NotSupported     = TEXT("NOT_SUPPORTED");
}

namespace AssetDumpHandler
{
    struct FDumpSingleResult
    {
        TArray<FString> WrittenPaths;
        TArray<TPair<FString, FString>> FileErrors; // filename -> reason
        FString DumpDir;
        FString ErrorCode;    // empty on success
        FString ErrorMessage; // empty on success
        FString Mode;         // "dump" or "diff" after a successful call
        // Cooperative async sweeps may return before writing when an asset's
        // platform data is still compiling. No arbitrary UObject work is preempted.
        bool bDeferredForCompilation = false;

        // The widget-preview aspect (preview.png) is opt-in AND only runs for
        // UWidgetBlueprint assets, so the two alpha facts carry a "did it run" gate rather
        // than standing alone. Without the gate a 0.0 fraction on a dump that never captured
        // anything reads as "measured, and none of it was transparent" -- a measurement
        // nobody took. Every one of the three is copied from
        // WidgetDesignerCaptureUtil::FCaptureInfo after a successful capture; none is ever
        // assigned a literal, which is what makes the reported fraction evidence about these
        // pixels rather than a promise about the code path.
        bool   bWidgetPreviewCaptured = false;
        bool   bWidgetPreviewOpaqueStamped = false;
        double WidgetPreviewAlphaZeroFraction = 0.0;
    };

    // BaselineDirty is the pre-dump dirty-package snapshot used by the cache
    // eligibility gate (see AssetDumpHandlerInternal.h); an empty set treats any
    // current dirt as dump-induced.
    PINWRIGHT_API FDumpSingleResult DumpSingleAsset(const FString& NormalizedPath, const FString& OutRoot, bool bDiff = false, bool bIncludeWidgetScreenshot = false, const TSet<FName>& BaselineDirty = TSet<FName>(), bool bDeferAsyncCompilation = false);

    // Returns the full object path ("/Game/Pkg.Inner", FAssetData::GetObjectPathString())
    // queued for a folder-sweep entry. Loading must target the specific inner object —
    // multi-asset packages (e.g. "Bake Out Materials" GUID outputs) have no object named
    // after the package tail, so a bare package name fails to load. Dump dirs stay
    // package-shaped regardless: DumpSingleAsset keys the dir off the loaded asset's
    // outermost package name, not this queue path.
    PINWRIGHT_API FString MakePendingDumpPath(const FAssetData& Data);

    PINWRIGHT_API bool ShouldSkipFolderDumpAsset(const FAssetData& Data, bool bIncludeLevels = false);

    // PendingAssets is consumed with Pop(), so the backing array is sorted in
    // reverse lexical order to produce ascending, deterministic work order.
    PINWRIGHT_API void SortPendingAssetsForProcessing(TArray<FPendingDumpEntry>& PendingAssets);

    inline constexpr double AsyncCompilationTimeoutSeconds = 120.0;
    PINWRIGHT_API bool HasAsyncCompilationTimedOut(
        double WaitStartedSeconds,
        double NowSeconds,
        double TimeoutSeconds = AsyncCompilationTimeoutSeconds);

    // Floor on the watermark trigger. A release step costs a full compilation drain
    // and a full-purge collect, so a watermark the sweep cannot get back under (the
    // editor's own baseline is already over it, or the survivors are all referenced)
    // must not turn into one release per asset.
    inline constexpr int32 DumpReleaseMinAssetsBetweenSteps = 25;

    // Pure decision for the folder sweep's release step, evaluated once per tick.
    // Fires when IntervalAssets > 0 and that many assets have been processed since the
    // last release, or when WatermarkFraction > 0 and the process working set has
    // reached that fraction of physical RAM with at least
    // DumpReleaseMinAssetsBetweenSteps assets processed. Zero for either knob disables
    // that trigger; zero for both disables the release step entirely.
    PINWRIGHT_API bool ShouldRunDumpReleaseStep(
        int32 AssetsSinceRelease,
        int32 IntervalAssets,
        uint64 WorkingSetBytes,
        uint64 TotalPhysicalBytes,
        float WatermarkFraction);

    PINWRIGHT_API TArray<FString> ReconcileMirrorSubtree(const FString& SweptRoot,
                                                                           const TSet<FString>& LiveDirs);

    struct FFolderDumpStart
    {
        FString   ErrorCode;              // empty on success
        FString   ErrorMessage;
        FString   RootDir;                // swept subtree root (absolute)
        FString   FolderPath;             // echoed input, normalized
        FDateTime StartedAt;              // UTC when the sweep was queued
        int32     AssetCount = 0;         // number of filtered unique package candidates
        int32     QueuedCount = 0;        // number of assets queued to process
        int32     UnchangedCount = 0;     // number of cache-fresh assets skipped
    };

    struct FSingleAssetDumpStart
    {
        FString   ErrorCode;              // empty on success
        FString   ErrorMessage;
        FString   AssetPath;
        FString   DumpDir;
        FDateTime StartedAt;              // UTC when the dump was queued
        bool      bDiff = false;
    };

    struct FFolderDumpStatus
    {
        bool      bInProgress = false;
        FString   RootDir;
        FString   FolderPath;
        FString   AssetPath;
        FDateTime StartedAt;
        FString   CurrentAsset;
        FString   CurrentPhase;
        FDateTime CurrentPhaseStartedAt;
        double    CurrentPhaseElapsedSeconds = 0.0;
        double    LastAssetElapsedSeconds = 0.0;
    };

    PINWRIGHT_API FFolderDumpStart StartAsyncFolderDump(
        const FString& FolderPath, bool bRecursive, const FString& OutRoot, bool bIncludeLevels = false, bool bIncludeWidgetScreenshot = false, bool bForce = false, bool bDeferPreflight = false);

    PINWRIGHT_API FSingleAssetDumpStart StartAsyncSingleAssetDump(
        const FString& NormalizedPath, const FString& OutRoot, bool bDiff = false, bool bIncludeWidgetScreenshot = false);

    // Associates the ticket allocated by FHandlerContext::StartJob with the
    // already-started ticker and installs the cancellation callback.
    PINWRIGHT_API bool AttachJobTicketToAsyncDump(const FString& TicketId);

    PINWRIGHT_API bool IsWorldAssetPath(const FString& NormalizedPath);

    PINWRIGHT_API FFolderDumpStatus GetAsyncFolderDumpStatus();
}
