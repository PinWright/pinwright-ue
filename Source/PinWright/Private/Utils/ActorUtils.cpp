// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/ActorUtils.h"

#include "Compat/EngineVersionCompat.h"
#include "Editor.h"
#include "EngineUtils.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#include "EditorAssetLibrary.h"
#include "Engine/LevelScriptActor.h"
#include "Dom/JsonObject.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpActorUtils, Log, All);

namespace
{
    // Single per-actor bounds-union rule shared by both SumActorBounds overloads:
    // skip null/ALevelScriptActor, union only boxes that report IsValid.
    void AccumulateActorBounds(FBox& InOutBounds, AActor* Actor)
    {
        if (!Actor || Actor->IsA(ALevelScriptActor::StaticClass()))
        {
            return;
        }
        const FBox ActorBounds = Actor->GetComponentsBoundingBox();
        if (ActorBounds.IsValid)
        {
            InOutBounds += ActorBounds;
        }
    }
}

namespace
{
    // One tier of McpActorUtils::ResolveActor's precedence ladder, applied to a whole
    // candidate set at once. Evaluating a tier across EVERY actor before falling to the
    // next is what makes the outcome independent of level iteration order: the old
    // implementation broke out of the loop on the first actor matching label OR name OR
    // path, so which actor won depended on where it sat in the array.
    void ActorResolveGatherTier(
        TConstArrayView<AActor*> Actors,
        TFunctionRef<bool(AActor*)> Predicate,
        TArray<AActor*>& OutMatches)
    {
        for (AActor* A : Actors)
        {
            if (A && Predicate(A))
            {
                OutMatches.Add(A);
            }
        }
    }

    // Turn one tier's match set into a resolution: nothing found leaves the caller to try
    // the next tier, exactly one resolves, and more than one is Ambiguous. The unique tiers
    // (object path, internal FName) are handled by the same rule rather than assumed
    // singular - if the engine's own uniqueness invariant ever broke, this reports it
    // instead of silently returning the first of two.
    // Returns true when the tier decided the outcome, so the caller stops descending.
    bool ActorResolveFinishTier(
        TArray<AActor*>& Matches,
        McpActorUtils::EActorMatchKind Kind,
        McpActorUtils::FActorResolution& OutResolution)
    {
        if (Matches.Num() == 0)
        {
            return false;
        }
        OutResolution.MatchedBy = Kind;
        if (Matches.Num() == 1)
        {
            OutResolution.Status = McpActorUtils::EActorResolveStatus::Resolved;
            OutResolution.Actor = Matches[0];
        }
        else
        {
            OutResolution.Status = McpActorUtils::EActorResolveStatus::Ambiguous;
            OutResolution.Candidates = MoveTemp(Matches);
        }
        return true;
    }

    // Run the whole precedence ladder over ONE candidate set. Returns true when that set
    // decided the outcome (resolved or ambiguous) and false when nothing matched at any
    // tier, which is what lets the caller try the editor world after the PIE world comes up
    // empty - preserving FindActorByName's original "PIE first, else editor" fallback.
    bool ActorResolveRunTiers(
        TConstArrayView<AActor*> Candidates,
        const FString& ActorName,
        McpActorUtils::FActorResolution& OutResolution,
        McpActorUtils::EActorResolvePolicy Policy)
    {
        using McpActorUtils::EActorMatchKind;

        if (Policy != McpActorUtils::EActorResolvePolicy::ExactLabel)
        {
            // Tier 1: internal object name. Unique within a level, so this is the collision-safe
            // key the wiki tells callers to prefer - and it must therefore outrank a label that
            // merely happens to spell the same string on a different actor.
            {
                TArray<AActor*> Matches;
                ActorResolveGatherTier(Candidates, [&ActorName](AActor* A)
                    { return A->GetName().Equals(ActorName, ESearchCase::IgnoreCase); }, Matches);
                if (ActorResolveFinishTier(Matches, EActorMatchKind::ObjectName, OutResolution))
                {
                    return true;
                }
            }
        }

        // Tier 3: exact display label. NOT unique - two actors legitimately share "Tree".
        {
            TArray<AActor*> Matches;
            ActorResolveGatherTier(Candidates, [&ActorName](AActor* A)
                { return A->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase); }, Matches);
            if (ActorResolveFinishTier(Matches, EActorMatchKind::Label, OutResolution))
            {
                return true;
            }
        }

        // The legacy default policy keeps the historical label-substring tier for
        // non-geometry callers. ExactIdentity and ExactLabel intentionally stop here.
        if (Policy == McpActorUtils::EActorResolvePolicy::Default)
        {
            TArray<AActor*> Matches;
            ActorResolveGatherTier(Candidates, [&ActorName](AActor* A)
                { return A->GetActorLabel().Contains(ActorName, ESearchCase::IgnoreCase); }, Matches);
            if (ActorResolveFinishTier(Matches, EActorMatchKind::LabelSubstring, OutResolution))
            {
                return true;
            }
        }

