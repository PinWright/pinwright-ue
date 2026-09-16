// Copyright (c) 2026 Alexander Penkin. MIT License.

// ScatterLayoutHandler.cpp - spatial.scatter_layout: GENERATE a set of transforms from a rule.
//
// Every other verb in this namespace answers a question about a placement that already exists
// (measure_*, verify_*, find_clear_placement) or moves one thing (place_*, ground_*). None of them
// produces a set. This one does, and it is a PURE FUNCTION: no world, no actor, no asset, no
// trace, no transaction. Its transforms[] entries carry the same {location, rotation, scale} shape
// foliage.add_instances, actor.spawn_batch and actor.set_instance_transforms already accept, so the
// pipeline is: generate here, place with one of those, then seat with spatial.ground_instances or
// spatial.ground_actors. This verb deliberately does not know which, and never traces: the Z it
// emits is the region's own.
//
// WHY A VERB AND NOT A DOC PARAGRAPH. docs/wiki-src/level-building.instancing-and-scatter.md has
// specified this algorithm to four decimal places for a long time and shipped no implementation, so
// every caller hand-rolled it in python.execute. The doc's NUMBERS are the easy part; its
// DISTRIBUTION is not. A hand-rolled version on this project drew the jitter as a uniform choice
// over (-1, 0, 1) times the jitter amount instead of a continuous offset. On a hex lattice with
// half-step row offsets that maps two thirds of the points onto one sublattice: it is a second,
// coarser lattice wearing jitter's name. It renders as banding from overhead and as rows from the
// ground, and it passes every check a caller would think to run - instance count, spacing
// statistics and bounds are all correct. Tests/Spatial/TestScatterLayout.cpp asserts the
// distribution itself, which is the only thing that catches it.
//
// TWO PROPERTIES ARE LOAD-BEARING, and both are testable with no editor and no fixture:
//   * REPRODUCIBLE. The same seed, region and knobs return byte-identical transforms. The four
//     random draws per point happen in a fixed order and happen UNCONDITIONALLY, so turning
//     randomYaw off does not shift the scale sequence - a knob documented to change only rotation
//     must not quietly change size.
//   * ONE GLOBAL GRID. The lattice is anchored at the WORLD ORIGIN rather than at the region, and
//     each point's randomness is hashed from (seed, column, row) rather than drawn from one running
//     stream. Two overlapping regions therefore agree exactly on their intersection instead of
//     double-seeding it - the doc's "one global grid, not one grid per zone" bullet, which neither
//     a region-anchored lattice nor a single sequential stream can honour.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Utils/JsonBuilders.h"
#include "Utils/JsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/RandomStream.h"
#include "Math/Transform.h"

namespace
{
    // Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
    // helper with a common name would collide with a sibling handler TU once Unity merges them.

    // Named constant seed, per the doc's "fix the seed" bullet: an unseeded call is still
    // reproducible, so a diff between two runs is meaningful rather than noise.
    constexpr int32 ScatterLayoutDefaultSeed = 1337;

    // The doc's "roughly +/-0.18 of the spacing".
    constexpr double ScatterLayoutDefaultJitter = 0.18;

    // Past half a step a point crosses into its neighbour's cell and the lattice stops
    // guaranteeing anything about spacing, so the fraction is refused rather than clamped.
    constexpr double ScatterLayoutMaxJitter = 0.5;

    // The doc's "+/-15% uniform scale".
    constexpr double ScatterLayoutDefaultScaleMin = 0.85;
    constexpr double ScatterLayoutDefaultScaleMax = 1.15;

    // sqrt(3)/2 - the row pitch that makes a half-step-offset lattice triangular, so all six
    // nearest neighbours of an interior point sit at exactly `spacing`.
    constexpr double ScatterLayoutHexRowFactor = 0.86602540378443864676;

    constexpr int32 ScatterLayoutDefaultMaxPoints = 5000;
    constexpr int32 ScatterLayoutMaxAllowedPoints = 100000;

    // Ceiling on a lattice index before it is narrowed to int32. Well inside the type, so the
    // narrowing is defined; the point budget refuses long before a legitimate call gets near it.
    constexpr double ScatterLayoutMaxLatticeIndex = 1.0e9;

