// Copyright (c) 2026 Alexander Penkin. MIT License.

// Path sanitization and validation utilities for PinWright
#pragma once

#include "CoreMinimal.h"

// Check if a path starts with any registered mount point (plugins, DLC, etc.)
PINWRIGHT_API bool IsValidMountPoint(const FString& Path);

// TRUE when handing InPath to a load OR a create can reach CreatePackage's Fatal and END THE
// PROCESS. Refuse the request when this returns true; never "sanitize and continue" - a caller
// that wrote "//" asked for a package that does not exist, and silently repairing it is how a
// verb reports success about a DIFFERENT asset.
//
// THE RULE IS EXACTLY ONE SUBSTRING, "//", AND THAT IS NOT A SIMPLIFICATION.
//   * CreatePackage logs at Fatal for a name containing "//"
//     (CoreUObject/Private/UObject/UObjectGlobals.cpp:1094-1096). Fatal is NOT compiled out in
//     any configuration: it ends the PROCESS and every unsaved package in it, so the caller's own
//     `if (!Package)` after the call is never reached and there is no defence downstream.
//   * LOADS reach the same Fatal, which is why this predicate is not only for direct-create
//     sites. StaticLoadObjectInternal calls ResolveName2(..., Create=true) (:1427), and
//     ResolveName2 calls CreatePackage on the partial name (:1310). FindObject is safe - it
//     resolves with Create=false.
//   * A DOT-FREE STRING STILL GETS THERE. ResolveName2 returns immediately at :1241 when the name
//     carries no '.' / ':' delimiter, and StaticLoadObjectInternal then re-enters itself with
//     InName + "." + GetShortName(InName) (:1474-1482); the second pass has the dot and walks
//     into :1310. So LoadObject(nullptr, TEXT("A//B")) is an editor kill, with no leading slash
//     and no dot in the caller's text.
//   * "//" IS THE ONLY LETHAL PROPERTY. CreatePackage's other Fatal, the empty package name
//     (:1117-1119), is unreachable through ResolveName2:1310 - the partial name there always
//     contains a '/' and no delimiter - so it only ever threatened direct-create sites, which
//     compose their own names.
//
// USABLE ON EVERY INPUT SHAPE, AND THAT IS THE POINT. A bare short name ("PointLight"), a package
// path ("/Game/A/B"), an object path ("/Game/A/B.B"), a subobject path ("/Game/A/B.B:Comp"), a
// generated-class path ("/Game/A/BP_X.BP_X_C") and a plugin mount ("/MyPlugin/A/B") all pass, so
// ONE guard can sit above a resolver that accepts several of those shapes at once without the
// guard having to know which one it was handed.
//
// FPackageName::IsValidLongPackageName IS NOT THE GUARD for a class or object reference. It
// rejects '.' (INVALID_LONGPACKAGE_CHARACTERS, Core/Public/UObject/NameTypes.h:197) and rejects a
// short name for having no leading slash - so on a class reference it would turn away
// "PointLight", "/Script/UMG.UserWidget" and "/Game/A/BP_X.BP_X_C", roughly half the shapes the
// class resolvers document. It remains the right check where the input is known to be a package
// path being composed for CreatePackage (see Handlers/PackagePathCompose.h); it is the wrong
// check at a resolver, and nothing wider than this predicate is correct there.
//
// DELIBERATELY NO BACKSLASH HANDLING. The engine compares the literal "//", so "\\" cannot reach
// the Fatal; refusing it here would narrow the one property that makes this predicate applicable
// to every shape, in exchange for catching a string that is merely malformed rather than lethal.
//
// NOTHING ELSE IN THIS FILE BACKS THIS PREDICATE UP - do not weaken it on the assumption that
// something does. In particular IsValidMountPoint above is NOT a "//" guard and cannot become one:
// it asks FPackageName::GetPackageMountPoint, which never runs IsValidTextForLongPackageName (where
// the engine's "//" rule lives) and confirms the hit with a helper that explicitly STRIPS duplicate
// separators following the parent - so "/Game//X" answers TRUE there, both before and after that
// predicate's rewrite. SanitizeProjectRelativePath does collapse "//", but it is a normalizer that
// only some callers run, not a gate every path passes. This predicate, the dispatch-boundary type
// gate (Handlers/ParamTypeCheck.h) and the point-of-use load guard (Utils/GuardedLoad.h, which is
// built on this predicate and exists for the values that boundary cannot see - nested values and
// IR source text) are the whole defence.
PINWRIGHT_API bool CanReachCreatePackageFatal(const FString& InPath);

// Removes control characters (ASCII codes less than 32) from the input JSON string.
PINWRIGHT_API FString SanitizeIncomingJson(const FString& In);

