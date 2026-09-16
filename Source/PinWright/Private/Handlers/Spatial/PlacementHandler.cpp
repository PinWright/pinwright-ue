// Copyright (c) 2026 Alexander Penkin. MIT License.

// PlacementHandler.cpp - mutating spatial-placement RPCs:
//   spatial.place_on_surface - raycast a target surface, then rest an EXISTING actor ON
//                              it, pivot-corrected via the actor's bounds half-extent so
//                              it never sinks into the surface.
//   spatial.place_relative   - place actor B by relation to an anchor A (on_top_of,
//                              left_of, in_front_of, ...) using pure world-AABB arithmetic.
//
// Both are the payoff of the spatial loop (raycast -> measure -> place): the agent states
// intent and the solver computes the transform. They read world-space AABBs the same way
// actor.get_bounding_box / spatial.measure_* do (GetActorBounds(false,...) -> FBox), apply
// the move with Modify() + SetActorTransform(TeleportPhysics) like actor.set_transform, and
// return an INLINE verification block (resting gap / edge gap / overlap) computed from the
// post-move bounds so a caller does not need a follow-up verify_placement round-trip.
// Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "Handlers/Render/ViewProjectionUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/JsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Compat/EngineVersionCompat.h"
#if UE_VERSION_OLDER_THAN(5, 4, 0)
// UE 5.3: Editor/ActorPositioning.h is a private UnrealEd header (made public in 5.4), so the
// surface-aligned transform's no-ActorFactory path is replicated inline below from the viewport
// snap settings it reads.
#include "Settings/LevelEditorViewportSettings.h"
#else
#include "Editor/ActorPositioning.h"
#endif

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Math/Box.h"

namespace
{
    // A tiny lift used to start the worldPoint downward trace just above the given point
    // so a surface that sits exactly at that point is still hit (a trace that starts on
    // the surface can register a zero-distance self-miss).
    constexpr double PlacementWorldPointLift = 50.0;

    // Downward-trace reach for worldPoint / dropDown probes (cm). Large enough to clear a
    // dropped actor's start height without inviting far-away geometry into a real scene.
    constexpr double PlacementMaxDrop = 1.0e6;

    // Forward reach of the screen-deprojected ray (cm), matching spatial.raycast's default.
    constexpr double PlacementScreenRayLength = 1.0e7;

    // Vertical/lateral slack when deciding "overlapping": a clean rest touches the surface
    // exactly (inclusive FBox::Intersect would call that an overlap), so the placed box is
    // shrunk by this before intersecting. Only penetration deeper than this reads as overlap.
    constexpr double PlacementOverlapEps = 0.1;

    // Default grid used when snapGrid is requested without a positive gridSize.
    constexpr double PlacementDefaultGridSize = 10.0;

    // How far the actor's post-move reported location may sit from the location computed for it
    // before `placed` is reported false. Well inside anything visible and well outside float
    // noise on a 100 km world - the same reasoning as GroundPlacement's MaxSeatErrorCm (1 cm).
    constexpr double PlacementReadbackToleranceCm = 0.01;

    TSharedPtr<FJsonObject> PlacementMakeVectorObject(const FVector& Vec)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), Vec.X);
        Obj->SetNumberField(TEXT("y"), Vec.Y);
        Obj->SetNumberField(TEXT("z"), Vec.Z);
        return Obj;
    }

    TSharedPtr<FJsonObject> PlacementMakeRotatorObject(const FRotator& Rot)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("pitch"), Rot.Pitch);
        Obj->SetNumberField(TEXT("yaw"), Rot.Yaw);
        Obj->SetNumberField(TEXT("roll"), Rot.Roll);
        return Obj;
    }

    // Coordinate-convention echo attached to every placement result (mirrors RaycastHandler
    // / MeasureHandler) so a caller never has to guess units/handedness.
    void PlacementAddAxisEcho(const TSharedPtr<FJsonObject>& Data)
    {
        Data->SetStringField(TEXT("units"), TEXT("cm"));
        Data->SetStringField(TEXT("axis"), TEXT("+X fwd, +Y right, +Z up, left-handed"));
    }

    // World-space AABB of an actor, built the same way actor.get_bounding_box /
    // spatial.measure_* do so all the spatial numbers agree.
    FBox PlacementActorBox(AActor* Actor)
    {
        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent);
        return FBox(Origin - Extent, Origin + Extent);
    }

    // {location,rotation,scale} echo of an actor's current world transform.
    TSharedPtr<FJsonObject> PlacementTransformObject(AActor* Actor)
    {
        const FTransform Xf = Actor->GetActorTransform();
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetObjectField(TEXT("location"), PlacementMakeVectorObject(Xf.GetLocation()));
        Obj->SetObjectField(TEXT("rotation"), PlacementMakeRotatorObject(Xf.GetRotation().Rotator()));
        Obj->SetObjectField(TEXT("scale"), PlacementMakeVectorObject(Xf.GetScale3D()));
        return Obj;
    }

    // Editor world by default; PIE world when a play session is active (mirrors
    // RaycastResolveWorld) so a placement targets the same actors lookups resolve against.
    UWorld* PlacementResolveWorld()
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

    // Apply a computed transform to an actor the same way actor.set_transform does:
    // Modify() for undo/transaction, teleport (no physics interpolation), and flag the
    // render state + package dirty.
    void PlacementApplyTransform(AActor* Actor, const FVector& Location, const FQuat& Rotation)
    {
        Actor->Modify();
        Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
        Actor->SetActorRotation(Rotation, ETeleportType::TeleportPhysics);
        Actor->MarkComponentsRenderStateDirty();
        Actor->MarkPackageDirty();
    }

    void PlacementApplyLocation(AActor* Actor, const FVector& Location)
    {
        Actor->Modify();
        Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
        Actor->MarkComponentsRenderStateDirty();
        Actor->MarkPackageDirty();
    }

    // True when the moved actor penetrates the surface/anchor box beyond the touch slack.
    bool PlacementBoxesOverlap(const FBox& MovedBox, const FBox& OtherBox)
    {
        return MovedBox.ExpandBy(-PlacementOverlapEps).Intersect(OtherBox);
    }
}

