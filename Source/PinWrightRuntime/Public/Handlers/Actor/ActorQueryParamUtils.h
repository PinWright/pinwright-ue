// Copyright (c) 2026 Alexander Penkin. MIT License.

// Alias helper for the actor.* SEARCH verbs' two drifting wire slots: the free-text
// search fragment (actor.find_by_name) and the class identifier (actor.find_by_class).
//
// Two defects of the same shape motivated this, both in Handlers/Actor/QueryHandler.cpp:
//
//   1. `actor.find_by_name` declared its sole slot as a bare required `name` with no
//      aliases, and the body read only `name`. The VALUE is a case-insensitive substring
//      — the verb summary, the param help and the wiki page all call it one — so the key
//      a caller reaches for is `pattern` / `substring` / `query` / `search` / `filter`,
//      every one of which hard-failed MISSING_REQUIRED_PARAM 'name'. The set below is not
//      guesswork: `docs/wiki-src/actor.md`'s own `### actor.find_by_name` section already
//      enumerated `substring` / `query` / `search` as the wrong guesses callers make, and
//      `filter` is what the sibling `actor.list` — registered 140 lines earlier in the
//      SAME file — calls the identical thing (a case-insensitive substring matched against
//      label AND name; that is actor.list's documented DEFAULT semantics, so the alias
//      changes no matching behaviour). `query` has a second, stronger claim: this verb's
//      own RESPONSE echoes the fragment back under the key `query`, so an input/output key
//      mismatch lived inside one verb. `pattern` is declared by no verb anywhere on the
//      surface, so it could never have meant anything else here.
//
//   2. `actor.find_by_class` is the exact defect commit 644db343 fixed for the spawn
//      slots: the body already read `Ctx.GetStringFirstOf({className, class})` and the
//      param help advertised "'class' alias accepted", but the FParamSpec carried no
//      Aliases — so the dispatcher's UNKNOWN_PARAMS / MISSING_REQUIRED_PARAM gate rejected
//      `class` BEFORE the body ran. A spelling was documented as accepted and was refused.
//
// Deliberately NOT in the search-fragment set: `actorName`, `actor_name`, `objectPath`,
// `actorPath`. Those are ActorNameParamUtils' IDENTITY slot — "the one actor I mean",
// resolved through McpActorUtils::ResolveActor, which REFUSES an ambiguous match with
// AMBIGUOUS_ACTOR_NAME. `actor.find_by_name`'s slot is the opposite contract: a fragment
// that is expected to match many actors and returns all of them. Widening the identity
// keys onto it would make one key mean "exactly one actor, ambiguity refused" on ~144
// verbs and "substring, many rows" on this one — the same blur commit 644db343 refused
// when it kept `actor.spawn_batch` out of the label set. The path-shaped members would
// also be dead on arrival: this verb rejects any fragment containing '/', '\' or '..'.
//
// Canonical names are unchanged, so no existing caller or wiki example breaks. The
// dispatcher honors FParamSpec.Aliases for both required-param satisfaction
// (PayloadHasParamOrAlias) and the known-params set (AddKnownParamNames); the body must
// still read via the matching Resolve* helper for the alias to reach the handler.
//
// Mirrors the per-family alias helpers (ActorNameParamUtils, SpawnParamUtils,
// AssetPathParamUtils, LevelNameParamUtils) built on the generic Handlers/ParamAliasUtils.h.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/HandlerContext.h"

namespace ActorQueryParamUtils
{

// ---- free-text search fragment (actor.find_by_name) --------------------------

// Accepted wire keys for the search-fragment slot, canonical first. `name` stays canonical
// (it is the documented key and the one every existing caller and wiki example uses).
// Ordered by the evidence behind each: `pattern` is the natural spelling for a substring
// query and collides with nothing on the surface; `filter` is what actor.list calls the
// identical value with identical default semantics; `query` is what THIS verb's response
// echoes the value back as, and the surface-dominant search-string spelling
// (asset.search, blueprint.search, system.console.search, material/class/Niagara search);
// `substring` and `search` are the two remaining wrong guesses actor.md already documented.
inline const TArray<FString>& SearchFragmentKeys()
{
    static const TArray<FString> Keys = {
        TEXT("name"),
        TEXT("pattern"),
        TEXT("filter"),
        TEXT("query"),
        TEXT("substring"),
        TEXT("search")
    };
    return Keys;
}

// Required search-fragment spec carrying the alias set above. Required, so a payload
// naming none of the keys is refused by the dispatcher with MISSING_REQUIRED_PARAM that
// lists every accepted spelling.
inline FParamSpec SearchFragmentParamReq(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*SearchFragmentKeys()[0], TEXT("string"), Desc,
        /*bRequired=*/true, SearchFragmentKeys());
}

// Read the search fragment from whichever accepted key the caller used. Returns the empty
// string when every key is absent or blank; the body then sends its own domain error.
inline FString ResolveSearchFragment(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(SearchFragmentKeys());
}

// One canonical sentence naming every accepted spelling, for the empty-fragment error.
// Derived from SearchFragmentKeys() so the message cannot drift from the declared set.
inline FString SearchFragmentKeyList()
{
    return FString::Join(SearchFragmentKeys(), TEXT(", "));
}

// ---- class identifier (actor.find_by_class) ----------------------------------

// Accepted wire keys for the class-lookup slot, canonical first. `class` was read
// body-side and advertised in the param help long before it was declared.
inline const TArray<FString>& FindClassNameKeys()
{
    static const TArray<FString> Keys = {
        TEXT("className"),
        TEXT("class")
    };
    return Keys;
}

inline FParamSpec FindClassNameParamReq(const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*FindClassNameKeys()[0], TEXT("classref"), Desc,
        /*bRequired=*/true, FindClassNameKeys());
}

inline FString ResolveFindClassName(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(FindClassNameKeys());
}

} // namespace ActorQueryParamUtils
