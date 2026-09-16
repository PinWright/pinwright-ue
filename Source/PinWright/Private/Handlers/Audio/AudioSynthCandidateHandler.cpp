// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioSynthCandidateHandler.cpp - audio.synth.list_candidates / audio.synth.discard
//
// The two verbs over FPwCandidateRegistry (Private/AudioGen), the session-scoped store of
// rendered synth candidates. Everything about eviction that the agent needs to act on -
// current bytes, the budget, and how many candidates have already been reclaimed - rides
// along on both responses, so a candidate never disappears without the caller having been
// able to see it coming.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ErrorCodes.h"

#include "AudioGen/PwAudioAudition.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "State/PluginState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    // Fills the audition seam declared in AudioGen/PwAudioAudition.h. The registry is the only
    // thing that can turn a candidateId into audio, and the audition handler deliberately links
    // neither the registry nor FPluginState, so the binding lives in this file - the one place
    // that already owns both sides. Without it audio.synth.audition rejects every candidateId.
    bool ResolvePwCandidateForAudition(const FString& CandidateId,
                                       FPwAudioBuffer& OutBuffer,
                                       FString& OutErrorCode,
                                       FString& OutError,
                                       TSharedPtr<FJsonObject>& OutErrorData)
    {
        FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();
        const FPwCandidateLookupResult Result = Registry.Get(CandidateId);
        if (!Result.IsHit())
        {
            // The code first: it is what error handling branches on, and evicted /
            // never-generated / unknown-id have divergent remedies. MissErrorCode is the same
            // mapping the list/discard verbs use, so the audition door cannot drift from them.
            OutErrorCode = FPwCandidateRegistry::MissErrorCode(Result.Status);
            // Structured next: the status/removal/budget block is what survives the
            // oversize-spill rewrite, and it is what tells an evicted candidate from a bad id.
            OutErrorData = Registry.BuildMissPayload(CandidateId, Result);
            OutError = FPwCandidateRegistry::MakeMissMessage(CandidateId, Result);
            return false;
        }
        OutBuffer = Result.Candidate->Buffer;
        return true;
    }

    // Bound at static init, alongside the handler registrations in this file, so the seam is
    // live for the whole session. Single-cast is correct here: the registry is the only thing
    // that can resolve a candidate id, so there is never a second resolver to lose.
    struct FPwAuditionResolverBinder
    {
        FPwAuditionResolverBinder()
        {
            PwAuditionCandidateResolver().BindStatic(&ResolvePwCandidateForAudition);
        }
    };
    FPwAuditionResolverBinder GPwAuditionResolverBinder;

    // Collects the requested ids from the singular `id` and the plural `ids`, in order and
    // de-duplicated. De-duplication is what makes a retry of {ids:[x,x]} converge instead of
    // reporting the second x as already-discarded (rule §8).
    TArray<FString> ReadRequestedIds(const FHandlerContext& Ctx, bool& bOutSawEmptyArray)
    {
        bOutSawEmptyArray = false;

        TArray<FString> Ordered;
        TSet<FString> Seen;
        const auto AddId = [&Ordered, &Seen](const FString& Raw)
        {
            FString Id = Raw;
            Id.TrimStartAndEndInline();
            if (!Id.IsEmpty() && !Seen.Contains(Id))
            {
                Seen.Add(Id);
                Ordered.Add(Id);
            }
        };

        const FString Single = Ctx.GetStringFirstOf(
            {TEXT("id"), TEXT("candidateId"), TEXT("candidate_id")});
        if (!Single.IsEmpty())
        {
            AddId(Single);
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = Ctx.GetArray(TEXT("ids"));
        if (Array == nullptr)
        {
            Array = Ctx.GetArray(TEXT("candidateIds"));
        }
        if (Array == nullptr)
        {
            Array = Ctx.GetArray(TEXT("candidate_ids"));
        }
        if (Array != nullptr)
        {
            // An explicitly empty array is a selector that matched nothing, which the caller
            // must be told about rather than have read as "no selector given".
            bOutSawEmptyArray = (Array->Num() == 0);
            for (const TSharedPtr<FJsonValue>& Value : *Array)
            {
                if (Value.IsValid() && Value->Type == EJson::String)
                {
                    AddId(Value->AsString());
                }
            }
        }

        return Ordered;
    }
}

