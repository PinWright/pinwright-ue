// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveConditionEval.h"

// File-local helpers. Names are Drive-prefixed so they cannot collide with other
// anonymous-namespace symbols when Unity merges translation units.
namespace
{
    // Integer comparison for the Count condition.
    bool DriveCompareInt(int32 A, EDriveCompareOp Op, int32 B)
    {
        switch (Op)
        {
        case EDriveCompareOp::Equal:          return A == B;
        case EDriveCompareOp::NotEqual:       return A != B;
        case EDriveCompareOp::Less:           return A < B;
        case EDriveCompareOp::LessOrEqual:    return A <= B;
        case EDriveCompareOp::Greater:        return A > B;
        case EDriveCompareOp::GreaterOrEqual: return A >= B;
        }
        return false;
    }

    // Human-readable symbol for a compare op, used in the Expected string.
    const TCHAR* DriveCompareOpSymbol(EDriveCompareOp Op)
    {
        switch (Op)
        {
        case EDriveCompareOp::Equal:          return TEXT("==");
        case EDriveCompareOp::NotEqual:       return TEXT("!=");
        case EDriveCompareOp::Less:           return TEXT("<");
        case EDriveCompareOp::LessOrEqual:    return TEXT("<=");
        case EDriveCompareOp::Greater:        return TEXT(">");
        case EDriveCompareOp::GreaterOrEqual: return TEXT(">=");
        }
        return TEXT("?");
    }

    // Screen-space rect of an element: top-left = AbsolutePosition,
    // bottom-right = AbsolutePosition + AbsoluteSize.
    FBox2D DriveRectFromElement(const FDriveElement& Element)
    {
        return FBox2D(Element.AbsolutePosition, Element.AbsolutePosition + Element.AbsoluteSize);
    }

    // Inclusive containment: every corner of Inner lies on or within Outer.
    bool DriveBoxContains(const FBox2D& Outer, const FBox2D& Inner)
    {
        return Inner.Min.X >= Outer.Min.X && Inner.Min.Y >= Outer.Min.Y &&
               Inner.Max.X <= Outer.Max.X && Inner.Max.Y <= Outer.Max.Y;
    }

    FString DriveFormatBox(const FBox2D& Box)
    {
        return FString::Printf(TEXT("[(%g,%g)..(%g,%g)]"),
            Box.Min.X, Box.Min.Y, Box.Max.X, Box.Max.Y);
    }

    // The string a text condition compares against, and the field name to report in Expected/Actual.
    struct FDriveTextComparand
    {
        FString Text;
        const TCHAR* FieldName;
    };

    // An editable widget's live typed value (Element.Value) is preferred when present so
    // text_equals/text_contains verify what the caller typed rather than the widget's
    // accessible/HINT text (which is what Label falls back to for an editable field). A
    // non-editable widget has an empty Value, so it keeps comparing against Label as before.
    FDriveTextComparand DriveTextComparand(const FDriveElement& Element)
    {
        if (!Element.Value.IsEmpty())
        {
            return { Element.Value, TEXT("value") };
        }
        return { Element.Label, TEXT("label") };
    }
}

bool FDriveConditionEval::ElementMatchesTarget(const FDriveElement& Element, const FString& Target)
{
    // Exact, case-sensitive Handle match takes precedence; otherwise a
    // case-insensitive Label match. Empty fields never match (so an empty
    // Target matches nothing).
    if (!Element.Handle.IsEmpty() && Element.Handle.Equals(Target, ESearchCase::CaseSensitive))
    {
        return true;
    }
    if (!Element.Label.IsEmpty() && Element.Label.Equals(Target, ESearchCase::IgnoreCase))
    {
        return true;
    }
    return false;
}

const FDriveElement* FDriveConditionEval::FindMatch(const TArray<FDriveElement>& Elements, const FString& Target)
{
    for (const FDriveElement& Element : Elements)
    {
        if (ElementMatchesTarget(Element, Target))
        {
            return &Element;
        }
    }
    return nullptr;
}

int32 FDriveConditionEval::CountMatches(const TArray<FDriveElement>& Elements, const FString& Target)
{
    int32 Count = 0;
    for (const FDriveElement& Element : Elements)
    {
        if (ElementMatchesTarget(Element, Target))
        {
            ++Count;
        }
    }
    return Count;
}

int32 FDriveConditionEval::SeverityRank(const FString& Severity)
{
    const FString Token = Severity.ToLower();
    if (Token == TEXT("trace"))   return 0;
    if (Token == TEXT("debug"))   return 1;
    if (Token == TEXT("info"))    return 2;
    if (Token == TEXT("warning")) return 3;
    if (Token == TEXT("error"))   return 4;
    if (Token == TEXT("fatal"))   return 5;
    return INDEX_NONE;
}

