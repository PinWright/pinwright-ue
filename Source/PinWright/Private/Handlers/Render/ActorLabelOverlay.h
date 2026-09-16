// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"

// Pure selection + placement math for render.capture_annotated's actor-label overlay: the
// "label everything visible so an agent can DISCOVER actors it did not know about" mode.
//
// Deliberately free of UObject, Slate, viewport and drawing dependencies - it consumes candidates
// the handler has already projected and returns which of them earn a row and a painted label.
// That is the same split FDriveSetOfMarkLayout uses for the Set-of-Mark UI overlay
// (Handlers/Drive/DriveSetOfMarkLayout.h), and for the same reason: the interesting behaviour
// (ordering, capping, thresholding, de-overlap, behind-camera rejection) is then unit-testable on
// synthetic input with no editor, no world and no RHI. Deterministic: identical input always
// yields identical output.
namespace PinWrightActorLabels
{
    // Default cap on how many actors get a row and a label. Matches
    // FDriveSetOfMarkRenderer::DefaultMarkCap, the repo's existing answer to the same question.
    // NOTE for callers: ~50 rows is also roughly where the serialized actorLabels block starts
    // pressing the 10,000-char inline response budget and spills to Saved/PinWright/HttpResponses/.
    constexpr int32 DefaultMaxLabels = 50;

    // Hard ceiling regardless of what the caller asks for (and the meaning of maxLabels:0, which
    // follows actor.list's "0 = no caller cap" convention). Bounds both the response size and the
    // O(n^2) de-overlap scan below.
    constexpr int32 MaxAllowedLabels = 500;

    // Default minimum projected footprint, in square pixels, for an actor to be worth labelling.
    // 256 px^2 is a 16x16 block: below that the 6x10-glyph label is larger than the thing it names,
    // which is exactly the distant-clutter case this threshold exists to drop. Set 0 to keep
    // zero-footprint actors (lights, empty actors) whose bounds project to a degenerate rect.
    constexpr double DefaultMinScreenAreaPx = 256.0;

    // Default minimum pixel distance between two PAINTED label anchors. The label line height is
    // GGlyphH * GLabelScale + 2 * GLabelScale = 14 px in AnnotatedCaptureHandler.cpp, so 24 px
    // leaves headroom for two stacked labels.
    constexpr int32 DefaultMinLabelSpacingPx = 24;

    // Ceiling on how many actors are projected in one call, independent of maxLabels. Projection
    // is 9 matrix transforms per candidate against one already-built view, so this is cheap - the
    // ceiling exists so a pathological world (a million-actor stress level) degrades into a
    // reported partial answer instead of an unbounded game-thread stall.
    constexpr int32 MaxProjectedCandidates = 20000;

    // Caller-supplied options, parsed from the `actorLabels` argument object.
    struct FActorLabelOptions
    {
        // World Outliner folder PREFIX, case-insensitive: "Blockout/Towers" also matches
        // "Blockout/Towers/North". Empty = no folder filter.
        FString Folder;
        // Exact FName tag equality, matching actor.find_by_tag's default matchType. Empty = none.
        FString Tag;
        // Short class name or full script/asset path; subclasses included. Resolved by the handler
        // through ResolveUClass BEFORE any capture work, so an unresolvable class is a typed
        // CLASS_NOT_FOUND rather than a silently empty result. Empty = no class filter.
        FString ClassName;
        // Name/label filter: substring, or wildcard when it contains '*' or '?'. See
        // MatchesNameFilter. Empty = no name filter.
        FString Filter;

        double MinScreenAreaPx = DefaultMinScreenAreaPx;
        int32 MaxLabels = DefaultMaxLabels;
        int32 MinLabelSpacingPx = DefaultMinLabelSpacingPx;
        // Paint the names onto the image. False still returns the full machine-readable map - the
        // map is data, not paint, and is the more valuable half of the feature.
        bool bDraw = true;
    };

    // One projected actor. The handler fills this from the shared projection session; every field
    // is already in screen space so BuildLayout needs no view.
    struct FActorLabelCandidate
    {
        // Unique internal object name (the object-path leaf, e.g. StaticMeshActor_17). This is the
        // collision-safe key to hand to actor.select / spatial.* - display labels are NOT unique
        // (see ACTORNAME_COLLISION_STEER in Handlers/ParamSpec.h).
        FString Name;
        FString Label;
        FString ClassName;
        FString Folder;

        // Bounds-centre pixel, TOP-LEFT origin. May fall outside the frame; see bCentroidOnScreen.
        double CentroidPixelX = 0.0;
        double CentroidPixelY = 0.0;

