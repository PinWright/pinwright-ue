// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PwSource/PwSourceRecompileGuard.h"

#include "Utils/PathUtils.h"

namespace
{
    struct FPwSourceLoss
    {
        const FPwSourceStateEntry* Current = nullptr;
        FString CurrentValue;
        FString DesiredValue;
    };

    // One spelling for both sides of an asset-valued field. The live side reads GetPathName()
    // (/Pkg.Object) and a source file writes the package path (/Pkg); comparing those raw makes
    // one asset look like two. A value that is not a content path at all is returned untouched -
    // the guard reports what it was given rather than inventing a spelling for it.
    FString PwSourceRecompileGuard_Canonical(const FPwSourceStateEntry& Entry, const FString& Value)
    {
        if (!Entry.bAssetPath || Value.IsEmpty())
        {
            return Value;
        }
        FString ObjectPath;
        FString Error;
        return NormalizeToObjectPath(Value, ObjectPath, Error) ? ObjectPath : Value;
    }

    TMap<FString, const FPwSourceStateEntry*> PwSourceRecompileGuard_Index(
        TArrayView<const FPwSourceStateEntry> Entries)
    {
        TMap<FString, const FPwSourceStateEntry*> Result;
        for (const FPwSourceStateEntry& Entry : Entries)
        {
            Result.Add(Entry.Key, &Entry);
        }
        return Result;
    }

    FString PwSourceRecompileGuard_Describe(const FPwSourceLoss& Loss)
    {
        const FPwSourceStateEntry& Current = *Loss.Current;
        FString Result = FString::Printf(TEXT("%s is '%s'"), *Current.Field, *Loss.CurrentValue);
        if (!Loss.DesiredValue.IsEmpty())
        {
            Result += FString::Printf(TEXT(" but the source requests '%s'"), *Loss.DesiredValue);
        }
        else
        {
            Result += TEXT(" but the source does not declare it");
        }
        if (!Current.Origin.IsEmpty())
        {
            Result += FString::Printf(TEXT("; %s"), *Current.Origin);
        }
        return Result;
    }
}

bool PwSourceRecompileGuard::Check(const FPwSourceRecompileGuardRequest& Request,
                                   TArray<FPwDiagnostic>& OutDiagnostics)
{
    if (!Request.bSameSourceRecompile && !Request.bTakeover)
    {
        return true;
    }

    const TMap<FString, const FPwSourceStateEntry*> Current =
        PwSourceRecompileGuard_Index(Request.Current);
    const TMap<FString, const FPwSourceStateEntry*> Desired =
        PwSourceRecompileGuard_Index(Request.Desired);
    const bool bHasBaseline = Request.BaselineVersion == StateVersion && Request.Baseline;

    TArray<FPwSourceLoss> Losses;
    for (const TPair<FString, const FPwSourceStateEntry*>& Pair : Current)
    {
        const FString& Key = Pair.Key;
        const FPwSourceStateEntry& CurrentEntry = *Pair.Value;
        const FPwSourceStateEntry* const* DesiredEntry = Desired.Find(Key);
        const FString DesiredValue = DesiredEntry ? (*DesiredEntry)->Value : FString();

        // Compare - and report - one spelling per side. Without this an asset-valued field is a
        // permanent false positive: the live object path never string-equals the package path a
        // source can write, so every asset an out-of-band verb has ever touched stays stuck in
        // the "needs overwrite" state and the signal stops meaning anything.
        const FString CurrentComparable =
            PwSourceRecompileGuard_Canonical(CurrentEntry, CurrentEntry.Value);
        const FString DesiredComparable =
            PwSourceRecompileGuard_Canonical(CurrentEntry, DesiredValue);

        if (CurrentComparable == DesiredComparable)
        {
            // An out-of-band edit that has since been written into the source is source-owned
            // now. This is the accepting half of the three-way rule.
            continue;
        }

        if (Request.bSameSourceRecompile && bHasBaseline)
        {
            const FString* BaselineValue = Request.Baseline->Find(Key);
            const FString PreviousValue = BaselineValue ? *BaselineValue : FString();
            if (CurrentComparable == PwSourceRecompileGuard_Canonical(CurrentEntry, PreviousValue))
            {
                // The live asset still matches the last generated output. A different desired
                // value is an ordinary source edit, not an out-of-band mutation.
                continue;
            }
        }

        Losses.Add({&CurrentEntry, CurrentComparable, DesiredComparable});
    }

    if (Losses.Num() == 0)
    {
        return true;
    }

    TArray<FString> Descriptions;
    Descriptions.Reserve(Losses.Num());
    for (const FPwSourceLoss& Loss : Losses)
    {
        Descriptions.Add(PwSourceRecompileGuard_Describe(Loss));
    }

    const FString Permission = Request.bOverwrite
        ? TEXT("overwrite=true explicitly permits discarding this state; the rebuild will not carry it forward.")
        : TEXT("The asset was not changed. Put the state in the source, or pass overwrite=true to discard it deliberately.");
    FString BaselineNote;
    FString Action;
    if (Request.bTakeover)
    {
        Action = TEXT("Taking over");
        if (!Request.CurrentSourcePath.IsEmpty())
        {
            BaselineNote = FString::Printf(
                TEXT("The existing baseline belongs to source '%s', so source '%s' cannot use it as proof that this state is reproducible."),
                *Request.CurrentSourcePath, *Request.RequestedSourcePath);
        }
        else
        {
            BaselineNote = TEXT("The existing asset has no source baseline, so the incoming source cannot prove that this state is reproducible.");
        }
    }
    else
    {
        Action = TEXT("Recompiling");
        BaselineNote = bHasBaseline
            ? TEXT("It differs from the state recorded by the previous source compile.")
            : TEXT("This asset predates the state baseline, so the compiler fails closed when live state differs from the source.");
    }
    const FString Message = FString::Printf(
        TEXT("%s %s asset '%s' would discard state the source does not reproduce: %s. %s %s"),
        *Action, *Request.FormatName, *Request.AssetPath, *FString::Join(Descriptions, TEXT(" | ")),
        *BaselineNote, *Permission);

    FPwDiagnostic Diagnostic = Request.bOverwrite
        ? FPwDiagnostic::MakeWarning(PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE,
              Request.Line, Request.Column, Message)
        : FPwDiagnostic::MakeError(PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE,
              Request.Line, Request.Column, Message);
    Diagnostic.ScopeLabel = TEXT("asset");
    Diagnostic.ScopeName = Request.AssetPath;
    OutDiagnostics.Add(MoveTemp(Diagnostic));
    return Request.bOverwrite;
}

TMap<FString, FString> PwSourceRecompileGuard::MakeStateMap(
    TArrayView<const FPwSourceStateEntry> State)
{
    TMap<FString, FString> Result;
    for (const FPwSourceStateEntry& Entry : State)
    {
        Result.Add(Entry.Key, Entry.Value);
    }
    return Result;
}
