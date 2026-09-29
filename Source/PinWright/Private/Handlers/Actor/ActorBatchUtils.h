// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared batch driver for the per-actor transform verbs (actor.set_transform, actor.nudge).
// Both accept an `actors[]` entry array beside their single-actor form: every entry carries
// its own actor identity plus the same per-actor fields the single form takes at top level.
//
// Two phases, so a malformed request never half-applies:
//   1. ReadEntries validates the whole request shape (non-empty array of objects, each with
//      an identity, no single-form fields at top level, the verb's own per-entry shape check)
//      and refuses with INVALID_ARGUMENT naming every faulty index before anything is touched.
//   2. RunBatch resolves and applies each entry in order inside ONE FScopedTransaction and
//      reports index-aligned per-entry results. A failed entry (missing, ambiguous, verb-level
//      refusal) does not roll back the entries that succeeded.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Templates/Function.h"

namespace ActorBatchUtils
{

// Outcome of one per-actor operation. On success Data is the row payload; on failure Data
// optionally carries structured detail (e.g. ambiguity candidates) beside Code/Message.
struct FEntryOutcome
{
    bool bOk = false;
    FString Code;
    FString Message;
    TSharedPtr<FJsonObject> Data;
};

inline FEntryOutcome Succeed(const TSharedPtr<FJsonObject>& Data)
{
    FEntryOutcome Out;
    Out.bOk = true;
    Out.Data = Data;
    return Out;
}

inline FEntryOutcome Fail(const TCHAR* Code, const FString& Message,
    const TSharedPtr<FJsonObject>& Detail = nullptr)
{
    FEntryOutcome Out;
    Out.Code = Code;
    Out.Message = Message;
    Out.Data = Detail;
    return Out;
}

// Single-form delivery of an outcome: the exact response the verb sent before batching existed.
inline void SendOutcome(const FHandlerContext& Ctx, const FEntryOutcome& Outcome)
{
    if (Outcome.bOk)
    {
        Ctx.SendSuccess(Outcome.Data);
    }
    else
    {
        Ctx.SendError(Outcome.Code, Outcome.Message, Outcome.Data);
    }
}

// First non-empty actor identity in an entry, read over the same key set the single form accepts.
inline FString EntryActorName(const TSharedPtr<FJsonObject>& Entry)
{
    for (const FString& Key : ActorNameParamUtils::ActorNameKeys())
    {
        FString Value;
        if (Entry.IsValid() && Entry->TryGetStringField(Key, Value) && !Value.IsEmpty())
        {
            return Value;
        }
    }
    return FString();
}

// A resolution that did not name exactly one actor, as an outcome. Role names the slot
// ("actor", "lookAt target") so a batch row says which identity failed.
inline FEntryOutcome ResolutionFailure(const FString& Identifier,
    const McpActorUtils::FActorResolution& Resolution, const TCHAR* Role)
{
    if (Resolution.IsAmbiguous())
    {
        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Candidates;
        for (AActor* Candidate : Resolution.Candidates)
        {
            if (!Candidate)
            {
                continue;
            }
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetStringField(TEXT("label"), Candidate->GetActorLabel());
            C->SetStringField(TEXT("name"), Candidate->GetName());
            C->SetStringField(TEXT("path"), Candidate->GetPathName());
            C->SetStringField(TEXT("class"), Candidate->GetClass()->GetPathName());
            Candidates.Add(MakeShared<FJsonValueObject>(C));
        }
        Detail->SetArrayField(TEXT("candidates"), Candidates);
        return Fail(ErrorCodes::ERR_AMBIGUOUS_ACTOR_NAME,
            FString::Printf(TEXT("%s '%s' matches %d actors; re-issue with a unique internal object name or path from candidates."),
                Role, *Identifier, Candidates.Num()),
            Detail);
    }
    return Fail(ErrorCodes::ERR_ACTOR_NOT_FOUND,
        FString::Printf(TEXT("%s '%s' matches no actor by display label, internal object name, or object path."),
            Role, *Identifier));
}

// Phase 1. Reads and validates actors[]; on any fault sends INVALID_ARGUMENT (listing every
// fault) and returns false with nothing mutated. SingleFormKeys are the top-level keys that
// belong inside each entry in batch form (the identity slot plus the verb's per-actor fields);
// accepting them beside actors[] would leave the caller guessing which one applied.
inline bool ReadEntries(const FHandlerContext& Ctx, const TArray<FString>& SingleFormKeys,
    TFunctionRef<bool(const TSharedPtr<FJsonObject>&, FString&)> ValidateEntry,
    TArray<TSharedPtr<FJsonObject>>& OutEntries)
{
    OutEntries.Reset();
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
    if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("actors"), Raw) || !Raw)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actors must be an array of entry objects."));
        return false;
    }
    if (Raw->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actors[] is empty; pass at least one entry."));
        return false;
    }

    TArray<FString> Faults;
    for (const FString& Key : SingleFormKeys)
    {
        if (Payload->HasField(Key))
        {
            Faults.Add(FString::Printf(TEXT("top-level '%s' is not accepted beside actors[]; put it inside each entry"), *Key));
        }
    }
    for (int32 Index = 0; Index < Raw->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>* EntryObj = nullptr;
        if (!(*Raw)[Index].IsValid() || !(*Raw)[Index]->TryGetObject(EntryObj) || !EntryObj || !EntryObj->IsValid())
        {
            Faults.Add(FString::Printf(TEXT("actors[%d] is not an object"), Index));
            continue;
        }
        if (EntryActorName(*EntryObj).IsEmpty())
        {
            Faults.Add(FString::Printf(TEXT("actors[%d] has no actor identity (one of: %s)"),
                Index, *FString::Join(ActorNameParamUtils::ActorNameKeys(), TEXT(", "))));
        }
        FString EntryError;
        if (!ValidateEntry(*EntryObj, EntryError))
        {
            Faults.Add(FString::Printf(TEXT("actors[%d]: %s"), Index, *EntryError));
        }
        OutEntries.Add(*EntryObj);
    }
    if (Faults.Num() > 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Nothing was applied; the batch is malformed: %s."), *FString::Join(Faults, TEXT("; "))));
        return false;
    }
    return true;
}

