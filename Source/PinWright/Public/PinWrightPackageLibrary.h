// Copyright (c) 2026 Alexander Penkin. MIT License.

// PinWrightPackageLibrary.h - the Python-callable package dirty-flag surface.
//
// UE 5.8 exposes no `mark_package_dirty` / `set_dirty_flag` to Python:
// UObjectBaseUtility::MarkPackageDirty (UObjectBaseUtility.h:527) and
// UPackage::SetDirtyFlag (Package.h:649) carry no UFUNCTION macro, and
// PyGenUtil::IsScriptExposedFunction (PyGenUtil.cpp:1615) exports a function only
// when FUNC_BlueprintCallable or FUNC_BlueprintEvent is set. `unreal.Package` exists
// only as a forced empty wrapper (pulled in as a referenced parameter type) and
// carries no dirty methods at all.
//
// The two engine UFUNCTIONs that do reach the flag are documented on each function
// below; both are usable and neither covers levels + honest results + path input,
// which is what this library adds. See docs/wiki-src/python.md.
//
// Every function is static and BlueprintCallable on a UBlueprintFunctionLibrary in an
// Editor module with LoadingPhase "Default", which is the ordinary route to
// `unreal.PinWrightPackageLibrary.<snake_case_name>()`: the Python plugin wraps types
// from modules loaded after it starts via FPythonScriptPlugin::OnModulesChanged ->
// FPyWrapperTypeRegistry::GenerateWrappedTypesForModule (PythonScriptPlugin.cpp:1323,
// PyWrapperTypeRegistry.cpp:1073), so the loading phase is not a constraint.
//
// No out-parameters, deliberately: PyGenUtil folds a return value plus out-params into
// a Python TUPLE, and a non-empty tuple is always truthy - so `if lib.mark(...)` would
// silently pass on failure. Refusal reasons travel through
// DescribeMarkDirtyBlocker and the LogPinWrightPackageDirty log category instead.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PinWrightPackageLibrary.generated.h"

class AActor;

UCLASS()
class PINWRIGHT_API UPinWrightPackageLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Mark the package that owns Object dirty, so the next save actually writes it.
     * The replacement for the unexposed Actor.mark_package_dirty / Package.set_dirty_flag.
     *
     * Returns true only when the package is observably dirty afterwards (UPackage::IsDirty),
     * including when it was already dirty. Returns false - never silently - for a null or
     * garbage object, a transient object, the transient package, a native script package,
     * a PIE duplicate, a cooked package, or when the editor suppresses dirtying (load,
     * undo/redo, open transaction, cook, async load). Call DescribeMarkDirtyBlocker for
     * the reason, or read the LogPinWrightPackageDirty warning.
     *
     * Honours World Partition / OFPA: an actor in an external package dirties THAT
     * package, not the map's. Use GetPackageName to see which one was touched.
     *
     * Engine alternatives, for reference: EditorAssetSubsystem.set_dirty_flag(obj, True)
     * works for content assets but refuses non-assets and any package containing a map;
     * SystemLibrary.transact_object(obj) dirties anything but returns void and writes the
     * object into the open transaction, which corrupts undo when called after the edit.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Package")
    static bool MarkPackageDirty(UObject* Object);

    /**
     * Mark the package that owns Actor dirty. Identical contract to MarkPackageDirty,
     * typed for the level-editing case that the engine's own EditorAssetSubsystem.
     * set_dirty_flag cannot serve at all (it refuses !IsAsset() objects and any package
     * where ContainsMap() is true, EditorAssetSubsystem.cpp:1162-1171).
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Package")
    static bool MarkActorPackageDirty(AActor* Actor);

    /**
     * Mark a loaded package dirty by path. Accepts a package path ("/Game/Maps/MyMap")
     * or an object path ("/Game/Maps/MyMap.MyMap").
     *
     * Does NOT load: an unloaded package holds no in-memory edit to persist, so an
     * unloaded path returns false rather than pulling assets in as a side effect.
     * Returns false on a malformed path; never asserts.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Package")
    static bool MarkPackageDirtyByPath(const FString& AssetPath);

    /** True when the package that owns Object currently needs saving. False for null/garbage. */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Package")
    static bool IsPackageDirty(UObject* Object);

    /** True when the loaded package at AssetPath currently needs saving. False when unloaded or malformed. */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Package")
    static bool IsPackageDirtyByPath(const FString& AssetPath);

    /**
     * "" when MarkPackageDirty(Object) would be permitted, otherwise the reason it would
     * be refused. Read-only. This is the diagnostic that turns a bare False into an
     * actionable message.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Package")
    static FString DescribeMarkDirtyBlocker(UObject* Object);

    /**
     * Long package name of the package that owns Object - the package a save would
     * actually write. Under World Partition this is the actor's external package
     * (/Game/Maps/__ExternalActors__/...), not the map, which is why a caller must print
     * this rather than the path it passed in. "" for null/garbage.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Package")
    static FString GetPackageName(UObject* Object);
};
