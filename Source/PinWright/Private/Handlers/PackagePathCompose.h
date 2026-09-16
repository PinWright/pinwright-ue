// Copyright (c) 2026 Alexander Penkin. MIT License.

// PackagePathCompose.h - compose a package path CreatePackage cannot die on.
//
// CreatePackage (UObjectGlobals.cpp:1086-1120) logs at Fatal - which is NOT compiled out in any
// configuration - for two inputs: a name containing "//" (:1094-1096) and an empty name (:1118).
// Fatal ends the PROCESS, so an unvalidated caller string reaching CreatePackage does not fail the
// call, it kills the editor and every unsaved package in it. Measured on board
// B-foliage-add-type-name-with-slash-kills-the-editor: foliage.add_type {name:"/Game/X/Y"} was
// concatenated onto "/Game/Foliage", produced "/Game/Foliage//Game/X/Y" and killed a shared editor
// outright. There is no defence downstream - an `if (!Package)` check after the call is never
// reached - so the only place to stop it is before the concatenation.
//
// This helper started file-local in FoliageHandler.cpp and was lifted here when
// B-landscape-create-grass-type-name-with-slash-kills-the-editor found the same shape in
// landscape.create_grass_type, a different namespace. Any handler that composes
// "<folder>/<caller name>" and hands the result to CreatePackage must route through here.
//
// The engine's own rules are the answer rather than a hand-rolled character list, and both halves
// are needed:
//   * FName::IsValidXName + INVALID_OBJECTNAME_CHARACTERS is what UObject naming itself rejects -
//     '/', '.', ':' and the rest - so a path-shaped name is refused before it is concatenated.
//   * FPackageName::IsValidLongPackageName is what CreatePackage's own inputs must satisfy: it
//     rejects "//", the empty/too-short name, a missing leading slash, a trailing slash,
//     INVALID_LONGPACKAGE_CHARACTERS (which is where '\' is caught) and an unmounted root.
// Both OutReason texts are the engine's, surfaced verbatim by the callers, so a refused caller is
// told which rule it broke rather than being handed this plugin's paraphrase of it.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "Misc/PackageName.h"
#include "UObject/NameTypes.h"

// Returns false with OutError set (and OutPackagePath left empty) when the bare AssetName or the
// composed "<FolderPath>/<AssetName>" would not survive CreatePackage. `inline`, not `static`:
// this header is included from several handler translation units that a Unity build may merge.
//
// THE JOIN IS FString::operator/, NOT Printf("%s/%s"), and the difference is the whole reason the
// per-cluster trailing-slash trims that used to wrap this call are gone. PathAppend
// (Core/Private/Containers/String.cpp.inl:855-885) absorbs ONE separator: it pops the left side's
// terminator when FolderPath already ends in '/' or '\' instead of adding a second one, so
// `path: "/Game/Materials/"` composes "/Game/Materials/M_Foo" and is accepted, where Printf
// composed "/Game/Materials//M_Foo" and was refused for a "//" the caller never wrote. Every
// caller therefore stopped needing its own trim, and none of them can forget it.
//
// It absorbs ONE, so the guard itself is unchanged: "/Game/X//" / "Y" is still "/Game/X//Y", a
// "//" genuinely in the caller's text still reaches IsValidLongPackageName, and a "//" in the
// leaf name is refused a step earlier by IsValidXName. This is strictly widening - no input that
// was accepted before is refused now.
inline bool PinWrightComposeAssetPackagePath(const FString &FolderPath,
                                             const FString &AssetName,
                                             FString &OutPackagePath,
                                             FString &OutError) {
  OutPackagePath.Reset();

  FText NameReason;
  if (!FName::IsValidXName(AssetName, INVALID_OBJECTNAME_CHARACTERS, &NameReason)) {
    OutError = FString::Printf(TEXT("'%s' is not a bare asset name: %s"), *AssetName,
                               *NameReason.ToString());
    return false;
  }

  const FString Candidate = FolderPath / AssetName;
  FText PathReason;
  if (!FPackageName::IsValidLongPackageName(Candidate, /*bIncludeReadOnlyRoots=*/true,
                                            &PathReason)) {
    OutError = FString::Printf(TEXT("'%s' is not a valid package path: %s"), *Candidate,
                               *PathReason.ToString());
    return false;
  }

  OutPackagePath = Candidate;
  return true;
}