REGISTER_RPC_HANDLER("audio.synth.list_candidates", "audio.synth",
    "List the rendered synth candidates held in this session, newest first, with the registry's "
    "current byte usage and budget so eviction is visible before it happens.",
    RPC_PARAMS(
        RPC_PARAM_DEF("limit", "integer", "Maximum rows to return (1-200)", "20"),
        RPC_PARAM_DEF("offset", "integer", "Rows to skip before the first returned row", "0"),
        RPC_PARAM_OPT_ALIAS("fields", "array|string",
            "Per-row column allow-list (e.g. [\"id\",\"recipeDigest\"]). Omit for every column.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "boolean",
            "Shorthand for fields=[id, createdAt, recipeDigest] - the columns needed to pick a candidate.", "names_only")
    ))
{
    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), 20), 1, 200);
    const int32 Offset = FMath::Max(0, Ctx.GetInt(TEXT("offset"), 0));

    // Per-row projection mirrors actor.list / asset list verbs. namesOnly keeps the three
    // columns an agent needs to pick a candidate; the full row carries the artifact flags too.
    // Probe keys are lowercase to match the lowercased set ReadFieldProjection returns.
    const TSet<FString> Fields = Ctx.ReadFieldProjection(
        {TEXT("id"), TEXT("createdat"), TEXT("recipedigest")});
    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key)
    {
        return !bProject || Fields.Contains(FString(Key));
    };

    int32 Total = 0;
    const TArray<FPwCandidateSummary> Page = Registry.List(Offset, Limit, Total);

    TArray<TSharedPtr<FJsonValue>> Rows;
    Rows.Reserve(Page.Num());
    for (const FPwCandidateSummary& Row : Page)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (Wants(TEXT("id")))              Obj->SetStringField(TEXT("id"), Row.CandidateId);
        if (Wants(TEXT("createdat")))       Obj->SetStringField(TEXT("createdAt"), Row.CreatedAt.ToIso8601());
        if (Wants(TEXT("durationseconds"))) Obj->SetNumberField(TEXT("durationSeconds"), Row.DurationSeconds);
        if (Wants(TEXT("samplerate")))      Obj->SetNumberField(TEXT("sampleRate"), Row.SampleRate);
        if (Wants(TEXT("approxbytes")))     Obj->SetNumberField(TEXT("approxBytes"), static_cast<double>(Row.ApproxBytes));
        if (Wants(TEXT("hasanalysis")))     Obj->SetBoolField(TEXT("hasAnalysis"), Row.bHasAnalysis);
        if (Wants(TEXT("exported")))        Obj->SetBoolField(TEXT("exported"), Row.bExported);
        // Only emitted when there is a path: an empty string beside exported=false would be
        // noise repeated on every unexported row.
        if (Row.bExported && Wants(TEXT("exportedassetpath")))
        {
            Obj->SetStringField(TEXT("exportedAssetPath"), Row.ExportedAssetPath);
        }
        if (Wants(TEXT("images")))          Obj->SetNumberField(TEXT("images"), Row.NumImages);
        // The digest, never the recipe: a dozen full recipes spill the 10,000-char response
        // threshold, which costs the agent the whole listing.
        if (Wants(TEXT("recipedigest")))    Obj->SetStringField(TEXT("recipeDigest"), Row.RecipeDigest);
        Rows.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("candidates"), Rows);
    Result->SetNumberField(TEXT("returned"), Rows.Num());
    Result->SetNumberField(TEXT("total"), Total);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetNumberField(TEXT("offset"), Offset);
    Result->SetObjectField(TEXT("registry"),
        FPwCandidateRegistry::UsageToJson(Registry.Usage()));

    // An empty page and an empty registry are different facts and point at different fixes,
    // so the message says which one happened rather than leaving the caller to infer it.
    FString Message;
    if (Total == 0)
    {
        Message = TEXT("No candidates exist in this session; render one first.");
    }
    else if (Rows.Num() == 0)
    {
        Message = FString::Printf(
            TEXT("Offset %d is past the last of %d candidates; nothing to return at that page."),
            Offset, Total);
    }
    else
    {
        Message = FString::Printf(TEXT("%d of %d candidates."), Rows.Num(), Total);
    }

    Ctx.SendSuccess(Message, Result);
    return true;
}

