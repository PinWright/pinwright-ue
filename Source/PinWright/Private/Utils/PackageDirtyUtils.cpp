// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/PackageDirtyUtils.h"

#include "CoreGlobals.h"
#include "Misc/PackageName.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogPinWrightPackageDirty);

namespace PinWright::PackageDirty
{
    namespace
    {
        // Mirrors the transient walk in UObjectBaseUtility::MarkPackageDirty
        // (UObjectBaseUtility.cpp:239-249): the engine stops at the first RF_Transient
        // object on the way up to the package and returns true WITHOUT dirtying
        // anything. Detecting it here is the only way to turn that lie into a refusal.
        const UObject* FindTransientAncestorOfPackage(const UObject* Object)
        {
            const UObject* It = Object;
            const UPackage* Package = nullptr;
            while (It && !Package)
            {
                if (It->HasAnyFlags(RF_Transient))
                {
                    return It;
                }
                Package = It->GetExternalPackage();
                It = It->GetOuter();
            }
            return nullptr;
        }

        FOutcome MakeRefusal(const FString& Reason, const UPackage* Package)
        {
            FOutcome Out;
            Out.Status = EMarkDirtyStatus::Refused;
            Out.Reason = Reason;
            if (Package)
            {
                Out.PackageName = Package->GetName();
            }
            return Out;
        }

        // The shared commit step. OptionalObject, when supplied, is the object the
        // caller named: routing through UObject::MarkPackageDirty rather than
        // UPackage::SetDirtyFlag keeps engine parity (PackageMarkedDirtyEvent fires,
        // which is what refreshes the content browser's dirty marker and drives the
        // source-control checkout hook), and re-applies the engine's own load / undo /
        // cook suppression policy. The verdict is then read back off the package,
        // never taken from the call's return value.
        FOutcome CommitDirty(UPackage* Package, UObject* OptionalObject)
        {
            const FString Blocker = DescribePackageBlocker(Package);
            if (!Blocker.IsEmpty())
            {
                // Verbose, not Warning: a guard refusal is a caller mistake that the
                // return value and DescribeBlocker already report synchronously, so it
                // is not silent. Only the engine-suppression branch below is surprising
                // enough to warrant a warning, and logging guard refusals at Warning
                // would also make every negative-path automation test log noisy.
                UE_LOG(LogPinWrightPackageDirty, Verbose, TEXT("Dirty refused: %s"), *Blocker);
                return MakeRefusal(Blocker, Package);
            }

            FOutcome Out;
            Out.PackageName = Package->GetName();

            const bool bWasDirty = Package->IsDirty();
            if (OptionalObject)
            {
                OptionalObject->MarkPackageDirty();
            }
            else
            {
                // A UPackage resolves itself as its own package
                // (UObjectBase::GetExternalPackage, UObjectBase.cpp:342-346).
                Package->MarkPackageDirty();
            }

            if (!Package->IsDirty())
            {
                // Reached only through the engine's own policy gate
                // (UObjectBaseUtility.cpp:256-263). Warned about because the caller
                // could not have predicted it from the object alone, and because the
                // consequence is a later save that writes nothing.
                const FString Reason = FString::Printf(
                    TEXT("the editor suppressed the dirty on '%s' (package load, undo/redo, an open transaction, a cook, or async loading in progress)"),
                    *Out.PackageName);
                UE_LOG(LogPinWrightPackageDirty, Warning, TEXT("Dirty suppressed: %s"), *Reason);
                return MakeRefusal(Reason, Package);
            }

            Out.Status = bWasDirty ? EMarkDirtyStatus::AlreadyDirty : EMarkDirtyStatus::Dirtied;
            return Out;
        }
    }

    FString DescribePackageBlocker(const UPackage* Package)
    {
        if (!Package)
        {
            return TEXT("no package could be resolved");
        }
        if (Package == GetTransientPackage())
        {
            return TEXT("the object lives in the transient package, which is never saved");
        }
        if (Package->HasAnyFlags(RF_Transient))
        {
            return FString::Printf(TEXT("package '%s' is transient, so it is never saved"), *Package->GetName());
        }
        if (Package->HasAnyPackageFlags(PKG_CompiledIn))
        {
            return FString::Printf(TEXT("package '%s' is a native script package and has no file to save"), *Package->GetName());
        }
        if (Package->HasAnyPackageFlags(PKG_PlayInEditor))
        {
            return FString::Printf(TEXT("package '%s' is a PIE duplicate; edit the editor-world object instead, PIE packages are discarded on stop"), *Package->GetName());
        }
        if (Package->HasAnyPackageFlags(PKG_Cooked))
        {
            return FString::Printf(TEXT("package '%s' is cooked and cannot be modified"), *Package->GetName());
        }
        if (!GIsEditor)
        {
            return TEXT("the editor is not running; package dirtying is an editor-only concept");
        }
        if (IsRunningCookCommandlet())
        {
            return TEXT("a cook commandlet is running; dirtying is suppressed during cook");
        }
        if (IsGarbageCollecting())
        {
            return TEXT("garbage collection is in progress; retry after it completes");
        }
        return FString();
    }