// ---- spatial.place_on_surface ----
REGISTER_RPC_HANDLER("spatial.place_on_surface", "spatial",
    "Rest an EXISTING actor ON a surface. Raycast the target surface (via screen pixel, a "
    "world point traced straight down, or dropDown from the actor's current position), then "
    "place the actor so its bounds bottom sits on the hit - pivot-corrected by the actor's "
    "bounds half-extent so it never sinks. Optionally align the actor's up-axis to the surface "
    "normal, add an offset along the normal, and snap to a grid. Returns the applied transform, "
    "the hit surface, and an inline verification (restingGap ~ 0 when flush; overlapping). "
    "A trace that hits nothing is an error in every mode - SURFACE_NOT_FOUND for screen/dropDown, "
    "SURFACE_TRACE_MISSED for worldPoint - and nothing is moved; the verb no longer treats the "
    "caller's own point as a surface unless assumePointIsSurface says to. "
    "SUPERSEDED for terrain work by spatial.ground_actors, which samples the actor's footprint "
    "instead of one point, refuses an actor whose footprint overhangs the ground, and re-measures "
    "after the move; this verb still rests on ONE point at the actor's bounds plane, so a boulder "
    "on a slope comes to balance on a corner. "
    "Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Display label / internal name / path of the actor to place." ACTORNAME_COLLISION_STEER)),
        RPC_PARAM_OPT("screen", "object",
            "Screen-pixel target: {x, y, location, rotation, fov?, orthoWidth?, projectionMode?, "
            "width, height}. orthoWidth is world centimetres (default 2000) and an orthographic "
            "rotation must look along a world axis, exactly as the capture required. Deprojects "
            "pixel (x,y) through the given viewport pose to a ray, then traces the scene. Provide "
            "one of screen / worldPoint / dropDown."),
        FParamSpec{TEXT("worldPoint"), TEXT("object"),
            TEXT("World point {x,y,z} in cm; a straight-down trace from just above it finds the real "
                 "surface + normal. Nothing below the point is SURFACE_TRACE_MISSED and no move - "
                 "see assumePointIsSurface for the opt-in that keeps the old behaviour. "
                 "Provide one of screen / worldPoint / dropDown."),
            false, TEXT(""), TArray<FString>({TEXT("world_point")})},
        FParamSpec{TEXT("assumePointIsSurface"), TEXT("boolean"),
            TEXT("worldPoint mode only. When the downward trace hits nothing, treat the caller's own "
                 "point as the surface (up-normal) instead of failing. Default false. The result then "
                 "carries surface.measured:false so the invented surface is never mistaken for a "
                 "measured one - this is the fallback that used to be silent and put props beyond "
                 "the map edge, so it is opt-in per call and reported when taken."),
            false, TEXT(""), TArray<FString>({TEXT("assume_point_is_surface")})},
        FParamSpec{TEXT("dropDown"), TEXT("boolean"),
            TEXT("When true, drop the actor straight down onto the surface below its current position. "
                 "Provide one of screen / worldPoint / dropDown."),
            false, TEXT(""), TArray<FString>({TEXT("drop_down")})},
        FParamSpec{TEXT("alignToNormal"), TEXT("boolean"),
            TEXT("Align the actor's up-axis (+Z) to the surface normal. Default false (keep current rotation)."),
            false, TEXT(""), TArray<FString>({TEXT("align_to_normal")})},
        RPC_PARAM_OPT("offset", "number",
            "Extra offset along the surface normal in cm after resting (e.g. hover the actor above the surface). Default 0."),
        FParamSpec{TEXT("snapGrid"), TEXT("boolean"),
            TEXT("Snap the final location to a grid. Default false."),
            false, TEXT(""), TArray<FString>({TEXT("snap_grid")})},
        FParamSpec{TEXT("gridSize"), TEXT("number"),
            TEXT("Grid cell size in cm used when snapGrid is true (default 10)."),
            false, TEXT(""), TArray<FString>({TEXT("grid_size")})}
    ))
{
    FString ActorName;
    if (!ActorNameParamUtils::RequireActorName(Ctx, ActorName))
    {
        return true;
    }

    UWorld* World = PlacementResolveWorld();
    if (!World)
    {
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"),
            TEXT("No editor/PIE world available for the placement."));
        return true;
    }

    AActor* Found = McpActorUtils::FindActorByName(World, ActorName);
    if (!Found)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("Actor not found: '%s'"), *ActorName));
        return true;
    }

    // Pre-move world AABB (half-extent lifts the pivot so the actor rests ON the surface).
    FVector BoundsOrigin = FVector::ZeroVector;
    FVector Extent = FVector::ZeroVector;
    Found->GetActorBounds(false, BoundsOrigin, Extent);

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // Resolve which target mode the caller asked for.
    TSharedPtr<FJsonObject> ScreenObj = Ctx.GetObject(TEXT("screen"));
    const bool bHasWorldPoint =
        Payload.IsValid() && (Payload->HasField(TEXT("worldPoint")) || Payload->HasField(TEXT("world_point")));
    const bool bDropDown = Ctx.GetBoolFirstOf({TEXT("dropDown"), TEXT("drop_down")}, false);

    // Resolved surface (world impact point + unit normal + the actor that was hit).
    FVector SurfaceLocation = FVector::ZeroVector;
    FVector SurfaceNormal = FVector::UpVector;
    AActor* SurfaceActor = nullptr;
    FString ResolvedMode;
    // False on exactly one path: the worldPoint fallback, where no trace ever answered and the
    // "surface" is the caller's own point. Set where the surface is established, never later, so
    // the reported flag cannot drift from the branch that produced the number.
    bool bSurfaceMeasured = true;

    if (ScreenObj.IsValid())
    {
        ResolvedMode = TEXT("screen");

        PinWrightRenderCapture::FViewportCaptureRequest View;
        View.Width = GetJsonIntField(ScreenObj, TEXT("width"), 1024);
        View.Height = GetJsonIntField(ScreenObj, TEXT("height"), 1024);
        View.ProjectionMode = GetJsonStringField(ScreenObj, TEXT("projectionMode"), TEXT("perspective")).ToLower();
        View.Location = ExtractVectorField(ScreenObj, TEXT("location"), FVector::ZeroVector);
        View.Rotation = ExtractRotatorField(ScreenObj, TEXT("rotation"), FRotator::ZeroRotator);
        View.Fov = static_cast<float>(GetJsonNumberField(ScreenObj, TEXT("fov"), 50.0));
        View.OrthoWidth = static_cast<float>(GetJsonNumberField(ScreenObj, TEXT("orthoWidth"),
            static_cast<double>(PinWrightRenderCapture::DefaultOrthoWorldWidth)));

        const double PixelX = GetJsonNumberField(ScreenObj, TEXT("x"), View.Width * 0.5);
        const double PixelY = GetJsonNumberField(ScreenObj, TEXT("y"), View.Height * 0.5);

        FVector RayOrigin = FVector::ZeroVector;
        FVector RayDir = FVector::ForwardVector;
        FString DeprojectErr;
        if (!PinWrightViewProjection::DeprojectScreenToWorld(
                View, static_cast<float>(PixelX), static_cast<float>(PixelY), RayOrigin, RayDir, DeprojectErr))
        {
            // The util returns "CODE: message"; forward the real code (it can be
            // UNSUPPORTED_ORTHOGRAPHIC_ROTATION as well as a missing-viewport code) rather than
            // reporting every failure as a missing viewport.
            FString DeprojectCode = TEXT("NO_ACTIVE_LEVEL_VIEWPORT");
            int32 ColonIndex = INDEX_NONE;
            if (DeprojectErr.FindChar(TEXT(':'), ColonIndex) && ColonIndex > 0)
            {
                DeprojectCode = DeprojectErr.Left(ColonIndex).TrimStartAndEnd();
            }
            Ctx.SendError(DeprojectCode,
                FString::Printf(TEXT("Could not deproject screen pixel: %s"), *DeprojectErr));
            return true;
        }

        const FVector RayEnd = RayOrigin + RayDir.GetSafeNormal() * PlacementScreenRayLength;
        const SpatialTraceUtils::FSpatialHit Hit =
            SpatialTraceUtils::TraceLine(World, RayOrigin, RayEnd, ECC_Visibility, /*bTraceComplex=*/false, { Found });
        if (!Hit.bHit)
        {
            Ctx.SendError(TEXT("SURFACE_NOT_FOUND"),
                TEXT("Screen ray hit no surface (the placed actor is ignored so it can't hit itself)."));
            return true;
        }
        SurfaceLocation = Hit.Location;
        SurfaceNormal = Hit.Normal.GetSafeNormal();
        SurfaceActor = Hit.HitActor.Get();
    }
    else if (bHasWorldPoint)
    {
        ResolvedMode = TEXT("worldPoint");

        FVector WorldPoint = ExtractVectorField(Payload, TEXT("worldPoint"),
            ExtractVectorField(Payload, TEXT("world_point"), FVector::ZeroVector));
        const FVector Start = WorldPoint + FVector(0.0, 0.0, PlacementWorldPointLift);
        const FVector End = WorldPoint - FVector(0.0, 0.0, PlacementMaxDrop);
        const SpatialTraceUtils::FSpatialHit Hit =
            SpatialTraceUtils::TraceLine(World, Start, End, ECC_Visibility, /*bTraceComplex=*/false, { Found });
        if (Hit.bHit)
        {
            SurfaceLocation = Hit.Location;
            SurfaceNormal = Hit.Normal.GetSafeNormal();
            SurfaceActor = Hit.HitActor.Get();
        }
        else if (Ctx.GetBoolFirstOf({TEXT("assumePointIsSurface"), TEXT("assume_point_is_surface")}, false))
        {
            // Opt-in fallback: no surface under the point -> use the point itself with an
            // up-normal. Legitimate when the caller already knows the height it wants (a
            // hand-authored layout, a point above water). Recorded as unmeasured so the
            // response can say which it was.
            SurfaceLocation = WorldPoint;
            SurfaceNormal = FVector::UpVector;
            bSurfaceMeasured = false;
        }
        else
        {
            // The trace found nothing and the caller did not ask for the point to stand in for
            // a surface. This used to invent the surface silently and answer placed:true, which
            // is why props ended up beyond the map edge, above holes and over water - exactly
            // where a downward trace has nothing to hit. Refuse instead, and move nothing.
            Ctx.SendError(ErrorCodes::ERR_SURFACE_TRACE_MISSED,
                FString::Printf(
                    TEXT("Nothing was hit within %.0f cm below the given worldPoint "
                         "(%.2f, %.2f, %.2f), so there is no measured surface to rest '%s' on. "
                         "The actor was not moved. Pick a point over geometry, use dropDown to "
                         "probe from where the actor already stands, or pass "
                         "assumePointIsSurface:true to place at the point itself - that answer "
                         "comes back with surface.measured:false. For terrain, prefer "
                         "spatial.ground_actors: it samples the whole footprint and refuses an "
                         "actor that overhangs the ground."),
                    PlacementMaxDrop, WorldPoint.X, WorldPoint.Y, WorldPoint.Z,
                    *Found->GetActorLabel()));
            return true;
        }
    }
    else if (bDropDown)
    {
        ResolvedMode = TEXT("dropDown");

        const FBox CurrentBox(BoundsOrigin - Extent, BoundsOrigin + Extent);
        const SpatialTraceUtils::FSpatialHit Hit =
            SpatialTraceUtils::TraceGroundBelow(World, CurrentBox, static_cast<float>(PlacementMaxDrop), { Found });
        if (!Hit.bHit)
        {
            Ctx.SendError(TEXT("SURFACE_NOT_FOUND"),
                FString::Printf(TEXT("No surface found within %.0f cm below '%s'."),
                    PlacementMaxDrop, *Found->GetActorLabel()));
            return true;
        }
        SurfaceLocation = Hit.Location;
        SurfaceNormal = Hit.Normal.GetSafeNormal();
        SurfaceActor = Hit.HitActor.Get();
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("Provide a target surface: one of 'screen' {x,y,...}, 'worldPoint' {x,y,z}, or 'dropDown':true."));
        return true;
    }

    if (SurfaceNormal.IsNearlyZero())
    {
        SurfaceNormal = FVector::UpVector;
    }

    const bool bAlignToNormal = Ctx.GetBoolFirstOf({TEXT("alignToNormal"), TEXT("align_to_normal")}, false);
    const double Offset = Ctx.GetNumber(TEXT("offset"), 0.0);
    const bool bSnapGrid = Ctx.GetBoolFirstOf({TEXT("snapGrid"), TEXT("snap_grid")}, false);
    double GridSize = Ctx.GetNumber(TEXT("gridSize"), Ctx.GetNumber(TEXT("grid_size"), PlacementDefaultGridSize));
    if (GridSize <= 0.0)
    {
        GridSize = PlacementDefaultGridSize;
    }

    // Rotation: optionally tilt the current rotation so the actor's +Z points along the
    // surface normal. FActorPositioning::GetSurfaceAlignedTransform only rotates when an
    // ActorFactory is supplied, so the aligned rotation is computed here and handed in as
    // the start transform (the engine call then just contributes the pivot-lift offset).
    const FQuat CurrentQuat = Found->GetActorQuat();
    FQuat AlignDelta = FQuat::Identity;
    if (bAlignToNormal)
    {
        AlignDelta = FQuat::FindBetweenNormals(FVector::UpVector, SurfaceNormal);
    }
    const FQuat RestQuat = bAlignToNormal ? (AlignDelta * CurrentQuat) : CurrentQuat;

    FTransform StartTransform = Found->GetActorTransform();
    StartTransform.SetRotation(RestQuat);

    // Engine solve: lifts the pivot off the surface along the normal by the bounds
    // half-extent projected onto the normal (BoxPushOut), so a centered-pivot actor rests
    // exactly ON the surface instead of half-buried.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // UE 5.3: FActorPositioning::GetSurfaceAlignedTransform's header is private; replicate its
    // no-ActorFactory path verbatim: offset = normal * max(viewport snap offset, bounds push-out),
    // rotation = the start transform's (already RestQuat via StartTransform above).
    const ULevelEditorViewportSettings* ViewportSettings = GetDefault<ULevelEditorViewportSettings>();
    const double SnapOffsetExtent = ViewportSettings->SnapToSurface.bEnabled
        ? static_cast<double>(ViewportSettings->SnapToSurface.SnapOffsetExtent)
        : 0.0;
    const double CollisionOffsetExtent = FVector::BoxPushOut(SurfaceNormal, Extent);
    const FTransform Rest(
        StartTransform.GetRotation(),
        SurfaceLocation + SurfaceNormal * FMath::Max(SnapOffsetExtent, CollisionOffsetExtent));