// Normalize and validate a project-relative asset path.
// Ensures the returned path is normalized, begins with a leading '/', rejects
// any path containing directory traversal sequences (".."), and accepts common
// roots (/Game, /Engine, /Script) or plugin-like roots (heuristic).
PINWRIGHT_API FString SanitizeProjectRelativePath(const FString& InPath);

// Normalize a MOUNTED CONTENT path for an asset-authoring verb: SanitizeProjectRelativePath as
// the front end (traversal, drive letters, duplicate slashes, mount point), then backslashes to
// forward slashes, then any trailing separators dropped.
//
// Returns EMPTY when the input is not a mounted content path. Empty is a REFUSAL, never "the
// project root": a caller that treats it as a folder composes "/<name>" and creates an asset at
// an unmounted root.
//
// IT DOES NOT REWRITE "/Content" TO "/Game", and the per-cluster copies this replaced only
// appeared to. The rewrite sat BELOW the mount check, where it could never fire, because
// "/Content" is not a mount point - see the .cpp for the engine's registered root list and why
// re-adding it would be a corruption risk rather than a feature. A verb that must accept
// "/Content/..." needs the rewrite ABOVE the sanitizer and therefore its own function;
// SoundCueDumpBuilder::NormalizeSoundCuePath is that case and deliberately does not fold in here.
//
// What the per-cluster copies got genuinely WRONG, and the reason one shared definition exists:
// the anim and texture copies never called SanitizeProjectRelativePath, so they collapsed no
// interior "//" and rejected no ".."; and they rewrote "/Content" with an UNANCHORED
// ReplaceInline, which turns "/MyPlugin/Content/X" into "/MyPlugin/Game/X" - a package that does
// not exist - so a plugin-mounted destination silently became a different, missing one.
PINWRIGHT_API FString NormalizeContentAssetPath(const FString& InPath);

// Sanitize a file path for use with file operations (export/import snapshot, etc.).
// Unlike SanitizeProjectRelativePath which requires asset roots,
// this function accepts any project-relative file path while still enforcing security.
PINWRIGHT_API FString SanitizeProjectFilePath(const FString& InPath);

// Validate a basic asset path format.
PINWRIGHT_API bool IsValidAssetPath(const FString& Path);

// Turn a content path into the OBJECT-path form the asset registry resolves:
// `/Game/A/SM_X` -> `/Game/A/SM_X.SM_X`. A path that already carries an object name
// (`/Game/A/SM_X.SM_X`, or a subobject path `/Game/A/SM_X.SM_X:Component`) is returned
// unchanged.
//
// WHY THIS EXISTS. FSoftObjectPath built from a PACKAGE path is invalid, so
// IAssetRegistry::GetAssetByObjectPath returns nothing for it - and a verb that treats "nothing"
// as "the asset is broken" turns a caller's argument-form mistake into a content verdict about a
// healthy asset (board ticket B-mesh-audit-package-path-reads-as-broken-asset). Normalizing first
// removes the ambiguity, and the false return then means only one thing.
//
// FALSE means the string is not a content path at all - empty, not rooted at a mount point, a
// bare name, a trailing slash, a dot in a directory component, an empty object name after the
// dot. That is a CALLER ERROR and must be reported as one (INVALID_ARGUMENT naming the expected
// form), never folded in with "no such asset" and never with "this asset would not load". Those
// are three different facts and a caller needs to tell them apart.
//
// A true return says the string is WELL FORMED, never that the asset exists; resolving it is the
// caller's next step, and a well-formed path that resolves to nothing is ASSET_NOT_FOUND.
PINWRIGHT_API bool NormalizeToObjectPath(const FString& InPath, FString& OutObjectPath,
                                         FString& OutError);

// Validate and sanitize a BARE asset name - the leaf, never a path.
//
// Every character invalid in a UObject name is replaced with a single underscore (consecutive
// replacements collapse), SQL-injection punctuation included, and the result is trimmed,
// prefixed if it does not start with a letter or '_', truncated to 64 and defaulted to "Asset"
// when nothing survives.
//
// '/' AND '.' ARE AMONG THOSE CHARACTERS. They were missing until 2026-08-31, which made the
// function's name a lie: SanitizeAssetName("Foo/Bar") returned "Foo/Bar" unchanged, and the
// caller then composed a package path with a folder separator inside its leaf. Both are in the
// engine's own INVALID_OBJECTNAME_CHARACTERS (Core/Public/UObject/NameTypes.h:191).
PINWRIGHT_API FString SanitizeAssetName(const FString& InName);

// Validate and normalize a full asset path for creation.
// Combines path and name validation, returns validated path or empty on failure.
PINWRIGHT_API bool ValidateAssetCreationPath(
    const FString& FolderPath,
    const FString& AssetName,
    FString& OutFullPath,
    FString& OutError);
