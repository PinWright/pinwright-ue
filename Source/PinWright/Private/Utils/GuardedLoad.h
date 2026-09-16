// Copyright (c) 2026 Alexander Penkin. MIT License.

// GuardedLoad.h - the load that cannot end the editor, for every string the dispatch boundary
// cannot see.
//
// WHAT IS LETHAL, ONCE MORE. CreatePackage logs at Fatal for a package name containing "//"
// (CoreUObject/Private/UObject/UObjectGlobals.cpp:1094-1096). Fatal is not compiled out in any
// configuration: it ends the PROCESS and every unsaved package in it, so the caller's own
// `if (!Loaded)` after the call is never reached. Every LOAD is a door to it -
// StaticLoadObjectInternal calls ResolveName2(..., Create=true) (:1427), which calls CreatePackage
// on the partial name (:1310) - and a dot-free, slash-free-prefix string still arrives, because
// StaticLoadObjectInternal re-enters itself with `InName + "." + GetShortName(InName)`
// (:1474-1482). FindObject is safe (Create=false).
//
// WHY A POINT-OF-USE GUARD EXISTS AT ALL, GIVEN THE DISPATCH GATE. The dispatch-boundary type gate
// (Handlers/ParamTypeCheck.h, read by Dispatch/RpcDispatcher.cpp) closes this class for every
// TOP-LEVEL wire parameter declared `path` or `classref`, and that remains the right layer for
// them: one rule, 1,000+ declarations, no per-site memory. It is structurally blind to two input
// shapes, and both are confirmed reachers rather than hypotheses:
//
//   1. NESTED VALUES. A path inside an array element (`foliageTypes[].meshPath`,
//      `nodes[].texturePath`, `stems[].assetPath`, `captures[].attribute`) or in the VALUE half of
//      a map (`texture: {ParamName: AssetPath}`) is not a top-level param and has no declared type
//      of its own. FParamSpec::NestedKeys is an untyped allow-list of KEYS, so there is nowhere to
//      hang the rule, and on the map shape a key-shaped rule could not reach the value anyway.
//      Board B-nested-path-values-reach-createpackage-fatal.
//   2. IR SOURCE TEXT. AGIR / CRIR / BPIR / MGIR class and asset references are substrings of the
//      `text` parameter, which is an IR document and must stay typed `string` - typing it `path`
//      is nonsense and `classref` would refuse every valid program. The reference is produced by
//      the PARSER, mid-compile, and no tokenizer filters '/': the IR comment character is '#', and
//      Unquote() returns its input verbatim. Board B-ir-source-class-refs-reach-createpackage-fatal.
//
// A guard keyed on where the lethal call HAPPENS answers both at once, and answers the third case
// nobody declares either - a string this plugin composed itself. It cannot be bypassed by input
// shape, because it never looks at the input's shape.
//
// THIS IS NOT A REPLACEMENT FOR THE DISPATCH GATE and must not be read as licence to retype a
// `path` parameter back to `string`. The gate refuses the request with the offending value quoted,
// before any work happens; this refuses one load, deep inside a verb that may already have created
// something. Defence in depth, in that order.
//
// THE RULE IS EXACTLY CanReachCreatePackageFatal (Utils/PathUtils.h) - one substring, "//", and
// nothing wider. That predicate's header states why nothing wider is correct for a slot that
// legitimately holds a bare short name, an object path, a subobject path, a generated-class path
// or a plugin mount. Do not add FPackageName::IsValidLongPackageName here: it refuses '.' and
// refuses a leading-slash-less short name, i.e. most of the shapes these call sites accept.
//
// SANITIZING IS NOT AN OPTION, and a normalizer is not a guard. A caller that wrote "//" asked for
// a package that does not exist; collapsing it silently makes the verb report success about a
// DIFFERENT asset. Refuse, name the value, and let the caller fix it.
#pragma once

#include "CoreMinimal.h"
#include "PinWrightSubsystem.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/PathUtils.h"

namespace PinWrightGuardedLoad
{
    // The caller-facing sentence for one refused path. Deliberately the same wording as the
    // dispatch gate's PinWrightMakePathSeparatorFault (Handlers/ParamTypeCheck.h) so support can
    // grep one phrase and find both layers; the difference is that this one has no wire-name label
    // to quote, because the value did not arrive as a named parameter.
    inline FString MakeRefusal(const FString& Path)
    {
        return FString::Printf(
            TEXT("'%s' contains a doubled slash. A path may not contain '//' - it reaches ")
            TEXT("CreatePackage, which logs Fatal and ends the editor process rather than ")
            TEXT("returning an error. Send the path with single separators."),
            *Path);
    }

    // TRUE when Path must not be handed to a load. Fills OutRefusal (when non-null) with the
    // sentence above, so a call site with an error channel can report WHY instead of folding the
    // refusal into its not-found message.
    //
    // Logged at Warning, never Error: a malformed argument is a caller mistake, not a plugin
    // defect, and UAutomationControllerSettings::bElevateLogWarningsToErrors would turn a logged
    // Error into a suite failure. A test that provokes a refusal must declare the warning.
    inline bool IsRefused(const FString& Path, FString* OutRefusal = nullptr)
    {
        if (!CanReachCreatePackageFatal(Path))
        {
            return false;
        }

        const FString Refusal = MakeRefusal(Path);
        if (OutRefusal)
        {
            *OutRefusal = Refusal;
        }
        UE_LOG(LogPinWrightSubsystem, Warning, TEXT("PinWright refused a load: %s"), *Refusal);
        return true;
    }

    // THE guarded load. Drop-in for `LoadObject<T>(nullptr, *Path)`: same result for every string
    // that was ever going to resolve, nullptr instead of a dead editor for the one that was not.
    //
    // LoadFlags is threaded through for the sites that pass LOAD_NoWarn / LOAD_Quiet because a
    // miss is an expected outcome they report themselves; dropping those flags on conversion would
    // re-add the engine's duplicate load warning to a verb that deliberately suppressed it.
    template <typename T>
    T* LoadObjectChecked(const FString& Path, FString* OutRefusal = nullptr,
                         uint32 LoadFlags = LOAD_None)
    {
        if (IsRefused(Path, OutRefusal))
        {
            return nullptr;
        }
        return LoadObject<T>(nullptr, *Path, nullptr, LoadFlags);
    }
}