        // Screen-space AABB of the actor's 8 projected bounds corners, CLAMPED to the frame. Invalid
        // when the actor's bounds do not overlap the frame at all.
        FBox2D ClampedRect = FBox2D(ForceInit);
        // Area of ClampedRect. Ranking uses the CLAMPED area on purpose: a huge slab that is 95%
        // off-frame must not outrank a small prop that is fully visible.
        double ScreenArea = 0.0;
        // Camera to bounds-centre distance in cm; the ordering tie-break and a useful response field.
        double Distance = 0.0;

        // See IsBehindCamera. A true here is an unconditional rejection.
        bool bBehindCamera = false;
        bool bCentroidOnScreen = false;
    };

    // Why the scanned candidates did not all become rows. The invariants are exact and are asserted
    // by PinWright.render.actor_labels.StatsInvariant:
    //   Scanned      == BehindCamera + OffScreen + BelowMinScreenArea + TotalMatches
    //   TotalMatches == Placements.Num() + CapDropped
    //   Placements.Num() == Drawn + OverlapSkipped   (when Options.bDraw; Drawn == 0 otherwise)
    struct FActorLabelStats
    {
        int32 Scanned = 0;
        int32 BehindCamera = 0;
        int32 OffScreen = 0;
        int32 BelowMinScreenArea = 0;
        int32 TotalMatches = 0;
        int32 CapDropped = 0;
        int32 OverlapSkipped = 0;
        int32 Drawn = 0;
        // The cap actually applied after clamping MaxLabels into [1, MaxAllowedLabels]; echoed to
        // the caller so a clamped or defaulted value is never invisible.
        int32 ResolvedCap = DefaultMaxLabels;
    };

    // One surviving candidate, in emit order.
    struct FActorLabelPlacement
    {
        // Index into the BuildLayout input array.
        int32 CandidateIndex = INDEX_NONE;
        // In-frame paint anchor: the clamped rect's top-left, so a label never lands off-image even
        // when the actor's centroid projected outside the frame (same rule as FDriveMark::LabelAnchor).
        int32 AnchorX = 0;
        int32 AnchorY = 0;
        // False when the label was suppressed by draw:false or by the de-overlap skip. The ROW is
        // emitted either way - the machine-readable map never loses an actor to a drawing decision.
        bool bDrawn = false;
    };

    struct FActorLabelLayout
    {
        TArray<FActorLabelPlacement> Placements;
        FActorLabelStats Stats;
    };

    // True when WorldPoint sits on or behind the camera plane.
    //
    // EffectiveRotation MUST be FViewportCaptureOutput::EffectiveRotation - the pose the PIXELS
    // show - and never the requested rotation. An orthographic capture renders with the engine's
    // fixed orientation for the resolved ELevelViewportType, so a requested top-down yaw of 0 is
    // rendered as yaw 180; taking .Vector() off the request would flip the forward vector and
    // invert this test. For a perspective capture the two rotations are equal.
    //
    // Why this is a rejection and not just a flag: a behind-camera point still projects, through
    // |W|, to a MIRRORED on-screen pixel (ViewProjectionUtils.cpp passes
    // bShouldCalcOutsideViewPosition=true so overlay geometry can clip). Publishing that into an
    // actor->pixel map would hand an agent a plausible coordinate pointing at the wrong thing.
    bool IsBehindCamera(const FVector& CameraLocation, const FRotator& EffectiveRotation,
        const FVector& WorldPoint);

    // Case-insensitive match of Filter against an actor's internal name OR its display label.
    // A Filter containing '*' or '?' is treated as a wildcard pattern (FString::MatchesWildcard);
    // otherwise it is a substring test. That promote-on-metacharacter rule is asset.search's
    // (AssetManageHandler.cpp:1108, :1147-1156), reused so one string covers both "Tower" and
    // "SM_*_Tower". Case-insensitivity matches actor.find_by_name AND actor.list's default: the
    // wiki once called actor.list case-SENSITIVE, but the code always matched case-INsensitively
    // (see B-actor-list-filter-case-mismatch). actor.list has since gained opt-in caseSensitive /
    // matchMode via Utils/NameMatchFilter.h; this overlay filter deliberately exposes neither,
    // because a label overlay is a visual aid, not a counting surface. An empty Filter matches
    // everything.
    bool MatchesNameFilter(const FString& ActorName, const FString& ActorLabel, const FString& Filter);

    // Select, order, cap and place labels over a FrameWidth x FrameHeight frame. Pure: no
    // randomness, no drawing, no engine state. See FActorLabelStats for the accounting invariants.
    FActorLabelLayout BuildLayout(const TArray<FActorLabelCandidate>& Candidates,
        const FActorLabelOptions& Options);
}
