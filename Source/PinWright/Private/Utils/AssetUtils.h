// Copyright (c) 2026 Alexander Penkin. MIT License.

// Asset resolution, loading, saving, and editor helpers for PinWright
#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Utils/AssetSaveState.h"
#include "Utils/PathUtils.h"
#include "Dom/JsonObject.h"

class UPinWrightSubsystem;
class AActor;
class USceneComponent;
class ULevel;
class UMaterialInterface;
class UBlueprint;
class USkeleton;
class USkeletalMesh;
class UPhysicsAsset;
class FText;

// ============================================================================
// Asset Path Normalization
// ============================================================================

struct FNormalizedAssetPath
{
    FString Path;
    bool bIsValid;
    FString ErrorMessage;
};


// Normalize an input asset path to a valid long package name and validate it.
// Exported: called from the split-out engine-plugin integration modules (PinWrightPoseSearch).
//
// A bIsValid=true result names the CALLER'S package - the only rewrites applied are stripping an
// object/sub-object suffix, trimming trailing slashes, and prepending /Game/ to a path with no
// leading slash. It never substitutes a different package: an input that does not validate comes
// back bIsValid=false with ErrorMessage set, and Path holding the unusable cleaned form.
PINWRIGHT_API FNormalizedAssetPath NormalizeAssetPath(const FString& InPath);

// Convenience helper that tries to resolve the path and returns it, or empty if invalid.
FString TryResolveAssetPath(const FString& InPath,
                            FString* OutResolvedPath = nullptr,
                            FString* OutError = nullptr);

// Resolves an asset path from a partial path or short name.
FString ResolveAssetPath(const FString& InputPath);

// ============================================================================
// Asset Path Resolution (existence-checked)
// ============================================================================

// Verdict of ResolveAssetPathToPackage.
struct FResolvedAssetPackage
{
    // The package the caller named, e.g. "/Game/Foo/Bar". Set only when bIsValid.
    // This is the node the asset-registry dependency/referencer graph is keyed on.
    FName PackageName;
    // The registry row matched inside that package, e.g. "Bar". NAME_None when
    // the package resolved but the registry holds no row for it yet (scan still
    // in flight, or a package created in memory this session).
    FName AssetName;
    bool bIsValid = false;
    // Non-empty exactly when bIsValid is false.
    FString ErrorMessage;
};

// Resolve one caller-supplied asset path to the PACKAGE the asset-registry
// dependency/referencer graph is keyed on, and prove that package exists.
// Shared by asset.get_dependencies, asset.get_dependencies_classified,
// asset.references and asset.dependencies so all four accept the same spellings
// and refuse the same ones with the same verdict.
//
// ACCEPTS - every form below resolves to the same package node:
//   /Game/Foo/Bar             short package-path form (what asset.exists takes)
//   /Game/Foo/Bar.Bar         object-path form
//   /Game/Foo/Bar.Baz         object-path form whose ASSET name differs from the
//                             package leaf name. That is legal content and is why
//                             this does not simply append ".<leaf>" the way
//                             EditorScriptingHelpers::ConvertAnyPathToObjectPath
//                             does; a package may also hold several assets. The
//                             dependency graph is per PACKAGE, so every asset in
//                             one package resolves to the same node and therefore
//                             to the same answer. AssetName reports which row
//                             matched, so a caller can see which one it got.
//   /Game/Foo/Bar.Bar:Sub     sub-object suffix, trimmed back to the package
//   Class'/Game/Foo/Bar.Bar'  export-text form (handled by NormalizeAssetPath)
//
// REJECTS - bIsValid false with ErrorMessage set; callers emit ASSET_NOT_FOUND:
//   an empty or whitespace-only path
//   a path under no registered mount root (/Nonexistent/Foo/Bar)
//   a syntactically valid path whose package has no registry row, is not loaded,
//   and has no package file on disk
//
// It deliberately does NOT prove the named ASSET exists inside an existing
// package: /Game/Foo/Bar.NoSuchAsset resolves to /Game/Foo/Bar and reports
// whichever row the package does hold. These verbs answer a per-package
// question, so refusing there would reject a legitimate query.
FResolvedAssetPackage ResolveAssetPathToPackage(const FString& InPath);

// PIE-safe asset lookup. UEditorAssetLibrary refuses all reads while a PIE world exists,
// so request handlers must not use its false/null sentinels as existence evidence.
struct FResolvedAsset
{
    FName PackageName;
    FName AssetName;
    FSoftObjectPath ObjectPath;
    FAssetData AssetData;
    UObject* Object = nullptr;
    bool bExists = false;
    bool bRegistryOrMemoryExists = false;
    bool bPackageFileExists = false;
    bool bLoadAttempted = false;
    FString ErrorMessage;
};

