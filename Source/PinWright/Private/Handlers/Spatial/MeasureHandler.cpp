// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeasureHandler.cpp - non-mutating spatial-verification RPCs:
//   spatial.measure_distance  - center/pivot/edge-gap distance between two actors
//   spatial.measure_overlap   - AABB intersection test + per-axis penetration
//   spatial.verify_placement  - box-math checks (grounded / on / noOverlapWith / within)
//   spatial.find_clear_placement - SEARCH for a pose a footprint fits in, by box overlap
//
// The first three read actor world-space AABBs the same way actor.get_bounding_box does
// (GetActorBounds(false,...) -> FBox) so their numbers agree with that read.
// find_clear_placement does not: an AABB per actor cannot answer an occupancy SEARCH, so it
// queries the physics scene with a box overlap, which resolves against ISM/HISM per-instance
// bodies as well as actors. None of the four mutate the scene: measure_* is pure box math,
// verify_placement is box math plus a downward line trace via SpatialTraceUtils::TraceGroundBelow,
// and find_clear_placement only reads. Coordinates are in unreal units (cm); every result echoes
// units + an axis note.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/JsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/InstancedStaticMeshComponent.h" // find_clear_placement: naming the blocking instance
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h" // find_clear_placement: the assetPath footprint source
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Math/Box.h"

namespace
{
    // How far below the tested actor's bounds bottom the grounded/on probe traces,
    // beyond the requested gap tolerance, so a failing (too-large) gap can still find
    // the surface and report the measured gap instead of a bare "no ground" miss.
    constexpr float MeasureGroundProbeMargin = 5000.0f;

    // Float slack for boundary comparisons (a gap sitting exactly on the tolerance,
    // or a bounds edge exactly on a container wall, must not fail on rounding noise).
    constexpr double MeasureSmallEps = 0.01;

    // Gap tolerance used when a check doesn't carry its own: grounded's maxGap when
    // omitted, and the `on` rest tolerance (which has no maxGap of its own).
    constexpr double MeasureDefaultGroundGap = 2.0;

    // {x,y,z} JSON object.
    TSharedPtr<FJsonObject> MeasureMakeVectorObject(const FVector& Vec)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), Vec.X);
        Obj->SetNumberField(TEXT("y"), Vec.Y);
        Obj->SetNumberField(TEXT("z"), Vec.Z);
        return Obj;
    }

    // Coordinate-convention echo attached to every result (mirrors RaycastHandler)
    // so a caller never has to guess units/handedness for the returned numbers.
    void MeasureAddAxisEcho(const TSharedPtr<FJsonObject>& Data)
    {
        Data->SetStringField(TEXT("units"), TEXT("cm"));
        Data->SetStringField(TEXT("axis"), TEXT("+X fwd, +Y right, +Z up, left-handed"));
    }

    // World-space AABB of an actor, built the same way actor.get_bounding_box does:
    // GetActorBounds(false, Origin, Extent) -> FBox(Origin-Extent, Origin+Extent), so
    // GetCenter()==Origin and GetExtent()==Extent match that handler's numbers.
    FBox MeasureActorBox(AActor* Actor)
    {
        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent);
        return FBox(Origin - Extent, Origin + Extent);
    }

    // Resolve an actor by name/label/path. On failure sends ACTOR_NOT_FOUND naming
    // which slot ("a"/"b"/"actor") failed and returns nullptr; the caller returns.
    AActor* MeasureResolveActorOrError(const FHandlerContext& Ctx, const FString& Name, const TCHAR* Slot)
    {
        AActor* Found = McpActorUtils::FindActorByName(nullptr, Name);
        if (!Found)
        {
            Ctx.SendError(TEXT("ACTOR_NOT_FOUND"),
                FString::Printf(TEXT("Actor for '%s' not found: '%s'"), Slot, *Name));
        }
        return Found;
    }

    // A single {name, pass} check row for verify_placement's checks[] array. The
    // caller adds any per-check detail/metric fields before pushing it.
    TSharedPtr<FJsonObject> MeasureMakeCheck(const TCHAR* Name, bool bPass)
    {
        TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
        Check->SetStringField(TEXT("name"), Name);
        Check->SetBoolField(TEXT("pass"), bPass);
        return Check;
    }
}

// ---- spatial.measure_distance ----
REGISTER_RPC_HANDLER("spatial.measure_distance", "spatial",
    "Measure the distance between two actors by deterministic box math (no screenshots). "
    "Returns centerDistance (bounds-center to bounds-center), pivotDistance (actor pivot to pivot), "
    "edgeGap (nearest surface-to-surface gap, 0 when the AABBs overlap) and perAxisGap {x,y,z} "
    "(|centerDelta| - (extentA+extentB) per axis; negative = overlap on that axis). All three "
    "distances are always present; `mode` only selects which one is echoed as the headline `distance`. "
    "Coordinates are in unreal units (cm).",
    RPC_PARAMS(
        RPC_PARAM_REQ("a", "string", "Display label / internal name / path of the first actor."),
        RPC_PARAM_REQ("b", "string", "Display label / internal name / path of the second actor."),
        RPC_PARAM_DEF("mode", "string",
            "Headline selector: 'center' (default), 'pivot', or 'edge_gap'. Does not change which "
            "fields are returned - all of centerDistance/pivotDistance/edgeGap/perAxisGap are always present.",
            "center")
    ))
{
    FString NameA;
    if (!Ctx.RequireString(TEXT("a"), NameA)) { return true; }
    FString NameB;
    if (!Ctx.RequireString(TEXT("b"), NameB)) { return true; }

    const FString Mode = Ctx.GetString(TEXT("mode"), TEXT("center")).ToLower();
    if (Mode != TEXT("center") && Mode != TEXT("pivot") && Mode != TEXT("edge_gap"))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Unknown mode '%s'. Valid: center, pivot, edge_gap."), *Mode));
        return true;
    }

    AActor* ActorA = MeasureResolveActorOrError(Ctx, NameA, TEXT("a"));
    if (!ActorA) { return true; }
    AActor* ActorB = MeasureResolveActorOrError(Ctx, NameB, TEXT("b"));
    if (!ActorB) { return true; }

    const FBox BoxA = MeasureActorBox(ActorA);
    const FBox BoxB = MeasureActorBox(ActorB);

    const FVector CenterA = BoxA.GetCenter();
    const FVector CenterB = BoxB.GetCenter();
    const FVector ExtentA = BoxA.GetExtent();
    const FVector ExtentB = BoxB.GetExtent();
    const FVector PivotA = ActorA->GetActorLocation();
    const FVector PivotB = ActorB->GetActorLocation();

    const double CenterDistance = FVector::Dist(CenterA, CenterB);
    const double PivotDistance = FVector::Dist(PivotA, PivotB);
    // Nearest surface-to-surface distance between the two AABBs; 0 when they overlap.
    const double EdgeGap = FMath::Sqrt(BoxA.ComputeSquaredDistanceToBox(BoxB));
    // Per-axis signed gap: negative means the boxes overlap on that axis.
    const FVector PerAxisGap = (CenterA - CenterB).GetAbs() - (ExtentA + ExtentB);

    double Headline = CenterDistance;
    if (Mode == TEXT("pivot")) { Headline = PivotDistance; }
    else if (Mode == TEXT("edge_gap")) { Headline = EdgeGap; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("a"), ActorA->GetActorLabel());
    Data->SetStringField(TEXT("b"), ActorB->GetActorLabel());
    Data->SetStringField(TEXT("mode"), Mode);
    Data->SetNumberField(TEXT("distance"), Headline);
    Data->SetNumberField(TEXT("centerDistance"), CenterDistance);
    Data->SetNumberField(TEXT("pivotDistance"), PivotDistance);
    Data->SetNumberField(TEXT("edgeGap"), EdgeGap);
    Data->SetObjectField(TEXT("perAxisGap"), MeasureMakeVectorObject(PerAxisGap));
    MeasureAddAxisEcho(Data);

    Ctx.SendSuccess(Data);
    return true;
}

