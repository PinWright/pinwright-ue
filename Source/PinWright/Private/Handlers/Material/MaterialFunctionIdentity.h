// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// MaterialFunctionIdentity - persistent identity for a material function's FunctionInput /
// FunctionOutput expressions, plus the read that says when a caller has already lost it.
//
// WHY THIS EXISTS. A UMaterialExpressionMaterialFunctionCall does NOT serialize pointers to the
// function's input/output expressions: FFunctionExpressionInput::ExpressionInput and
// FFunctionExpressionOutput::ExpressionOutput are UPROPERTY(transient). What it serializes is a
// GUID per pin - ExpressionInputId / ExpressionOutputId - copied off
// UMaterialExpressionFunctionInput::Id / UMaterialExpressionFunctionOutput::Id when the call node
// last refreshed. On load, UMaterialExpressionMaterialFunctionCall::UpdateFromFunctionResource
// rebuilds both arrays from the function's CURRENT expressions and re-links purely by that GUID
// (Runtime/Engine/Private/Materials/MaterialExpressions.cpp, 5.8: FindInputById for the inputs,
// FixupReferencingInput -> FindOutputIndexById for every FExpressionInput in the consumer that
// reads an output). A GUID that no longer matches is not an error: the input connection is simply
// not carried over, and the referencing input is reset to {Expression=nullptr, OutputIndex=-1}.
// The consumer then compiles and renders as if the author had never wired it.
//
// Two ways to break that link, both of which PinWright could produce:
//
//  1. NEVER GIVING THE PIN AN ID. Id is not initialized by the constructor. The engine's own
//     creation paths call ConditionallyGenerateId; a creation path that does not leaves Id
//     all-zero, and a caller bound to that function caches an all-zero GUID and saves it.
//     UMaterialExpressionFunctionInput::PostLoad / UMaterialExpressionFunctionOutput::PostLoad
//     then call ConditionallyGenerateId(false), which mints a valid Id on load - so the function
//     comes back with an Id the saved caller has never seen, and the wires drop. Outside a cook
//     CookDeterminism::NewGuid is plain FGuid::NewGuid(), so this repeats with a different GUID on
//     every single load and no amount of re-loading converges.
//
//  2. REPLACING THE ID OF A PIN THAT ALREADY EXISTS. Anything that rebuilds a function graph -
//     MGIR Append empties the expression collection and re-emits it - creates brand new
//     FunctionInput / FunctionOutput objects with brand new Ids. The function is then correct in
//     isolation and every already-saved consumer of it is silently disconnected on its next load.
//     Identity therefore has to be carried across a rebuild by the one handle a rebuild preserves,
//     the pin NAME (which is also what the engine treats as renameable-without-breaking: see
//     UpdateFromFunctionResource propagating the new name over a matched Id).
//
// So: EnsurePersistentIds is the never-write-an-anonymous-pin guard for creation and save paths,
// CaptureIds / RestoreIds is the identity-preserving bracket for a graph rebuild, and
// CollectUnstableFunctionCalls is the read that makes the failure visible instead of silent.
//
// LIMIT, STATED PLAINLY. The detector is an in-memory read, and by the time PinWright can look at
// a freshly loaded asset the engine's PostLoad has already replaced an invalid on-disk Id with a
// fresh valid one and already discarded the caller's stale GUID inside
// UpdateFromFunctionResource. An asset poisoned by (1) BEFORE this guard shipped therefore reads
// clean here; what is detectable is the window in which the damage is created - a caller holding
// an Id that is invalid, or that no longer names any pin of the function it points at, i.e. a
// caller that has not been saved yet or has been saved against a function that moved underneath
// it. Repairing a legacy asset means re-saving the function (which persists the Id its PostLoad
// minted) and then re-wiring and re-saving the consumer; nothing can recover the wire itself,
// because the GUID that recorded it is overwritten during load.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"

namespace PinWright::MaterialFunctionIdentity
{
#if WITH_EDITORONLY_DATA

/** One pin of one call node whose cached GUID will not survive the consumer's next load. */
struct FUnstableFunctionCall
{
    FString NodeId;
    FString FunctionPath;
    FString PinKind;
    FString PinName;
    FString Reason;
};

/**
 * Give every FunctionInput / FunctionOutput in the graph a persistent Id if it has none.
 * Returns how many pins were anonymous. Never touches a pin that already carries a valid Id -
 * that Id is what saved callers match on.
 */
inline int32 EnsurePersistentIds(UMaterialFunctionInterface* Function)
{
    if (!Function)
    {
        return 0;
    }

    int32 Repaired = 0;
    for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
    {
        if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression.Get()))
        {
            if (!Input->Id.IsValid())
            {
                Input->ConditionallyGenerateId(false);
                ++Repaired;
            }
        }
        else if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression.Get()))
        {
            if (!Output->Id.IsValid())
            {
                Output->ConditionallyGenerateId(false);
                ++Repaired;
            }
        }
    }
    return Repaired;
}