// Resolves package and object path spellings through the registry, then optionally uses
// FindObject/LoadObject. This path has no EditorScriptingHelpers play-mode gate.
PINWRIGHT_API FResolvedAsset ResolveAsset(const FString& InPath, bool bLoadObject = false);

// PIE-safe content-directory probe backed by the asset registry and mounted disk path.
PINWRIGHT_API bool DoesAssetDirectoryExist(const FString& InPath);

// List asset object paths below a content directory without the PIE-gated editor library.
// Returns false when the path is invalid, missing, or the registry query cannot be completed.
// Callers performing deletion must refuse when enumeration fails.
PINWRIGHT_API bool GetAssetPathsUnderDirectory(const FString& InPath,
                                               bool bRecursive,
                                               TArray<FString>& OutPaths);

// ============================================================================
// Asset Save Helpers
// ============================================================================

// Mark an asset for a later save: MarkPackageDirty + FAssetRegistryModule::AssetCreated.
// It deliberately does NOT write the .uasset (the immediate write is the documented
// bulkdata-corruption vector on UE 5.7+), so NOTHING it does makes an edit durable.
//
// Returns void on purpose. It used to return bool, and for any non-null pointer that
// bool was the literal `true` — so every `Result->SetBoolField("saved", McpSafeAssetSave(X))`
// published a constant under a measurement name and told the caller its work was on disk
// when it was not. void makes that misuse impossible to write.
//
// To report persistence honestly after calling this, use AddMarkDirtySaveReport (below),
// which measures. To actually land a .uasset, use SaveAssetToDiskReportingPresence instead.
// Exported: called from the split-out engine-plugin integration modules (PinWrightChooser).
PINWRIGHT_API void McpSafeAssetSave(UObject* Asset);

// Result of creating a UPhysicsAsset from a skeletal mesh. The asset is always registered and
// marked dirty; when bSave is true, the helper also writes the package through the measured
// SaveAssetToDiskReportingPresence path and returns the full durability verdict. Keeping the
// creation and save outcome together prevents the two RPC callers from publishing a resident
// asset as if it had already landed on disk.
struct FPhysicsAssetCreateResult
{
    bool bSuccess = false;
    FString ErrorMessage;

    UPhysicsAsset* Asset = nullptr;
    FString AssetPath;
    FString PackageName;
    bool bSavedToDisk = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    bool bPendingFlush = false;
    int64 SizeBytes = 0;
};

// Create a UPhysicsAsset from a skeletal mesh WITHOUT UPhysicsAssetFactory's interactive
// body-generation modal (FactoryCreateNew -> CreatePhysicsAssetFromMesh -> OpenNewBodyDlg ->
// GEditor->EditorAddModalWindow), which wedges the game thread forever in a non-unattended
// MCP editor (B-physics-asset-factory-modal-hang). Allocates the asset under Outer/AssetName,
// then drives the same non-interactive core the factory runs after the modal returns Ok:
// FPhysicsAssetUtils::CreateFromSkeletalMesh with bSetToMesh/bShowProgress false so no modal
// or progress dialog reopens. The caller owns the domain error code. Single-sourced for
// skeleton.create_physics_asset and physics.setup_physics_simulation so the modal-bypass and
// persistence invariants can't drift between them.
PINWRIGHT_API FPhysicsAssetCreateResult McpCreatePhysicsAssetFromSkeletalMeshHeadless(
    UObject* Outer, FName AssetName, USkeletalMesh* Mesh, bool bSave);

// Returns true when the requested level identifier resolves to the current world.
PINWRIGHT_API bool DoesRequestedLevelMatchCurrentWorld(
    const FString& RequestedLevelPath,
    const FString& CurrentWorldPackageName,
    const FString& CurrentMapName);

// Probe whether a level package is live in memory or listed in the asset
// registry as a World, even when no .umap is on disk. With no file on disk this
// is the signal that distinguishes a real-but-unsaved world (LEVEL_NOT_PERSISTED)
// from a genuinely-missing path (FILE_NOT_FOUND). Single-sourced so both
// level.load (LevelHandler) and its alias editor.open_level (EditorCommandHandler)
// classify an unsaved-in-memory world identically. Accepts a package path, object
// path, or a path with a trailing .umap; normalises to the package name before
// the FindPackage and registry lookups.
PINWRIGHT_API bool IsLevelPackagePresentInMemoryOrRegistry(
    const FString& LevelPathOrPackageName);