        return false;
    }

    bool ActorResolveTryObjectPath(
        UWorld* SelectedWorld,
        const FString& ActorName,
        TFunctionRef<bool(AActor*)> CandidateFilter,
        McpActorUtils::EActorResolvePolicy Policy,
        McpActorUtils::FActorResolution& OutResolution)
    {
        if (Policy == McpActorUtils::EActorResolvePolicy::ExactLabel
            || !SelectedWorld || !ActorName.StartsWith(TEXT("/")))
        {
            return false;
        }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        AActor* const Actor = FindObject<AActor>(
            nullptr, FStringView(ActorName), EFindObjectFlags::None);
#else
        // EFindObjectFlags (and the FStringView overload that takes it) arrived in 5.7; the
        // pre-5.7 spelling is the TCHAR* overload with ExactClass defaulted off, which is what
        // EFindObjectFlags::None means.
        AActor* const Actor = FindObject<AActor>(nullptr, *ActorName);
#endif
        if (!IsValid(Actor) || Actor->GetWorld() != SelectedWorld || !CandidateFilter(Actor))
        {
            return false;
        }

        OutResolution.Status = McpActorUtils::EActorResolveStatus::Resolved;
        OutResolution.MatchedBy = McpActorUtils::EActorMatchKind::ObjectPath;
        OutResolution.Actor = Actor;
        return true;
    }
}

McpActorUtils::FActorResolution McpActorUtils::ResolveActorFiltered(
    UWorld* World,
    const FString& ActorName,
    TFunctionRef<bool(AActor*)> CandidateFilter,
    EActorResolvePolicy Policy)
{
    FActorResolution Resolution;
    if (ActorName.IsEmpty())
    {
        return Resolution;
    }

    // Search order preserves the historical FindActorByName behavior: an explicit World
    // wins; otherwise the PIE world is tried first and the editor world only if PIE
    // produced no match at any tier. Running the whole ladder per world (rather than
    // pooling both worlds' actors) is what keeps that fallback intact - pooling would let
    // a non-empty PIE world suppress the editor-world search entirely.
    struct FResolveWorldCandidate
    {
        UWorld* World = nullptr;
        bool bUseEditorActorSubsystem = false;
    };

    TArray<FResolveWorldCandidate> WorldCandidates;
    if (World)
    {
        WorldCandidates.Add({World, false});
    }
    else if (GEditor)
    {
        WorldCandidates.Add({GEditor->PlayWorld, false});
        WorldCandidates.Add({GEditor->GetEditorWorldContext().World(), true});
    }

    auto CollectWorldActors = [&CandidateFilter](UWorld* FromWorld, TArray<AActor*>& Out)
    {
        if (!FromWorld)
        {
            return;
        }
        for (TActorIterator<AActor> It(FromWorld); It; ++It)
        {
            if (AActor* A = *It; A && CandidateFilter(A))
            {
                Out.Add(A);
            }
        }
    };

    // Resolve an exact live object path before any engine-filtered editor actor
    // enumeration. This is what makes WorldSettings, brushes, AInfo actors, and hidden
    // actors addressable while still refusing a path that belongs to another world.
    for (const FResolveWorldCandidate& Candidate : WorldCandidates)
    {
        if (ActorResolveTryObjectPath(Candidate.World, ActorName, CandidateFilter, Policy, Resolution))
        {
            return Resolution;
        }
    }

    for (const FResolveWorldCandidate& Candidate : WorldCandidates)
    {
        if (!Candidate.World)
        {
            continue;
        }

        TArray<AActor*> Candidates;
        if (!Candidate.bUseEditorActorSubsystem)
        {
            CollectWorldActors(Candidate.World, Candidates);
        }
        else if (GEditor)
        {
            if (UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
            {
                for (AActor* A : ActorSS->GetAllLevelActors())
                {
                    if (A && CandidateFilter(A))
                    {
                        Candidates.Add(A);
                    }
                }
            }
        }

        if (ActorResolveRunTiers(Candidates, ActorName, Resolution, Policy))
        {
            return Resolution;
        }
    }

    // Fallback: try to load as asset if it looks like a path. Gate the load on a quiet
    // registry-existence probe first: UEditorAssetLibrary::LoadAsset logs an editor-level
    // "LoadAsset failed" error for any path with no asset behind it, so probing every
    // path-shaped name that didn't match a live actor (a common miss — a stale objectPath,
    // a typo'd /Game path) spams the log misleadingly. DoesAssetExist is a registry-only
    // lookup that returns false silently, so only paths that actually resolve reach LoadAsset.
    if (Policy != EActorResolvePolicy::ExactLabel &&
        ActorName.StartsWith(TEXT("/")) && UEditorAssetLibrary::DoesAssetExist(ActorName))
    {
        if (UObject* Obj = UEditorAssetLibrary::LoadAsset(ActorName))
        {
            if (AActor* AsActor = Cast<AActor>(Obj); AsActor && CandidateFilter(AsActor))
            {
                Resolution.Status = EActorResolveStatus::Resolved;
                Resolution.MatchedBy = EActorMatchKind::ObjectPath;
                Resolution.Actor = AsActor;
            }
        }
    }

    return Resolution;
}

McpActorUtils::FActorResolution McpActorUtils::ResolveActor(UWorld* World, const FString& ActorName)
{
    return ResolveActorFiltered(World, ActorName, [](AActor*) { return true; });
}

AActor* McpActorUtils::FindActorByName(UWorld* World, const FString& ActorName)
{
    const FActorResolution Resolution = ResolveActor(World, ActorName);
    if (Resolution.IsAmbiguous())
    {
        // Deliberately return nullptr rather than Candidates[0]. This function has ~100
        // call sites that cannot report a structured error; for those, "I will not guess"
        // is the only safe answer, because the alternative is mutating an actor the caller
        // did not name while reporting success.
        UE_LOG(LogMcpActorUtils, Warning,
               TEXT("FindActorByName: '%s' is ambiguous - it matches %d actors. Refusing to guess; "
                    "pass the unique internal object name or object path instead."),
               *ActorName, Resolution.Candidates.Num());
        return nullptr;
    }
    return Resolution.Actor;
}
bool McpActorUtils::CollectActorNames(const TSharedPtr<FJsonObject>& Payload, TArray<FString>& OutNames, bool& bOutArrayKeyPresent)
{
    bOutArrayKeyPresent = false;
    if (!Payload.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
    bOutArrayKeyPresent = Payload->TryGetArrayField(TEXT("actorNames"), NamesArray) && NamesArray;
    if (bOutArrayKeyPresent)
    {
        for (const TSharedPtr<FJsonValue>& Entry : *NamesArray)
        {
            if (!Entry.IsValid() || Entry->Type != EJson::String)
            {
                continue;
            }
            const FString Name = Entry->AsString().TrimStartAndEnd();
            if (!Name.IsEmpty())
            {
                OutNames.AddUnique(Name);
            }
        }
    }
    else
    {
        FString SingleName;
        if (Payload->TryGetStringField(TEXT("actorName"), SingleName))
        {
            SingleName = SingleName.TrimStartAndEnd();
            if (!SingleName.IsEmpty())
            {
                OutNames.AddUnique(SingleName);
            }
        }
    }

    return OutNames.Num() > 0;
}

AActor* McpActorUtils::FindActorByNameSimple(UWorld* World, const FString& ActorName)
{
    if (!World || ActorName.IsEmpty())
        return nullptr;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor)
            continue;
        if (Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase) ||
            Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase) ||
            Actor->GetPathName().Equals(ActorName, ESearchCase::IgnoreCase))
        {
            return Actor;
        }
    }
    return nullptr;
}

