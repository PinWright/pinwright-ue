// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveFingerprint.h"

namespace
{
    // Whole-pixel rect of an element. Rounding to the nearest integer kills sub-pixel
    // jitter so a few-thousandths-of-a-pixel layout wobble does not read as a change.
    struct FRoundedRect
    {
        int64 X = 0;
        int64 Y = 0;
        int64 W = 0;
        int64 H = 0;

        bool operator==(const FRoundedRect& Other) const
        {
            return X == Other.X && Y == Other.Y && W == Other.W && H == Other.H;
        }
    };

    FRoundedRect RoundRect(const FDriveElement& Element)
    {
        FRoundedRect Rect;
        Rect.X = static_cast<int64>(FMath::RoundToInt(Element.AbsolutePosition.X));
        Rect.Y = static_cast<int64>(FMath::RoundToInt(Element.AbsolutePosition.Y));
        Rect.W = static_cast<int64>(FMath::RoundToInt(Element.AbsoluteSize.X));
        Rect.H = static_cast<int64>(FMath::RoundToInt(Element.AbsoluteSize.Y));
        return Rect;
    }

    // 64-bit FNV-1a over a string's code units. Deterministic for a given input within a
    // run, which is all the fingerprint needs (previous vs current are compared on the same
    // machine). Casting through uint32 avoids sign-extension differences on signed TCHAR.
    uint64 Fnv1a64(const FString& Text)
    {
        uint64 Hash = 0xcbf29ce484222325ULL;
        const uint64 Prime = 0x100000001b3ULL;
        for (const TCHAR Ch : Text)
        {
            Hash ^= static_cast<uint64>(static_cast<uint32>(Ch));
            Hash *= Prime;
        }
        return Hash;
    }

    // Stable ordering used to normalize the visible set before hashing: by Type, then by the
    // rounded rect. Reordering the input array alone therefore cannot change the fingerprint.
    bool SignatureLess(const FDriveElement& A, const FDriveElement& B)
    {
        if (A.Type != B.Type)
        {
            return A.Type < B.Type;
        }
        const FRoundedRect RA = RoundRect(A);
        const FRoundedRect RB = RoundRect(B);
        if (RA.X != RB.X) { return RA.X < RB.X; }
        if (RA.Y != RB.Y) { return RA.Y < RB.Y; }
        if (RA.W != RB.W) { return RA.W < RB.W; }
        return RA.H < RB.H;
    }

    // Two elements match when their Type, visibility, whole-pixel rect, AND live editable
    // Value are equal. Used by Diff - the one-shot before->after response delta - to decide
    // "changed", so a value-only text edit (drive.type into a fixed-geometry field) is
    // reported rather than lost. Value is compared case-SENSITIVELY (FString's operator== is
    // case-insensitive) so a case-only edit - e.g. "aria" -> "Aria" - is still a real change,
    // matching the case-sensitive text_equals/text_contains conditions. Value is deliberately
    // NOT part of the per-tick settle fingerprint (Compute; see the header): the settle loop
    // must stay churn-tolerant, and only this one-shot diff is value-aware.
    bool SignatureEquals(const FDriveElement& A, const FDriveElement& B)
    {
        return A.Type == B.Type && A.bVisible == B.bVisible
            && A.Value.Equals(B.Value, ESearchCase::CaseSensitive) && RoundRect(A) == RoundRect(B);
    }
}

FDriveFingerprint FDriveChangeDetector::Compute(const TArray<FDriveElement>& Elements)
{
    // Gather only the visible elements; they are the ones that define the surface's shape.
    TArray<const FDriveElement*> Visible;
    Visible.Reserve(Elements.Num());
    for (const FDriveElement& Element : Elements)
    {
        if (Element.bVisible)
        {
            Visible.Add(&Element);
        }
    }

    // Normalize order so a pure reorder of the input does not change the fingerprint.
    Visible.Sort([](const FDriveElement& A, const FDriveElement& B)
    {
        return SignatureLess(A, B);
    });

    // Build a canonical digest: a count header plus one line per element with explicit field
    // delimiters so distinct (type, rect) tuples cannot alias into the same byte sequence.
    FString Digest = FString::Printf(TEXT("n=%d\n"), Visible.Num());
    for (const FDriveElement* Element : Visible)
    {
        const FRoundedRect Rect = RoundRect(*Element);
        Digest += FString::Printf(TEXT("%s|%lld|%lld|%lld|%lld\n"),
            *Element->Type, Rect.X, Rect.Y, Rect.W, Rect.H);
    }

    FDriveFingerprint Fingerprint;
    Fingerprint.VisibleCount = Visible.Num();
    Fingerprint.Hash = Fnv1a64(Digest);
    return Fingerprint;
}

bool FDriveChangeDetector::Equals(const FDriveFingerprint& A, const FDriveFingerprint& B)
{
    return A == B;
}

FDriveDiff FDriveChangeDetector::Diff(const TArray<FDriveElement>& Previous, const TArray<FDriveElement>& Current)
{
    // Index both sets by Handle. On a duplicate handle the last occurrence wins (handles are
    // assumed unique within an observation).
    TMap<FString, const FDriveElement*> PrevByHandle;
    PrevByHandle.Reserve(Previous.Num());
    for (const FDriveElement& Element : Previous)
    {
        PrevByHandle.Add(Element.Handle, &Element);
    }

    TMap<FString, const FDriveElement*> CurByHandle;
    CurByHandle.Reserve(Current.Num());
    for (const FDriveElement& Element : Current)
    {
        CurByHandle.Add(Element.Handle, &Element);
    }

    FDriveDiff Result;

    for (const TPair<FString, const FDriveElement*>& Pair : CurByHandle)
    {
        if (const FDriveElement* const* PrevElement = PrevByHandle.Find(Pair.Key))
        {
            if (!SignatureEquals(**PrevElement, *Pair.Value))
            {
                Result.Changed.Add(Pair.Key);
            }
        }
        else
        {
            Result.Appeared.Add(Pair.Key);
        }
    }

    for (const TPair<FString, const FDriveElement*>& Pair : PrevByHandle)
    {
        if (!CurByHandle.Contains(Pair.Key))
        {
            Result.Disappeared.Add(Pair.Key);
        }
    }

    // Sort for deterministic output independent of map iteration / input order.
    Result.Appeared.Sort();
    Result.Disappeared.Sort();
    Result.Changed.Sort();
    return Result;
}