// Mount-aware resolution of a level package name (e.g. /Engine/Maps/Templates/OpenWorld
// or /Game/Maps/MyLevel, with or without a trailing .umap) to its on-disk .umap
// filename. Uses FPackageName::TryConvertLongPackageNameToFilename so each mount
// (/Game -> project content, /Engine -> engine content, plugin roots -> their real
// content dirs) maps to the correct directory. Returns true and fills OutMapFilename
// on success; returns false when the package name has no registered mount.
// Replaces the broken RightChop(6)+ProjectContentDir() prefix-chop that assumed a
// fixed 6-char /Game/ prefix and corrupted every non-/Game mount.
PINWRIGHT_API bool ResolveLevelPackageToMapFilename(
    const FString& LevelPackageName,
    FString& OutMapFilename);

// True if a map package has a real .umap on disk under LevelPath. The single
// mount-aware .umap-on-disk probe shared by level.load (to decide whether a
// requested level can be loaded at all) and the read-only inspection getters
// (get_info / get_actors / get_bounds, to classify a FindLevelByPathLevel miss
// as on-disk-but-unloaded vs genuinely absent). Resolves the filename via
// ResolveLevelPackageToMapFilename + IFileManager::FileExists, falling back to
// FPackageName::DoesPackageExist. Accepts a package path, object path, or a path
// with a trailing .umap (normalised by the resolver). Returns false for empty.
PINWRIGHT_API bool DoesLevelMapExistOnDisk(const FString& LevelPath);

// Save success policy used by McpSafeLevelSave when direct file probing is unavailable.
PINWRIGHT_API bool ShouldTreatLevelSaveAsSuccess(
    bool bSaveReportedSuccess,
    bool bFileExistsOnDisk,
    bool bPackageExists,
    bool bAssetExists,
    bool bPackageClean);

// Stricter save success policy for the create-level-to-a-new-path flow. Unlike
// ShouldTreatLevelSaveAsSuccess (which intentionally OR-accepts package/clean/
// registry signals for package/mount workflows where the direct file probe is
// invalid), create_level has already proven the destination had no prior on-disk
// package, so the .umap landing on disk is the ONLY honest persistence signal.
// Returns true only when the save reported success AND the file exists on disk.
PINWRIGHT_API bool ShouldTreatCreateLevelSaveAsSuccess(
    bool bSaveReportedSuccess,
    bool bFileExistsOnDisk);

// Honest on-disk re-gate shared by the level save verbs (level.save / level.save_as).
// McpSafeLevelSave's lenient OR-policy (ShouldTreatLevelSaveAsSuccess) can report
// success for an unsaved in-memory-only world whose package is merely clean/registered,
// yielding a false saved:true with NO .umap on disk. This bundles the load-bearing
// re-gate: resolve LevelPackageOrSavePath to its .umap via ResolveLevelPackageToMapFilename
// (mount-aware, strips a trailing .umap), probe IFileManager::FileExists, then
// re-classify the reported-success boolean through ShouldTreatCreateLevelSaveAsSuccess.
// Returns the final honest saved bool. OutMapFilename receives the resolved .umap path
// (empty when the package has no registered mount) so callers can feed an asset-registry
// rescan. OutErrorCode is "" on success, SAVE_VERIFICATION_FAILED when the save reported
// success but no .umap landed, or SAVE_FAILED when the save itself did not report success.
// (B-level-save-saved-true-in-memory-no-umap)
PINWRIGHT_API bool VerifyLevelSavedToDisk(
    const FString& LevelPackageOrSavePath,
    bool bSaveReportedSuccess,
    FString& OutMapFilename,
    FString& OutErrorCode);

// Save success policy for content-asset save flows that must land an on-disk
// .uasset to count (e.g. niagara.save / niagara.create_*). Unlike the shared
// mark-dirty helper McpSafeAssetSave (which defers the disk write to avoid the
// bulkdata-corruption vector and returns void because it persists nothing), an
// asset-save RPC that promises persistence must only report saved:true when the file is
// actually on disk. Returns true only when the save reported success AND the
// file exists on disk; mirrors ShouldTreatCreateLevelSaveAsSuccess for levels.
PINWRIGHT_API bool ShouldTreatAssetSaveAsSuccess(
    bool bSaveReportedSuccess,
    bool bFileExistsOnDisk);

