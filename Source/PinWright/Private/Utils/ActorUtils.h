// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

class FJsonObject;

// Shared actor utility functions used by multiple handler files.
// Extracted from _ControlHandlers.cpp during Phase 13 migration.

namespace McpActorUtils
{
    // How an identifier matched, most-specific first. Zero value is "no match" so a
    // default-constructed resolution is a failure (rpc-design.md §2).
    enum class EActorMatchKind : uint8
    {
        None = 0,
        // Full object path (/Game/Maps/M.M:PersistentLevel.StaticMeshActor_3). Unique by
        // construction.
        ObjectPath,
        // Internal FName (GetName(), e.g. StaticMeshActor_3). Unique within a level.
        ObjectName,
        // Display label (GetActorLabel(), what the World Outliner shows). NOT unique.
        Label,
        // Case-insensitive substring of a display label. NOT unique.
        LabelSubstring,
    };

    // Select the identifier tiers a caller accepts. Default preserves the historical
    // actor.* lookup contract; geometry uses ExactIdentity so a near-matching display
    // label cannot select an unintended actor, while label-based reuse uses ExactLabel
    // so an operation's create label cannot be interpreted as another identity kind.
    enum class EActorResolvePolicy : uint8
    {
        Default = 0,
        ExactIdentity,
        ExactLabel,
    };

    // NotFound is 0, so a default-constructed FActorResolution reports failure and there
    // is no default-constructible success (rpc-design.md §2).
    enum class EActorResolveStatus : uint8
    {
        NotFound = 0,
        Resolved,
        // The identifier matched more than one actor at the same precedence tier. The
        // caller must NOT pick one - see FActorResolution::Candidates.
        Ambiguous,
    };

    // Outcome of resolving one actor identifier. Ambiguity is a first-class result rather
    // than a silently-picked first match: labels are not unique, so an identifier that
    // names two actors is a question the caller has to answer, not one the resolver may
    // guess at. Candidates is populated only when Status == Ambiguous.
    struct FActorResolution
    {
        EActorResolveStatus Status = EActorResolveStatus::NotFound;
        EActorMatchKind MatchedBy = EActorMatchKind::None;
        AActor* Actor = nullptr;
        TArray<AActor*> Candidates;

        // Success is the conjunction of an explicit status and a non-null actor, so a
        // path that forgets to assign one of the two cannot report success.
        bool IsResolved() const { return Status == EActorResolveStatus::Resolved && Actor != nullptr; }
        bool IsAmbiguous() const { return Status == EActorResolveStatus::Ambiguous; }
    };

    // Resolve one actor identifier against the world, with an explicit ambiguity verdict.
    //
    // Precedence - each tier is evaluated across EVERY actor before the next is tried, so
    // the outcome does not depend on level iteration order:
    //   1. object path  (unique by construction)   -> resolve
    //   2. internal name(unique within the level)  -> resolve
    //   3. exact label  (NOT unique)               -> 1 match resolves; >1 is Ambiguous
    //   4. label substring (NOT unique)            -> 1 match resolves; >1 is Ambiguous
    // Tiers 1-2 cannot collide, so they resolve deterministically. Tiers 3-4 can, and
    // when they do the resolver refuses rather than returning whichever actor came first.
    // Matching is case-insensitive throughout, as it has always been.
    //
    // World is honored when non-null; when null the PIE world is searched first (if a PIE
    // session is active) and then the editor world, preserving FindActorByName's behavior.
    PINWRIGHT_API FActorResolution ResolveActor(UWorld* World, const FString& ActorName);

    // Resolve one actor identifier using the same precedence and ambiguity rules, but only
    // considering actors accepted by CandidateFilter. This keeps class-specific callers from
    // letting an unrelated actor's label decide their result while preserving exact path/name
    // precedence within the filtered class. The predicate is evaluated only during resolution;
    // it is not retained after this call returns.
    PINWRIGHT_API FActorResolution ResolveActorFiltered(
        UWorld* World,
        const FString& ActorName,
        TFunctionRef<bool(AActor*)> CandidateFilter,
        EActorResolvePolicy Policy = EActorResolvePolicy::Default);

    // Find an actor in the world by label, name, or path.
    // Thin wrapper over ResolveActor: returns the actor only when the identifier resolved
    // unambiguously, and nullptr when it matched nothing OR matched several actors. It
    // therefore cannot return "some actor that happens to share the label you asked for" -
    // the defect this wrapper used to have. Handlers that can produce a structured error
    // should call ResolveActor (or ActorNameParamUtils::RequireResolvedActor) instead, so
    // an ambiguous identifier reports AMBIGUOUS_ACTOR_NAME with its candidates rather than
    // an indistinguishable ACTOR_NOT_FOUND.
    // Exported: called from the split-out engine-plugin integration modules (PinWrightPCG).
    PINWRIGHT_API AActor* FindActorByName(UWorld* World, const FString& ActorName);

    // Gather the requested actor names from a request payload, accepting either the
    // actorNames array or the singular actorName scalar. Encapsulates the shared
    // dual-accept contract used by actor.select / actor.delete: walk actorNames (skip
    // invalid/non-string entries, TrimStartAndEnd, drop empties, dedupe via AddUnique);
    // only when actorNames is absent fall back to the singular actorName scalar.
    // OutArrayKeyPresent reports whether the actorNames key existed at all, so callers
    // that treat a present-but-empty array specially (actor.select clears the selection)
    // can distinguish it from an absent key. Returns true when at least one usable name
    // was collected; on false the caller emits its own INVALID_ARGUMENT error.
    bool CollectActorNames(const TSharedPtr<FJsonObject>& Payload, TArray<FString>& OutNames, bool& bOutArrayKeyPresent);

    // Find an actor in the world by iterating TActorIterator (label or name match).
    // Lighter-weight variant that does not use EditorActorSubsystem.
    AActor* FindActorByNameSimple(UWorld* World, const FString& ActorName);

    // Iterate actors with a predicate.  Iteration stops when Predicate returns false.
    void ForEachActor(UWorld* World, TFunctionRef<bool(AActor*)> Predicate);

    // Resolve which UWorld a read-only query should run against.
    //   Mode == "editor"        -> editor world.
    //   Mode == "pie"           -> PIE world (GEditor->PlayWorld) or nullptr.
    //   Mode == "auto" or empty -> PIE world if active, else editor world.
    // OutResolvedMode receives the canonical mode the caller asked for
    // ("editor"/"pie"/"auto"), so callers can echo it back to the client.
    PINWRIGHT_API UWorld* ResolveQueryWorld(const FString& Mode, FString& OutResolvedMode);

    // Sum each actor's component bounding box into one enclosing FBox.
    // Seeds FBox(ForceInit) so the result stays invalid until a real actor is
    // found, skips ALevelScriptActor, and unions only boxes that report IsValid
    // so the (0,0,0) seed and bounds-less actors (e.g. lights) don't drag the
    // result toward the origin. The returned box's IsValid is false when no
    // actor contributed finite renderable bounds. Used by both level.get_bounds
    // (level-scoped: ULevel::Actors) and world.set_bounds auto-calculate
    // (world-scoped: the UWorld* overload iterating TActorIterator).
    PINWRIGHT_API FBox SumActorBounds(TConstArrayView<AActor*> Actors);
    PINWRIGHT_API FBox SumActorBounds(UWorld* World);
}
