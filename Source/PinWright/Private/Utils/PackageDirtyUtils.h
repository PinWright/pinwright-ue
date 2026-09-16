// Copyright (c) 2026 Alexander Penkin. MIT License.

// PackageDirtyUtils.h - verified package dirtying, shared by the Python-callable
// UPinWrightPackageLibrary and the asset.mark_dirty / asset.is_dirty RPC verbs.
//
// Why this exists at all: on UE 5.8 neither UObjectBaseUtility::MarkPackageDirty
// (UObjectBaseUtility.h:527) nor UPackage::SetDirtyFlag (Package.h:649) carries a
// UFUNCTION macro, so PyGenUtil::IsScriptExposedFunction (PyGenUtil.cpp:1615) never
// exports them and Python has no `mark_package_dirty` / `set_dirty_flag` at all.
// Two engine UFUNCTIONs do reach the flag, and both fall short of what handlers and
// scripts here need:
//   - UEditorAssetSubsystem::SetDirtyFlag (EditorAssetSubsystem.cpp:1162) refuses
//     !IsAsset() objects AND any package where ContainsMap() is true, so it cannot
//     dirty a level - which is the case that loses work (see board B1/E-python-cannot-
//     mark-package-dirty).
//   - UKismetSystemLibrary::TransactObject (KismetSystemLibrary.cpp:3742) forwards to
//     UObject::Modify() and dirties anything, but returns void, so a refusal is
//     indistinguishable from success; and a post-hoc Modify() snapshots the ALREADY
//     changed value into the transaction buffer, corrupting undo.
//
// Honesty contract: UObject::MarkPackageDirty() returns true in two cases where it
// did nothing at all - a transient object (UObjectBaseUtility.cpp:244) and an object
// with no package (:283) - so its return value must never be forwarded to a caller.
// Every function here resolves the package first, refuses with a printable reason,
// and reports the observed UPackage::IsDirty() rather than the call's return value.
//
// GC safety: nothing here loads, saves, or allocates UObjects, so none of it can
// trigger a CollectGarbage() while a python.execute frame is on the stack (the
// EXCEPTION_ACCESS_VIOLATION in FPythonScriptPlugin::OnPreGarbageCollect, board
// B-python-execute-reentrant-gc-crash). Path lookup is FindPackage, never LoadPackage.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Package.h"
// WeakObjectPtrTemplates.h declares TWeakObjectPtr but not its default base FWeakObjectPtr,
// and does not pull the header that defines it. Under a unity build something else in the
// blob always had; a strict-includes (Rocket / BuildPlugin) compile of the one TU that
// instantiates TWeakObjectPtr<UPackage> fails with "C2504: 'FWeakObjectPtr': base class
// undefined" without this.
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

PINWRIGHT_API DECLARE_LOG_CATEGORY_EXTERN(LogPinWrightPackageDirty, Log, All);

namespace PinWright::PackageDirty
{
    enum class EMarkDirtyStatus : uint8
    {
        // The package was clean and is now dirty.
        Dirtied,
        // The package was already dirty; the call was a no-op but the post-condition holds.
        AlreadyDirty,
        // Nothing was dirtied. Reason is non-empty and printable.
        Refused,
    };

    struct FOutcome
    {
        EMarkDirtyStatus Status = EMarkDirtyStatus::Refused;

        // Long package name of the resolved package, empty when resolution failed.
        // Under World Partition / OFPA this is the ACTOR's external package, not the
        // map's - callers that print a path must print this one, not their input.
        FString PackageName;

        // Empty unless Status == Refused.
        FString Reason;

        // True when the package is dirty after the call, whether or not this call did it.
        bool IsDirtyNow() const { return Status != EMarkDirtyStatus::Refused; }
    };

    // "" when dirtying Object's package is permitted, otherwise the reason it is not.
    // Read-only: resolves nothing, mutates nothing, never loads.
    PINWRIGHT_API FString DescribeBlocker(const UObject* Object);

    // Same, for an already-resolved package.
    PINWRIGHT_API FString DescribePackageBlocker(const UPackage* Package);

    // Resolve the loaded package behind a package path ("/Game/Maps/Foo") or an object
    // path ("/Game/Maps/Foo.Foo", "/Game/A/B.B:Sub"). Deliberately does NOT load: an
    // unloaded package has nothing to dirty, and loading from a mark-dirty call would
    // be a surprising side effect on the game thread. Returns null + OutReason.
    PINWRIGHT_API UPackage* FindLoadedPackageForPath(const FString& InPath, FString& OutReason);

    // Mark the package that owns Object dirty, then verify the flag actually took.
    PINWRIGHT_API FOutcome MarkObjectDirty(UObject* Object);

    // Mark an already-resolved package dirty, then verify.
    PINWRIGHT_API FOutcome MarkPackageDirty(UPackage* Package);

    // Resolve InPath to a loaded package and mark it dirty, then verify.
    PINWRIGHT_API FOutcome MarkPathDirty(const FString& InPath);

    // Read-only dirty probe. False for null/garbage objects.
    PINWRIGHT_API bool IsObjectPackageDirty(const UObject* Object);

    // Long package name of Object's package ("" for null/garbage). Honours OFPA
    // external packages, so this is the package a save would actually write.
    PINWRIGHT_API FString GetPackageName(const UObject* Object);

    // RAII capture/restore of package dirty flags around a READ.
    //
    // Why a read needs this: several engine "read" APIs are edit interfaces
    // underneath and dirty a package they never write. The documented case is the
    // landscape heightmap read, where FLandscapeTextureDataInfo's constructor calls
    // Texture->Modify(bShouldDirtyPackage) whether the caller intends to write or not
    // (see the read-dirtying note in Handlers/Environment/LandscapeHandler.cpp). A read
    // verb that dirties destroys the only signal a host project has that a WRITE landed,
    // because a save is gated on the dirty flag and an in-memory read-back cannot fail.
    //
    // The contract is PRESERVE, not clear: a read that runs on an already-dirty package
    // must leave it dirty. Restoring a flag that is already correct is free —
    // UPackage::SetDirtyFlag early-outs when the value is unchanged (Package.cpp:172-176).
    //
    // A restore that actually fires is logged at Warning naming the package: it means
    // something in the read path still dirties, i.e. the root cause moved rather than
    // went away, and this guard is now masking it.
    class PINWRIGHT_API FScopedPackageDirtyRestore
    {
    public:
        FScopedPackageDirtyRestore() = default;
        ~FScopedPackageDirtyRestore();

        FScopedPackageDirtyRestore(const FScopedPackageDirtyRestore&) = delete;
        FScopedPackageDirtyRestore& operator=(const FScopedPackageDirtyRestore&) = delete;

        // Capture the current dirty flag of Object's package. Null-safe, and idempotent
        // per package: a second capture of the same package keeps the FIRST reading,
        // which is the one that predates the read. Honours OFPA external packages, so
        // capturing an actor captures the package a save would write.
        void Capture(const UObject* Object);

        // Distinct packages under guard.
        int32 Num() const { return Captured.Num(); }

    private:
        // Weak: a package collected during the scope has nothing left to restore, and
        // holding it alive from a read verb would be a worse side effect than the dirty
        // flag this guard exists to remove.
        TArray<TPair<TWeakObjectPtr<UPackage>, bool>> Captured;
    };
}