// Save a content asset to disk for real (SaveLoadedAssetThrottled, i.e.
// UEditorAssetLibrary::SaveLoadedAsset behind the Blueprint integrity / throttle
// gate) and report saved honestly, gated on the .uasset actually landing on disk
// via ShouldTreatAssetSaveAsSuccess. Use this for save flows that promise disk
// persistence (e.g. niagara.save / niagara.create_*) instead of the mark-dirty
// McpSafeAssetSave, which writes nothing. bForce bypasses the save
// throttle. When non-null, OutPackageName receives the package long name and
// OutSizeBytes the on-disk file size (0 when no file is present). Returns false
// for a null asset, for a package with no registered mount root, or when no
// .uasset reached disk.
//
// The disk gate is freshness, not bare existence: it captures the file's
// timestamp/size and the package's dirty flag BEFORE the save, and an unchanged
// file under a package that was dirty going in reports false. Without that, a
// throttle-skipped save was indistinguishable from a real one, because the
// .uasset written by the PREVIOUS save satisfies an existence probe.
//
// OutState receives the FULL verdict (EAssetSaveState, Utils/AssetSaveState.h). The bool
// return answers only "is it durable" and therefore answers false identically for a
// deferred edit, a failed write and a package that can never be written - three situations
// with three different remedies, all of which this function has already measured. Every
// caller that puts the result on the wire should pass OutState and report it; the trailing
// defaulted pointer exists so the ~15 call sites that predate it keep their exact meaning.
//
// Postcondition, relied on by the report emitter: the bool return is exactly
// IsAssetSaveStateDurable(*OutState).
//
// bAllowDivergedOverwrite is forwarded verbatim to SaveLoadedAssetThrottled's out-of-band
// guard. Left false - the default every existing call site keeps - a package whose .uasset
// changed on disk since it was loaded or last saved reports EAssetSaveState::DiskStateDiverged
// and nothing is written. Set it only when the CALLER has been told explicitly to discard what
// is on disk; it is not implied by bForce.
PINWRIGHT_API bool SaveAssetToDiskReportingPresence(
    UObject* Asset,
    bool bForce,
    FString* OutPackageName = nullptr,
    int64* OutSizeBytes = nullptr,
    EAssetSaveState* OutState = nullptr,
    bool bAllowDivergedOverwrite = false);

// Record an honest persistence verdict on a create/save response, shared by the
// content-create RPCs that force-save through SaveAssetToDiskReportingPresence
// (audio.authoring.create_*, niagara.create_*, metasound create). saved is true
// only when a requested save actually wrote the .uasset to disk; saveRequested
// echoes the caller's save flag so a saved:false can be told apart from a save
// that was never requested; pendingFlush keeps the legacy requested-but-not-durable signal so
// a caller never reads existsAfter:true as proof of durable state. A BlockedByPie state is the
// explicit exception: it emits pendingFlush:false because no flush can write until PIE ends.
//
// State, when supplied, adds `saveState` (the stable wire spelling from
// AssetSaveStateToWire) and `saveDetail` (what to do about it), and lets BlockedByPie clear
// pendingFlush. It is a TOptional rather than a defaulted enumerator because "the caller did
// not report a state" is not a save state - inventing an Unspecified enumerator would put a
// non-state into every switch over EAssetSaveState.
//
// Passing a State whose durability disagrees with bSavedToDisk is a caller bug and is
// logged: the two must come from the same measurement.
//
// On the requested-but-not-durable branch this ALSO publishes the PIE block when one is in
// force (`pieActive`, `editorMode`, `pieWorlds` - Utils/PieSaveBlockGuard.h). That is deliberately
// done here rather than at each call site: while PIE is up the editor refuses every single-asset
// write in the process, so the ~90 handlers that save on the caller's behalf and pass no State
// still name the blocker instead of answering with a bare pendingFlush
// (B-asset-save-omits-savestate-pie-block #4/#5, which caught the defect on
// material.authoring.set_material_instance_parameters and compile_material). Nothing is added
// when PIE is not running, so a payload from an ordinary editor is unchanged.
PINWRIGHT_API void AddAssetSaveReport(
    const TSharedPtr<FJsonObject>& Result,
    bool bSaveRequested,
    bool bSavedToDisk,
    const TOptional<EAssetSaveState>& State = TOptional<EAssetSaveState>());

// The PRE-FLIGHT form of the block above, for a verb that must refuse before doing its work
// rather than after: true when a play session is already refusing every single-asset write in
// this process, and then Result carries the same blocked-by-PIE save report AddAssetSaveReport
// emits after the fact (saveRequested, saved:false, pendingFlush:false, saveState:"blockedByPie",
// saveDetail, pieActive, editorMode, pieWorlds) and OutDescription names the holding session for
// the refusal message. Nothing is written and OutDescription is left alone when no session is up.
//
// Reporting the block afterwards is enough for a verb whose only cost is the write. It is not
// enough for one that MUTATES A LOADED ASSET on the way there - a rebuilt UStaticMesh whose write
// is then refused leaves the in-memory object ahead of its .uasset in an editor other agents
// share, with no verb that reconciles the two, so the next save-all from any stream persists a
// revision nobody reviewed. Probing first costs two pointer reads and leaves memory and disk
// agreeing. Send the result with ErrorCodes::ERR_PIE_ACTIVE, as asset.save does, so one refusal
// vocabulary covers both timings.
PINWRIGHT_API bool AddPieSaveRefusalReport(
    const TSharedPtr<FJsonObject>& Result,
    FString& OutDescription);