// ---- spatial.measure_overlap ----
REGISTER_RPC_HANDLER("spatial.measure_overlap", "spatial",
    "Non-mutating AABB overlap test between two actors. Returns overlapping (FBox::Intersect), "
    "perAxisPenetration {x,y,z} ((extentA+extentB) - |centerDelta| per axis; positive where the "
    "boxes overlap on that axis), and - when overlapping - the minimum-translation separating axis "
    "(penetrationAxis) and its depth (penetrationDepth, the smallest push-out). Pure box math; does "
    "not modify geometry (unlike geometry.boolean_intersection). Coordinates are in unreal units (cm).",
    RPC_PARAMS(
        RPC_PARAM_REQ("a", "string", "Display label / internal name / path of the first actor."),
        RPC_PARAM_REQ("b", "string", "Display label / internal name / path of the second actor.")
    ))
{
    FString NameA;
    if (!Ctx.RequireString(TEXT("a"), NameA)) { return true; }
    FString NameB;
    if (!Ctx.RequireString(TEXT("b"), NameB)) { return true; }

    AActor* ActorA = MeasureResolveActorOrError(Ctx, NameA, TEXT("a"));
    if (!ActorA) { return true; }
    AActor* ActorB = MeasureResolveActorOrError(Ctx, NameB, TEXT("b"));
    if (!ActorB) { return true; }

    const FBox BoxA = MeasureActorBox(ActorA);
    const FBox BoxB = MeasureActorBox(ActorB);

    const bool bOverlapping = BoxA.Intersect(BoxB);

    // Per-axis penetration: positive where the boxes overlap on that axis. When they
    // truly intersect, all three components are >= 0.
    const FVector Penetration =
        (BoxA.GetExtent() + BoxB.GetExtent()) - (BoxA.GetCenter() - BoxB.GetCenter()).GetAbs();

    // Separating axis = the axis with the SMALLEST positive penetration (least push-out
    // needed to break the overlap).
    int32 MinAxis = 0;
    double MinPenetration = Penetration.X;
    if (Penetration.Y < MinPenetration) { MinPenetration = Penetration.Y; MinAxis = 1; }
    if (Penetration.Z < MinPenetration) { MinPenetration = Penetration.Z; MinAxis = 2; }
    static const TCHAR* AxisNames[3] = { TEXT("x"), TEXT("y"), TEXT("z") };

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("a"), ActorA->GetActorLabel());
    Data->SetStringField(TEXT("b"), ActorB->GetActorLabel());
    Data->SetBoolField(TEXT("overlapping"), bOverlapping);
    // Only meaningful when overlapping; report a neutral 0 / empty axis otherwise.
    Data->SetStringField(TEXT("penetrationAxis"), bOverlapping ? AxisNames[MinAxis] : TEXT(""));
    Data->SetNumberField(TEXT("penetrationDepth"), bOverlapping ? MinPenetration : 0.0);
    Data->SetObjectField(TEXT("perAxisPenetration"), MeasureMakeVectorObject(Penetration));
    MeasureAddAxisEcho(Data);

    Ctx.SendSuccess(Data);
    return true;
}

