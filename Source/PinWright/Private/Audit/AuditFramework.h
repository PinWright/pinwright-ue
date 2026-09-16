// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"

// The shared audit contract: the one definition of what an audit verb's `pass` means, of the
// finding statuses it reports, and of the check-descriptor table it selects checks from.
//
// WHY THIS IS ONE FILE AND NOT FOUR COPIES
//
// level.audit, geometry.audit_static_meshes, landscape.audit_shape and
// skeleton.audit_skin_weights each independently grew the same rule:
//
//     pass = no finding at or above failOn AND zero unrunnable AND not truncated
//
// That rule is the one thing standing between "we failed to measure it" and "it passed" -
// the defect class this project keeps paying for (an ortho sweep that exited 0 having examined
// nothing; a suite that reported green after running 2012 of 3658 tests; a skin audit that
// answered profileCount:0 with a success status having read no skinning at all). Written four
// times it drifts, and the copy that drifts is the copy that starts calling unmeasured a pass.
// So it is written once, here, and every audit derives its verdict through it.
//
// HEADER-ONLY, ON PURPOSE. PinWrightGeometry is a separate module from PinWright; it already
// adds Source/PinWright/Private to its PrivateIncludePaths (PinWrightGeometry.Build.cs) so
// moved files keep their `#include "Handlers/..."` shape, which makes `#include
// "Audit/AuditFramework.h"` resolve from both modules with no new link edge and no *_API
// export macro. Everything below is inline, constexpr or a template for that reason. Nothing
// here touches a UObject, a world, JSON or the engine, so it is unit-testable on bare values.
//
// The interface rules this encodes are docs/rpc-design.md §18; its own rule net is
// PinWright.infra.audit_contract.* in Tests/Infra/TestAuditContract.cpp.
namespace PinWrightAudit
{
    // ---- Severity ---------------------------------------------------------------------------

    // What a finding costs under the default failOn. Error is reserved for conditions that
    // cannot be deliberate; everything a human might have meant is a Warning.
    enum class ESeverity : uint8 { Warning, Error };

    // Wire spelling. One definition so three verbs cannot spell the same concept differently.
    inline const TCHAR* SeverityToWire(ESeverity Severity)
    {
        return Severity == ESeverity::Error ? TEXT("error") : TEXT("warning");
    }

    // ---- Finding status ---------------------------------------------------------------------

    enum class EFindingStatus : uint8
    {
        // The check ran and the subject failed it.
        Flagged,
        // The check could NOT run for this subject. Never folded into "clean": that fold is
        // the entire defect this enum exists to make impossible.
        Unrunnable
    };

    inline const TCHAR* StatusToWire(EFindingStatus Status)
    {
        return Status == EFindingStatus::Unrunnable ? TEXT("unrunnable") : TEXT("flagged");
    }

    // ---- failOn -----------------------------------------------------------------------------

    // Which severities the caller wants to fail on. It moves ONLY the severity bar; it can
    // never reach the unrunnable or truncation terms of the verdict - see FVerdict::DerivePass.
    enum class EFailOn : uint8 { Error, Any, None };

    // Case-insensitive, on the exact vocabulary every audit already publishes. False for
    // anything else, which callers must turn into an INVALID_ARGUMENT rather than a default:
    // a misspelled failOn silently meaning "error" would be a bar nobody set.
    inline bool ParseFailOn(const FString& Token, EFailOn& OutFailOn)
    {
        if (Token.Equals(TEXT("error"), ESearchCase::IgnoreCase)) { OutFailOn = EFailOn::Error; return true; }
        if (Token.Equals(TEXT("any"), ESearchCase::IgnoreCase))   { OutFailOn = EFailOn::Any;   return true; }
        if (Token.Equals(TEXT("none"), ESearchCase::IgnoreCase))  { OutFailOn = EFailOn::None;  return true; }
        return false;
    }

    inline const TCHAR* FailOnToWire(EFailOn FailOn)
    {
        switch (FailOn)
        {
        case EFailOn::Any:  return TEXT("any");
        case EFailOn::None: return TEXT("none");
        case EFailOn::Error:
        default:            return TEXT("error");
        }
    }

    // ---- The verdict ------------------------------------------------------------------------

    // The four numbers every audit's `pass` is derived from, and nothing else. Deliberately a
    // plain aggregate with no engine types: the rule below is the thing under test, so it must
    // be drivable from a test that owns no world, no asset and no mesh.
    struct FVerdict
    {
        // Findings at severity Error. Unrunnable rows are NOT counted here - they are their
        // own term, because a check that did not run has no severity.
        int32 ErrorCount = 0;
        int32 WarningCount = 0;
        // Checks that could not be evaluated. The term failOn cannot reach.
        int32 UnrunnableCount = 0;
        // The sweep did not cover the set it was asked about (a page short of the match set,
        // a findings array clipped, a budget spent). Also unreachable by failOn.
        bool bTruncated = false;

        // The only term failOn moves.
        bool SeverityClean(EFailOn FailOn) const
        {
            switch (FailOn)
            {
            case EFailOn::None: return true;
            case EFailOn::Any:  return ErrorCount == 0 && WarningCount == 0;
            case EFailOn::Error:
            default:            return ErrorCount == 0;
            }
        }

