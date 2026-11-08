// Copyright (c) 2026 Alexander Penkin. MIT License.

// Game-safe asset helpers: path resolution, Blueprint lookup, actor spawn, verification.
// Editor-only saves and asset creation: the editor module's Utils/AssetUtils.h (which includes this).
#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Utils/PathUtils.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include <type_traits>

class AActor;
class USceneComponent;
class UBlueprint;
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
PINWRIGHTRUNTIME_API FNormalizedAssetPath NormalizeAssetPath(const FString& InPath);

// Convenience helper that tries to resolve the path and returns it, or empty if invalid.
PINWRIGHTRUNTIME_API FString TryResolveAssetPath(const FString& InPath,
                            FString* OutResolvedPath = nullptr,
                            FString* OutError = nullptr);

// Resolves an asset path from a partial path or short name.
PINWRIGHTRUNTIME_API FString ResolveAssetPath(const FString& InputPath);

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
PINWRIGHTRUNTIME_API FResolvedAssetPackage ResolveAssetPathToPackage(const FString& InPath);

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
PINWRIGHTRUNTIME_API FResolvedAsset ResolveAsset(const FString& InPath, bool bLoadObject = false);

// PIE-safe content-directory probe backed by the asset registry and mounted disk path.
PINWRIGHTRUNTIME_API bool DoesAssetDirectoryExist(const FString& InPath);

// List asset object paths below a content directory without the PIE-gated editor library.
// Returns false when the path is invalid, missing, or the registry query cannot be completed.
// Callers performing deletion must refuse when enumeration fails.
PINWRIGHTRUNTIME_API bool GetAssetPathsUnderDirectory(const FString& InPath,
                                               bool bRecursive,
                                               TArray<FString>& OutPaths);
// True when the asset's in-memory state IS its on-disk state: the package has a
// .uasset on disk AND holds no unsaved changes. This is the honest answer to "is
// this edit durable" for every flow that only marks the package dirty
// (McpSafeAssetSave) rather than writing, and it is a measurement — a transient
// package, a never-written package, and a package with pending edits all report
// false. Null-safe (returns false). Mount-aware: a package name with no registered
// mount root reports false rather than crashing, because it resolves the filename
// through FPackageName::TryConvertLongPackageNameToFilename and not the
// LongPackageNameToFilename variant, which is Fatal on an unmounted root.
PINWRIGHTRUNTIME_API bool IsAssetPersistedToDisk(const UObject* Asset);

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
PINWRIGHTRUNTIME_API void AddMarkDirtySaveReport(
    const TSharedPtr<FJsonObject>& Result,
    UObject* Asset,
    bool bSaveRequested);

// Force a synchronous scan of a specific package or folder path.
PINWRIGHTRUNTIME_API void ScanPathSynchronous(const FString& InPath, bool bRecursive = true);

// Locate and load a Blueprint asset from a variety of request formats.
PINWRIGHTRUNTIME_API UBlueprint* LoadBlueprintAsset(const FString& Req,
                               FString& OutNormalized,
                               FString& OutError);

// Find a normalized Blueprint package path for the given request string without loading the asset.
PINWRIGHTRUNTIME_API bool FindBlueprintNormalizedPath(const FString& Req, FString& OutNormalized);

// ============================================================================
// Generic UObject Resolution
// ============================================================================

// Resolve an arbitrary UObject by full path. Tries StaticFindObject first
// (catches transient instances, subsystems, already-loaded objects), then
// falls back to StaticLoadObject (loads asset on miss). Populates OutError
// when both lookups fail.
PINWRIGHTRUNTIME_API UObject* ResolveUObjectByPath(const FString& Path,
                                                              FString& OutError);

// ============================================================================
// String Conversion Helpers
// ============================================================================

PINWRIGHTRUNTIME_API FString ConvertToString(const FString& In);
PINWRIGHTRUNTIME_API FString ConvertToString(const FName& In);
PINWRIGHTRUNTIME_API FString ConvertToString(const FText& In);

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
PINWRIGHTRUNTIME_API void AddActorVerification(TSharedPtr<FJsonObject> Response, AActor* Actor);

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
PINWRIGHTRUNTIME_API void AddActorNameDeduplicationSignal(TSharedPtr<FJsonObject> Response, AActor* Actor, const FString& RequestedName);
PINWRIGHTRUNTIME_API void AddChainableActorFields(TSharedPtr<FJsonObject> Response, AActor* Actor);
PINWRIGHTRUNTIME_API void AddComponentVerification(TSharedPtr<FJsonObject> Response, USceneComponent* Component);
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
PINWRIGHTRUNTIME_API void AddAssetVerification(TSharedPtr<FJsonObject> Response, UObject* Asset);
PINWRIGHTRUNTIME_API void AddAssetVerificationNested(TSharedPtr<FJsonObject> Response, const FString& FieldName, UObject* Asset);
PINWRIGHTRUNTIME_API bool VerifyAssetExists(TSharedPtr<FJsonObject> Response, const FString& AssetPath);

// True when a content file (.uasset or .umap) for PackageName is on disk. This is the
// DISK half of the pair above: VerifyAssetExists asks the asset registry, and the two can
// disagree - the engine's delete path can drop a registry row while leaving the file, and a
// registry-only post-check then reports a surviving file as a successful delete.
PINWRIGHTRUNTIME_API bool DoesPackageFileExistOnDisk(const FString& PackageName);

// ============================================================================
// Asset Registry Tags
// ============================================================================

// Project an asset's FAssetData::TagsAndValues into a typed name->value JSON
// object (each tag's FName key -> FAssetTagValueRef::AsString()). Single source
// of truth for the "tags" map shared by asset.get and asset.get_metadata, so the
// doc-promised "matches asset.get_metadata" shape is guaranteed by code rather
// than kept in lock-step by hand. Note: asset.list emits a names-only ARRAY of
// the same data and is intentionally a different shape — it does NOT use this.
PINWRIGHTRUNTIME_API TSharedPtr<FJsonObject> BuildAssetRegistryTagsObject(const struct FAssetData& AssetData);

// Non-template body of SpawnActorInActiveWorld. Editor: PIE world, else the editor world
// (EditorActorSubsystem in an interactive editor), label applied. Game: PinWright::GetTargetWorld(),
// no label (game builds have none).
PINWRIGHTRUNTIME_API AActor* SpawnActorInActiveWorldImpl(UClass* ActorClass, const FVector& Location,
                                                         const FRotator& Rotation,
                                                         const FString& OptionalLabel);

template <typename T>
T* SpawnActorInActiveWorld(UClass* ActorClass, const FVector& Location,
                           const FRotator& Rotation,
                           const FString& OptionalLabel)
{
    static_assert(std::is_base_of<AActor, T>::value,
                  "T must be derived from AActor");
    return Cast<T>(SpawnActorInActiveWorldImpl(ActorClass, Location, Rotation, OptionalLabel));
}