// ---- spatial.verify_placement ----
REGISTER_RPC_HANDLER("spatial.verify_placement", "spatial",
    "Verify an actor's placement with deterministic box math instead of eyeballing a screenshot. "
    "Runs only the checks present in `expect`: grounded {maxGap} (a downward trace from the bounds "
    "bottom must hit within [0, maxGap]); on (actorName - like grounded, but the surface directly "
    "below must be that actor); noOverlapWith (array of actor names - none may intersect this actor's "
    "AABB); within ({min,max} box or an actor name whose AABB must fully contain this actor's AABB). "
    "Returns pass (all requested checks passed) and checks[] with a per-check pass + detail. "
    "Non-mutating. Coordinates are in unreal units (cm).",
    RPC_PARAMS(
        RPC_PARAM_REQ("actor", "string", "Display label / internal name / path of the actor to verify."),
        RPC_PARAM_REQ("expect", "object",
            "Expectations object; each key is an independent check run only when present: "
            "grounded {maxGap}, on (actorName string), noOverlapWith (array of actor names), "
            "within ({min:{x,y,z}, max:{x,y,z}} or an actor-name string).")
    ))
{
    FString ActorName;
    if (!Ctx.RequireString(TEXT("actor"), ActorName)) { return true; }

    TSharedPtr<FJsonObject> Expect;
    if (!Ctx.RequireObject(TEXT("expect"), Expect)) { return true; }

    AActor* Target = MeasureResolveActorOrError(Ctx, ActorName, TEXT("actor"));
    if (!Target) { return true; }

    const FBox TargetBox = MeasureActorBox(Target);
    UWorld* World = Target->GetWorld();

    TArray<TSharedPtr<FJsonValue>> Checks;
    bool bAllPass = true;

    // --- grounded: trace straight down from the bounds bottom; PASS if it hits within
    //     [0, maxGap]. The trace starts at the bounds bottom-center, so a real hit has a
    //     non-negative gap; the gap >= -eps guard documents the "not clipping into the
    //     surface" intent (a downward-from-bottom trace can't report a gap above the
    //     start point, so clipping shows up as a miss rather than a negative gap).
    if (Expect->HasTypedField<EJson::Object>(TEXT("grounded")))
    {
        const TSharedPtr<FJsonObject> GroundedObj = Expect->GetObjectField(TEXT("grounded"));
        double MaxGap = MeasureDefaultGroundGap;
        if (GroundedObj->HasTypedField<EJson::Number>(TEXT("maxGap")))
        {
            MaxGap = GroundedObj->GetNumberField(TEXT("maxGap"));
        }
        else if (GroundedObj->HasTypedField<EJson::Number>(TEXT("max_gap")))
        {
            MaxGap = GroundedObj->GetNumberField(TEXT("max_gap"));
        }

        const float ProbeDrop = FMath::Max((float)MaxGap, 0.0f) + MeasureGroundProbeMargin;
        const SpatialTraceUtils::FSpatialHit Hit =
            SpatialTraceUtils::TraceGroundBelow(World, TargetBox, ProbeDrop, { Target });

        bool bCheckPass = false;
        TSharedPtr<FJsonObject> Check = MeasureMakeCheck(TEXT("grounded"), false);
        Check->SetNumberField(TEXT("maxGap"), MaxGap);
        Check->SetBoolField(TEXT("hasGround"), Hit.bHit);
        if (Hit.bHit)
        {
            const double Gap = Hit.Distance;
            bCheckPass = (Gap >= -MeasureSmallEps) && (Gap <= MaxGap + MeasureSmallEps);
            Check->SetNumberField(TEXT("gap"), Gap);
            if (AActor* GroundActor = Hit.HitActor.Get())
            {
                Check->SetStringField(TEXT("groundActor"), GroundActor->GetActorLabel());
            }
        }
        else
        {
            Check->SetStringField(TEXT("detail"),
                FString::Printf(TEXT("no ground found within %.1f cm below the actor"), ProbeDrop));
        }
        Check->SetBoolField(TEXT("pass"), bCheckPass);
        bAllPass = bAllPass && bCheckPass;
        Checks.Add(MakeShared<FJsonValueObject>(Check));
    }

    // --- on: the surface directly below (within a default rest tolerance) must be the
    //     named actor. Same downward trace as grounded, but keyed on the hit actor.
    if (Expect->HasTypedField<EJson::String>(TEXT("on")))
    {
        const FString OnName = Expect->GetStringField(TEXT("on"));
        AActor* OnActor = McpActorUtils::FindActorByName(nullptr, OnName);

        const float ProbeDrop = (float)MeasureDefaultGroundGap + MeasureGroundProbeMargin;
        const SpatialTraceUtils::FSpatialHit Hit =
            SpatialTraceUtils::TraceGroundBelow(World, TargetBox, ProbeDrop, { Target });
        AActor* HitActor = Hit.HitActor.Get();

        const bool bWithinTolerance = Hit.bHit && (Hit.Distance <= MeasureDefaultGroundGap + MeasureSmallEps);
        const bool bCheckPass = (OnActor != nullptr) && (HitActor == OnActor) && bWithinTolerance;

        TSharedPtr<FJsonObject> Check = MeasureMakeCheck(TEXT("on"), bCheckPass);
        Check->SetStringField(TEXT("expected"), OnName);
        Check->SetNumberField(TEXT("tolerance"), MeasureDefaultGroundGap);
        Check->SetBoolField(TEXT("hasGround"), Hit.bHit);
        if (HitActor)
        {
            Check->SetStringField(TEXT("actual"), HitActor->GetActorLabel());
        }
        if (Hit.bHit)
        {
            Check->SetNumberField(TEXT("gap"), Hit.Distance);
        }
        if (!OnActor)
        {
            Check->SetStringField(TEXT("detail"),
                FString::Printf(TEXT("expected support actor not found: '%s'"), *OnName));
        }
        bAllPass = bAllPass && bCheckPass;
        Checks.Add(MakeShared<FJsonValueObject>(Check));
    }

    // --- noOverlapWith: none of the named actors may intersect this actor's AABB.
    //     Unresolved names are reported but don't fail the check (a stale name is not
    //     an overlap). Uses the same non-destructive FBox::Intersect as measure_overlap.
    if (Expect->HasTypedField<EJson::Array>(TEXT("noOverlapWith")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Names = Expect->GetArrayField(TEXT("noOverlapWith"));
        TArray<TSharedPtr<FJsonValue>> Overlapping;
        TArray<TSharedPtr<FJsonValue>> Unresolved;
        for (const TSharedPtr<FJsonValue>& Val : Names)
        {
            FString Name;
            if (!Val.IsValid() || !Val->TryGetString(Name) || Name.IsEmpty())
            {
                continue;
            }
            AActor* Other = McpActorUtils::FindActorByName(nullptr, Name);
            if (!Other)
            {
                Unresolved.Add(MakeShared<FJsonValueString>(Name));
                continue;
            }
            if (TargetBox.Intersect(MeasureActorBox(Other)))
            {
                Overlapping.Add(MakeShared<FJsonValueString>(Other->GetActorLabel()));
            }
        }

        const bool bCheckPass = (Overlapping.Num() == 0);
        TSharedPtr<FJsonObject> Check = MeasureMakeCheck(TEXT("noOverlapWith"), bCheckPass);
        Check->SetArrayField(TEXT("overlapping"), Overlapping);
        Check->SetArrayField(TEXT("unresolved"), Unresolved);
        bAllPass = bAllPass && bCheckPass;
        Checks.Add(MakeShared<FJsonValueObject>(Check));
    }

    // --- within: a container box ({min,max}) or an actor's AABB must fully contain this
    //     actor's AABB (componentwise, with an eps tolerance on each wall).
    if (Expect->HasField(TEXT("within")))
    {
        TSharedPtr<FJsonObject> Check = MeasureMakeCheck(TEXT("within"), false);
        bool bCheckPass = false;
        bool bHaveContainer = false;
        FBox Container(ForceInit);

        if (Expect->HasTypedField<EJson::Object>(TEXT("within")))
        {
            const TSharedPtr<FJsonObject> WithinObj = Expect->GetObjectField(TEXT("within"));
            if (WithinObj->HasField(TEXT("min")) && WithinObj->HasField(TEXT("max")))
            {
                const FVector Min = ExtractVectorField(WithinObj, TEXT("min"), FVector::ZeroVector);
                const FVector Max = ExtractVectorField(WithinObj, TEXT("max"), FVector::ZeroVector);
                Container = FBox(Min, Max);
                bHaveContainer = true;
            }
            else
            {
                Check->SetStringField(TEXT("detail"),
                    TEXT("within object must have both 'min' {x,y,z} and 'max' {x,y,z}"));
            }
        }
        else if (Expect->HasTypedField<EJson::String>(TEXT("within")))
        {
            const FString ContainerName = Expect->GetStringField(TEXT("within"));
            Check->SetStringField(TEXT("containerActor"), ContainerName);
            if (AActor* ContainerActor = McpActorUtils::FindActorByName(nullptr, ContainerName))
            {
                Container = MeasureActorBox(ContainerActor);
                bHaveContainer = true;
            }
            else
            {
                Check->SetStringField(TEXT("detail"),
                    FString::Printf(TEXT("container actor not found: '%s'"), *ContainerName));
            }
        }
        else
        {
            Check->SetStringField(TEXT("detail"),
                TEXT("within must be an object {min,max} or an actor-name string"));
        }

        if (bHaveContainer)
        {
            bCheckPass =
                (TargetBox.Min.X >= Container.Min.X - MeasureSmallEps) &&
                (TargetBox.Min.Y >= Container.Min.Y - MeasureSmallEps) &&
                (TargetBox.Min.Z >= Container.Min.Z - MeasureSmallEps) &&
                (TargetBox.Max.X <= Container.Max.X + MeasureSmallEps) &&
                (TargetBox.Max.Y <= Container.Max.Y + MeasureSmallEps) &&
                (TargetBox.Max.Z <= Container.Max.Z + MeasureSmallEps);
            Check->SetObjectField(TEXT("containerMin"), MeasureMakeVectorObject(Container.Min));
            Check->SetObjectField(TEXT("containerMax"), MeasureMakeVectorObject(Container.Max));
            Check->SetObjectField(TEXT("actorMin"), MeasureMakeVectorObject(TargetBox.Min));
            Check->SetObjectField(TEXT("actorMax"), MeasureMakeVectorObject(TargetBox.Max));
        }

        Check->SetBoolField(TEXT("pass"), bCheckPass);
        bAllPass = bAllPass && bCheckPass;
        Checks.Add(MakeShared<FJsonValueObject>(Check));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Target->GetActorLabel());
    Data->SetBoolField(TEXT("pass"), bAllPass);
    Data->SetArrayField(TEXT("checks"), Checks);
    MeasureAddAxisEcho(Data);

    Ctx.SendSuccess(Data);
    return true;
}

// ================= spatial.find_clear_placement =================
//
// The one occupancy question the rest of this namespace cannot ask: not "does THIS transform
// clear THESE named actors" but "is there room for a W x D footprint anywhere in here, and if
// not, what is standing in the way". Every other spatial verb takes the placement as INPUT;
// this one searches for it.
//
// It is built on box overlap queries (SpatialTraceUtils::ProbeFootprintOccupancy) rather than on
// client-side OBB/SAT arithmetic for one reason that is not convenience: a physics overlap
// resolves against the per-INSTANCE bodies of an ISM/HISM. Instanced scatter is what actually
// fills a dressed level and it is invisible to every name-based occupancy input in this
// namespace - a holder's single AABB spans the whole scatter - so a verb that saw only actors
// would answer the wrong question confidently.
namespace
{
    // Vertical size of the probe box when the caller gives a footprint with no height. A
    // footprint-only query is asking about ground room, so a 1 m slab is the model.
    constexpr double FindClearDefaultFootprintHeightCm = 100.0;

    constexpr double FindClearDefaultYawStepDeg = 90.0;

    // Floor on a positive yawStep. 0.5 deg is already 360 orientations of a box that repeats
    // every 180, and anything finer only exists to blow the yaw list up before the pose budget
    // can refuse it.
    constexpr double FindClearMinYawStepDeg = 0.5;

    // Ceiling on how far clearance is measured. Past it the answer is reported as capped rather
    // than as a number, because a search that measured to infinity would probe the whole level.
    constexpr double FindClearDefaultClearanceProbeCm = 200.0;

    // Seat the probe box this far above the measured ground so resting ON a surface does not
    // register as overlapping it. Same role as PlacementOverlapEps in PlacementHandler.
    constexpr double FindClearGroundLiftCm = 0.5;

    // Bisection steps inside MeasureFootprintClearance: clearance resolves to probe / 2^6.
    constexpr int32 FindClearBisectionSteps = 6;

    constexpr int32 FindClearDefaultMaxPoses = 5000;
    constexpr int32 FindClearMaxAllowedPoses = 100000;
    constexpr int32 FindClearDefaultMaxResults = 10;
    constexpr int32 FindClearMaxAllowedResults = 200;

    // Report caps. A dressed level can bind a search with hundreds of instances of one scatter;
    // the aggregate row is the useful answer and the full list is not.
    constexpr int32 FindClearMaxReportedBinders = 10;
    constexpr int32 FindClearMaxTrackedBinders = 256;
    constexpr int32 FindClearMaxReportedInstances = 8;

    // Editor world by default; PIE world when a play session is active - the same rule the
    // placement verbs use, so a search resolves against the actors lookups resolve against.
    UWorld* FindClearResolveWorld()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        if (GEditor->PlayWorld)
        {
            return GEditor->PlayWorld;
        }
        return GEditor->GetEditorWorldContext().World();
    }

    // One clear pose, before ranking and trimming.
    struct FFindClearPose
    {
        FVector Location = FVector::ZeroVector; // world centre of the fitted footprint box
        double Yaw = 0.0;
        double ClearanceCm = 0.0;
        bool bClearanceCapped = false;
        double DistanceFromCenterCm = 0.0;
        bool bHasGround = false;
        FVector GroundLocation = FVector::ZeroVector;
        FString GroundActor;
        bool bHasBinder = false;
        SpatialTraceUtils::FSpatialOccupant Binder;
    };

    // One XY cell of the search grid, solved once and reused by every yaw at that cell.
    struct FFindClearColumn
    {
        double X = 0.0;
        double Y = 0.0;
        bool bHasGround = false;
        double GroundZ = 0.0;
        FVector GroundLocation = FVector::ZeroVector;
        FString GroundActorName;
        // Every actor any of this column's ground probes rested on. Excluded from that column's
        // occupancy query: the surface you are standing on is not an obstacle to you, and
        // without this every pose on a slope reads as occupied by the terrain under it.
        TArray<AActor*> GroundActors;
    };

    // Running tally of what blocked poses, keyed on the blocking COMPONENT so a scatter reports
    // as one row naming its instances rather than as one row per instance.
    struct FFindClearBinderTally
    {
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<UPrimitiveComponent> Component;
        bool bInstanced = false;
        int32 BlockedPoses = 0;
        TArray<int32> InstanceIndices;
    };

    void FindClearRecordBinder(TArray<FFindClearBinderTally>& Tallies,
                               const SpatialTraceUtils::FSpatialOccupant& Occupant)
    {
        UPrimitiveComponent* Component = Occupant.Component.Get();
        for (FFindClearBinderTally& Tally : Tallies)
        {
            if (Tally.Component.Get() == Component)
            {
                ++Tally.BlockedPoses;
                if (Occupant.InstanceIndex != INDEX_NONE
                    && Tally.InstanceIndices.Num() < FindClearMaxReportedInstances)
                {
                    Tally.InstanceIndices.AddUnique(Occupant.InstanceIndex);
                }
                return;
            }
        }
        if (Tallies.Num() >= FindClearMaxTrackedBinders)
        {
            // Past this many distinct blockers the ranked head of the list is already the
            // answer; tracking more costs memory and tells the caller nothing new.
            return;
        }
        FFindClearBinderTally Tally;
        Tally.Actor = Occupant.Actor;
        Tally.Component = Occupant.Component;
        Tally.bInstanced = Occupant.bInstanced;
        Tally.BlockedPoses = 1;
        if (Occupant.InstanceIndex != INDEX_NONE)
        {
            Tally.InstanceIndices.Add(Occupant.InstanceIndex);
        }
        Tallies.Add(MoveTemp(Tally));
    }

    // {actor, component, instanced, instanceIndex} for one occupant - the "what actually blocked
    // it" half of a rejection, which a boolean cannot carry.
    TSharedPtr<FJsonObject> FindClearOccupantObject(const SpatialTraceUtils::FSpatialOccupant& Occupant)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (AActor* Actor = Occupant.Actor.Get())
        {
            Obj->SetStringField(TEXT("actor"), Actor->GetActorLabel());
            Obj->SetStringField(TEXT("actorPath"), Actor->GetPathName());
            Obj->SetStringField(TEXT("actorClass"), Actor->GetClass()->GetName());
        }
        if (UPrimitiveComponent* Component = Occupant.Component.Get())
        {
            Obj->SetStringField(TEXT("component"), Component->GetName());
            Obj->SetStringField(TEXT("componentClass"), Component->GetClass()->GetName());
        }
        Obj->SetBoolField(TEXT("instanced"), Occupant.bInstanced);
        if (Occupant.InstanceIndex != INDEX_NONE)
        {
            Obj->SetNumberField(TEXT("instanceIndex"), Occupant.InstanceIndex);
        }
        return Obj;
    }

    // Appends every non-empty string in a JSON array param to Out. Non-string entries are skipped.
    void FindClearAppendStrings(const TArray<TSharedPtr<FJsonValue>>* Values, TArray<FString>& Out)
    {
        if (!Values)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Entry;
            if (Value.IsValid() && Value->TryGetString(Entry))
            {
                Entry.TrimStartAndEndInline();
                if (!Entry.IsEmpty())
                {
                    Out.AddUnique(Entry);
                }
            }
        }
    }
}