        // THE RULE. Deliberately strict, and the two terms after the severity test do not
        // consult failOn at all:
        //   - an UNRUNNABLE check did not run, and a check that did not run is not a check
        //     that passed. failOn:"none" says "no finding should fail me"; it does not and
        //     must not say "measure nothing and call it clean".
        //   - a TRUNCATED sweep audited part of the set. Half a folder reported clean as
        //     "the folder is clean" is how check_actors once exited 0 having examined nothing.
        // An audit with no truncation concept simply leaves bTruncated false; there is no
        // second rule for it.
        bool DerivePass(EFailOn FailOn) const
        {
            return SeverityClean(FailOn) && UnrunnableCount == 0 && !bTruncated;
        }

        // For call sites that still hold the caller's raw string. An UNPARSEABLE token is
        // treated as the strictest bar rather than the loosest: a verb that reached here with
        // a bad token has a validation gap, and failing closed keeps that gap from reading as
        // a pass. Handlers validate with ParseFailOn first and never rely on this.
        bool DerivePass(const FString& FailOn) const
        {
            EFailOn Parsed = EFailOn::Any;
            ParseFailOn(FailOn, Parsed);
            return DerivePass(Parsed);
        }
    };

    // ---- The pass-rule sentence --------------------------------------------------------------

    // The rule, in the words the responses publish, built from one source so a reader comparing
    // two audits' `passRule` fields is comparing the same sentence.
    //
    // bIncludeTruncation adds the third term; omit it for an audit that reads its whole subject
    // in one pass and therefore has no truncation to report (landscape.audit_shape refuses an
    // oversized region up front rather than measuring part of it).
    // Elaboration, when given, is appended verbatim as a following sentence.
    inline FString PassRuleText(bool bIncludeTruncation, const TCHAR* Elaboration = nullptr)
    {
        FString Rule = TEXT("pass = no finding at or above failOn, AND zero unrunnable checks");
        if (bIncludeTruncation)
        {
            Rule += TEXT(", AND the sweep was not truncated");
        }
        Rule += TEXT(".");
        if (Elaboration && *Elaboration)
        {
            Rule += TEXT(" ");
            Rule += Elaboration;
        }
        return Rule;
    }

    // ---- Check descriptor tables --------------------------------------------------------------
    //
    // Each audit keeps its OWN descriptor struct, because the extra columns are genuinely
    // per-audit (level.audit's bNeedsSurface / bNeedsPlayArea, geometry's bNeedsClosed) and
    // flattening them into one struct would put fields on tables that cannot use them. What is
    // shared is the SHAPE every descriptor already has - `.Check`, `.Id`, `.bDefaultOn` - and
    // the operations over it. The helpers below are templates on that shape, so a table adds a
    // column without touching this file and a new audit gets ParseCheckId's semantics for free.

    // The selection bitmask every audit uses. Kept a uint32 by static_assert at each table.
    template <typename TCheck>
    constexpr uint32 CheckBit(TCheck Check) { return 1u << static_cast<uint32>(Check); }

    template <typename TCheck>
    constexpr bool HasCheck(uint32 Mask, TCheck Check) { return (Mask & CheckBit(Check)) != 0; }

    // Wire id -> check. Trims surrounding whitespace, compares case-insensitively.
    //
    // The RETURN VALUE is the contract: false means the id is unknown, and every caller must
    // turn that into an error rather than skipping the entry. A typo in `checks` that silently
    // ran nothing is indistinguishable from a subject that passed every check, which is the
    // same false green the verdict above exists to prevent - one argument earlier.
    template <typename TInfo>
    bool ParseCheckId(const TArray<TInfo>& Table, const FString& Id, decltype(TInfo::Check)& OutCheck)
    {
        const FString Trimmed = Id.TrimStartAndEnd();
        for (const TInfo& Info : Table)
        {
            if (Trimmed.Equals(Info.Id, ESearchCase::IgnoreCase))
            {
                OutCheck = Info.Check;
                return true;
            }
        }
        return false;
    }

    // The table row for a check. Clamped rather than checked so a corrupt enum value cannot
    // read out of bounds; the table/enum sync assert at each AllChecks() is what catches the
    // real drift.
    template <typename TInfo>
    const TInfo& CheckInfo(const TArray<TInfo>& Table, decltype(TInfo::Check) Check)
    {
        const int32 Index = FMath::Clamp(static_cast<int32>(Check), 0, Table.Num() - 1);
        return Table[Index];
    }

    // Mask of every check the predicate accepts. The named masks below are the two every audit
    // wants; a per-audit column (bNeedsSurface, bNeedsClosed) uses this directly.
    template <typename TInfo, typename TPredicate>
    uint32 MaskWhere(const TArray<TInfo>& Table, TPredicate Predicate)
    {
        uint32 Mask = 0;
        for (const TInfo& Info : Table)
        {
            if (Predicate(Info)) { Mask |= CheckBit(Info.Check); }
        }
        return Mask;
    }

    template <typename TInfo>
    uint32 DefaultCheckMask(const TArray<TInfo>& Table)
    {
        return MaskWhere(Table, [](const TInfo& Info) { return Info.bDefaultOn; });
    }

    template <typename TInfo>
    uint32 AllCheckMask(const TArray<TInfo>& Table)
    {
        return MaskWhere(Table, [](const TInfo&) { return true; });
    }

    // "a, b, c" - the valid-id list an unknown-id rejection names. Derived from the table so a
    // check added to the table cannot go unmentioned in the error that rejects its typo.
    template <typename TInfo>
    FString ValidCheckIdList(const TArray<TInfo>& Table)
    {
        FString Valid;
        for (const TInfo& Info : Table)
        {
            if (!Valid.IsEmpty()) { Valid += TEXT(", "); }
            Valid += Info.Id;
        }
        return Valid;
    }
}