    // Below this a lattice over any useful region is astronomically large; refused up front so
    // the budget error is about the region rather than about a typo'd spacing.
    constexpr double ScatterLayoutMinSpacingCm = 0.01;

    // A box or a circle, in XY. Z is carried for the emitted plane only.
    struct FScatterLayoutRegion
    {
        bool bCircle = false;
        FVector Center = FVector::ZeroVector;
        double Radius = 0.0;
        FVector Min = FVector::ZeroVector;
        FVector Max = FVector::ZeroVector;
    };

    // Same two region shapes spatial.find_clear_placement accepts, so a caller who carved a layout
    // and then wants a hero prop placed in it describes the area the same way twice.
    bool ScatterLayoutParseRegion(const TSharedPtr<FJsonObject>& Obj,
        FScatterLayoutRegion& Out, FString& OutError)
    {
        if (!Obj.IsValid())
        {
            OutError = TEXT("expected an object");
            return false;
        }
        if (Obj->HasField(TEXT("radius")))
        {
            Out.bCircle = true;
            Out.Center = ExtractVectorField(Obj, TEXT("center"), FVector::ZeroVector);
            Out.Radius = GetJsonNumberField(Obj, TEXT("radius"), 0.0);
            if (Out.Radius <= 0.0)
            {
                OutError = TEXT("radius must be positive");
                return false;
            }
            Out.Min = Out.Center - FVector(Out.Radius, Out.Radius, 0.0);
            Out.Max = Out.Center + FVector(Out.Radius, Out.Radius, 0.0);
            return true;
        }
        if (Obj->HasField(TEXT("min")) && Obj->HasField(TEXT("max")))
        {
            Out.Min = ExtractVectorField(Obj, TEXT("min"), FVector::ZeroVector);
            Out.Max = ExtractVectorField(Obj, TEXT("max"), FVector::ZeroVector);
            if (Out.Max.X <= Out.Min.X || Out.Max.Y <= Out.Min.Y)
            {
                OutError = TEXT("max must exceed min on X and Y");
                return false;
            }
            Out.Center = (Out.Min + Out.Max) * 0.5;
            return true;
        }
        OutError = TEXT("expected {center:{x,y,z}, radius} or {min:{x,y,z}, max:{x,y,z}}");
        return false;
    }

    // XY only. A layout is a 2D rule; the Z it emits is the region centre's and is never tested,
    // because bounding a point in Z here would reject placements the caller is about to seat
    // against the ground anyway.
    bool ScatterLayoutContainsXY(const FScatterLayoutRegion& Region, double X, double Y)
    {
        if (Region.bCircle)
        {
            const double DX = X - Region.Center.X;
            const double DY = Y - Region.Center.Y;
            return (DX * DX + DY * DY) <= (Region.Radius * Region.Radius);
        }
        return X >= Region.Min.X && X <= Region.Max.X && Y >= Region.Min.Y && Y <= Region.Max.Y;
    }

    // Per-point stream keyed on the GLOBAL lattice index rather than on an iteration counter, so a
    // point's jitter, yaw and scale are properties of WHERE it sits, not of which region happened
    // to enumerate it. That is what lets two overlapping regions agree on their intersection.
    FRandomStream ScatterLayoutPointStream(int32 Seed, int32 Column, int32 Row)
    {
        uint32 Hash = ::GetTypeHash(Seed);
        Hash = HashCombine(Hash, ::GetTypeHash(Column));
        Hash = HashCombine(Hash, ::GetTypeHash(Row));
        return FRandomStream(static_cast<int32>(Hash));
    }
}