REGISTER_RPC_HANDLER("spatial.find_clear_placement", "spatial",
    "SEARCH for somewhere a footprint fits, instead of verifying a spot already chosen. Sweeps an "
    "XY grid x yaw grid over a search region, tests every pose with a physics BOX OVERLAP, and "
    "returns the clear poses ranked, each carrying its MEASURED clearance (bisected, not assumed). "
    "Rejections are attributed: the response names the blocking actor, its component, and - for an "
    "ISM/HISM - the INSTANCE. That is the half no name-based occupancy input here can express, "
    "because a scatter holder's single AABB spans the whole scatter, so naming the holder says "
    "nothing about which instance is in the way. Non-mutating: nothing is spawned or moved. "
    "An obstacle has to have collision to be seen, exactly as for spatial.raycast. "
    "Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        RPC_PARAM_OPT("footprint", "object",
            "The box to fit: {width, depth, height?} in cm (height defaults to 100). Provide "
            "footprint OR assetPath."),
        FParamSpec{TEXT("assetPath"), TEXT("path"),
            TEXT("Static mesh whose own bounds become the footprint, so the caller does not "
                 "re-derive them. Provide footprint OR assetPath."),
            false, TEXT(""), TArray<FString>({TEXT("asset_path"), TEXT("meshPath")})},
        RPC_PARAM_REQ("region", "object",
            "Where to search: {center:{x,y,z}, radius} or {min:{x,y,z}, max:{x,y,z}}. The region "
            "also bounds the ground probe in Z - each column is traced from the region's top face "
            "down to its bottom face."),
        RPC_PARAM_OPT("step", "number",
            "XY grid spacing in cm. Default: half the footprint's shorter side, floored at 10."),
        FParamSpec{TEXT("yawStep"), TEXT("number"),
            TEXT("Yaw increment in degrees (default 90). Only yaws in [0, 180) are tried - a "
                 "rectangular footprint maps onto itself under a half turn, so the rest repeat. "
                 "0 searches yaw 0 only; anything else below 0.5 is refused rather than "
                 "silently coarsened."),
            false, TEXT("90"), TArray<FString>({TEXT("yaw_step")})},
        FParamSpec{TEXT("minClearance"), TEXT("number"),
            TEXT("Free space required around the footprint in cm (default 0). A pose whose "
                 "measured clearance falls below this is rejected and attributed."),
            false, TEXT("0"), TArray<FString>({TEXT("min_clearance")})},
        FParamSpec{TEXT("clearanceProbe"), TEXT("number"),
            TEXT("Ceiling on how far clearance is measured, in cm (default 200; raised to "
                 "minClearance when that is larger). A pose still free at the ceiling reports "
                 "clearanceCapped:true, and its number is a floor rather than the real gap."),
            false, TEXT("200"), TArray<FString>({TEXT("clearance_probe")})},
        FParamSpec{TEXT("ignoreActors"), TEXT("array"),
            TEXT("Names / labels / paths excluded from the occupancy query entirely - the actor "
                 "being placed, or props being replaced. Names that do not resolve are echoed "
                 "back as unresolvedIgnoreActors."),
            false, TEXT(""), TArray<FString>({TEXT("ignore_actors")})},
        FParamSpec{TEXT("onlyClasses"), TEXT("array"),
            TEXT("Count as obstacles ONLY actors whose class or any ancestor class matches, as a "
                 "case-insensitive substring of the class name or its /Script path. Empty = "
                 "everything the channel answers for."),
            false, TEXT(""), TArray<FString>({TEXT("only_classes")})},
        FParamSpec{TEXT("keepOut"), TEXT("object"),
            TEXT("Optional corridor to stay clear of: {points:[{x,y,z},...], radius}. A pose is "
                 "rejected when its centre is nearer the polyline than radius + the footprint's "
                 "circumscribed radius - conservative by construction, so it never passes a pose "
                 "that actually intrudes."),
            false, TEXT(""), TArray<FString>({TEXT("keep_out")})},
        FParamSpec{TEXT("seatOnGround"), TEXT("boolean"),
            TEXT("Rest the footprint on the surface under each column (default true). False "
                 "tests the footprint centred at the region's Z instead - the airborne-corridor "
                 "case."),
            false, TEXT("true"), TArray<FString>({TEXT("seat_on_ground")})},
        RPC_PARAM_DEF("rank", "string",
            "Result order: 'clearance' (default, roomiest first) or 'distance' (nearest the "
            "region centre first). Both numbers are on every returned pose either way.",
            "clearance"),
        FParamSpec{TEXT("maxResults"), TEXT("number"),
            TEXT("How many clear poses to return (default 10, ceiling 200). `found` reports how "
                 "many there were before the trim."),
            false, TEXT("10"), TArray<FString>({TEXT("max_results")})},
        FParamSpec{TEXT("maxPoses"), TEXT("number"),
            TEXT("Budget on poses evaluated (default 5000, ceiling 100000). A grid that would "
                 "exceed it is REFUSED with the computed count rather than silently searching "
                 "part of the region - raise step, shrink region, raise yawStep, or raise this."),
            false, TEXT("5000"), TArray<FString>({TEXT("max_poses")})}
    ))
{
    UWorld* World = FindClearResolveWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"),
            TEXT("No editor/PIE world available to search."));
        return true;
    }

    // ---- Footprint: an explicit box, or a mesh's own bounds ----
    double Width = 0.0;
    double Depth = 0.0;
    double Height = 0.0;
    FString FootprintSource;
    const TSharedPtr<FJsonObject> FootprintObj = Ctx.GetObject(TEXT("footprint"));
    const FString AssetPath =
        Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("asset_path"), TEXT("meshPath")});

    if (FootprintObj.IsValid())
    {
        FootprintSource = TEXT("footprint");
        Width = GetJsonNumberField(FootprintObj, TEXT("width"), 0.0);
        Depth = GetJsonNumberField(FootprintObj, TEXT("depth"), 0.0);
        Height = GetJsonNumberField(FootprintObj, TEXT("height"), FindClearDefaultFootprintHeightCm);
    }
    else if (!AssetPath.IsEmpty())
    {
        FootprintSource = TEXT("assetPath");
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
        if (!Mesh)
        {
            Ctx.SendError(TEXT("MESH_NOT_FOUND"),
                FString::Printf(TEXT("No static mesh loaded from assetPath '%s'."), *AssetPath));
            return true;
        }
        // The mesh's own extent, read the same way asset.dump reports it, so the two agree.
        const FVector Extent = Mesh->GetBounds().BoxExtent;
        Width = Extent.X * 2.0;
        Depth = Extent.Y * 2.0;
        Height = Extent.Z * 2.0;
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("Provide a footprint: either 'footprint' {width, depth, height?} or 'assetPath' "
                 "naming a static mesh whose bounds should be used."));
        return true;
    }

    if (Width <= 0.0 || Depth <= 0.0)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(
                TEXT("Footprint width and depth must both be positive (got %.2f x %.2f cm)."),
                Width, Depth));
        return true;
    }
    if (Height <= 0.0)
    {
        Height = FindClearDefaultFootprintHeightCm;
    }

    const FVector HalfExtent(Width * 0.5, Depth * 0.5, Height * 0.5);
    const double CircumRadiusXY =
        FMath::Sqrt(HalfExtent.X * HalfExtent.X + HalfExtent.Y * HalfExtent.Y);
    const double CircumRadius3D =
        FMath::Sqrt(CircumRadiusXY * CircumRadiusXY + HalfExtent.Z * HalfExtent.Z);

    // ---- Search region ----
    TSharedPtr<FJsonObject> RegionObj;
    if (!Ctx.RequireObject(TEXT("region"), RegionObj)) { return true; }

    FVector RegionMin = FVector::ZeroVector;
    FVector RegionMax = FVector::ZeroVector;
    FVector RegionCenter = FVector::ZeroVector;
    double RegionRadius = 0.0;
    const bool bSphereRegion = RegionObj->HasField(TEXT("radius"));

    if (bSphereRegion)
    {
        RegionCenter = ExtractVectorField(RegionObj, TEXT("center"), FVector::ZeroVector);
        RegionRadius = GetJsonNumberField(RegionObj, TEXT("radius"), 0.0);
        if (RegionRadius <= 0.0)
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("region.radius must be positive."));
            return true;
        }
        RegionMin = RegionCenter - FVector(RegionRadius);
        RegionMax = RegionCenter + FVector(RegionRadius);
    }
    else if (RegionObj->HasField(TEXT("min")) && RegionObj->HasField(TEXT("max")))
    {
        RegionMin = ExtractVectorField(RegionObj, TEXT("min"), FVector::ZeroVector);
        RegionMax = ExtractVectorField(RegionObj, TEXT("max"), FVector::ZeroVector);
        if (RegionMax.X <= RegionMin.X || RegionMax.Y <= RegionMin.Y || RegionMax.Z <= RegionMin.Z)
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                TEXT("region.max must exceed region.min on every axis."));
            return true;
        }
        RegionCenter = (RegionMin + RegionMax) * 0.5;
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("region must be {center:{x,y,z}, radius} or {min:{x,y,z}, max:{x,y,z}}."));
        return true;
    }

    // ---- Search knobs ----
    double Step = Ctx.GetNumber(TEXT("step"), 0.0);
    if (Step <= 0.0)
    {
        Step = FMath::Max(10.0, 0.5 * FMath::Min(Width, Depth));
    }

    const double YawStep =
        Ctx.GetNumber(TEXT("yawStep"), Ctx.GetNumber(TEXT("yaw_step"), FindClearDefaultYawStepDeg));
    if (YawStep > 0.0 && YawStep < FindClearMinYawStepDeg)
    {
        // Refused rather than clamped: silently coarsening the sweep would answer a different
        // question than the caller asked, and a sub-half-degree step is already 360 orientations
        // of a box that is symmetric every 180 degrees.
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("yawStep %.4f deg is below the %.1f deg minimum. Use 0 for a "
                                 "single yaw, or a step of at least %.1f."),
                YawStep, FindClearMinYawStepDeg, FindClearMinYawStepDeg));
        return true;
    }
    TArray<double> Yaws;
    if (YawStep <= 0.0)
    {
        Yaws.Add(0.0);
    }
    else
    {
        // A rectangular footprint maps onto itself under a 180 degree turn, so the second half
        // of the circle would only re-test poses already tested.
        for (double Yaw = 0.0; Yaw < 180.0 - UE_KINDA_SMALL_NUMBER; Yaw += YawStep)
        {
            Yaws.Add(Yaw);
        }
        if (Yaws.Num() == 0)
        {
            Yaws.Add(0.0);
        }
    }

    const double MinClearance = FMath::Max(0.0,
        Ctx.GetNumber(TEXT("minClearance"), Ctx.GetNumber(TEXT("min_clearance"), 0.0)));
    double ClearanceProbe = Ctx.GetNumber(TEXT("clearanceProbe"),
        Ctx.GetNumber(TEXT("clearance_probe"), FindClearDefaultClearanceProbeCm));
    // The probe has to reach at least the caller's own threshold, or minClearance could never
    // be shown to be met.
    ClearanceProbe = FMath::Max(ClearanceProbe, MinClearance);

    const bool bSeatOnGround =
        Ctx.GetBoolFirstOf({TEXT("seatOnGround"), TEXT("seat_on_ground")}, true);

    const FString Rank = Ctx.GetString(TEXT("rank"), TEXT("clearance")).ToLower();
    if (Rank != TEXT("clearance") && Rank != TEXT("distance"))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Unknown rank '%s'. Valid: clearance, distance."), *Rank));
        return true;
    }

    const TOptional<int32> MaxResultsOpt =
        Ctx.GetIntFirstOf({TEXT("maxResults"), TEXT("max_results")});
    const int32 MaxResults = FMath::Clamp(MaxResultsOpt.Get(FindClearDefaultMaxResults),
        1, FindClearMaxAllowedResults);
    const TOptional<int32> MaxPosesOpt = Ctx.GetIntFirstOf({TEXT("maxPoses"), TEXT("max_poses")});
    const int32 MaxPoses = FMath::Clamp(MaxPosesOpt.Get(FindClearDefaultMaxPoses),
        1, FindClearMaxAllowedPoses);

    // ---- Obstacle filter ----
    TArray<FString> IgnoreNames;
    FindClearAppendStrings(Ctx.GetArray(TEXT("ignoreActors")), IgnoreNames);
    FindClearAppendStrings(Ctx.GetArray(TEXT("ignore_actors")), IgnoreNames);

    TArray<AActor*> IgnoreActors;
    TArray<TSharedPtr<FJsonValue>> UnresolvedIgnore;
    for (const FString& Name : IgnoreNames)
    {
        if (AActor* Found = McpActorUtils::FindActorByName(World, Name))
        {
            IgnoreActors.AddUnique(Found);
        }
        else
        {
            UnresolvedIgnore.Add(MakeShared<FJsonValueString>(Name));
        }
    }

    SpatialTraceUtils::FSpatialHitFilter ObstacleFilter;
    FindClearAppendStrings(Ctx.GetArray(TEXT("onlyClasses")), ObstacleFilter.OnlyClasses);
    FindClearAppendStrings(Ctx.GetArray(TEXT("only_classes")), ObstacleFilter.OnlyClasses);

    // ---- Keep-out polyline ----
    TArray<FVector> KeepOutPoints;
    double KeepOutRadius = 0.0;
    TSharedPtr<FJsonObject> KeepOutObj = Ctx.GetObject(TEXT("keepOut"));
    if (!KeepOutObj.IsValid())
    {
        KeepOutObj = Ctx.GetObject(TEXT("keep_out"));
    }
    if (KeepOutObj.IsValid())
    {
        KeepOutRadius = GetJsonNumberField(KeepOutObj, TEXT("radius"), 0.0);
        const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
        if (KeepOutObj->TryGetArrayField(TEXT("points"), Points) && Points)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Points)
            {
                const TSharedPtr<FJsonObject>* PointObj = nullptr;
                if (Value.IsValid() && Value->TryGetObject(PointObj) && PointObj)
                {
                    KeepOutPoints.Add(FVector(
                        GetJsonNumberField(*PointObj, TEXT("x"), 0.0),
                        GetJsonNumberField(*PointObj, TEXT("y"), 0.0),
                        GetJsonNumberField(*PointObj, TEXT("z"), 0.0)));
                }
            }
        }
        if (KeepOutRadius > 0.0 && KeepOutPoints.Num() == 0)
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                TEXT("keepOut.radius was given with no keepOut.points; a corridor with no "
                     "polyline would silently gate nothing."));
            return true;
        }
    }
    // Anything nearer the polyline than this could intrude on it. Measured from the pose CENTRE
    // against the footprint's circumscribed radius, which is conservative by construction: it
    // never passes a pose that actually intrudes, and it needs no segment/OBB arithmetic of its
    // own. FMath::PointDistToSegment clamps to the segment - the hand-rolled version of this
    // measures to the infinite LINE and reads 0 everywhere.
    const double KeepOutThreshold = KeepOutRadius + CircumRadius3D;

    // ---- Build the XY grid, then check the budget BEFORE any trace runs ----
    TArray<FFindClearColumn> Columns;
    const double SpanX = RegionMax.X - RegionMin.X;
    const double SpanY = RegionMax.Y - RegionMin.Y;
    const int32 CountX = FMath::Max(1, FMath::FloorToInt32(SpanX / Step) + 1);
    const int32 CountY = FMath::Max(1, FMath::FloorToInt32(SpanY / Step) + 1);

    // Guard the ALLOCATION before the grid is materialised. The exact budget check below runs on
    // the built column list, so without this a 1 cm step over a kilometre would build millions of
    // columns first and only then be refused. The 2x headroom covers a sphere region, whose
    // bounding square over-counts the real column set by about 1.28x.
    const int64 GridUpperBound = static_cast<int64>(CountX) * static_cast<int64>(CountY)
        * static_cast<int64>(Yaws.Num());
    if (GridUpperBound > 2 * static_cast<int64>(MaxPoses))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(
                TEXT("The requested grid spans up to %lld poses (%d x %d columns x %d yaws), far "
                     "over the %d-pose budget. Nothing was searched. Raise 'step' (currently "
                     "%.1f cm), shrink 'region', raise 'yawStep' (currently %.1f deg), or raise "
                     "'maxPoses'."),
                GridUpperBound, CountX, CountY, Yaws.Num(), MaxPoses, Step, YawStep));
        return true;
    }

    for (int32 IndexX = 0; IndexX < CountX; ++IndexX)
    {
        const double X = RegionMin.X + IndexX * Step;
        for (int32 IndexY = 0; IndexY < CountY; ++IndexY)
        {
            const double Y = RegionMin.Y + IndexY * Step;
            if (bSphereRegion)
            {
                const double DeltaX = X - RegionCenter.X;
                const double DeltaY = Y - RegionCenter.Y;
                if (DeltaX * DeltaX + DeltaY * DeltaY > RegionRadius * RegionRadius)
                {
                    continue;
                }
            }
            FFindClearColumn Column;
            Column.X = X;
            Column.Y = Y;
            Columns.Add(Column);
        }
    }

    const int64 PlannedPoses = static_cast<int64>(Columns.Num()) * static_cast<int64>(Yaws.Num());
    if (PlannedPoses > static_cast<int64>(MaxPoses))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(
                TEXT("The requested grid is %lld poses (%d columns x %d yaws), over the %d-pose "
                     "budget. Nothing was searched: a partial sweep would report 'no room' for a "
                     "region it never looked at. Raise 'step' (currently %.1f cm), shrink "
                     "'region', raise 'yawStep' (currently %.1f deg), or raise 'maxPoses'."),
                PlannedPoses, Columns.Num(), Yaws.Num(), MaxPoses, Step, YawStep));
        return true;
    }

    // ---- Ground per column: 5 probes (centre plus the footprint's circumscribed extremes),
    //      seated on the HIGHEST hit so a box on a slope rests on the high side instead of
    //      cutting into it. Yaw-independent, so it is solved once per column. ----
    if (bSeatOnGround)
    {
        const FVector ProbeOffsets[5] = {
            FVector::ZeroVector,
            FVector(CircumRadiusXY, 0.0, 0.0),
            FVector(-CircumRadiusXY, 0.0, 0.0),
            FVector(0.0, CircumRadiusXY, 0.0),
            FVector(0.0, -CircumRadiusXY, 0.0)
        };
        for (FFindClearColumn& Column : Columns)
        {
            for (const FVector& Offset : ProbeOffsets)
            {
                const FVector Start(Column.X + Offset.X, Column.Y + Offset.Y, RegionMax.Z);
                const FVector End(Start.X, Start.Y, RegionMin.Z);
                const SpatialTraceUtils::FSpatialHit Hit = SpatialTraceUtils::TraceLine(
                    World, Start, End, ECC_Visibility, /*bTraceComplex=*/false, IgnoreActors);
                if (!Hit.bHit)
                {
                    continue;
                }
                if (AActor* GroundActor = Hit.HitActor.Get())
                {
                    Column.GroundActors.AddUnique(GroundActor);
                }
                if (!Column.bHasGround || Hit.Location.Z > Column.GroundZ)
                {
                    Column.bHasGround = true;
                    Column.GroundZ = Hit.Location.Z;
                    Column.GroundLocation = Hit.Location;
                    Column.GroundActorName =
                        Hit.HitActor.IsValid() ? Hit.HitActor->GetActorLabel() : FString();
                }
            }
        }
    }

    // ---- Sweep ----
    TArray<FFindClearPose> ClearPoses;
    TArray<FFindClearBinderTally> Binders;
    int32 RejectedOccupied = 0;
    int32 RejectedClearance = 0;
    int32 RejectedKeepOut = 0;
    int32 RejectedNoGround = 0;

    SpatialTraceUtils::FSpatialFootprintProbe Probe;
    Probe.HalfExtent = HalfExtent;
    Probe.Channel = ECC_Visibility;
    Probe.Filter = ObstacleFilter;

    for (const FFindClearColumn& Column : Columns)
    {
        if (bSeatOnGround && !Column.bHasGround)
        {
            RejectedNoGround += Yaws.Num();
            continue;
        }

        const FVector Center(Column.X, Column.Y,
            bSeatOnGround ? (Column.GroundZ + FindClearGroundLiftCm + HalfExtent.Z)
                          : RegionCenter.Z);

        // Keep-out is yaw-independent (measured from the centre against the circumscribed
        // radius), so it is decided once per column rather than once per pose.
        if (KeepOutPoints.Num() > 0)
        {
            double NearestCm = TNumericLimits<double>::Max();
            if (KeepOutPoints.Num() == 1)
            {
                NearestCm = FVector::Dist(Center, KeepOutPoints[0]);
            }
            else
            {
                for (int32 Index = 0; Index + 1 < KeepOutPoints.Num(); ++Index)
                {
                    NearestCm = FMath::Min(NearestCm,
                        static_cast<double>(FMath::PointDistToSegment(
                            Center, KeepOutPoints[Index], KeepOutPoints[Index + 1])));
                }
            }
            if (NearestCm < KeepOutThreshold)
            {
                RejectedKeepOut += Yaws.Num();
                continue;
            }
        }

        Probe.Center = Center;
        Probe.IgnoreActors = IgnoreActors;
        for (AActor* GroundActor : Column.GroundActors)
        {
            Probe.IgnoreActors.AddUnique(GroundActor);
        }

        for (const double Yaw : Yaws)
        {
            Probe.Rotation = FRotator(0.0, Yaw, 0.0).Quaternion();
            const SpatialTraceUtils::FSpatialClearanceResult Clearance =
                SpatialTraceUtils::MeasureFootprintClearance(World, Probe, MinClearance,
                    ClearanceProbe, FindClearBisectionSteps);

            // bMeetsMinimum, never `ClearanceCm >= MinClearance`: the reported clearance is a
            // bisected LOWER bound, so comparing it against the threshold would reject a pose
            // whose real clearance is exactly the threshold.
            if (!Clearance.bMeetsMinimum)
            {
                if (Clearance.bClear) { ++RejectedClearance; } else { ++RejectedOccupied; }
                if (Clearance.Binders.Num() > 0)
                {
                    FindClearRecordBinder(Binders, Clearance.Binders[0]);
                }
                continue;
            }

            FFindClearPose Pose;
            Pose.Location = Center;
            Pose.Yaw = Yaw;
            Pose.ClearanceCm = Clearance.ClearanceCm;
            Pose.bClearanceCapped = Clearance.bCapped;
            Pose.DistanceFromCenterCm = FVector::Dist(Center, RegionCenter);
            Pose.bHasGround = Column.bHasGround;
            Pose.GroundLocation = Column.GroundLocation;
            Pose.GroundActor = Column.GroundActorName;
            if (Clearance.Binders.Num() > 0)
            {
                Pose.bHasBinder = true;
                Pose.Binder = Clearance.Binders[0];
            }
            ClearPoses.Add(MoveTemp(Pose));
        }
    }

    const int32 FoundCount = ClearPoses.Num();
    const int32 Evaluated = static_cast<int32>(PlannedPoses);

    if (Rank == TEXT("distance"))
    {
        ClearPoses.Sort([](const FFindClearPose& A, const FFindClearPose& B)
        {
            if (!FMath::IsNearlyEqual(A.DistanceFromCenterCm, B.DistanceFromCenterCm))
            {
                return A.DistanceFromCenterCm < B.DistanceFromCenterCm;
            }
            return A.ClearanceCm > B.ClearanceCm;
        });
    }
    else
    {
        ClearPoses.Sort([](const FFindClearPose& A, const FFindClearPose& B)
        {
            if (!FMath::IsNearlyEqual(A.ClearanceCm, B.ClearanceCm))
            {
                return A.ClearanceCm > B.ClearanceCm;
            }
            return A.DistanceFromCenterCm < B.DistanceFromCenterCm;
        });
    }

    TArray<TSharedPtr<FJsonValue>> PoseValues;
    const int32 EmitPoses = FMath::Min(MaxResults, ClearPoses.Num());
    for (int32 Index = 0; Index < EmitPoses; ++Index)
    {
        const FFindClearPose& Pose = ClearPoses[Index];
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetObjectField(TEXT("location"), MeasureMakeVectorObject(Pose.Location));
        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), 0.0);
        Rotation->SetNumberField(TEXT("yaw"), Pose.Yaw);
        Rotation->SetNumberField(TEXT("roll"), 0.0);
        Obj->SetObjectField(TEXT("rotation"), Rotation);
        Obj->SetNumberField(TEXT("clearanceCm"), Pose.ClearanceCm);
        Obj->SetBoolField(TEXT("clearanceCapped"), Pose.bClearanceCapped);
        Obj->SetNumberField(TEXT("distanceFromCenterCm"), Pose.DistanceFromCenterCm);
        if (Pose.bHasGround)
        {
            Obj->SetObjectField(TEXT("groundLocation"),
                MeasureMakeVectorObject(Pose.GroundLocation));
            if (!Pose.GroundActor.IsEmpty())
            {
                Obj->SetStringField(TEXT("groundActor"), Pose.GroundActor);
            }
        }
        if (Pose.bHasBinder)
        {
            // What ends this pose's clearance. Present even on an ACCEPTED pose, because "it
            // fits, and the nearest thing to it is that HISM instance" is the actionable form.
            Obj->SetObjectField(TEXT("clearanceBinder"), FindClearOccupantObject(Pose.Binder));
        }
        PoseValues.Add(MakeShared<FJsonValueObject>(Obj));
    }

    Binders.Sort([](const FFindClearBinderTally& A, const FFindClearBinderTally& B)
    {
        return A.BlockedPoses > B.BlockedPoses;
    });
    TArray<TSharedPtr<FJsonValue>> BinderValues;
    const int32 EmitBinders = FMath::Min(FindClearMaxReportedBinders, Binders.Num());
    for (int32 Index = 0; Index < EmitBinders; ++Index)
    {
        const FFindClearBinderTally& Tally = Binders[Index];
        SpatialTraceUtils::FSpatialOccupant Occupant;
        Occupant.Actor = Tally.Actor;
        Occupant.Component = Tally.Component;
        Occupant.bInstanced = Tally.bInstanced;
        TSharedPtr<FJsonObject> Obj = FindClearOccupantObject(Occupant);
        Obj->SetNumberField(TEXT("blockedPoses"), Tally.BlockedPoses);
        if (Tally.bInstanced)
        {
            TArray<TSharedPtr<FJsonValue>> Indices;
            for (const int32 InstanceIndex : Tally.InstanceIndices)
            {
                Indices.Add(MakeShared<FJsonValueNumber>(InstanceIndex));
            }
            Obj->SetArrayField(TEXT("instanceIndices"), Indices);
            if (const UInstancedStaticMeshComponent* Instanced =
                    Cast<UInstancedStaticMeshComponent>(Tally.Component.Get()))
            {
                Obj->SetNumberField(TEXT("instanceCount"), Instanced->GetInstanceCount());
            }
        }
        BinderValues.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> FootprintOut = MakeShared<FJsonObject>();
    FootprintOut->SetNumberField(TEXT("width"), Width);
    FootprintOut->SetNumberField(TEXT("depth"), Depth);
    FootprintOut->SetNumberField(TEXT("height"), Height);
    FootprintOut->SetStringField(TEXT("source"), FootprintSource);
    if (FootprintSource == TEXT("assetPath"))
    {
        FootprintOut->SetStringField(TEXT("assetPath"), AssetPath);
    }

    TSharedPtr<FJsonObject> RegionOut = MakeShared<FJsonObject>();
    RegionOut->SetStringField(TEXT("mode"), bSphereRegion ? TEXT("sphere") : TEXT("box"));
    RegionOut->SetObjectField(TEXT("center"), MeasureMakeVectorObject(RegionCenter));
    RegionOut->SetObjectField(TEXT("min"), MeasureMakeVectorObject(RegionMin));
    RegionOut->SetObjectField(TEXT("max"), MeasureMakeVectorObject(RegionMax));
    if (bSphereRegion)
    {
        RegionOut->SetNumberField(TEXT("radius"), RegionRadius);
    }

    TSharedPtr<FJsonObject> Search = MakeShared<FJsonObject>();
    Search->SetNumberField(TEXT("step"), Step);
    Search->SetNumberField(TEXT("yawStep"), YawStep);
    Search->SetNumberField(TEXT("yawCount"), Yaws.Num());
    Search->SetNumberField(TEXT("columns"), Columns.Num());
    Search->SetNumberField(TEXT("minClearance"), MinClearance);
    Search->SetNumberField(TEXT("clearanceProbe"), ClearanceProbe);
    Search->SetBoolField(TEXT("seatOnGround"), bSeatOnGround);
    Search->SetStringField(TEXT("rank"), Rank);
    if (KeepOutPoints.Num() > 0)
    {
        Search->SetNumberField(TEXT("keepOutPoints"), KeepOutPoints.Num());
        Search->SetNumberField(TEXT("keepOutThresholdCm"), KeepOutThreshold);
    }

    TSharedPtr<FJsonObject> Rejected = MakeShared<FJsonObject>();
    Rejected->SetNumberField(TEXT("total"), Evaluated - FoundCount);
    Rejected->SetNumberField(TEXT("occupied"), RejectedOccupied);
    Rejected->SetNumberField(TEXT("clearance"), RejectedClearance);
    Rejected->SetNumberField(TEXT("keepOut"), RejectedKeepOut);
    Rejected->SetNumberField(TEXT("noGround"), RejectedNoGround);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("found"), FoundCount);
    Data->SetNumberField(TEXT("evaluated"), Evaluated);
    Data->SetObjectField(TEXT("footprint"), FootprintOut);
    Data->SetObjectField(TEXT("region"), RegionOut);
    Data->SetObjectField(TEXT("search"), Search);
    Data->SetArrayField(TEXT("poses"), PoseValues);
    Data->SetArrayField(TEXT("binders"), BinderValues);
    Data->SetNumberField(TEXT("binderCount"), Binders.Num());
    Data->SetObjectField(TEXT("rejected"), Rejected);
    if (UnresolvedIgnore.Num() > 0)
    {
        Data->SetArrayField(TEXT("unresolvedIgnoreActors"), UnresolvedIgnore);
    }
    MeasureAddAxisEcho(Data);

    Ctx.SendSuccess(Data);
    return true;
}