// Publishes the .uasset's on-disk size measured AFTER a save, alongside the flag that stops a
// non-durable save's number being read as "bytes this call wrote".
//
// `sizeBytes` was the most misleading field on a failed save: it is whatever the file happened
// to hold, so a blocked write over an existing asset answered with a plausible - and on a second
// attempt, byte-identical - size, and a blocked write over a new asset answered 0. Callers read
// the first as proof of a write and the second as a sentinel, and both readings were wrong
// (B-asset-save-pie-failure-reports-pendingflush #2/#3/#4). The number stays, because it is a
// true measurement of the file; `sizeBytesIsStale:true` is added when a requested save did not
// become durable and a pre-existing file supplied the count, which is exactly the case that
// reads as a successful write.
PINWRIGHT_API void AddAssetSaveSizeReport(
    const TSharedPtr<FJsonObject>& Result,
    int64 SizeBytesOnDisk,
    bool bSavedToDisk);

// True when the asset's in-memory state IS its on-disk state: the package has a
// .uasset on disk AND holds no unsaved changes. This is the honest answer to "is
// this edit durable" for every flow that only marks the package dirty
// (McpSafeAssetSave) rather than writing, and it is a measurement — a transient
// package, a never-written package, and a package with pending edits all report
// false. Null-safe (returns false). Mount-aware: a package name with no registered
// mount root reports false rather than crashing, because it resolves the filename
// through FPackageName::TryConvertLongPackageNameToFilename and not the
// LongPackageNameToFilename variant, which is Fatal on an unmounted root.
PINWRIGHT_API bool IsAssetPersistedToDisk(const UObject* Asset);

// Honest persistence verdict for the mark-dirty-only path (McpSafeAssetSave).
// Emits the same {saveRequested, saved, pendingFlush} wire contract as
// AddAssetSaveReport so the two save families stay indistinguishable to a caller,
// plus markedForSave to tell "deferred to a later flush" apart from "never asked
// for". saved is measured via IsAssetPersistedToDisk, never assumed: after a
// mark-dirty the package is dirty, so saved is normally false and pendingFlush
// true, which is the truth the old constant `true` was hiding. Use this at every
// site that previously reported `saved` from McpSafeAssetSave's return.
//
// Carries the same PIE block as AddAssetSaveReport on the pending branch. The mark itself is
// never blocked by PIE, but the flush this report tells the caller to run is, so a
// markedForSave:true / pendingFlush:true answer issued during PIE would otherwise send the
// caller straight into the refusal this whole guard exists to name.
PINWRIGHT_API void AddMarkDirtySaveReport(
    const TSharedPtr<FJsonObject>& Result,
    UObject* Asset,
    bool bSaveRequested);

// Safely save a level with UE 5.7+ compatibility workarounds.
bool McpSafeLevelSave(ULevel* Level, const FString& FullPath, int32 MaxRetries = 5);

// Material fallback helper for robust material loading across UE versions.
UMaterialInterface* McpLoadMaterialWithFallback(
    const FString& MaterialPath,
    bool bSilent = false);

// Outcome of SaveLoadedAssetThrottled. Deliberately NOT a bool.
//
// It used to return bool, and two of the paths that write nothing returned `true`:
// the transient-package early-out and the throttle skip whose own log line says
// "skipping save". FSaveThrottler's window is 0.5s (State/SaveThrottler.h), which is
// well inside the rate an agent issues sequential edits to one asset, so the second
// edit reported saved:true with the change still only in memory — and the on-disk
// hardening (ShouldTreatAssetSaveAsSuccess) could not catch it, because it probes
// existence and the file from the FIRST save is present.
//
// An enum class does not implicitly convert to bool, so every caller must state which
// outcomes it counts as persisted; route through WasSavePersisted rather than
// comparing enumerators by hand.
enum class ESaveLoadedAssetOutcome : uint8
{
    // UEditorAssetLibrary::SaveLoadedAsset reported a successful write.
    Saved,
    // Throttle window suppressed the save, but the package held no unsaved changes,
    // so the on-disk revision already matches memory. Nothing was lost.
    SkippedAlreadyClean,
    // Throttle window suppressed the save while the package was still dirty. The
    // caller's edit is NOT on disk. This is the honest form of the old
    // "return true; // Throttled -- treat as success".
    SkippedThrottledDirty,
    // The asset lives in the transient package, or a package flagged RF_Transient.
    // It can never be persisted at all, so no save was attempted.
    NotPersistable,
    // A save was attempted and failed, or was refused up front (Blueprint graph
    // integrity check).
    Failed,
    // Refused before any byte was written: the .uasset on disk changed since the package was
    // loaded or last saved, so this write would have discarded an out-of-band change. The one
    // outcome that is not a fault - the package is fine and the caller has two remedies
    // (re-read the file, or overwrite it deliberately via bAllowDivergedOverwrite).
    RefusedDiskStateDiverged,