#else
    FPositioningData PositioningData(SurfaceLocation, SurfaceNormal);
    PositioningData.UsePlacementExtent(Extent);
    PositioningData.AlignToSurfaceRotation(bAlignToNormal);
    PositioningData.UseStartTransform(StartTransform);
    const FTransform Rest = FActorPositioning::GetSurfaceAlignedTransform(PositioningData);
#endif

    FVector FinalLocation = Rest.GetLocation();

    // Pivot correction for an OFF-center pivot: the engine positions the pivot assuming the
    // bounds are centered on it. Shift so the bounds CENTER lands where the engine put the
    // pivot, so the bounds bottom rests flush regardless of pivot offset (no-op for a
    // centered pivot like the basic Cube).
    FVector PivotToCenter = BoundsOrigin - Found->GetActorLocation();
    if (bAlignToNormal)
    {
        PivotToCenter = AlignDelta.RotateVector(PivotToCenter);
    }
    FinalLocation -= PivotToCenter;

    // Caller offset along the normal (e.g. hover above the surface).
    FinalLocation += SurfaceNormal * Offset;

    // Optional grid snap of the final pivot location.
    if (bSnapGrid)
    {
        FinalLocation.X = FMath::GridSnap(FinalLocation.X, GridSize);
        FinalLocation.Y = FMath::GridSnap(FinalLocation.Y, GridSize);
        FinalLocation.Z = FMath::GridSnap(FinalLocation.Z, GridSize);
    }

    PlacementApplyTransform(Found, FinalLocation, RestQuat);

    // ---- Inline verification from the POST-move bounds ----
    const FBox PlacedBox = PlacementActorBox(Found);
    const FVector PlacedCenter = PlacedBox.GetCenter();
    const FVector PlacedExtent = PlacedBox.GetExtent();
    // Signed distance from the surface to the placed bounds' nearest face along the normal:
    // ~0 when flush, positive when hovering, negative when sunk.
    const double RestingGap =
        FVector::DotProduct(PlacedCenter - SurfaceLocation, SurfaceNormal)
        - FVector::BoxPushOut(SurfaceNormal, PlacedExtent);
    const bool bOverlapping =
        (SurfaceActor != nullptr) && PlacementBoxesOverlap(PlacedBox, PlacementActorBox(SurfaceActor));

    TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
    Surface->SetObjectField(TEXT("location"), PlacementMakeVectorObject(SurfaceLocation));
    Surface->SetObjectField(TEXT("normal"), PlacementMakeVectorObject(SurfaceNormal));
    // The load-bearing field: false means no trace ever answered and `location`/`normal` are the
    // caller's own point with an assumed up-normal, not an observation of the world.
    Surface->SetBoolField(TEXT("measured"), bSurfaceMeasured);
    if (SurfaceActor)
    {
        TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
        ActorObj->SetStringField(TEXT("name"), SurfaceActor->GetActorLabel());
        ActorObj->SetStringField(TEXT("path"), SurfaceActor->GetPathName());
        Surface->SetObjectField(TEXT("actor"), ActorObj);
    }

    TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
    Verification->SetNumberField(TEXT("restingGap"), RestingGap);
    Verification->SetBoolField(TEXT("overlapping"), bOverlapping);
    // restingGap is measured against SurfaceLocation, so on the assumed-surface path it says
    // "the actor sits on the point you named" and nothing about the world. Stated here rather
    // than left for the caller to infer from surface.measured.
    Verification->SetBoolField(TEXT("againstMeasuredSurface"), bSurfaceMeasured);

    // Readback, not a literal: the location the actor actually reports after the move, compared
    // with what was computed for it. This is what fails when something refused the move.
    const FVector PlacedLocation = Found->GetActorLocation();
    const double PlacementErrorCm = FVector::Dist(PlacedLocation, FinalLocation);
    const bool bPlaced = PlacementErrorCm <= PlacementReadbackToleranceCm;
    Verification->SetNumberField(TEXT("placementErrorCm"), PlacementErrorCm);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("placed"), bPlaced);
    if (!bPlaced)
    {
        Data->SetStringField(TEXT("reasonCode"), ErrorCodes::ERR_INVALID_PLACEMENT);
        Data->SetStringField(TEXT("reason"),
            FString::Printf(TEXT("The actor reports a location %.3f cm from the one computed for "
                                 "it, beyond the %.3f cm readback tolerance. Something refused or "
                                 "overrode the move."),
                PlacementErrorCm, PlacementReadbackToleranceCm));
    }
    Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
    Data->SetStringField(TEXT("mode"), ResolvedMode);
    Data->SetObjectField(TEXT("transform"), PlacementTransformObject(Found));
    Data->SetObjectField(TEXT("surface"), Surface);
    Data->SetObjectField(TEXT("verification"), Verification);
    PlacementAddAxisEcho(Data);

    Ctx.SendSuccess(Data);
    return true;
}