// Phase 2. Applies every entry in order under one transaction and sends the batch response:
//   { success (every entry applied), total, succeededCount, failedCount,
//     results[] (index-aligned: {index, requestedName, success, ...row | errorCode, message, ...detail}),
//     missing[] / ambiguous[] + ambiguousCount (only when non-empty, as actor.set_folder / actor.delete) }
// When no entry applied the same body goes out as an error carrying the first failure's code.
inline void RunBatch(const FHandlerContext& Ctx, const TCHAR* TransactionLabel,
    const TArray<TSharedPtr<FJsonObject>>& Entries,
    TFunctionRef<FEntryOutcome(AActor*, const TSharedPtr<FJsonObject>&)> Apply)
{
    TArray<TSharedPtr<FJsonValue>> Results;
    TArray<TSharedPtr<FJsonValue>> Missing;
    TArray<TSharedPtr<FJsonValue>> Ambiguous;
    int32 Succeeded = 0;
    int32 FirstFailure = INDEX_NONE;
    FString FirstFailureCode;
    FString FirstFailureMessage;

    {
        FScopedTransaction Transaction(FText::FromString(TransactionLabel));
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>& Entry = Entries[Index];
            const FString Name = EntryActorName(Entry);

            FEntryOutcome Outcome;
            const McpActorUtils::FActorResolution Resolution = McpActorUtils::ResolveActor(nullptr, Name);
            if (!Resolution.IsResolved())
            {
                Outcome = ResolutionFailure(Name, Resolution, TEXT("actor"));
                if (Resolution.IsAmbiguous())
                {
                    TSharedPtr<FJsonObject> Amb = MakeShared<FJsonObject>();
                    Amb->SetStringField(TEXT("requestedName"), Name);
                    Amb->SetArrayField(TEXT("candidates"), Outcome.Data->GetArrayField(TEXT("candidates")));
                    Ambiguous.Add(MakeShared<FJsonValueObject>(Amb));
                }
                else
                {
                    Missing.Add(MakeShared<FJsonValueString>(Name));
                }
            }
            else
            {
                Outcome = Apply(Resolution.Actor, Entry);
            }

            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            if (Outcome.Data.IsValid())
            {
                Row->Values = Outcome.Data->Values;
            }
            Row->SetNumberField(TEXT("index"), Index);
            Row->SetStringField(TEXT("requestedName"), Name);
            Row->SetBoolField(TEXT("success"), Outcome.bOk);
            if (Outcome.bOk)
            {
                ++Succeeded;
            }
            else
            {
                Row->SetStringField(TEXT("errorCode"), Outcome.Code);
                Row->SetStringField(TEXT("message"), Outcome.Message);
                if (FirstFailure == INDEX_NONE)
                {
                    FirstFailure = Index;
                    FirstFailureCode = Outcome.Code;
                    FirstFailureMessage = Outcome.Message;
                }
            }
            Results.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    const int32 Failed = Entries.Num() - Succeeded;
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), Failed == 0);
    Data->SetNumberField(TEXT("total"), Entries.Num());
    Data->SetNumberField(TEXT("succeededCount"), Succeeded);
    Data->SetNumberField(TEXT("failedCount"), Failed);
    Data->SetArrayField(TEXT("results"), Results);
    if (Missing.Num() > 0)
    {
        Data->SetArrayField(TEXT("missing"), Missing);
    }
    if (Ambiguous.Num() > 0)
    {
        Data->SetArrayField(TEXT("ambiguous"), Ambiguous);
        Data->SetNumberField(TEXT("ambiguousCount"), Ambiguous.Num());
    }

    if (Succeeded == 0)
    {
        Ctx.SendError(FirstFailureCode,
            FString::Printf(TEXT("No entry was applied (%d failed). First failure, actors[%d]: %s Per-entry results are in results[]."),
                Failed, FirstFailure, *FirstFailureMessage),
            Data);
        return;
    }
    Ctx.SendSuccess(Data);
}

} // namespace ActorBatchUtils