/** Pin-name -> Id maps taken before a rebuild, so the rebuilt pins can be handed the same identity. */
struct FIdSnapshot
{
    TMap<FName, FGuid> InputIds;
    TMap<FName, FGuid> OutputIds;

    bool IsEmpty() const { return InputIds.Num() == 0 && OutputIds.Num() == 0; }
};

inline void CaptureIds(UMaterialFunctionInterface* Function, FIdSnapshot& OutSnapshot)
{
    if (!Function)
    {
        return;
    }

    for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
    {
        if (const UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression.Get()))
        {
            // First pin of a given name wins, and an anonymous pin is not recorded: handing an
            // invalid Id back would recreate exactly the state this file exists to prevent.
            if (Input->Id.IsValid() && !Input->InputName.IsNone())
            {
                OutSnapshot.InputIds.FindOrAdd(Input->InputName, Input->Id);
            }
        }
        else if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression.Get()))
        {
            if (Output->Id.IsValid() && !Output->OutputName.IsNone())
            {
                OutSnapshot.OutputIds.FindOrAdd(Output->OutputName, Output->Id);
            }
        }
    }
}

/**
 * Hand each rebuilt pin back the Id the pin of the same name carried before the rebuild.
 * Each snapshot entry is consumed once, so two pins that ended up sharing a name cannot end up
 * sharing an Id - a duplicate Id resolves to the first match and moves a caller's wire.
 * Returns how many pins kept their previous identity.
 *
 * OutOrphanedPinNames collects the snapshot entries no rebuilt pin claimed. Those are pins the
 * rebuild renamed or dropped, and every caller wired to one loses that wire on its next load -
 * a real disconnection, created at this instant, that nothing else in the graph records. The
 * caller is expected to report them; discarding them silently is what made the rename case
 * indistinguishable from a clean rebuild.
 */
inline int32 RestoreIds(UMaterialFunctionInterface* Function, const FIdSnapshot& Snapshot,
    TArray<FString>* OutOrphanedPinNames = nullptr)
{
    if (!Function || Snapshot.IsEmpty())
    {
        return 0;
    }

    TMap<FName, FGuid> RemainingInputs = Snapshot.InputIds;
    TMap<FName, FGuid> RemainingOutputs = Snapshot.OutputIds;

    int32 Restored = 0;
    for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
    {
        if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression.Get()))
        {
            FGuid PreviousId;
            if (RemainingInputs.RemoveAndCopyValue(Input->InputName, PreviousId))
            {
                Input->Id = PreviousId;
                ++Restored;
            }
        }
        else if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression.Get()))
        {
            FGuid PreviousId;
            if (RemainingOutputs.RemoveAndCopyValue(Output->OutputName, PreviousId))
            {
                Output->Id = PreviousId;
                ++Restored;
            }
        }
    }

    if (OutOrphanedPinNames)
    {
        for (const TPair<FName, FGuid>& Orphan : RemainingInputs)
        {
            OutOrphanedPinNames->Add(FString::Printf(TEXT("input '%s'"), *Orphan.Key.ToString()));
        }
        for (const TPair<FName, FGuid>& Orphan : RemainingOutputs)
        {
            OutOrphanedPinNames->Add(FString::Printf(TEXT("output '%s'"), *Orphan.Key.ToString()));
        }
    }
    return Restored;
}

namespace Private
{
    inline void CountPinIds(UMaterialFunctionInterface* Function,
        TMap<FGuid, int32>& OutInputIds, TMap<FGuid, int32>& OutOutputIds)
    {
        if (!Function)
        {
            return;
        }

        for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
        {
            if (const UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression.Get()))
            {
                ++OutInputIds.FindOrAdd(Input->Id, 0);
            }
            else if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression.Get()))
            {
                ++OutOutputIds.FindOrAdd(Output->Id, 0);
            }
        }
    }

    inline FString ClassifyPinId(const FGuid& CachedId, const TMap<FGuid, int32>& LiveIds)
    {
        if (!CachedId.IsValid())
        {
            return TEXT("missing-persistent-id");
        }
        const int32 Matches = LiveIds.FindRef(CachedId);
        if (Matches == 0)
        {
            return TEXT("stale-persistent-id");
        }
        if (Matches > 1)
        {
            return TEXT("duplicate-persistent-id");
        }
        return FString();
    }
}