// ---- spatial.scatter_layout ----
REGISTER_RPC_HANDLER("spatial.scatter_layout", "spatial",
    "GENERATE a set of transforms from a rule - the only verb in this namespace that PRODUCES "
    "placements instead of measuring, verifying or moving one. Pure function: no world, no actor, "
    "no asset, no trace; nothing is spawned, moved or saved. Lays a jittered hex (or square) "
    "lattice over a region and returns transforms[] in the same {location, rotation, scale} shape "
    "foliage.add_instances, actor.spawn_batch and actor.set_instance_transforms accept. Seat the "
    "result afterwards with spatial.ground_instances or spatial.ground_actors: the Z here is the "
    "region centre's and nothing was traced, so the layout is floating until you ground it. "
    "Reproducible by construction - the same seed, region and knobs return byte-identical "
    "transforms - and the lattice is anchored at the WORLD ORIGIN with per-point randomness keyed "
    "on the global lattice index, so two overlapping regions agree exactly on their intersection "
    "instead of double-seeding it. Jitter is a CONTINUOUS uniform offset in [-jitter, +jitter] x "
    "spacing on both axes; a discrete jitter collapses the points back onto a coarser lattice and "
    "renders as banding while passing every count, spacing and bounds check. "
    "Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        FParamSpec{TEXT("region"), TEXT("object"),
            TEXT("Where to lay the lattice: {min:{x,y,z}, max:{x,y,z}} or {center:{x,y,z}, "
                 "radius} - the same two shapes spatial.find_clear_placement takes. Tested in XY "
                 "only; every emitted location takes the region centre's Z."),
            true, TEXT(""), TArray<FString>({TEXT("bounds")})},
        RPC_PARAM_REQ("spacing", "number",
            "Distance between neighbouring lattice centres, in cm. Space by CANOPY or footprint "
            "diameter rather than by taste: 1.0-1.25 canopy diameters reads as a stand, so a "
            "950 cm canopy wants roughly 1000-1200."),
        RPC_PARAM_DEF("pattern", "string",
            "'hex' (default) - a triangular lattice, rows pitched spacing*sqrt(3)/2 apart with "
            "alternate rows offset half a step - or 'square'. Hex is the one that reads as "
            "organic; a square grid reads as a grid from any angle.",
            "hex"),
        RPC_PARAM_DEF("jitter", "number",
            "Per-axis offset as a FRACTION of spacing, drawn continuously and uniformly from "
            "[-jitter, +jitter] (default 0.18). 0 emits the bare lattice. Refused above 0.5, "
            "past which a point crosses into its neighbour's cell and the lattice stops "
            "guaranteeing spacing at all.",
            "0.18"),
        RPC_PARAM_DEF("seed", "number",
            "Integer seed, echoed back. Defaults to 1337 so an unseeded call is still "
            "reproducible - a diff between two runs is only meaningful because this is fixed.",
            "1337"),
        FParamSpec{TEXT("scaleRange"), TEXT("object"),
            TEXT("Uniform-scale multipliers {min, max}, drawn continuously per point (default "
                 "{0.85, 1.15}, the doc's +/-15%). Pass {min:1, max:1} for no size variation."),
            false, TEXT(""), TArray<FString>({TEXT("scale_range")})},
        FParamSpec{TEXT("randomYaw"), TEXT("boolean"),
            TEXT("Yaw each point fully at random (default true). Pitch and roll are ALWAYS zero "
                 "and there is no knob for them: a scatter that varies them lays its meshes on "
                 "their sides, which still reads correct from directly overhead."),
            false, TEXT("true"), TArray<FString>({TEXT("random_yaw")})},
        RPC_PARAM_OPT("exclude", "array",
            "Regions to carve out, each in the same shape as `region`. A point whose JITTERED "
            "position falls inside any of them is dropped and counted in `excluded`. Carving here "
            "rather than by hand-deleting instances afterwards is what makes the carve survive a "
            "rebuild. An unparseable entry is refused, never skipped."),
        FParamSpec{TEXT("maxPoints"), TEXT("number"),
            TEXT("Budget on lattice points considered (default 5000, ceiling 100000). A region "
                 "that would exceed it is REFUSED with the computed count rather than truncated "
                 "to a layout that stops halfway across the region."),
            false, TEXT("5000"), TArray<FString>({TEXT("max_points")})}
    ))
{
    // ---- Region ----
    TSharedPtr<FJsonObject> RegionObj = Ctx.GetObject(TEXT("region"));
    if (!RegionObj.IsValid())
    {
        RegionObj = Ctx.GetObject(TEXT("bounds"));
    }
    if (!RegionObj.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("region is required: {min:{x,y,z}, max:{x,y,z}} or {center:{x,y,z}, radius}."));
        return true;
    }

    FScatterLayoutRegion Region;
    FString RegionError;
    if (!ScatterLayoutParseRegion(RegionObj, Region, RegionError))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("region: %s."), *RegionError));
        return true;
    }

    // ---- Lattice knobs ----
    double Spacing = 0.0;
    if (!Ctx.RequireNumber(TEXT("spacing"), Spacing))
    {
        return true;
    }
    if (Spacing < ScatterLayoutMinSpacingCm)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("spacing must be at least %.2f cm (got %.4f)."),
                ScatterLayoutMinSpacingCm, Spacing));
        return true;
    }

    const FString Pattern = Ctx.GetString(TEXT("pattern"), TEXT("hex")).ToLower();
    if (Pattern != TEXT("hex") && Pattern != TEXT("square"))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Unknown pattern '%s'. Valid: hex, square."), *Pattern));
        return true;
    }
    const bool bHex = (Pattern == TEXT("hex"));

    const double Jitter = Ctx.GetNumber(TEXT("jitter"), ScatterLayoutDefaultJitter);
    if (Jitter < 0.0 || Jitter > ScatterLayoutMaxJitter)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("jitter is a fraction of spacing and must be in [0, %.2f] "
                                 "(got %.4f)."), ScatterLayoutMaxJitter, Jitter));
        return true;
    }

    const int32 Seed = Ctx.GetIntFirstOf({TEXT("seed")}).Get(ScatterLayoutDefaultSeed);

    double ScaleMin = ScatterLayoutDefaultScaleMin;
    double ScaleMax = ScatterLayoutDefaultScaleMax;
    TSharedPtr<FJsonObject> ScaleObj = Ctx.GetObject(TEXT("scaleRange"));
    if (!ScaleObj.IsValid())
    {
        ScaleObj = Ctx.GetObject(TEXT("scale_range"));
    }
    if (ScaleObj.IsValid())
    {
        ScaleMin = GetJsonNumberField(ScaleObj, TEXT("min"), ScatterLayoutDefaultScaleMin);
        ScaleMax = GetJsonNumberField(ScaleObj, TEXT("max"), ScatterLayoutDefaultScaleMax);
        if (ScaleMin <= 0.0 || ScaleMax < ScaleMin)
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("scaleRange must satisfy 0 < min <= max (got %.4f..%.4f)."),
                    ScaleMin, ScaleMax));
            return true;
        }
    }

    const bool bRandomYaw = Ctx.GetBoolFirstOf({TEXT("randomYaw"), TEXT("random_yaw")}, true);

    const TOptional<int32> MaxPointsOpt =
        Ctx.GetIntFirstOf({TEXT("maxPoints"), TEXT("max_points")});
    const int32 MaxPoints = FMath::Clamp(MaxPointsOpt.Get(ScatterLayoutDefaultMaxPoints),
        1, ScatterLayoutMaxAllowedPoints);

    // ---- Carve-outs ----
    TArray<FScatterLayoutRegion> Excludes;
    if (const TArray<TSharedPtr<FJsonValue>>* ExcludeValues = Ctx.GetArray(TEXT("exclude")))
    {
        for (int32 Index = 0; Index < ExcludeValues->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& Value = (*ExcludeValues)[Index];
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FScatterLayoutRegion Carve;
            FString CarveError = TEXT("expected an object");
            if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry ||
                !ScatterLayoutParseRegion(*Entry, Carve, CarveError))
            {
                // Refused, never skipped. A carve-out dropped silently leaves instances standing
                // in the corridor it existed to clear, and the layout still looks plausible.
                Ctx.SendError(TEXT("INVALID_PARAMS"),
                    FString::Printf(TEXT("exclude[%d]: %s."), Index, *CarveError));
                return true;
            }
            Excludes.Add(Carve);
        }
    }

    // ---- Lattice, anchored at the WORLD ORIGIN (not at the region) ----
    const double ColStep = Spacing;
    const double RowStep = bHex ? (Spacing * ScatterLayoutHexRowFactor) : Spacing;

    // One full step of margin on every side, so a lattice point sitting just outside the region
    // can still be jittered into it and the half-step row offset never clips the first column.
    // Solved in double and range-checked BEFORE anything narrows to an index: a region far from
    // the origin, or a large one over a small spacing, produces a lattice index outside int32,
    // and narrowing that is undefined behaviour rather than a big number.
    const double FirstColD = FMath::FloorToDouble((Region.Min.X - Spacing) / ColStep);
    const double LastColD = FMath::CeilToDouble((Region.Max.X + Spacing) / ColStep);
    const double FirstRowD = FMath::FloorToDouble((Region.Min.Y - Spacing) / RowStep);
    const double LastRowD = FMath::CeilToDouble((Region.Max.Y + Spacing) / RowStep);

    // Written as a negated <= so a non-finite coordinate (which compares false against
    // everything) is refused rather than sailing through an unsatisfied >.
    if (!(FMath::Abs(FirstColD) <= ScatterLayoutMaxLatticeIndex &&
          FMath::Abs(LastColD) <= ScatterLayoutMaxLatticeIndex &&
          FMath::Abs(FirstRowD) <= ScatterLayoutMaxLatticeIndex &&
          FMath::Abs(LastRowD) <= ScatterLayoutMaxLatticeIndex))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("The region divided by the spacing lands outside the representable lattice "
                 "index range. Either the region coordinates are not finite, the region sits "
                 "astronomically far from the world origin, or the spacing is far too small "
                 "for it."));
        return true;
    }

    const double PlannedPointsD =
        (LastColD - FirstColD + 1.0) * (LastRowD - FirstRowD + 1.0);
    if (PlannedPointsD > static_cast<double>(MaxPoints))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(
                TEXT("The requested lattice is %.0f points (%.0f columns x %.0f rows), over the "
                     "%d-point budget. Nothing was generated: a truncated layout stops partway "
                     "across the region and reads as a bug in the level rather than in the call. "
                     "Raise 'spacing' (currently %.1f cm), shrink 'region', or raise 'maxPoints'."),
                PlannedPointsD, LastColD - FirstColD + 1.0, LastRowD - FirstRowD + 1.0,
                MaxPoints, Spacing));
        return true;
    }

    const int32 FirstCol = static_cast<int32>(FirstColD);
    const int32 LastCol = static_cast<int32>(LastColD);
    const int32 FirstRow = static_cast<int32>(FirstRowD);
    const int32 LastRow = static_cast<int32>(LastRowD);

    const int32 ColumnCount = LastCol - FirstCol + 1;
    const int32 RowCount = LastRow - FirstRow + 1;
    const int64 PlannedPoints = static_cast<int64>(ColumnCount) * static_cast<int64>(RowCount);

    const double JitterCm = Jitter * Spacing;
    const double PlaneZ = Region.Center.Z;

    TArray<TSharedPtr<FJsonValue>> TransformValues;
    TransformValues.Reserve(static_cast<int32>(PlannedPoints));
    int32 OutsideRegion = 0;
    int32 ExcludedPoints = 0;

    for (int32 Row = FirstRow; Row <= LastRow; ++Row)
    {
        // The half-step offset on alternate rows is what turns a square grid into the triangular
        // lattice; without it the row pitch alone just makes a stretched rectangle.
        const double RowOffsetX = (bHex && (Row & 1) != 0) ? (ColStep * 0.5) : 0.0;

        for (int32 Col = FirstCol; Col <= LastCol; ++Col)
        {
            FRandomStream Stream = ScatterLayoutPointStream(Seed, Col, Row);

            // All four draws happen, unconditionally and in this order, even when the knob they
            // feed is off. Skipping the yaw draw when randomYaw is false would shift every
            // subsequent scale, so a knob documented to change only rotation would silently
            // change size too. Mapped in double so the offsets stay CONTINUOUS - the defect this
            // verb exists to prevent is a jitter quantised onto a handful of values.
            const double UnitJitterX = Stream.GetFraction();
            const double UnitJitterY = Stream.GetFraction();
            const double UnitYaw = Stream.GetFraction();
            const double UnitScale = Stream.GetFraction();

            const double X = Col * ColStep + RowOffsetX + (UnitJitterX * 2.0 - 1.0) * JitterCm;
            const double Y = Row * RowStep + (UnitJitterY * 2.0 - 1.0) * JitterCm;

            // Containment is tested AFTER jitter, so every returned point really is inside the
            // region the caller named rather than inside it plus a jitter's worth of slop.
            if (!ScatterLayoutContainsXY(Region, X, Y))
            {
                ++OutsideRegion;
                continue;
            }

            bool bCarved = false;
            for (const FScatterLayoutRegion& Carve : Excludes)
            {
                if (ScatterLayoutContainsXY(Carve, X, Y))
                {
                    bCarved = true;
                    break;
                }
            }
            if (bCarved)
            {
                ++ExcludedPoints;
                continue;
            }

            const double Scale = ScaleMin + UnitScale * (ScaleMax - ScaleMin);
            // FRotator is (Pitch, Yaw, Roll): pitch and roll stay literal zeroes here.
            const FTransform Point(
                FRotator(0.0, bRandomYaw ? (UnitYaw * 360.0) : 0.0, 0.0),
                FVector(X, Y, PlaneZ),
                FVector(Scale));
            TransformValues.Add(
                MakeShared<FJsonValueObject>(JsonBuilders::BuildTransformJson(Point)));
        }
    }

    // ---- Response ----
    TSharedPtr<FJsonObject> RegionOut = MakeShared<FJsonObject>();
    RegionOut->SetStringField(TEXT("mode"), Region.bCircle ? TEXT("circle") : TEXT("box"));
    RegionOut->SetObjectField(TEXT("center"), JsonBuilders::BuildVectorJson(Region.Center));
    RegionOut->SetObjectField(TEXT("min"), JsonBuilders::BuildVectorJson(Region.Min));
    RegionOut->SetObjectField(TEXT("max"), JsonBuilders::BuildVectorJson(Region.Max));
    if (Region.bCircle)
    {
        RegionOut->SetNumberField(TEXT("radius"), Region.Radius);
    }

    // Every knob that shaped the output is echoed, so a layout can be reproduced from its own
    // response without the caller having kept the request.
    TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
    Layout->SetStringField(TEXT("pattern"), Pattern);
    Layout->SetNumberField(TEXT("spacing"), Spacing);
    Layout->SetNumberField(TEXT("rowStep"), RowStep);
    Layout->SetNumberField(TEXT("jitter"), Jitter);
    Layout->SetNumberField(TEXT("jitterCm"), JitterCm);
    Layout->SetNumberField(TEXT("seed"), Seed);
    Layout->SetNumberField(TEXT("scaleMin"), ScaleMin);
    Layout->SetNumberField(TEXT("scaleMax"), ScaleMax);
    Layout->SetBoolField(TEXT("randomYaw"), bRandomYaw);
    Layout->SetNumberField(TEXT("columns"), ColumnCount);
    Layout->SetNumberField(TEXT("rows"), RowCount);
    Layout->SetNumberField(TEXT("maxPoints"), MaxPoints);
    Layout->SetNumberField(TEXT("excludeRegions"), Excludes.Num());

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), TransformValues.Num());
    // count + outsideRegion + excluded == latticePoints, always: nothing is dropped unattributed.
    Data->SetNumberField(TEXT("latticePoints"), static_cast<double>(PlannedPoints));
    Data->SetNumberField(TEXT("outsideRegion"), OutsideRegion);
    Data->SetNumberField(TEXT("excluded"), ExcludedPoints);
    Data->SetObjectField(TEXT("region"), RegionOut);
    Data->SetObjectField(TEXT("layout"), Layout);
    Data->SetArrayField(TEXT("transforms"), TransformValues);
    Data->SetStringField(TEXT("units"), TEXT("cm"));
    Data->SetStringField(TEXT("axis"), TEXT("+X fwd, +Y right, +Z up, left-handed"));

    Ctx.SendSuccess(Data);
    return true;
}
