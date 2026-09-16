// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Wire vocabulary for the "success reported, effect absent" defect class
// (docs/rpc-design.md §1 and §5). Two distinct shapes share one problem — the
// caller cannot tell a durable write from a doomed one — so both get a named,
// measured block instead of being left to prose:
//
//   derivedWrite    — the value the verb was asked to write is RE-DERIVED by the
//                     engine from some other authoritative state. The write lands,
//                     survives read-back, survives save, and is recomputed away on
//                     the next load. Nothing short of a genuine reload can see it.
//
//   consumerRefresh — the verb edited an asset whose already-built consumers cache
//                     derived data (shader maps, material instances, generated
//                     meshes). The edit is durable; it simply has not reached those
//                     consumers, so a downstream measurement scores the edit as a
//                     no-op.
//
// Both blocks are for the case the verb CANNOT fix. When the verb can fix it
// (compile_material rebuilding landscape MICs), it fixes it and reports the
// measured counts through FConsumerRefreshReport anyway, so a caller can tell an
// edit that reached 3 landscapes from one that reached none.
namespace PinWright::DerivedState
{
    // Emits `derivedWrite: {property, derivedFrom, authoritativeVerb, survivesReload,
    // explanation}`. survivesReload is always false here: the block exists only for
    // writes that do not.
    //
    // Call this INSTEAD of reporting a plain success when the verb went ahead with a
    // doomed write, or alongside SendError when it refused. AuthoritativeVerb must
    // name a verb that exists — an unreachable remedy is worse than none
    // (rpc-design.md §7).
    inline void AddDerivedWriteReport(
        const TSharedPtr<FJsonObject>& Result,
        const FString& Property,
        const FString& DerivedFrom,
        const FString& AuthoritativeVerb,
        const FString& Explanation)
    {
        if (!Result.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetStringField(TEXT("property"), Property);
        Block->SetStringField(TEXT("derivedFrom"), DerivedFrom);
        Block->SetStringField(TEXT("authoritativeVerb"), AuthoritativeVerb);
        Block->SetBoolField(TEXT("survivesReload"), false);
        Block->SetStringField(TEXT("explanation"), Explanation);
        Result->SetObjectField(TEXT("derivedWrite"), Block);
    }

    // Counted result of pushing an asset edit into the consumers that cache data
    // derived from it. Default-constructed it reports "nothing measured, nothing
    // refreshed" — the failure direction (rpc-design.md §2: the zero value is a
    // failure), so a path that forgets to measure cannot report coverage.
    struct FConsumerRefreshReport
    {
        // False until something actually enumerated consumers. A false here means the
        // numbers below are not measurements and must not be read as any.
        bool bMeasured = false;

        // Consumers found holding data derived from the edited asset.
        int32 ConsumersFound = 0;

        // Of those, the ones this verb rebuilt — measured, not counted from the number of
        // rebuild calls issued.
        int32 ConsumersRefreshed = 0;

        // Of those, the ones that held nothing derived to rebuild, so the rebuild ran and
        // correctly changed nothing. Kept separate from ConsumersRefreshed because the two
        // are different facts: one is "the cache was replaced", the other is "there was no
        // cache". Collapsing them would let an empty consumer score as a refreshed one.
        int32 ConsumersWithNothingToRefresh = 0;

        // Sub-objects under the refreshed consumers (landscape components, etc.).
        // Purely evidential: it distinguishes "refreshed 1 landscape of 4096
        // components" from "refreshed 1 hollow actor".
        int32 SubObjectsRefreshed = 0;

        // Names of the refreshed consumers, so the caller can check the one it cares
        // about is in the list rather than trusting a count.
        TArray<FString> Refreshed;

        // Consumer kinds this verb knowingly does NOT refresh. Naming the gap is the
        // fix; silently repairing every consumer kind would be an unannounced global
        // mutation (rpc-design.md §11).
        TArray<FString> NotRefreshed;

        // What a caller should do about anything in NotRefreshed. Empty when the
        // coverage is complete.
        FString Remedy;

        // Every consumer found was either rebuilt or had nothing to rebuild, and that was
        // measured rather than assumed. A verb with zero consumers is complete: there was
        // nothing to miss.
        bool IsComplete() const
        {
            return bMeasured &&
                ((ConsumersRefreshed + ConsumersWithNothingToRefresh) == ConsumersFound);
        }

        // Fold one asset's refresh into a running total, for verbs that finalize SEVERAL
        // assets in one call (material.compile_mgir compiles every block in the document).
        //
        // Counts sum: a landscape consuming two of the edited masters is genuinely found
        // and rebuilt twice, and summing keeps IsComplete()'s arithmetic true across the
        // whole call. Names are AddUnique'd instead, because Refreshed exists for a caller
        // to check "is the landscape I care about in this list" — repeats would answer
        // that question no better and read as more coverage than there is.
        //
        // bMeasured is OR'd: one asset that could not be looked at does not erase the
        // measurement of the others, and the per-asset gap shows up as found > refreshed.
        void Accumulate(const FConsumerRefreshReport& Other)
        {
            bMeasured |= Other.bMeasured;
            ConsumersFound += Other.ConsumersFound;
            ConsumersRefreshed += Other.ConsumersRefreshed;
            ConsumersWithNothingToRefresh += Other.ConsumersWithNothingToRefresh;
            SubObjectsRefreshed += Other.SubObjectsRefreshed;
            for (const FString& Name : Other.Refreshed)
            {
                Refreshed.AddUnique(Name);
            }
            for (const FString& Name : Other.NotRefreshed)
            {
                NotRefreshed.AddUnique(Name);
            }
        }
    };

    // Emits `consumerRefresh: {measured, consumersFound, consumersRefreshed,
    // subObjectsRefreshed, refreshed[], notRefreshed[], complete, remedy?}`.
    //
    // `complete` is the field a caller should branch on: it is the conjunction of
    // "we looked" and "we got them all", so an unmeasured run cannot score like a
    // clean one.
    inline void AddConsumerRefreshReport(
        const TSharedPtr<FJsonObject>& Result,
        const FConsumerRefreshReport& Report)
    {
        if (!Result.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetBoolField(TEXT("measured"), Report.bMeasured);
        Block->SetNumberField(TEXT("consumersFound"), Report.ConsumersFound);
        Block->SetNumberField(TEXT("consumersRefreshed"), Report.ConsumersRefreshed);
        Block->SetNumberField(TEXT("consumersWithNothingToRefresh"),
            Report.ConsumersWithNothingToRefresh);
        Block->SetNumberField(TEXT("subObjectsRefreshed"), Report.SubObjectsRefreshed);
        Block->SetBoolField(TEXT("complete"), Report.IsComplete());

        TArray<TSharedPtr<FJsonValue>> RefreshedArray;
        for (const FString& Name : Report.Refreshed)
        {
            RefreshedArray.Add(MakeShared<FJsonValueString>(Name));
        }
        Block->SetArrayField(TEXT("refreshed"), RefreshedArray);

        TArray<TSharedPtr<FJsonValue>> NotRefreshedArray;
        for (const FString& Name : Report.NotRefreshed)
        {
            NotRefreshedArray.Add(MakeShared<FJsonValueString>(Name));
        }
        Block->SetArrayField(TEXT("notRefreshed"), NotRefreshedArray);

        if (!Report.Remedy.IsEmpty())
        {
            Block->SetStringField(TEXT("remedy"), Report.Remedy);
        }

        Result->SetObjectField(TEXT("consumerRefresh"), Block);
    }
}