    // Refused before any byte was written because a PIE session is running:
    // UEditorAssetLibrary::SaveLoadedAsset opens with
    // EditorScriptingHelpers::CheckIfInEditorAndPIE() and returns false for EVERY call while
    // GEditor->PlayWorld or GIsPlayInEditorWorld is set. Detected up front rather than inferred
    // from the resulting false, so the caller is told a fact instead of a guess, and so a
    // blocked write stops being reported through the throttle-named failure channel
    // (B-asset-save-pie-failure-reports-pendingflush). See Utils/PieSaveBlockGuard.h.
    RefusedBlockedByPie
};

// True only for the outcomes where the asset's current state is on disk.
// SkippedThrottledDirty, NotPersistable, Failed, RefusedDiskStateDiverged and
// RefusedBlockedByPie are all false.
bool WasSavePersisted(ESaveLoadedAssetOutcome Outcome);

// Throttled wrapper around UEditorAssetLibrary::SaveLoadedAsset to avoid
// triggering rapid repeated SavePackage calls.
// Uses FSaveThrottler via FPluginState for thread-safe throttle state.
//
// THE ONE PLACE THIS PLUGIN CALLS UEditorAssetLibrary::SaveLoadedAsset, and therefore the one
// chokepoint where the out-of-band-overwrite guard is applied on behalf of every single-asset
// write site (the ~31 that route through SaveAssetToDiskReportingPresence plus the ~26 that call
// here directly). bAllowDivergedOverwrite is the deliberate override: with it false - the
// default, and what every existing call site keeps meaning - a package whose file changed under
// it is REFUSED with RefusedDiskStateDiverged rather than written over. It is a separate
// parameter from bForce on purpose: bForce means "bypass the 0.5s throttle" and must not
// silently acquire "and discard whatever is on disk". See Utils/PackageDiskStateGuard.h.
ESaveLoadedAssetOutcome SaveLoadedAssetThrottled(UObject* Asset,
                              double ThrottleSecondsOverride = -1.0,
                              bool bForce = false,
                              bool bAllowDivergedOverwrite = false);

// Force a synchronous scan of a specific package or folder path.
void ScanPathSynchronous(const FString& InPath, bool bRecursive = true);

// ============================================================================
// Blueprint Helpers
// ============================================================================

// Locate and load a Blueprint asset from a variety of request formats.
UBlueprint* LoadBlueprintAsset(const FString& Req,
                               FString& OutNormalized,
                               FString& OutError);

// Find a normalized Blueprint package path for the given request string without loading the asset.
bool FindBlueprintNormalizedPath(const FString& Req, FString& OutNormalized);

// Prepare a destination UPackage for a raw UBlueprintFactory / UAnimBlueprintFactory
// create path, guarding against a name collision that would otherwise crash the editor.
//
// When a UBlueprint of this name already exists (on disk, loaded in memory, or as a
// duplicate object name inside an existing in-memory package),
// UAnimBlueprintFactory/UBlueprintFactory::FactoryCreateNew ->
// FKismetEditorUtilities::CreateBlueprint fires a fatal
// check(FindObject<UBlueprint>(Outer, ...) == 0). This helper performs the full
// three-part guard (DoesAssetExist || LoadObject<UBlueprint> pre-package, then
// CreatePackage, then FindObject<UBlueprint>(Package, *Name) post-package) in one
// place so factory-driven creators don't each re-inline the subtle sequence.
//
// PackagePath and AssetName are validated against the engine's own package and
// object-name rules BEFORE anything else happens: a path CreatePackage would log
// Fatal on (notably one containing "//") ends the editor PROCESS rather than
// failing the call, so a caller-supplied path cannot be passed through unchecked.
// PackagePath is checked AS GIVEN, so it must be the finished package name — a
// trailing slash is refused, which composing it with FString::operator/ (which
// does not double, and drops nothing) never produces.
//
// On success returns true and sets OutPackage to the created package. On a name
// collision returns false with OutError populated (and bOutNameCollision true);
// on a malformed path/name or a package-creation failure returns false with
// OutError populated and bOutNameCollision false — a caller that distinguishes
// "already exists" from "bad argument" should branch on bOutNameCollision and
// surface OutError, which quotes the engine's own reason text.
// OutAssetObjectPath always receives "PackagePath.AssetName".
PINWRIGHT_API bool PrepareBlueprintPackageGuardingNameCollision(
    const FString& PackagePath,
    const FString& AssetName,
    UPackage*& OutPackage,
    FString& OutAssetObjectPath,
    bool& bOutNameCollision,
    FString& OutError);

