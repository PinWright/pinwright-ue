// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared create-verb destination-slot resolver for material.authoring.create_* handlers.
//
// Background (E-material-create-combined-assetpath-split): every *operate* verb in
// material.authoring takes a single combined `assetPath` (MaterialHandlerUtils), but the
// create_* verbs split the destination into two slots — `name` (leaf) + `path` (folder) —
// with no way to pass the one fully-qualified asset path the rest of the namespace accepts.
// A caller priming on "every material verb takes one assetPath" guesses `assetPath` on the
// create verb and eats a hard MISSING_REQUIRED_PARAM 'name' round-trip.
//
// This is the convention for the whole create-* surface (the audio/level/volume create-verb
// tickets are blockedBy / mirror this one): the create verbs additionally accept a combined
// `assetPath` (and the `assetName` casing synonym for `name`) and split it server-side into
// leaf name + parent folder, WITHOUT changing the meaning of the existing folder `path` slot.
// The folder `path` stays a folder; only an explicit combined `assetPath` is split, so there
// is no folder-vs-asset ambiguity. Existing `name`+`path` callers are unaffected.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/PackagePathCompose.h"
#include "Misc/PackageName.h"

namespace MaterialCreatePathParamUtils
{

// Accepted wire names for an *explicit* leaf name (no path-splitting), canonical first.
// `name`      — the historical split-slot leaf name (canonical, unchanged for existing callers)
// `assetName` — casing/synonym sibling used by the param-alias precedent tickets
// ResolveCreateNameAndFolder reads through these for the explicit-name branch, so the advertised
// aliases and the read keys cannot diverge (the GeometryNameParamUtils single-source pattern).
inline const TArray<FString>& ExplicitNameKeys()
{
    static const TArray<FString> Keys = {
        TEXT("name"),
        TEXT("assetName")
    };
    return Keys;
}

// Accepted wire names for the create-verb leaf-name slot, canonical first. This is the spec-side
// alias set (the dispatcher's required-param check accepts any of these): the explicit-name keys
// plus `assetPath`, a combined full asset path that is split into leaf name + parent folder when
// supplied without an explicit name, matching the operate-verb single-path shape.
inline const TArray<FString>& CreateNameKeys()
{
    static const TArray<FString> Keys = []
    {
        TArray<FString> All = ExplicitNameKeys();
        All.Add(TEXT("assetPath"));
        return All;
    }();
    return Keys;
}

// Accepted wire names for the destination *folder* slot, canonical first. `folder` is a
// readability synonym; both mean "the folder to create the asset in" — NOT a combined path.
inline const TArray<FString>& CreateFolderKeys()
{
    static const TArray<FString> Keys = {
        TEXT("path"),
        TEXT("folder")
    };
    return Keys;
}

// Read the optional destination folder, defaulting to DefaultFolder when no folder slot was given.
// Single 'optional folder with default' read for the whole create-* surface — one edit site if the
// folder-defaulting rule ever changes (e.g. trimming a trailing slash, honoring a new alias).
inline FString ResolveFolder(const FHandlerContext& Ctx, const FString& DefaultFolder)
{
    const FString Folder = Ctx.GetStringFirstOf(CreateFolderKeys());
    return Folder.IsEmpty() ? DefaultFolder : Folder;
}

// FParamSpec for the required leaf-name slot. Canonical name stays `name` (so existing
// callers and wiki examples are unchanged); `assetName`/`assetPath` ride as aliases so the
// dispatcher's required-param check is satisfied by a combined-`assetPath`-only call and
// neither alias trips UNKNOWN_PARAMS.
//
// DELIBERATELY `string`, NOT `path`, and this is a contract decision rather than an oversight.
// The slot is a BARE LEAF: `name`/`assetName` are object names, and the type field is rendered
// into every wiki page as the caller-facing contract, so declaring `path` here would advertise
// "an asset/package path" for a slot documented as the opposite. The lethal `//` is still refused
// on every branch - `FName::IsValidXName` rejects it in the leaf, and the combined-`assetPath`
// branch is split and then validated as a whole by `FPackageName::IsValidLongPackageName` inside
// PinWrightComposeAssetPackagePath - and ResolveCreateAssetPackagePath runs ABOVE every other
// asset resolution in these verbs, so no load ever sees the raw string. Aliases inherit the
// spec's type (RpcDispatcher::CollectDeclaredTypesByWireName), which is why `assetPath` cannot be
// typed separately without moving to FParamSpec::TypedAliases; it does not need to be.
inline FParamSpec MaterialCreateNameParamReq(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(TEXT("name"), TEXT("string"), Desc, true, CreateNameKeys());
}

// FParamSpec for the optional destination-folder slot. Canonical stays `path`; `folder`
// rides as an alias. Desc should state the per-verb default folder.
//
// TYPED `path`, WHICH IS WHAT PUTS THE DOUBLED-SLASH REFUSAL AT THE DISPATCH BOUNDARY for the
// destination slot of all eight material.authoring.create_* verbs at once. This slot is always an
// asset/package folder and never a disk path, so `filepath` (which carries no `//` rule, because a
// UNC path normalises to `//server/share`) would be the wrong token here. `path` accepts exactly
// the JSON shapes `string` accepts, so no working caller changes shape
// (Handlers/ParamTypeCheck.h); the alias `folder` inherits the type and therefore the rule.
//
// The composer's own IsValidLongPackageName check STAYS - it is defence in depth for the
// internally-composed and default-folder strings the gate never sees. One consequence is worth
// knowing when reading the tests: `ExpectInstanceFolderRefused` in
// Tests/Material/TestMaterialCreateNamePathSafety.cpp drives `path: "/Game//Materials"` and is now
// answered by the gate rather than by the composer. Its three assertions still hold (the gate
// sends the same INVALID_ARGUMENT and quotes the offending value), but it no longer discriminates
// a composer that checked only the bare name - that property is pinned directly instead by
// PinWright.core.path.compose_asset_package_path.* in Tests/Core/TestPathUtils.cpp.
inline FParamSpec MaterialCreateFolderParamOpt(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(TEXT("path"), TEXT("path"), Desc, false, CreateFolderKeys());
}

// Split a combined asset path (`/Game/Foo/Bar/M_Leaf` or the object-path form
// `/Game/Foo/Bar/M_Leaf.M_Leaf`) into leaf name + parent folder. Returns false if the input
// has no folder component (a bare leaf with no `/`), in which case the caller falls back to
// the verb's default folder. On that false return OutName is still set to the normalized bare
// leaf (the same ObjectPathToPackageName form the split would use) so the caller reuses it
// without re-parsing, and OutFolder is left untouched.
inline bool SplitCombinedAssetPath(const FString& Combined, FString& OutName, FString& OutFolder)
{
    // Strip any trailing `.Object` / `:Subobject` so an object path reduces to its package path.
    const FString PackageName = FPackageName::ObjectPathToPackageName(Combined);

    int32 SlashIndex = INDEX_NONE;
    if (!PackageName.FindLastChar(TEXT('/'), SlashIndex) || SlashIndex <= 0)
    {
        // No parent folder to derive — hand back the normalized bare leaf for the default-folder
        // fallback, then signal 'no folder' so the caller defaults it.
        OutName = PackageName;
        return false;
    }

    OutFolder = FPackageName::GetLongPackagePath(PackageName);
    OutName = FPackageName::GetLongPackageAssetName(PackageName);
    return !OutName.IsEmpty() && !OutFolder.IsEmpty();
}

// Resolve the leaf asset name + destination folder for a material.authoring.create_* verb.
//
// Precedence (existing callers first, so behavior is unchanged for them):
//   1. An explicit `name` / `assetName` → that is the leaf; folder = `path`/`folder` or DefaultFolder.
//   2. Otherwise a combined `assetPath` → split into leaf name + parent folder.
//      (A leaf-only `assetPath` with no `/` falls back to DefaultFolder for the folder.)
//
// On success returns true with OutName/OutFolder populated. On total failure (no name source
// at all) sends MISSING_REQUIRED_PARAM via Ctx and returns false. Note the dispatcher's
// required-param check already guarantees one of name/assetName/assetPath is present, so the
// failure path here is defensive (e.g. all three supplied but empty).
inline bool ResolveCreateNameAndFolder(FHandlerContext& Ctx, const FString& DefaultFolder,
                                       FString& OutName, FString& OutFolder)
{
    // Explicit leaf name wins — keeps the split-slot contract for existing callers. Read through
    // ExplicitNameKeys() so the advertised aliases and the read keys cannot drift.
    const FString ExplicitName = Ctx.GetStringFirstOf(ExplicitNameKeys());
    if (!ExplicitName.IsEmpty())
    {
        OutName = ExplicitName;
        OutFolder = ResolveFolder(Ctx, DefaultFolder);
        return true;
    }

    // No explicit name: accept the combined single-path shape the operate verbs already take.
    const FString Combined = Ctx.GetString(TEXT("assetPath"));
    if (!Combined.IsEmpty())
    {
        FString SplitName, SplitFolder;
        if (SplitCombinedAssetPath(Combined, SplitName, SplitFolder))
        {
            OutName = SplitName;
            OutFolder = SplitFolder;
            return true;
        }

        // A bare leaf with no folder component: reuse the normalized leaf the split already
        // computed (SplitName), default the folder.
        if (!SplitName.IsEmpty())
        {
            OutName = SplitName;
            OutFolder = ResolveFolder(Ctx, DefaultFolder);
            return true;
        }
    }

    Ctx.SendError(ErrorCodes::ERR_MISSING_REQUIRED_PARAM,
        TEXT("Missing required parameter 'name' (type: string). Accepted aliases: name, assetName, assetPath. ")
        TEXT("Pass a separate 'name' (+ optional folder 'path'), or one combined 'assetPath' that is split into name + folder."));
    return false;
}

// Resolve the destination AND compose the package path the create verb hands to CreatePackage.
//
// Every material.authoring.create_* verb below create_material used to end
// ResolveCreateNameAndFolder and then compose `Path / Name` itself, raw, straight into
// CreatePackage. CreatePackage (UObjectGlobals.cpp:1087-1120) logs at **Fatal** - not compiled
// out in any configuration - for a name containing "//" and for a name that resolves to empty,
// so an unvalidated caller string reaching it does not fail the call: it ends the editor PROCESS
// and every unsaved package in it, and the handler's own `if (!Package)` is never reached
// (board B-createpackage-unvalidated-paths-plugin-wide; measured on
// B-foliage-add-type-name-with-slash-kills-the-editor). Note FString::operator/ does not double a
// leading slash - PathAppend (Core/Private/Containers/String.cpp.inl:855-885) pops the terminator
// first - but that is irrelevant here: `name: "a//b"` is a one-argument kill through either
// composition, and the combined-`assetPath` shape can carry "//" in the folder half as well.
//
// One entry point for the whole family, so the rule cannot be applied to six of the seven: the
// bare leaf is checked against UObject's own naming rules and the composed path against
// CreatePackage's own input rules, with both engine reason texts surfaced verbatim
// (Handlers/PackagePathCompose.h).
//
// ORDERING IS LOAD-BEARING. Call this before the verb resolves any other asset argument (e.g.
// create_material_instance's parentMaterial): the regression test pairs a bad `name` with a
// well-formed parentMaterial naming nothing, so a build WITHOUT this guard bails ASSET_NOT_FOUND
// at that load - above the concatenation - and the test goes red instead of taking the suite host
// down with it.
inline bool ResolveCreateAssetPackagePath(FHandlerContext& Ctx, const FString& DefaultFolder,
                                          FString& OutName, FString& OutFolder,
                                          FString& OutPackagePath)
{
    if (!ResolveCreateNameAndFolder(Ctx, DefaultFolder, OutName, OutFolder))
    {
        return false;
    }

    // OutFolder goes in untrimmed: PinWrightComposeAssetPackagePath joins with FString::operator/,
    // which absorbs one trailing separator, so `path: "/Game/Materials/"` still composes
    // "/Game/Materials/M_Foo". The local TrimTrailingFolderSeparator that used to wrap this
    // argument existed only to undo the composer's old Printf join and is deleted.
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(OutFolder, OutName, OutPackagePath, PathError))
    {
        // ErrorCodes:: rather than a raw literal: this header already cites the registry above,
        // and PinWright.core.error_codes.RegistryAdoptingFilesUseConstantsOnly holds an adopting
        // file to zero hand-spelled codes.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("%s Pass a bare asset name in 'name' and choose the folder with "
                                 "'path' (default %s), or one combined 'assetPath'."),
                *PathError, *DefaultFolder));
        return false;
    }
    return true;
}

} // namespace MaterialCreatePathParamUtils
