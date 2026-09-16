// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"

struct FParamAliasSpec
{
    FString Name;
    FString Type;
    FString Description;
};

// Parameter specification for discovery/documentation
struct FParamSpec
{
    FString Name;        // "class_name"
    // The declared type expression: a `|`-separated union over
    // {string, number, integer, boolean, bool, object, array, any, path, classref, filepath}.
    // Read at runtime by Handlers/ParamTypeCheck.h (shape) and by the dispatcher's path-separator
    // pass (safety); rendered into every wiki page as the caller-facing contract.
    //
    // THE THREE PATH-SHAPED SPELLINGS ARE NOT COSMETIC - pick between them per parameter:
    //   path     - an asset / package / object path (`/Game/A/B`, `/Game/A/B.B`, `/Game/A/B.B:C`).
    //   classref - a class reference: a bare short name (`PointLight`), `/Script/UMG.UserWidget`,
    //              a Blueprint asset path or its `_C` generated-class path, or a plugin mount -
    //              every shape ClassUtils::ResolveUClass documents.
    //   filepath - a path on DISK. It deliberately carries NO doubled-slash rule, because a UNC
    //              path normalises to `//server/share`; typing a disk path `path` starts refusing
    //              working callers.
    // `path` and `classref` carry the doubled-slash refusal (see ParamTypeCheck.h); all three
    // accept exactly the JSON shapes `string` accepts, so retyping changes no shape verdict.
    FString Type;
    FString Description; // "The class to spawn"
    bool bRequired;      // true = error if missing
    FString Default;     // "" = no default
    TArray<FString> Aliases;
    TArray<FParamAliasSpec> TypedAliases;
    // The DECLARED nested schema for an object/array parameter: every key this slot accepts one
    // level down. EMPTY MEANS UNDECLARED, NOT EMPTY-SET - the nested surface of a parameter that
    // declares nothing here is not validated at all, exactly as before. Populating it opts that one
    // parameter into the dispatcher's fourth pass (Handlers/NestedParamKeyCheck.h), which refuses
    // UNKNOWN_NESTED_PARAMS for any other key. That is a compatibility break for callers sending a
    // stray nested key, so it is adopted one parameter at a time with the description updated in
    // the same commit. Board B-declared-param-guard-blind-to-nested-keys.
    TArray<FString> NestedKeys;
};

// Shared collision-safe steer for the singular `actorName` slot. Appended (via
// adjacent string-literal concatenation, e.g. RPC_PARAM_REQ(... "Display label..."
// ACTORNAME_COLLISION_STEER)) to every per-actor verb's actorName description so the
// one fact lives once. RPC_PARAM_REQ wraps the whole literal in TEXT(), which
// concatenates the adjacent fragments at compile time — no runtime cost.
#define ACTORNAME_COLLISION_STEER " Display labels are not unique. A name matching several actors is refused with AMBIGUOUS_ACTOR_NAME listing each candidate; re-issue with the unique internal object name (the object-path leaf, e.g. PointLight_1) as the collision-safe key."

// Convenience macros for handler files
#define RPC_PARAM_REQ(Name, Type, Desc) \
    FParamSpec{TEXT(Name), TEXT(Type), TEXT(Desc), true, TEXT("")}

#define RPC_PARAM_OPT(Name, Type, Desc) \
    FParamSpec{TEXT(Name), TEXT(Type), TEXT(Desc), false, TEXT("")}

#define RPC_PARAM_DEF(Name, Type, Desc, Default) \
    FParamSpec{TEXT(Name), TEXT(Type), TEXT(Desc), false, TEXT(Default)}

// An optional object/array parameter whose nested keys ARE declared, and are therefore refused when
// the caller sends one this slot does not accept. The tail is the allow-list, written as TEXT()
// literals: RPC_PARAM_OPT_NESTED("grid", "object", "...", TEXT("spacing"), TEXT("extent")).
// Declare here only what the verb actually READS one level down - the allow-list is a promise the
// gate enforces, so a key listed but not read becomes an accepted-and-discarded input again.
#define RPC_PARAM_OPT_NESTED(Name, Type, Desc, ...) \
    FParamSpec{TEXT(Name), TEXT(Type), TEXT(Desc), false, TEXT(""), \
        TArray<FString>(), TArray<FParamAliasSpec>(), TArray<FString>({__VA_ARGS__})}

#define RPC_PARAMS(...) TArray<FParamSpec>({__VA_ARGS__})
#define RPC_NO_PARAMS TArray<FParamSpec>()