    FString DescribeBlocker(const UObject* Object)
    {
        if (!Object)
        {
            return TEXT("object is null");
        }
        if (!IsValid(Object))
        {
            return TEXT("object is garbage or pending destruction");
        }
        if (const UObject* Transient = FindTransientAncestorOfPackage(Object))
        {
            return FString::Printf(TEXT("'%s' is transient (RF_Transient), so it is never saved and dirtying it is meaningless"),
                *Transient->GetPathName());
        }
        return DescribePackageBlocker(Object->GetPackage());
    }

    UPackage* FindLoadedPackageForPath(const FString& InPath, FString& OutReason)
    {
        OutReason.Reset();

        FString PackageName = InPath;
        PackageName.TrimStartAndEndInline();
        if (PackageName.IsEmpty())
        {
            OutReason = TEXT("path is empty");
            return nullptr;
        }

        // Accept object paths as well as package paths. A long package name can never
        // contain '.', so everything from the first dot on is the object/subobject part
        // ("/Game/Maps/Foo.Foo", "/Game/A/B.B:Sub" -> "/Game/Maps/Foo", "/Game/A/B").
        int32 DotIndex = INDEX_NONE;
        if (PackageName.FindChar(TEXT('.'), DotIndex))
        {
            PackageName.LeftInline(DotIndex);
        }

        FText InvalidReason;
        if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ true, &InvalidReason))
        {
            OutReason = FString::Printf(TEXT("'%s' is not a valid package path (%s); expected a long package name such as /Game/Maps/MyMap"),
                *InPath, *InvalidReason.ToString());
            return nullptr;
        }

        // FindPackage, never LoadPackage: an unloaded package holds no in-memory edits
        // to persist, and loading here would be an unrequested side effect that can
        // pull in arbitrary assets on the game thread.
        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (!Package)
        {
            OutReason = FString::Printf(TEXT("package '%s' is not loaded; nothing in memory to mark dirty (load the asset first, then retry)"),
                *PackageName);
            return nullptr;
        }
        return Package;
    }

    FOutcome MarkObjectDirty(UObject* Object)
    {
        const FString Blocker = DescribeBlocker(Object);
        if (!Blocker.IsEmpty())
        {
            UE_LOG(LogPinWrightPackageDirty, Verbose, TEXT("MarkObjectDirty refused: %s"), *Blocker);
            return MakeRefusal(Blocker, Object ? Object->GetPackage() : nullptr);
        }

        // CommitDirty logs its own refusals at the right verbosity for each kind.
        return CommitDirty(Object->GetPackage(), Object);
    }

    FOutcome MarkPackageDirty(UPackage* Package)
    {
        return CommitDirty(Package, nullptr);
    }

    FOutcome MarkPathDirty(const FString& InPath)
    {
        FString Reason;
        UPackage* Package = FindLoadedPackageForPath(InPath, Reason);
        if (!Package)
        {
            UE_LOG(LogPinWrightPackageDirty, Verbose, TEXT("MarkPathDirty refused: %s"), *Reason);
            return MakeRefusal(Reason, nullptr);
        }
        return MarkPackageDirty(Package);
    }

    bool IsObjectPackageDirty(const UObject* Object)
    {
        if (!Object || !IsValid(Object))
        {
            return false;
        }
        const UPackage* Package = Object->GetPackage();
        return Package && Package->IsDirty();
    }

    FString GetPackageName(const UObject* Object)
    {
        if (!Object || !IsValid(Object))
        {
            return FString();
        }
        const UPackage* Package = Object->GetPackage();
        return Package ? Package->GetName() : FString();
    }

    void FScopedPackageDirtyRestore::Capture(const UObject* Object)
    {
        if (!Object || !IsValid(Object))
        {
            return;
        }
        UPackage* Package = Object->GetPackage();
        if (!Package)
        {
            return;
        }
        for (const TPair<TWeakObjectPtr<UPackage>, bool>& Entry : Captured)
        {
            if (Entry.Key.Get() == Package)
            {
                return;
            }
        }
        Captured.Emplace(Package, Package->IsDirty());
    }

    FScopedPackageDirtyRestore::~FScopedPackageDirtyRestore()
    {
        for (const TPair<TWeakObjectPtr<UPackage>, bool>& Entry : Captured)
        {
            UPackage* Package = Entry.Key.Get();
            if (!Package || Package->IsDirty() == Entry.Value)
            {
                continue;
            }
            if (!Entry.Value)
            {
                UE_LOG(LogPinWrightPackageDirty, Warning,
                    TEXT("A read dirtied package '%s'; restoring it to clean. The guard held, but something in the read path still marks the package dirty — find and remove that call rather than relying on this restore."),
                    *Package->GetName());
            }
            Package->SetDirtyFlag(Entry.Value);
        }
    }
}
