// Copyright (c) 2026 Alexander Penkin. MIT License.

// AssetMarkDirtyHandler.cpp - asset.mark_dirty, asset.is_dirty
//
// The MCP-side twin of UPinWrightPackageLibrary, so a caller does not have to route a
// one-line dirty through python.execute (which runs synchronously on the game thread
// and carries its own documented hazards, including the GC-during-Python crash class,
// board B-python-execute-reentrant-gc-crash).
//
// Why a mark-dirty verb is needed at all: a save no-ops on a clean package. Any verb
// that mutated in memory without the Modify()/MarkPackageDirty() ceremony leaves the
// edit unsaveable, and until now the only recovery was a blunt force-save of the whole
// package (EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)), which rewrites
// packages that did not need rewriting.
//
// Both verbs report the RESOLVED package name, not the caller's input: under World
// Partition / OFPA an actor's package is not the map's, so echoing the input would be
// a lie about what a subsequent save will write.
//
// All policy lives in Utils/PackageDirtyUtils.h, shared with the Python-callable
// UPinWrightPackageLibrary so the two surfaces cannot drift apart.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Asset/AssetPathParamUtils.h"
#include "Utils/PackageDirtyUtils.h"

#include "Dom/JsonObject.h"
#include "UObject/Package.h"

// ---- asset.mark_dirty ----
REGISTER_RPC_HANDLER("asset.mark_dirty", "asset",
    "Mark ONE loaded package dirty without writing anything to disk, so a later asset.save / "
    "level.save / editor.save_all actually persists it. Use after a mutation that changed memory "
    "but left the package clean - a save no-ops on a clean package and the edit is lost on editor "
    "close. Works on maps and on non-asset objects, which the engine's own "
    "EditorAssetSubsystem.set_dirty_flag refuses outright. Returns the RESOLVED package name: under "
    "World Partition an actor's package is not the map's. Refuses with MARK_DIRTY_REFUSED and a "
    "printable reason for transient / PIE / cooked / native packages and while the editor suppresses "
    "dirtying; it never reports a success it did not achieve.",
    RPC_PARAMS(
        AssetPathParamUtils::AssetPathParamReq(TEXT("assetPath"), TEXT("path"),
            TEXT("Package path (/Game/Maps/MyMap) or object path (/Game/Maps/MyMap.MyMap) of a LOADED package. Alias: path. Not loaded = PACKAGE_NOT_FOUND; this verb never loads."))
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(AssetPathParamUtils::AssetPathKeys(), AssetPath)) return true;

    FString ResolveReason;
    UPackage* Package = PinWright::PackageDirty::FindLoadedPackageForPath(AssetPath, ResolveReason);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_NOT_FOUND, ResolveReason);
        return true;
    }

    const PinWright::PackageDirty::FOutcome Outcome = PinWright::PackageDirty::MarkPackageDirty(Package);
    if (!Outcome.IsDirtyNow())
    {
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("package"), Outcome.PackageName);
        ErrorData->SetBoolField(TEXT("isDirty"), false);
        Ctx.SendError(ErrorCodes::ERR_MARK_DIRTY_REFUSED,
            FString::Printf(TEXT("Refused to mark '%s' dirty: %s"), *AssetPath, *Outcome.Reason),
            ErrorData);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("package"),   Outcome.PackageName);
    // wasDirty distinguishes "this call set the flag" from "it was already set", so an
    // idempotent script can tell whether its own edit was the one that dirtied.
    Result->SetBoolField(TEXT("wasDirty"),
        Outcome.Status == PinWright::PackageDirty::EMarkDirtyStatus::AlreadyDirty);
    Result->SetBoolField(TEXT("isDirty"),     true);
    // Explicit: this verb writes nothing. Follow with asset.save / level.save.
    Result->SetBoolField(TEXT("saved"),       false);

    Ctx.SendSuccess(Result);
    return true;
}

// ---- asset.is_dirty ----
REGISTER_RPC_HANDLER("asset.is_dirty", "asset",
    "Read whether ONE loaded package currently needs saving. Zero side effects - it does not load, "
    "dirty, or save. The read-back for asset.mark_dirty and the cheap post-condition for a "
    "\"this sequence changed nothing\" claim, which asset.get cannot answer (it carries no dirty "
    "field) and editor.save_all can only answer destructively.",
    RPC_PARAMS(
        AssetPathParamUtils::AssetPathParamReq(TEXT("assetPath"), TEXT("path"),
            TEXT("Package path (/Game/Maps/MyMap) or object path of a LOADED package. Alias: path."))
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(AssetPathParamUtils::AssetPathKeys(), AssetPath)) return true;

    FString ResolveReason;
    UPackage* Package = PinWright::PackageDirty::FindLoadedPackageForPath(AssetPath, ResolveReason);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_NOT_FOUND, ResolveReason);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("package"),   Package->GetName());
    Result->SetBoolField(TEXT("isDirty"),     Package->IsDirty());

    Ctx.SendSuccess(Result);
    return true;
}