// Append Blueprint-family asset records whose GeneratedClass transitively subclasses RootNativeClass.
//
// Why this exists: FARFilter::ClassPaths + bRecursiveClasses matches assets by their own
// AssetClassPath, which for a Blueprint .uasset is /Script/Engine.Blueprint — never the
// native parent.  This helper performs the complementary walk via GetDerivedClassNames
// plus the FBlueprintTags::ParentClassPath asset tag so BP assets also get returned.
//
// PackagePaths (may be empty) scopes the search; bRecursivePaths follows subfolders.
// MaxResults: when non-negative, append stops once InOutAssets reaches that size.
// Duplicates against existing entries of InOutAssets (by SoftObjectPath) are skipped.
PINWRIGHT_API void AppendBlueprintAssetsDerivedFromNativeClass(
    class IAssetRegistry& AssetRegistry,
    class UClass* RootNativeClass,
    TConstArrayView<FName> PackagePaths,
    bool bRecursivePaths,
    int32 MaxResults,
    TArray<struct FAssetData>& InOutAssets);

// ============================================================================
// Generic UObject Resolution
// ============================================================================

// Resolve an arbitrary UObject by full path. Tries StaticFindObject first
// (catches transient instances, subsystems, already-loaded objects), then
// falls back to StaticLoadObject (loads asset on miss). Populates OutError
// when both lookups fail.
PINWRIGHT_API UObject* ResolveUObjectByPath(const FString& Path,
                                                              FString& OutError);

// ============================================================================
// SCS Node Helpers
// ============================================================================

// Find an SCS node by a case-insensitive name.
class USCS_Node* FindScsNodeByName(class USimpleConstructionScript* SCS,
                                    const FString& Name);

// ============================================================================
// String Conversion Helpers
// ============================================================================

FString ConvertToString(const FString& In);
FString ConvertToString(const FName& In);
FString ConvertToString(const FText& In);

// ============================================================================
// Actor Spawning
// ============================================================================

// Robust actor spawning that handles PIE and Editor modes.
// Template definition must be in the header.
template <typename T = AActor>
T* SpawnActorInActiveWorld(UClass* ActorClass, const FVector& Location,
                           const FRotator& Rotation,
                           const FString& OptionalLabel = FString());


// ============================================================================
// Verification Helpers
// ============================================================================

// Exported: called from the split-out engine-plugin integration modules (PinWrightGeometry).
PINWRIGHT_API void AddActorVerification(TSharedPtr<FJsonObject> Response, AActor* Actor);

// Companion to AddActorVerification for spawn verbs that use UE's Requested
// NameMode (SpawnParams.NameMode = ESpawnActorNameMode::Requested). That mode
// silently deduplicates the object name to <name>_0 when the requested name is
// already taken, while the label (= the actorName AddActorVerification writes)
// is allowed to collide. Call this AFTER AddActorVerification to surface the
// unique object name and a collision flag the verification helper does not set,
// so a caller can detect the rename and key follow-up calls on the
// round-trippable actorObjectName instead of the shared, non-unique actorName.
// Writes: requestedName (RequestedName), actorObjectName (Actor->GetName()),
// nameWasDeduplicated (Actor->GetName() != RequestedName).
void AddActorNameDeduplicationSignal(TSharedPtr<FJsonObject> Response, AActor* Actor, const FString& RequestedName);
void AddChainableActorFields(TSharedPtr<FJsonObject> Response, AActor* Actor);
void AddComponentVerification(TSharedPtr<FJsonObject> Response, USceneComponent* Component);
// Writes assetPath / packageName / assetName / assetClass plus a MEASURED persistence triple.
//
// assetPath is NOT clobbered. Call this after the handler has set its own assetPath and the
// handler's value is kept, PROVIDED it denotes the same package as Asset; a value naming a
// different package is replaced by the measured one and the response then carries
// requestedAssetPath plus assetPathSubstituted:true, so the substitution is never silent. It
// used to be an unconditional overwrite at ~50 call sites, which downgraded a correctly
// reported object path to the bare package path with nothing in the response saying so.
//
// packageName always carries the package handle (/Game/Folder/Asset) under its own key, so a
// caller needing it does not have to guess which form assetPath holds. It is packageName and
// not packagePath because asset.list / asset.search rows already spell the containing FOLDER
// packagePath, while asset.references / asset.dependencies already spell this value packageName.
//
//   existsAfter   - the asset is resolvable right now (registry lookup on its package)
//   existsOnDisk  - a .uasset for its package is on disk
//   pendingSave   - the package holds unsaved changes (emitted only when true)
// existsAfter used to be the literal `true`, which corroborated the equally constant
// saved:true from McpSafeAssetSave: two fields agreeing with each other while both
// disagreed with the disk. A transient-package asset (which can never be persisted)
// now reports existsAfter:false instead of true, and a mark-dirty-only create reports
// existsOnDisk:false / pendingSave:true instead of looking durable.
// Exported: called from the split-out engine-plugin integration modules (PinWrightChooser/PoseSearch).
PINWRIGHT_API void AddAssetVerification(TSharedPtr<FJsonObject> Response, UObject* Asset);
void AddAssetVerificationNested(TSharedPtr<FJsonObject> Response, const FString& FieldName, UObject* Asset);
bool VerifyAssetExists(TSharedPtr<FJsonObject> Response, const FString& AssetPath);