// ================= spatial.place_relative =================
namespace
{
    enum class EPlaceAxisAlign : uint8 { Center, Min, Max };

    // Reads one axis alignment ("min"/"center"/"max") from the align object, defaulting
    // to Center when absent or unrecognized.
    EPlaceAxisAlign PlacementReadAlign(const TSharedPtr<FJsonObject>& AlignObj, const TCHAR* Key)
    {
        if (!AlignObj.IsValid())
        {
            return EPlaceAxisAlign::Center;
        }
        FString Mode;
        if (AlignObj->TryGetStringField(Key, Mode))
        {
            Mode = Mode.ToLower();
            if (Mode == TEXT("min")) { return EPlaceAxisAlign::Min; }
            if (Mode == TEXT("max")) { return EPlaceAxisAlign::Max; }
        }
        return EPlaceAxisAlign::Center;
    }

    // Target center coordinate for a NON-stacking axis given the anchor's span on that
    // axis and B's half-size: center on the anchor, or flush its min/max face.
    double PlacementAlignedCenter(EPlaceAxisAlign Mode, double AnchorMin, double AnchorMax,
        double AnchorCenter, double BHalf)
    {
        switch (Mode)
        {
            case EPlaceAxisAlign::Min: return AnchorMin + BHalf;
            case EPlaceAxisAlign::Max: return AnchorMax - BHalf;
            default:                   return AnchorCenter;
        }
    }
}

