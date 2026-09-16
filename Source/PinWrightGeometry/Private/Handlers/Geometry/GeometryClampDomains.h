// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryClampDomains.h - the declared domain of every count this layer SILENTLY CLAMPS.
//
// Why this exists, and why it is not the `min` / `max` a parameter table already has.
//
// A front-end publishes two different things under names that read alike, and conflating them
// breaks one of them:
//
//   - A REFUSED domain. The value is rejected outside it, with an error naming the bound. That is
//     what a parameter table's enforced `min` / `max` pair is, and it is checked before an op
//     ever runs.
//   - A CLAMPED domain. The value is ACCEPTED outside it, moved to the nearest legal value, and a
//     warning says so. That is what this file describes.
//
// They cannot share a field. Declaring a radial count's clamped domain as an enforced range would
// turn every clamp into a refusal and destroy the second tier these ops document - a value of 0
// or less means "unset" and takes the verb's own default, which is a legal and useful thing to
// write and is OUTSIDE the clamped domain by construction. So the clamped domain travels
// separately, and this table is where it is declared.
//
// The table is advisory METADATA about behaviour, never a gate. Nothing in the clamp path reads
// it. What keeps it true is a test that MEASURES it: for every row, the op is actually run at the
// floor, one below the floor, at the ceiling, one above it and at zero, and the warnings it emits
// must match the row. A row that stops describing the call sites fails, and so does a call site
// that changes without its row. That measurement is the whole point - the alternative is a third
// hand-written list, which is the staleness this replaces rather than a fix for it.
//
// VOCABULARY IS THE RPC's, deliberately. `Label` is the string the clamp writes into the warning,
// which is the parameter name a `geometry.*` caller passed and greps the response for. A
// front-end that spells the same value differently translates on the way out (that is what
// Model/PwModelParser.h's PwModelWarningNames table is for); this layer stays ignorant of who
// called it, and this file must not learn another surface's spellings.
//
// ADDING A CLAMP means adding a row here in the same commit. The test names the offender either
// way round.
#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

namespace GeometryOps
{
namespace ClampDomains
{
    // One silently-clamped integer count.
    struct FClampDomain
    {
        // The RPC method that publishes this parameter, e.g. TEXT("geometry.create_cone").
        const TCHAR* RpcMethod;

        // The parameter name, which is also the label the clamp writes into its warning.
        const TCHAR* Label;

        // Inclusive floor when the op's revolve sweep does NOT close, and when it does.
        //
        // TWO FLOORS, because two ops expose the sweep angle and their floor genuinely depends on
        // it: a partial sweep is sound at 2 sections, while a sweep that closes wraps its last
        // section onto its first and leaves no interior at all, so 3 is the first value that
        // closes. On every row whose op has no angle parameter the two are EQUAL, which is what
        // keeps the pair readable rather than a special case bolted on for two entries.
        int32 ClampMinOpenSweep;
        int32 ClampMinClosedSweep;

        // Inclusive ceiling.
        int32 ClampMax;

        // The second tier. True when a value of 0 or less reads as "unset" and takes `Default`
        // instead of being raised to the floor - which is why the domain below must never be
        // published as an enforced range. False when 0 is an ordinary legal value (the
        // subdivision counts, where 0 means "no subdivision" and the engine floors it itself).
        bool bZeroMeansUnset;

        // What a value of 0 or less substitutes. Meaningless, and written 0, when
        // bZeroMeansUnset is false.
        int32 Default;

        // The op's own default sweep angle in degrees, or 0 when it has no angle parameter. Only
        // here so a reader can tell WHICH of the two floors an author who writes nothing gets:
        // `arch` defaults to a partial sweep and takes the open floor, `revolve` defaults to a
        // closed one and takes the closed floor, and they are otherwise the same rule.
        double DefaultSweepDegrees;

        // The floor an author who passes no angle actually meets.
        int32 FloorAtOpDefaults() const
        {
            return (DefaultSweepDegrees >= 360.0) ? ClampMinClosedSweep : ClampMinOpenSweep;
        }

        // True when the two floors differ, i.e. when this parameter's domain cannot be stated
        // without naming the sweep.
        bool IsSweepConditional() const
        {
            return ClampMinOpenSweep != ClampMinClosedSweep;
        }
    };

    // Every silently-clamped count in the primitive layer, in the order the generators appear in
    // GeometryOps_Primitives.cpp. Built once.
    TArrayView<const FClampDomain> Entries();

    // Nullptr when this method publishes no clamped count under that label.
    const FClampDomain* Find(const FString& RpcMethod, const FString& Label);
}
}