FDriveConditionResult FDriveConditionEval::Evaluate(
    const FDriveCondition& Condition,
    const TArray<FDriveElement>& Elements,
    const FDriveJournalDelta& Journal)
{
    FDriveConditionResult Result;

    switch (Condition.Type)
    {
    case EDriveConditionType::WidgetPresent:
    {
        const FDriveElement* Match = FindMatch(Elements, Condition.Target);
        Result.bMet = (Match != nullptr);
        Result.Expected = TEXT("present");
        Result.Actual = Result.bMet ? TEXT("present") : TEXT("absent");
        Result.Detail = Match
            ? FString::Printf(TEXT("matched element handle='%s' label='%s'"), *Match->Handle, *Match->Label)
            : FString::Printf(TEXT("no element matches target '%s'"), *Condition.Target);
        break;
    }

    case EDriveConditionType::WidgetAbsent:
    {
        const FDriveElement* Match = FindMatch(Elements, Condition.Target);
        Result.bMet = (Match == nullptr);
        Result.Expected = TEXT("absent");
        Result.Actual = Match ? TEXT("present") : TEXT("absent");
        Result.Detail = Match
            ? FString::Printf(TEXT("element handle='%s' label='%s' still matches target '%s'"), *Match->Handle, *Match->Label, *Condition.Target)
            : FString::Printf(TEXT("no element matches target '%s'"), *Condition.Target);
        break;
    }

    case EDriveConditionType::WidgetEnabled:
    {
        const FDriveElement* Match = FindMatch(Elements, Condition.Target);
        Result.Expected = TEXT("enabled");
        if (!Match)
        {
            Result.bMet = false;
            Result.Actual = TEXT("absent");
            Result.Detail = FString::Printf(TEXT("no element matches target '%s'"), *Condition.Target);
        }
        else
        {
            Result.bMet = Match->bEnabled;
            Result.Actual = Match->bEnabled ? TEXT("enabled") : TEXT("disabled");
            Result.Detail = FString::Printf(TEXT("element handle='%s' label='%s'"), *Match->Handle, *Match->Label);
        }
        break;
    }

    case EDriveConditionType::WidgetVisible:
    {
        const FDriveElement* Match = FindMatch(Elements, Condition.Target);
        Result.Expected = TEXT("visible");
        if (!Match)
        {
            Result.bMet = false;
            Result.Actual = TEXT("absent");
            Result.Detail = FString::Printf(TEXT("no element matches target '%s'"), *Condition.Target);
        }
        else
        {
            Result.bMet = Match->bVisible;
            Result.Actual = Match->bVisible ? TEXT("visible") : TEXT("hidden");
            Result.Detail = FString::Printf(TEXT("element handle='%s' label='%s'"), *Match->Handle, *Match->Label);
        }
        break;
    }

    case EDriveConditionType::TextEquals:
    {
        const FDriveElement* Match = FindMatch(Elements, Condition.Target);
        Result.Expected = FString::Printf(TEXT("label==\"%s\""), *Condition.ExpectedText);
        if (!Match)
        {
            Result.bMet = false;
            Result.Actual = TEXT("absent");
            Result.Detail = FString::Printf(TEXT("no element matches target '%s'"), *Condition.Target);
        }
        else
        {
            const FDriveTextComparand C = DriveTextComparand(*Match);
            Result.bMet = C.Text.Equals(Condition.ExpectedText, ESearchCase::CaseSensitive);
            // Report Expected against the same field as Actual (value= for an editable widget with
            // a typed value, else label=) so the two halves of a failure never name different fields.
            Result.Expected = FString::Printf(TEXT("%s==\"%s\""), C.FieldName, *Condition.ExpectedText);
            Result.Actual = FString::Printf(TEXT("%s=\"%s\""), C.FieldName, *C.Text);
            Result.Detail = FString::Printf(TEXT("element handle='%s'"), *Match->Handle);
        }
        break;
    }

    case EDriveConditionType::TextContains:
    {
        const FDriveElement* Match = FindMatch(Elements, Condition.Target);
        Result.Expected = FString::Printf(TEXT("label contains \"%s\""), *Condition.ExpectedText);
        if (!Match)
        {
            Result.bMet = false;
            Result.Actual = TEXT("absent");
            Result.Detail = FString::Printf(TEXT("no element matches target '%s'"), *Condition.Target);
        }
        else
        {
            const FDriveTextComparand C = DriveTextComparand(*Match);
            Result.bMet = C.Text.Contains(Condition.ExpectedText, ESearchCase::CaseSensitive);
            // Report Expected against the same field as Actual (value= for an editable widget with
            // a typed value, else label=) so the two halves of a failure never name different fields.
            Result.Expected = FString::Printf(TEXT("%s contains \"%s\""), C.FieldName, *Condition.ExpectedText);
            Result.Actual = FString::Printf(TEXT("%s=\"%s\""), C.FieldName, *C.Text);
            Result.Detail = FString::Printf(TEXT("element handle='%s'"), *Match->Handle);
        }
        break;
    }

    case EDriveConditionType::Count:
    {
        const int32 N = CountMatches(Elements, Condition.Target);
        Result.bMet = DriveCompareInt(N, Condition.CountOp, Condition.ExpectedCount);
        Result.Actual = FString::Printf(TEXT("count=%d"), N);
        Result.Expected = FString::Printf(TEXT("%s %d"), DriveCompareOpSymbol(Condition.CountOp), Condition.ExpectedCount);
        Result.Detail = FString::Printf(TEXT("%d element(s) match target '%s'"), N, *Condition.Target);
        break;
    }

    case EDriveConditionType::GeometryInBounds:
    {
        const FDriveElement* Match = FindMatch(Elements, Condition.Target);
        Result.Expected = Condition.ExpectedBounds.bIsValid
            ? FString::Printf(TEXT("within %s"), *DriveFormatBox(Condition.ExpectedBounds))
            : TEXT("within <unset bounds>");
        if (!Match)
        {
            Result.bMet = false;
            Result.Actual = TEXT("absent");
            Result.Detail = FString::Printf(TEXT("no element matches target '%s'"), *Condition.Target);
        }
        else if (Match->bGeometryStale)
        {
            // A stale element carries a zeroed rect, and a (0,0)-(0,0) box is CONTAINED by any
            // expected_bounds whose min is at or below the origin - so comparing it would report
            // met:true for a widget that is not on screen at all. There is no measurement to
            // compare, which is a not-met, not a pass.
            Result.bMet = false;
            Result.Actual = TEXT("stale");
            Result.Detail = FString::Printf(
                TEXT("element handle='%s' label='%s' is not being arranged (it or an ancestor is ")
                TEXT("collapsed/hidden), so it has no current geometry to compare"),
                *Match->Handle, *Match->Label);
        }
        else if (!Condition.ExpectedBounds.bIsValid)
        {
            const FBox2D Rect = DriveRectFromElement(*Match);
            Result.bMet = false;
            Result.Actual = DriveFormatBox(Rect);
            Result.Detail = TEXT("expected_bounds is not set");
        }
        else
        {
            const FBox2D Rect = DriveRectFromElement(*Match);
            Result.bMet = DriveBoxContains(Condition.ExpectedBounds, Rect);
            Result.Actual = DriveFormatBox(Rect);
            Result.Detail = FString::Printf(TEXT("element handle='%s' label='%s'"), *Match->Handle, *Match->Label);
        }
        break;
    }

    case EDriveConditionType::JournalEvent:
    {
        const FDriveJournalEvent* Found = nullptr;
        for (const FDriveJournalEvent& Event : Journal.Events)
        {
            if (Event.Name == Condition.Target)
            {
                Found = &Event;
                break;
            }
        }
        Result.bMet = (Found != nullptr);
        Result.Expected = FString::Printf(TEXT("event \"%s\" present"), *Condition.Target);
        Result.Actual = Result.bMet ? TEXT("present") : TEXT("absent");
        Result.Detail = Found
            ? FString::Printf(TEXT("event id='%s' severity='%s'"), *Found->Id, *Found->Severity)
            : FString::Printf(TEXT("no journal event named '%s' in delta (%d event(s))"), *Condition.Target, Journal.Events.Num());
        break;
    }

    case EDriveConditionType::JournalSeverity:
    {
        const int32 ThresholdRank = SeverityRank(Condition.SeverityThreshold);
        Result.Expected = FString::Printf(TEXT(">= %s"), *Condition.SeverityThreshold.ToLower());
        if (ThresholdRank == INDEX_NONE)
        {
            Result.bMet = false;
            Result.Actual = TEXT("n/a");
            Result.Detail = FString::Printf(TEXT("unrecognized severity threshold '%s'"), *Condition.SeverityThreshold);
            break;
        }

        int32 MaxRank = INDEX_NONE;
        FString MaxSeverity;
        for (const FDriveJournalEvent& Event : Journal.Events)
        {
            const int32 Rank = SeverityRank(Event.Severity);
            if (Rank > MaxRank)
            {
                MaxRank = Rank;
                MaxSeverity = Event.Severity;
            }
        }
        Result.bMet = (MaxRank != INDEX_NONE) && (MaxRank >= ThresholdRank);
        Result.Actual = (MaxRank == INDEX_NONE)
            ? FString(TEXT("max=<none>"))
            : FString::Printf(TEXT("max=%s"), *MaxSeverity.ToLower());
        Result.Detail = FString::Printf(TEXT("%d event(s) in delta"), Journal.Events.Num());
        break;
    }
    }

    return Result;
}