void McpActorUtils::ForEachActor(UWorld* World, TFunctionRef<bool(AActor*)> Predicate)
{
    if (!World)
        return;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && !Predicate(Actor))
        {
            break;
        }
    }
}

UWorld* McpActorUtils::ResolveQueryWorld(const FString& Mode, FString& OutResolvedMode)
{
    const FString Lower = Mode.ToLower();
    FString Canonical;
    if (Lower == TEXT("editor"))
    {
        Canonical = TEXT("editor");
    }
    else if (Lower == TEXT("pie"))
    {
        Canonical = TEXT("pie");
    }
    else
    {
        Canonical = TEXT("auto");
    }
    OutResolvedMode = Canonical;

    if (!GEditor)
    {
        return nullptr;
    }

    auto FindPieWorld = []() -> UWorld*
    {
        if (GEditor->PlayWorld)
        {
            return GEditor->PlayWorld;
        }
        for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
        {
            if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
            {
                return Ctx.World();
            }
        }
        return nullptr;
    };

    if (Canonical == TEXT("editor"))
    {
        return GEditor->GetEditorWorldContext().World();
    }

    if (Canonical == TEXT("pie"))
    {
        return FindPieWorld();
    }

    // "auto": PIE-first, fall back to editor world.
    if (UWorld* Pie = FindPieWorld())
    {
        return Pie;
    }
    return GEditor->GetEditorWorldContext().World();
}

FBox McpActorUtils::SumActorBounds(TConstArrayView<AActor*> Actors)
{
    FBox Bounds(ForceInit);
    for (AActor* Actor : Actors)
    {
        AccumulateActorBounds(Bounds, Actor);
    }
    return Bounds;
}

FBox McpActorUtils::SumActorBounds(UWorld* World)
{
    FBox Bounds(ForceInit);
    if (!World)
    {
        return Bounds;
    }
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AccumulateActorBounds(Bounds, *It);
    }
    return Bounds;
}