REGISTER_RPC_HANDLER("spatial.place_relative", "spatial",
    "Place actor B relative to an anchor actor A by world-AABB arithmetic (no raycast). "
    "relation is one of on_top_of | below | left_of | right_of | in_front_of | behind | "
    "centered_on | against_wall; B's relevant face is set `gap` cm from A's opposing face and "
    "the other axes are centered on A (override per axis with align {x,y,z} = min|center|max). "
    "Axis convention is left-handed +X forward, +Y right, +Z up (so left_of is -Y, in_front_of "
    "is +X). Returns the applied transform plus an inline verification (edgeGap ~ gap; overlapping). "
    "Coordinates are unreal units (cm).",
    RPC_PARAMS(
        FParamSpec{TEXT("actor"), TEXT("string"),
            TEXT("Display label / internal name / path of the actor to move (B). Required."),
            true, TEXT(""), TArray<FString>({TEXT("actorName"), TEXT("actorPath"), TEXT("objectPath")})},
        FParamSpec{TEXT("anchor"), TEXT("string"),
            TEXT("Display label / internal name / path of the anchor actor to place relative to (A). Required."),
            true, TEXT(""), TArray<FString>({TEXT("anchorName"), TEXT("anchorPath")})},
        RPC_PARAM_REQ("relation", "string",
            "on_top_of | below | left_of | right_of | in_front_of | behind | centered_on | against_wall."),
        RPC_PARAM_DEF("gap", "number",
            "Gap in cm between B's face and A's opposing face along the relation axis (default 0). Ignored for centered_on.",
            "0"),
        RPC_PARAM_OPT("align", "object",
            "Per-axis override for the non-relation axes: {x,y,z} each 'min'|'center'|'max' (default center).")
    ))
{
    const FString ActorName =
        Ctx.GetStringFirstOf({TEXT("actor"), TEXT("actorName"), TEXT("actorPath"), TEXT("objectPath")});
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"),
            TEXT("Missing required parameter 'actor' (the actor to move)."));
        return true;
    }
    const FString AnchorName =
        Ctx.GetStringFirstOf({TEXT("anchor"), TEXT("anchorName"), TEXT("anchorPath")});
    if (AnchorName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"),
            TEXT("Missing required parameter 'anchor' (the actor to place relative to)."));
        return true;
    }
    const FString Relation = Ctx.GetString(TEXT("relation")).ToLower();
    if (Relation.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"),
            TEXT("Missing required parameter 'relation'."));
        return true;
    }

    AActor* ActorB = McpActorUtils::FindActorByName(nullptr, ActorName);
    if (!ActorB)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("Actor to move not found: '%s'"), *ActorName));
        return true;
    }
    AActor* ActorA = McpActorUtils::FindActorByName(nullptr, AnchorName);
    if (!ActorA)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("Anchor actor not found: '%s'"), *AnchorName));
        return true;
    }

    const double Gap = Ctx.GetNumber(TEXT("gap"), 0.0);
    const TSharedPtr<FJsonObject> AlignObj = Ctx.GetObject(TEXT("align"));
    const EPlaceAxisAlign AlignX = PlacementReadAlign(AlignObj, TEXT("x"));
    const EPlaceAxisAlign AlignY = PlacementReadAlign(AlignObj, TEXT("y"));
    const EPlaceAxisAlign AlignZ = PlacementReadAlign(AlignObj, TEXT("z"));

    const FBox BoxA = PlacementActorBox(ActorA);
    const FBox BoxB = PlacementActorBox(ActorB);
    const FVector AMin = BoxA.Min;
    const FVector AMax = BoxA.Max;
    const FVector ACenter = BoxA.GetCenter();
    const FVector BCenter = BoxB.GetCenter();
    const FVector BHalf = BoxB.GetExtent();

    // Target world center for B. Each branch sets the relation (stacking) axis from A's
    // opposing face + gap + B's half-size, and aligns the other two axes.
    FVector TargetCenter = ACenter;
    int32 StackAxis = -1;   // 0=X 1=Y 2=Z, -1 = none (centered_on)
    bool bPositiveSide = true; // B on A's +face (min-face touches A) vs -face

    if (Relation == TEXT("on_top_of"))
    {
        TargetCenter.Z = AMax.Z + Gap + BHalf.Z;
        TargetCenter.X = PlacementAlignedCenter(AlignX, AMin.X, AMax.X, ACenter.X, BHalf.X);
        TargetCenter.Y = PlacementAlignedCenter(AlignY, AMin.Y, AMax.Y, ACenter.Y, BHalf.Y);
        StackAxis = 2; bPositiveSide = true;
    }
    else if (Relation == TEXT("below"))
    {
        TargetCenter.Z = AMin.Z - Gap - BHalf.Z;
        TargetCenter.X = PlacementAlignedCenter(AlignX, AMin.X, AMax.X, ACenter.X, BHalf.X);
        TargetCenter.Y = PlacementAlignedCenter(AlignY, AMin.Y, AMax.Y, ACenter.Y, BHalf.Y);
        StackAxis = 2; bPositiveSide = false;
    }
    else if (Relation == TEXT("right_of"))
    {
        TargetCenter.Y = AMax.Y + Gap + BHalf.Y;
        TargetCenter.X = PlacementAlignedCenter(AlignX, AMin.X, AMax.X, ACenter.X, BHalf.X);
        TargetCenter.Z = PlacementAlignedCenter(AlignZ, AMin.Z, AMax.Z, ACenter.Z, BHalf.Z);
        StackAxis = 1; bPositiveSide = true;
    }
    else if (Relation == TEXT("left_of"))
    {
        TargetCenter.Y = AMin.Y - Gap - BHalf.Y;
        TargetCenter.X = PlacementAlignedCenter(AlignX, AMin.X, AMax.X, ACenter.X, BHalf.X);
        TargetCenter.Z = PlacementAlignedCenter(AlignZ, AMin.Z, AMax.Z, ACenter.Z, BHalf.Z);
        StackAxis = 1; bPositiveSide = false;
    }
    else if (Relation == TEXT("in_front_of"))
    {
        TargetCenter.X = AMax.X + Gap + BHalf.X;
        TargetCenter.Y = PlacementAlignedCenter(AlignY, AMin.Y, AMax.Y, ACenter.Y, BHalf.Y);
        TargetCenter.Z = PlacementAlignedCenter(AlignZ, AMin.Z, AMax.Z, ACenter.Z, BHalf.Z);
        StackAxis = 0; bPositiveSide = true;
    }
    else if (Relation == TEXT("behind"))
    {
        TargetCenter.X = AMin.X - Gap - BHalf.X;
        TargetCenter.Y = PlacementAlignedCenter(AlignY, AMin.Y, AMax.Y, ACenter.Y, BHalf.Y);
        TargetCenter.Z = PlacementAlignedCenter(AlignZ, AMin.Z, AMax.Z, ACenter.Z, BHalf.Z);
        StackAxis = 0; bPositiveSide = false;
    }
    else if (Relation == TEXT("centered_on"))
    {
        TargetCenter.X = PlacementAlignedCenter(AlignX, AMin.X, AMax.X, ACenter.X, BHalf.X);
        TargetCenter.Y = PlacementAlignedCenter(AlignY, AMin.Y, AMax.Y, ACenter.Y, BHalf.Y);
        TargetCenter.Z = PlacementAlignedCenter(AlignZ, AMin.Z, AMax.Z, ACenter.Z, BHalf.Z);
        StackAxis = -1;
    }
    else if (Relation == TEXT("against_wall"))
    {
        // Stand B against A's front (+X) face, resting on A's base (bottom-aligned Z),
        // centered on A's Y. Treats the anchor as a wall/prop to lean against.
        TargetCenter.X = AMax.X + Gap + BHalf.X;
        TargetCenter.Z = AMin.Z + BHalf.Z;
        TargetCenter.Y = PlacementAlignedCenter(AlignY, AMin.Y, AMax.Y, ACenter.Y, BHalf.Y);
        StackAxis = 0; bPositiveSide = true;
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("Unknown relation '%s'. Valid: on_top_of, below, left_of, right_of, "
                "in_front_of, behind, centered_on, against_wall."), *Relation));
        return true;
    }

    // Move B rigidly so its bounds center lands on TargetCenter (no rotation change).
    const FVector NewLocation = ActorB->GetActorLocation() + (TargetCenter - BCenter);
    PlacementApplyLocation(ActorB, NewLocation);

    // ---- Inline verification from the POST-move bounds ----
    const FBox NewBoxB = PlacementActorBox(ActorB);
    const bool bOverlapping = PlacementBoxesOverlap(NewBoxB, BoxA);

    // Achieved gap between the two boxes' nearest faces along the stacking axis (~ gap).
    double EdgeGap = 0.0;
    if (StackAxis >= 0)
    {
        EdgeGap = bPositiveSide
            ? (NewBoxB.Min[StackAxis] - AMax[StackAxis])
            : (AMin[StackAxis] - NewBoxB.Max[StackAxis]);
    }

    TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
    Verification->SetBoolField(TEXT("overlapping"), bOverlapping);
    Verification->SetNumberField(TEXT("edgeGap"), EdgeGap);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("placed"), true);
    Data->SetStringField(TEXT("actor"), ActorB->GetActorLabel());
    Data->SetStringField(TEXT("anchor"), ActorA->GetActorLabel());
    Data->SetStringField(TEXT("relation"), Relation);
    Data->SetNumberField(TEXT("gap"), Gap);
    Data->SetObjectField(TEXT("transform"), PlacementTransformObject(ActorB));
    Data->SetObjectField(TEXT("verification"), Verification);
    PlacementAddAxisEcho(Data);

    Ctx.SendSuccess(Data);
    return true;
}