// True when a content file (.uasset or .umap) for PackageName is on disk. This is the
// DISK half of the pair above: VerifyAssetExists asks the asset registry, and the two can
// disagree - the engine's delete path can drop a registry row while leaving the file, and a
// registry-only post-check then reports a surviving file as a successful delete.
bool DoesPackageFileExistOnDisk(const FString& PackageName);

// ============================================================================
// Asset Registry Tags
// ============================================================================

// Project an asset's FAssetData::TagsAndValues into a typed name->value JSON
// object (each tag's FName key -> FAssetTagValueRef::AsString()). Single source
// of truth for the "tags" map shared by asset.get and asset.get_metadata, so the
// doc-promised "matches asset.get_metadata" shape is guaranteed by code rather
// than kept in lock-step by hand. Note: asset.list emits a names-only ARRAY of
// the same data and is intentionally a different shape — it does NOT use this.
TSharedPtr<FJsonObject> BuildAssetRegistryTagsObject(const struct FAssetData& AssetData);

// ============================================================================
// Control Rig Creation
// ============================================================================

// Create a Control Rig Blueprint asset. Returns nullptr on failure with OutError set.
// ControlRigBlueprintFactory is always available on UE 5.4+.
// AssetName must be a bare asset name and the composed "<PackagePath>/<AssetName>" a valid long
// package name; a malformed pair is refused (nullptr + OutError quoting the engine's reason)
// rather than reaching CreatePackage, whose Fatal on "//" would end the editor process.
UBlueprint* McpCreateControlRigBlueprint(const FString& AssetName,
                                         const FString& PackagePath,
                                         USkeleton* TargetSkeleton,
                                         FString& OutError);


// ============================================================================
// Template Implementation (must be in header)
// ============================================================================

#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#include <type_traits>

template <typename T>
T* SpawnActorInActiveWorld(UClass* ActorClass, const FVector& Location,
                           const FRotator& Rotation,
                           const FString& OptionalLabel)
{
    static_assert(std::is_base_of<AActor, T>::value,
                  "T must be derived from AActor");

    if (!GEditor || !ActorClass)
        return nullptr;

    AActor* Spawned = nullptr;
    auto SpawnDirectInWorld = [ActorClass, &Location, &Rotation](UWorld* World) -> AActor*
    {
        if (!World)
        {
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        return World->SpawnActor(ActorClass, &Location, &Rotation, SpawnParams);
    };

    // Check if PIE is active
    UWorld* TargetWorld = GEditor->PlayWorld;

    if (TargetWorld)
    {
        // PIE Path: Use World->SpawnActor for proper world context
        Spawned = SpawnDirectInWorld(TargetWorld);
    }
    else
    {
        // Editor Path:
        // - In unattended/null-RHI runs, avoid EditorActorSubsystem placement code
        //   because it depends on viewport hit-proxy state and can crash.
        // - In normal interactive editor, keep subsystem spawning behavior.
        const bool bNullRHI = FParse::Param(FCommandLine::Get(), TEXT("NullRHI"));
        const bool bPreferDirectEditorSpawn =
            IsRunningCommandlet() || FApp::IsUnattended() || bNullRHI;

        if (bPreferDirectEditorSpawn)
        {
            UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
            Spawned = SpawnDirectInWorld(EditorWorld);
        }
        else
        {
            UEditorActorSubsystem* ActorSS =
                GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
            if (ActorSS)
            {
                Spawned = ActorSS->SpawnActorFromClass(ActorClass, Location, Rotation);
                if (Spawned)
                {
                    // Explicit transform to ensure proper placement and registration
                    Spawned->SetActorLocationAndRotation(Location, Rotation, false, nullptr,
                                                         ETeleportType::TeleportPhysics);
                }
            }
            if (!Spawned)
            {
                UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
                Spawned = SpawnDirectInWorld(EditorWorld);
            }
        }
    }

    // Set optional label for easy identification in World Outliner
    if (Spawned && !OptionalLabel.IsEmpty())
    {
        Spawned->SetActorLabel(OptionalLabel);
    }

    return Cast<T>(Spawned);
}
