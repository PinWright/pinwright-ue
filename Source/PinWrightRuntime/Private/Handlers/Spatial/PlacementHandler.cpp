// Copyright (c) 2026 Alexander Penkin. MIT License.

// PlacementHandler.cpp (runtime) - spatial.place_relative: place actor B by relation to an
// anchor A (on_top_of, left_of, in_front_of, ...) using pure world-AABB arithmetic, in the
// editor/PIE world or a game process's live world. spatial.place_on_surface stays in the
// editor module's PlacementHandler.cpp (editor viewport deprojection + FActorPositioning).
//
// Bounds are read the same way actor.get_bounding_box / spatial.measure_* do
// (GetActorBounds(false,...) -> FBox), the move is Modify() + SetActorLocation(TeleportPhysics)
// like actor.set_transform, and an INLINE verification block (edge gap / overlap) is computed
// from the post-move bounds. Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Math/Box.h"

namespace
{
    // Vertical/lateral slack when deciding "overlapping": a clean rest touches the surface
    // exactly (inclusive FBox::Intersect would call that an overlap), so the placed box is
    // shrunk by this before intersecting. Only penetration deeper than this reads as overlap.
    constexpr double PlacementOverlapEps = 0.1;

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
    Data->SetStringField(TEXT("actor"), ActorNameParamUtils::ActorLabelOrName(ActorB));
    Data->SetStringField(TEXT("anchor"), ActorNameParamUtils::ActorLabelOrName(ActorA));
    Data->SetStringField(TEXT("relation"), Relation);
    Data->SetNumberField(TEXT("gap"), Gap);
    Data->SetObjectField(TEXT("transform"), PlacementTransformObject(ActorB));
    Data->SetObjectField(TEXT("verification"), Verification);
    PlacementAddAxisEcho(Data);

    Ctx.SendSuccess(Data);
    return true;
}
