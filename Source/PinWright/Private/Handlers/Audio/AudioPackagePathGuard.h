// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioPackagePathGuard.h - the ONE composition guard for every audio asset-creation verb.
//
// WHY IT IS ONE FUNCTION AND NOT FIFTEEN COPIES. Every audio create verb reads exactly the same
// two arguments and composes them exactly the same way: a `path` folder (already run through
// NormalizeAudioPath / MetaSound::NormalizeAudioAssetPath) and a bare `name` that also becomes the
// UObject name via FName(*Name). Fifteen sites - thirteen in AudioAuthoringHandler.cpp, two in
// MetaSound/MetaSoundPatchPresetHandler.cpp - all wrote `Path / Name` and handed the result
// straight to CreatePackage. Because the rule at every one of them is identical, guarding them
// individually would be fifteen chances to get it wrong and fifteen places a sixteenth verb could
// be added without one; routing them through here makes "the audio cluster validates its package
// path" a property of the cluster rather than of whoever last edited it.
//
// WHAT IT IS GUARDING AGAINST. CreatePackage (UObjectGlobals.cpp:1086-1120) logs at Fatal - not
// compiled out in any configuration - for a name containing "//" (:1094-1096) and for a name that
// resolves to empty (:1118). Fatal ends the PROCESS, so an unvalidated caller string reaching
// CreatePackage does not fail the call: it kills the editor and every unsaved package in it. The
// handler's own `if (!Package)` can never fire, because nothing after the call is reached. Note
// that FString::operator/ (PathAppend, Core/Private/Containers/String.cpp.inl:855-885) does NOT
// double a leading slash, so `Path / Name` is not the doubling composition FString::Printf is -
// but a `name` that itself contains "//" is a one-argument kill either way, which is why the bare
// name is checked before anything is concatenated onto it.
//
// The engine's own rules do the deciding (see Handlers/PackagePathCompose.h): FName::IsValidXName
// + INVALID_OBJECTNAME_CHARACTERS on the bare name, FPackageName::IsValidLongPackageName on the
// composed path, both reasons surfaced verbatim so a refused caller is told which rule it broke.
//
// Named namespace, inline function: these translation units share a Unity build, where a
// same-named static/anonymous helper in two merged TUs collides. Matches the convention of
// Tests/Infra/DispatcherTestHelpers.h and the other per-cluster helper headers.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/PackagePathCompose.h"

namespace PinWrightAudioPackagePath
{
    // Composes "<FolderPath>/<AssetName>" or refuses the request outright.
    //
    // Returns true with OutPackagePath set to a path CreatePackage cannot die on. Returns false
    // having ALREADY sent an INVALID_ARGUMENT refusal through Ctx, carrying the engine's own
    // reason text plus the two parameters the caller can act on - so a caller is never told only
    // that something was wrong. The caller's job on false is to return from the handler.
    //
    // A TRAILING SEPARATOR ON THE FOLDER IS HANDLED BY THE COMPOSER, not here. This function used
    // to trim it because PinWrightComposeAssetPackagePath joined with Printf("%s/%s"), which
    // doubled "/Game/Audio/Cues/" + "Name" into "/Game/Audio/Cues//Name" and got it refused for a
    // "//" the caller never wrote. That composer now joins with FString::operator/, whose
    // PathAppend (Core/Private/Containers/String.cpp.inl:855-885) pops the terminator instead of
    // adding a second one, so the trim had nothing left to undo. It absorbs exactly ONE, so a
    // folder genuinely containing "//" is still refused, and a "//" in the NAME is refused a step
    // earlier by IsValidXName - a one-argument kill either way.
    inline bool ComposeAudioAssetPackagePathOrRefuse(FHandlerContext& Ctx,
                                                     const FString& FolderPath,
                                                     const FString& AssetName,
                                                     FString& OutPackagePath)
    {
        FString PathError;
        if (PinWrightComposeAssetPackagePath(FolderPath, AssetName, OutPackagePath, PathError))
        {
            return true;
        }

        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("%s Pass a bare asset name in 'name' and choose the destination "
                                 "folder with 'path'."), *PathError));
        return false;
    }
}