REGISTER_RPC_HANDLER("audio.synth.discard", "audio.synth",
    "Discard one or more synth candidates by id, or every candidate with all=true. Idempotent, "
    "and reports per id what actually happened (discarded / already discarded / already evicted / "
    "not found).",
    RPC_PARAMS(
        RPC_PARAM_OPT("id", "string", "Candidate id to discard"),
        RPC_PARAM_OPT("ids", "array", "Candidate ids to discard in one call"),
        RPC_PARAM_OPT("all", "boolean",
            "Discard every resident candidate. Required to be explicit - there is no implicit discard-all.")
    ))
{
    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    const bool bAll = Ctx.GetBool(TEXT("all"), false);

    bool bSawEmptyArray = false;
    const TArray<FString> RequestedIds = ReadRequestedIds(Ctx, bSawEmptyArray);

    // Contradictory selection. Guessing which one the caller meant is exactly the unsafe
    // default rule §3 forbids, and the wrong guess destroys candidates.
    if (bAll && RequestedIds.Num() > 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("Pass either 'all': true or specific ids, not both - the two select different "
                 "sets and discarding is not reversible."));
        return true;
    }

    if (bAll)
    {
        const int32 NumDiscarded = Registry.DiscardAll();
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("all"), true);
        // Measured, not asserted: zero is the honest answer for a repeat call, and it is what
        // makes discard-all idempotent under the transport's response-only timeout.
        Result->SetNumberField(TEXT("discarded"), NumDiscarded);
        Result->SetObjectField(TEXT("registry"),
            FPwCandidateRegistry::UsageToJson(Registry.Usage()));
        Ctx.SendSuccess(FString::Printf(
            TEXT("Discarded %d resident candidate(s)."), NumDiscarded), Result);
        return true;
    }

    if (RequestedIds.Num() == 0)
    {
        // No selector at all. Not a silent no-op, and emphatically not an implicit
        // discard-everything (rule §3).
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            bSawEmptyArray
                ? TEXT("'ids' was an empty array, so the call selected no candidate. Pass at "
                       "least one id, or 'all': true to discard everything.")
                : TEXT("Pass 'id', 'ids', or 'all': true. A discard with no selector is "
                       "rejected rather than treated as discard-everything."));
        return true;
    }

    int32 NumDiscarded = 0;
    int32 NumAlreadyDiscarded = 0;
    int32 NumAlreadyEvicted = 0;
    int32 NumNotFound = 0;

    TArray<TSharedPtr<FJsonValue>> PerId;
    PerId.Reserve(RequestedIds.Num());
    for (const FString& Id : RequestedIds)
    {
        // The Discard return is the authority on what this call did; Get is used only to
        // decorate a miss with the removal record, and cannot turn a miss into a hit.
        const EPwCandidateLookup PriorStatus = Registry.Discard(Id);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("id"), Id);
        Entry->SetStringField(TEXT("outcome"),
            FPwCandidateRegistry::LexDiscardOutcome(PriorStatus));

        switch (PriorStatus)
        {
        case EPwCandidateLookup::Found:     ++NumDiscarded; break;
        case EPwCandidateLookup::Discarded: ++NumAlreadyDiscarded; break;
        case EPwCandidateLookup::Evicted:   ++NumAlreadyEvicted; break;
        default:                            ++NumNotFound; break;
        }

        if (PriorStatus != EPwCandidateLookup::Found)
        {
            const FPwCandidateLookupResult Miss = Registry.Get(Id);
            if (Miss.Removal.IsSet())
            {
                Entry->SetStringField(TEXT("reason"),
                    FPwCandidateRegistry::LexRemovalReason(Miss.Removal->Reason));
                Entry->SetStringField(TEXT("removedAt"), Miss.Removal->RemovedAt.ToIso8601());
            }
        }
        PerId.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("requested"), RequestedIds.Num());
    // Four counters rather than one success flag: "discarded 3" and "3 were already gone"
    // are different outcomes, and a blanket success cannot tell them apart.
    Result->SetNumberField(TEXT("discarded"), NumDiscarded);
    Result->SetNumberField(TEXT("alreadyDiscarded"), NumAlreadyDiscarded);
    Result->SetNumberField(TEXT("alreadyEvicted"), NumAlreadyEvicted);
    Result->SetNumberField(TEXT("notFound"), NumNotFound);
    Result->SetArrayField(TEXT("results"), PerId);
    Result->SetObjectField(TEXT("registry"),
        FPwCandidateRegistry::UsageToJson(Registry.Usage()));

    Ctx.SendSuccess(FString::Printf(
        TEXT("Discarded %d of %d requested (%d already discarded, %d already evicted, %d unknown)."),
        NumDiscarded, RequestedIds.Num(), NumAlreadyDiscarded, NumAlreadyEvicted, NumNotFound),
        Result);
    return true;
}
