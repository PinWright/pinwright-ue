// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/ActorLabelOverlay.h"

#include "Math/UnrealMathUtility.h"

namespace PinWrightActorLabels
{

bool IsBehindCamera(const FVector& CameraLocation, const FRotator& EffectiveRotation,
    const FVector& WorldPoint)
{
    const FVector Forward = EffectiveRotation.Vector();
    // A degenerate forward vector cannot classify anything; treat the point as in front and let
    // the projector's own behind-camera flag decide. FRotator::Vector() is unit length for every
    // finite rotator, so this is defence against NaN input only.
    if (Forward.IsNearlyZero())
    {
        return false;
    }
    // <= 0 rejects the camera plane itself: a point exactly on it projects to a division by zero.
    return FVector::DotProduct(WorldPoint - CameraLocation, Forward) <= 0.0;
}

bool MatchesNameFilter(const FString& ActorName, const FString& ActorLabel, const FString& Filter)
{
    if (Filter.IsEmpty())
    {
        return true;
    }
    if (Filter.Contains(TEXT("*")) || Filter.Contains(TEXT("?")))
    {
        return ActorName.MatchesWildcard(Filter, ESearchCase::IgnoreCase)
            || ActorLabel.MatchesWildcard(Filter, ESearchCase::IgnoreCase);
    }
    return ActorName.Contains(Filter, ESearchCase::IgnoreCase)
        || ActorLabel.Contains(Filter, ESearchCase::IgnoreCase);
}

FActorLabelLayout BuildLayout(const TArray<FActorLabelCandidate>& Candidates,
    const FActorLabelOptions& Options)
{
    FActorLabelLayout Result;
    Result.Stats.Scanned = Candidates.Num();

    // maxLabels 0 means "no cap of my own", following actor.list's limit convention; the hard
    // ceiling still applies and still reports through truncated, so the answer is never silently
    // unbounded.
    Result.Stats.ResolvedCap = (Options.MaxLabels <= 0)
        ? MaxAllowedLabels
        : FMath::Min(Options.MaxLabels, MaxAllowedLabels);

    // --- Eligibility. Ordering happens AFTER filtering so the cap keeps the most visually
    // prominent survivors, never whichever ones TActorIterator happened to yield first.
    TArray<int32> Eligible;
    Eligible.Reserve(Candidates.Num());
    for (int32 Index = 0; Index < Candidates.Num(); ++Index)
    {
        const FActorLabelCandidate& Candidate = Candidates[Index];

        if (Candidate.bBehindCamera)
        {
            ++Result.Stats.BehindCamera;
            continue;
        }
        // No positive-area overlap between the actor's projected bounds and the frame. This is the
        // inclusion rule, so an actor whose CENTROID is off-screen but whose BOUNDS still cross the
        // frame survives here and is reported with onScreen:false.
        if (!Candidate.ClampedRect.bIsValid)
        {
            ++Result.Stats.OffScreen;
            continue;
        }
        if (Candidate.ScreenArea < Options.MinScreenAreaPx)
        {
            ++Result.Stats.BelowMinScreenArea;
            continue;
        }
        Eligible.Add(Index);
    }
    Result.Stats.TotalMatches = Eligible.Num();

    // --- Deterministic order: biggest visible footprint first, then nearest, then name.
    // TArray::Sort is introsort (NOT stable) and TActorIterator's order is not guaranteed stable
    // across editor sessions, so the name tie-break is what makes two runs over the same scene
    // produce byte-identical output. Compare case-sensitively for a total order.
    Eligible.Sort([&Candidates](int32 A, int32 B)
    {
        const FActorLabelCandidate& CA = Candidates[A];
        const FActorLabelCandidate& CB = Candidates[B];
        if (!FMath::IsNearlyEqual(CA.ScreenArea, CB.ScreenArea))
        {
            return CA.ScreenArea > CB.ScreenArea;
        }
        if (!FMath::IsNearlyEqual(CA.Distance, CB.Distance))
        {
            return CA.Distance < CB.Distance;
        }
        return CA.Name.Compare(CB.Name, ESearchCase::CaseSensitive) < 0;
    });

    // --- Cap + placement.
    const int64 MinSpacingSq = static_cast<int64>(FMath::Max(Options.MinLabelSpacingPx, 0))
        * FMath::Max(Options.MinLabelSpacingPx, 0);

    Result.Placements.Reserve(FMath::Min(Eligible.Num(), Result.Stats.ResolvedCap));
    for (int32 Rank = 0; Rank < Eligible.Num(); ++Rank)
    {
        if (Rank >= Result.Stats.ResolvedCap)
        {
            // Over the cap: counted, not silently dropped (FDriveSetOfMarkLayout's rule).
            ++Result.Stats.CapDropped;
            continue;
        }

        const int32 Index = Eligible[Rank];
        const FActorLabelCandidate& Candidate = Candidates[Index];

        FActorLabelPlacement Placement;
        Placement.CandidateIndex = Index;
        Placement.AnchorX = FMath::RoundToInt(Candidate.ClampedRect.Min.X);
        Placement.AnchorY = FMath::RoundToInt(Candidate.ClampedRect.Min.Y);

        if (Options.bDraw)
        {
            // Cheap de-overlap: do not PAINT a label whose anchor lands within MinLabelSpacingPx of
            // one already painted. This is ANCHOR-PROXIMITY rejection, not text-extent de-overlap -
            // a long name can still overrun a neighbour horizontally, and it does not see the axis /
            // grid / AABB labels the same call may paint. Real de-overlap (measuring each string's
            // width and searching for a free slot) is not worth the cost here, and the honest
            // mitigation is that the ROW survives either way with drawn:false, so the
            // machine-readable map is complete even where the picture is crowded.
            //
            // O(n^2) over at most MaxAllowedLabels entries (250k integer comparisons worst case).
            bool bTooClose = false;
            for (const FActorLabelPlacement& Placed : Result.Placements)
            {
                if (!Placed.bDrawn)
                {
                    continue;
                }
                const int64 DX = static_cast<int64>(Placement.AnchorX) - Placed.AnchorX;
                const int64 DY = static_cast<int64>(Placement.AnchorY) - Placed.AnchorY;
                if (DX * DX + DY * DY < MinSpacingSq)
                {
                    bTooClose = true;
                    break;
                }
            }
            if (bTooClose)
            {
                ++Result.Stats.OverlapSkipped;
            }
            else
            {
                Placement.bDrawn = true;
                ++Result.Stats.Drawn;
            }
        }

        Result.Placements.Add(Placement);
    }

    return Result;
}

}