/**
 * Every call-node pin in the graph whose cached GUID will not re-link on the next load: no Id at
 * all, an Id no pin of the target function carries any more, or an Id two pins share.
 */
inline void CollectUnstableFunctionCalls(
    TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions,
    TArray<FUnstableFunctionCall>& OutUnstable)
{
    for (const TObjectPtr<UMaterialExpression>& Expression : Expressions)
    {
        UMaterialExpressionMaterialFunctionCall* Call =
            Cast<UMaterialExpressionMaterialFunctionCall>(Expression.Get());
        if (!Call || !Call->MaterialFunction)
        {
            continue;
        }

        TMap<FGuid, int32> LiveInputIds;
        TMap<FGuid, int32> LiveOutputIds;
        Private::CountPinIds(Call->MaterialFunction.Get(), LiveInputIds, LiveOutputIds);

        const FString NodeId = Call->MaterialExpressionGuid.ToString();
        const FString FunctionPath = Call->MaterialFunction->GetPathName();

        for (const FFunctionExpressionInput& CallInput : Call->FunctionInputs)
        {
            const FString Reason = Private::ClassifyPinId(CallInput.ExpressionInputId, LiveInputIds);
            if (Reason.IsEmpty())
            {
                continue;
            }
            const FName PinName = CallInput.ExpressionInput
                ? CallInput.ExpressionInput->InputName
                : CallInput.Input.InputName;
            OutUnstable.Add({ NodeId, FunctionPath, TEXT("input"), PinName.ToString(), Reason });
        }

        for (const FFunctionExpressionOutput& CallOutput : Call->FunctionOutputs)
        {
            const FString Reason = Private::ClassifyPinId(CallOutput.ExpressionOutputId, LiveOutputIds);
            if (Reason.IsEmpty())
            {
                continue;
            }
            const FName PinName = CallOutput.ExpressionOutput
                ? CallOutput.ExpressionOutput->OutputName
                : CallOutput.Output.OutputName;
            OutUnstable.Add({ NodeId, FunctionPath, TEXT("output"), PinName.ToString(), Reason });
        }
    }
}

/**
 * Adds the `functionCallIdentity` block to a read-back response, and ONLY when there is something
 * to report - an always-present empty array on every material read would be noise, and the whole
 * point of this block is that its presence is the signal.
 */
inline void AddUnstableFunctionCallReport(const TSharedPtr<FJsonObject>& Result, UMaterial* Material)
{
    if (!Result.IsValid() || !Material || !Material->GetEditorOnlyData())
    {
        return;
    }

    TArray<FUnstableFunctionCall> Unstable;
    CollectUnstableFunctionCalls(
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions, Unstable);
    if (Unstable.Num() == 0)
    {
        return;
    }

    TArray<TSharedPtr<FJsonValue>> Entries;
    Entries.Reserve(Unstable.Num());
    for (const FUnstableFunctionCall& Entry : Unstable)
    {
        TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
        EntryObj->SetStringField(TEXT("nodeId"), Entry.NodeId);
        EntryObj->SetStringField(TEXT("functionPath"), Entry.FunctionPath);
        EntryObj->SetStringField(TEXT("pinKind"), Entry.PinKind);
        EntryObj->SetStringField(TEXT("pinName"), Entry.PinName);
        EntryObj->SetStringField(TEXT("reason"), Entry.Reason);
        Entries.Add(MakeShared<FJsonValueObject>(EntryObj));
    }

    TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
    Block->SetArrayField(TEXT("unstable"), Entries);
    Block->SetStringField(TEXT("warning"),
        TEXT("A material function call links to its function's pins by GUID, not by name or index. ")
        TEXT("The listed pins cache a GUID the function no longer carries, so the engine drops ")
        TEXT("those wires the next time this material is loaded - silently, with no compile error: ")
        TEXT("the pin simply reads as never connected. Re-save the material FUNCTION first (that ")
        TEXT("persists the pin GUIDs it currently holds), then re-wire these pins and save this ")
        TEXT("material; saving this material alone re-records the same doomed GUID."));
    Result->SetObjectField(TEXT("functionCallIdentity"), Block);
}

#endif // WITH_EDITORONLY_DATA
} // namespace PinWright::MaterialFunctionIdentity
