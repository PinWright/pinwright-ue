// Copyright (c) 2026 Alexander Penkin. MIT License.

// PrimitiveHandler.cpp - Geometry primitive creation handlers (Phase 18)
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Geometry/GeometryOpWarnings.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Handlers/Geometry/GeometryNameParamUtils.h"
#include "Dom/JsonObject.h"


#include "Components/DynamicMeshComponent.h"
#include "DynamicMeshActor.h"
#include "UDynamicMesh.h"
#include "Editor.h"
#include "Engine/World.h"

// Shared description for the heightSteps param so create_cylinder and create_cone
// stay identical by construction (passed through RPC_PARAM_OPT's TEXT() wrapper).
#define HEIGHT_STEPS_PARAM_DESC "Axial subdivisions along the height (default 1); raise it so a downstream twist/bend/taper has intermediate side-wall loops to displace. Minimum 0 (no intermediate loop); a negative value clamps up and warns."

// Every handler below is a wrapper: read params, call one GeometryOps::Generate* on a transient
// mesh, spawn, echo. The mesh-building half lives in GeometryOps_Primitives.cpp so the .pwmodel
// compiler can call it with no actor in play.
//
// Spawn-transform convention (load-bearing): every Generate* call below receives
// FTransform::Identity, and the requested location/rotation/scale lives ONLY on the actor
// GeometryTarget::Spawn creates. The op's transform parameter bakes into the vertices, so
// passing it in both places double-applies the location and squares the scale — the full
// failure mode is documented on GeometryTarget::Spawn.
//
// Warning convention: every wrapper ends with GeometryOps::AddOpWarnings(Result, Op) immediately
// before SendSuccess. The op's clamps are the reason a caller's `segments: 1` becomes 3 and its
// `numSteps: 0` becomes the verb's default; before that call existed the wrapper dropped every one
// of those notes on the floor and answered with a bare success. `warnings` is emitted only when the op produced
// at least one, so a call that trips no clamp — which is every call the automation suite makes —
// has a byte-identical response. See GeometryOpWarnings.h.
//
// Response-shape convention for the whole create_* family, because it was two shapes until now:
//   - `class` is always "DynamicMeshActor". Only 7 of the 16 create verbs emitted it, so a
//     caller writing one branch over the family had to special-case which primitive it asked
//     for. The 8 that lacked it (cone, capsule, stairs, spiral_stairs, ring, arch, pipe, ramp)
//     now emit it too - purely additive, since none of them emitted a conflicting `class`.
//   - AddActorVerification(Result, NewActor) is always called, so every create verb reports the
//     actor that actually exists rather than the one that was requested. create_ramp and
//     create_procedural_mesh were the last two without it.
// Both go next to the `name` echo, which is where the seven that already had them put them.
//
// geometry.revolve is the 16th member of that family and BOTH sweeps missed it, because they
// keyed on the create_* NAME rather than on "spawns an actor". It takes the same optional `name`
// slot (GeometryNameParamUtils, with the same actorName alias every create verb carries), calls
// the same GeometryTarget::Spawn, and hands back the same kind of new DynamicMeshActor - only the
// spelling differs. It now emits `class` and calls AddActorVerification in the same two places.
// The membership test for this convention is therefore the SPAWN, not the prefix:
// geometry.import_obj / geometry.import_stl (MeshIOHandler.cpp) are the same case in the other
// file and were fixed in the same pass.


// ============================================================================
// create_box
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_box", "geometry", "Create a box/cube dynamic mesh actor. width/height/depth are the local X/Y/Z extents: 'height' is HORIZONTAL (local Y) and 'depth' is the VERTICAL extent (local Z). The response echoes sizeX/sizeY/sizeZ alongside them.",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale {x, y, z}"),
        RPC_PARAM_OPT("width", "number", "Extent along local X (default 100)."),
        RPC_PARAM_OPT("height", "number", "Extent along local Y - HORIZONTAL, not vertical (default 100). The vertical extent is 'depth'."),
        RPC_PARAM_OPT("depth", "number", "Extent along local Z - this is the VERTICAL extent in UE's Z-up world (default 100)."),
        RPC_PARAM_OPT("widthSegments", "number", "Vertices along the local-X edge, not quads (default 1). AppendBox raises the count to 2 internally, so 0, 1 and 2 all build the same unsubdivided 12-triangle box and 3 is the first value that adds a quad. 0 is legal and means no subdivision; a negative value clamps up to 0 and warns. Maximum 256."),
        RPC_PARAM_OPT("heightSegments", "number", "Vertices along the local-Y edge - see widthSegments for the 0/1/2 collapse (default 1)."),
        RPC_PARAM_OPT("depthSegments", "number", "Vertices along the local-Z edge - see widthSegments for the 0/1/2 collapse (default 1).")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedBox"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    // The published width/height/depth translate onto the axis-explicit Size the op takes; the
    // clamp that used to sit here runs inside GenerateBox and writes back, so the echo below is
    // still the effective value.
    GeometryOps::FBoxParams Params;
    Params.Size = FVector(
        Ctx.GetNumber(TEXT("width"), 100.0),
        Ctx.GetNumber(TEXT("height"), 100.0),
        Ctx.GetNumber(TEXT("depth"), 100.0));
    Params.Steps = FIntVector(
        Ctx.GetInt(TEXT("widthSegments"), 1),
        Ctx.GetInt(TEXT("heightSegments"), 1),
        Ctx.GetInt(TEXT("depthSegments"), 1));

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateBox(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetNumberField(TEXT("width"), Params.Size.X);
    Result->SetNumberField(TEXT("height"), Params.Size.Y);
    Result->SetNumberField(TEXT("depth"), Params.Size.Z);
    // Axis-explicit echo alongside the ambiguous legacy names: width/height/depth are
    // local X/Y/Z, so sizeZ (== depth) is the vertical extent. Additive - the three
    // legacy keys are unchanged.
    Result->SetNumberField(TEXT("sizeX"), Params.Size.X);
    Result->SetNumberField(TEXT("sizeY"), Params.Size.Y);
    Result->SetNumberField(TEXT("sizeZ"), Params.Size.Z);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_sphere
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_sphere", "geometry",
    "Create a box-topology sphere dynamic mesh actor: a rounded cube, with no poles and no seam. "
    "Not a lat/long sphere - the polyhedron and the triangle count both differ from "
    "append_sphere_lat_long at the same subdivisions.",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("radius", "number", "Sphere radius (default 50)"),
        RPC_PARAM_OPT("subdivisions", "number", "Quad steps per cube face, the same count on all three axes (default 16). Triangle count is 12*(subdivisions-1)^2. Minimum 2: 1 draws the same 12-triangle cube as 2 and is clamped up with a warning.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedSphere"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FSphereParams Params;
    Params.Radius = Ctx.GetNumber(TEXT("radius"), 50.0);
    Params.Subdivisions = Ctx.GetInt(TEXT("subdivisions"), 16);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateSphere(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetNumberField(TEXT("radius"), Params.Radius);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_cylinder
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_cylinder", "geometry", "Create a cylinder dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("radius", "number", "Cylinder radius (default 50)"),
        RPC_PARAM_OPT("height", "number", "Cylinder height (default 100)"),
        RPC_PARAM_OPT("segments", "number", "Radial segments (default 16). Minimum 3 - below that the engine draws the same triangular prism, so the op clamps up and warns."),
        RPC_PARAM_OPT("heightSteps", "number", HEIGHT_STEPS_PARAM_DESC)
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedCylinder"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FCylinderParams Params;
    Params.Radius = Ctx.GetNumber(TEXT("radius"), 50.0);
    Params.Height = Ctx.GetNumber(TEXT("height"), 100.0);
    Params.RadialSteps = Ctx.GetInt(TEXT("segments"), 16);
    Params.HeightSteps = Ctx.GetInt(TEXT("heightSteps"), 1);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateCylinder(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_cone
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_cone", "geometry", "Create a cone dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("baseRadius", "number", "Base radius (default 50)"),
        RPC_PARAM_OPT("topRadius", "number", "Top radius (default 0)"),
        RPC_PARAM_OPT("height", "number", "Cone height (default 100)"),
        RPC_PARAM_OPT("segments", "number", "Radial segments (default 16). Minimum 3 - below that the engine draws the same triangular prism, so the op clamps up and warns."),
        RPC_PARAM_OPT("heightSteps", "number", HEIGHT_STEPS_PARAM_DESC)
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedCone"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FConeParams Params;
    Params.BaseRadius = Ctx.GetNumber(TEXT("baseRadius"), 50.0);
    Params.TopRadius = Ctx.GetNumber(TEXT("topRadius"), 0.0);
    Params.Height = Ctx.GetNumber(TEXT("height"), 100.0);
    Params.RadialSteps = Ctx.GetInt(TEXT("segments"), 16);
    Params.HeightSteps = Ctx.GetInt(TEXT("heightSteps"), 1);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateCone(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // GetActorLabel(), not the requested Name: GeometryTarget::Spawn suffixes a label that is
    // already taken, so echoing the request reported an actor that does not exist under that
    // name and the caller's next verb, keyed on it, resolved the WRONG actor. This verb and
    // create_capsule were the last two still echoing the request; every sibling reports the
    // assigned label.
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_capsule
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_capsule", "geometry", "Create a capsule dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("radius", "number", "Capsule radius (default 50)"),
        RPC_PARAM_OPT("length", "number", "Capsule length (default 100)"),
        RPC_PARAM_OPT("hemisphereSteps", "number", "Hemisphere arc steps (default 4). Minimum 2 - the arc is a half-profile, so its floor is lower than the radial one; below it the op clamps up and warns."),
        RPC_PARAM_OPT("segments", "number", "Radial segments (default 16). Minimum 3 - below that the engine draws the same triangular prism, so the op clamps up and warns.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedCapsule"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FCapsuleParams Params;
    Params.Radius = Ctx.GetNumber(TEXT("radius"), 50.0);
    Params.Length = Ctx.GetNumber(TEXT("length"), 100.0);
    Params.HemisphereSteps = Ctx.GetInt(TEXT("hemisphereSteps"), 4);
    Params.RadialSteps = Ctx.GetInt(TEXT("segments"), 16);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateCapsule(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // GetActorLabel(), not the requested Name - see create_cone above for the collision case
    // this reports honestly.
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_torus
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_torus", "geometry", "Create a torus dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("majorRadius", "number", "Major radius (default 50)"),
        RPC_PARAM_OPT("minorRadius", "number", "Minor radius (default 20)"),
        RPC_PARAM_OPT("majorSegments", "number", "Steps around the major ring (default 16). Minimum 3 - a torus revolves a full 360 and a closed sweep at 2 steps cannot close (16 triangles, 16 boundary edges, not closed), so 2 clamps up and warns. create_arch takes 2 below 360, where the sweep stays open."),
        RPC_PARAM_OPT("minorSegments", "number", "Steps around the tube profile (default 8). Minimum 3 - NOT the same floor as majorSegments. Either below its floor clamps up and warns.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedTorus"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FTorusParams Params;
    Params.MajorRadius = Ctx.GetNumber(TEXT("majorRadius"), 50.0);
    Params.MinorRadius = Ctx.GetNumber(TEXT("minorRadius"), 20.0);
    Params.MajorSteps = Ctx.GetInt(TEXT("majorSegments"), 16);
    Params.MinorSteps = Ctx.GetInt(TEXT("minorSegments"), 8);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateTorus(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_plane
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_plane", "geometry", "Create a plane/rectangle dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("width", "number", "Plane width (default 100)"),
        RPC_PARAM_OPT("depth", "number", "Plane depth (default 100)"),
        RPC_PARAM_OPT("widthSubdivisions", "number", "Interior subdivisions along local X (default 1). Minimum 0 - a single quad with no interior loop, and a legal value, so unlike the radial counts 0 is honoured rather than replaced by the default. A negative clamps up to 0 and anything above 256 caps there; both warn."),
        RPC_PARAM_OPT("depthSubdivisions", "number", "Interior subdivisions along local Y (default 1). Minimum 0 - a single quad with no interior loop, and a legal value, so unlike the radial counts 0 is honoured rather than replaced by the default. A negative clamps up to 0 and anything above 256 caps there; both warn.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedPlane"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FPlaneParams Params;
    Params.Size = FVector2D(
        Ctx.GetNumber(TEXT("width"), 100.0),
        Ctx.GetNumber(TEXT("depth"), 100.0));
    Params.Steps = FIntPoint(
        Ctx.GetInt(TEXT("widthSubdivisions"), 1),
        Ctx.GetInt(TEXT("depthSubdivisions"), 1));

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GeneratePlane(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_disc
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_disc", "geometry", "Create a disc dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("radius", "number", "Disc radius (default 50)"),
        RPC_PARAM_OPT("segments", "number", "Segments around the rim (default 16). Minimum 3 - below that it is one triangle, so the op clamps up and warns.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedDisc"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FDiscParams Params;
    Params.Radius = Ctx.GetNumber(TEXT("radius"), 50.0);
    Params.AngleSteps = Ctx.GetInt(TEXT("segments"), 16);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateDisc(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_stairs
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_stairs", "geometry", "Create a linear staircase dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("stepWidth", "number", "Step width (default 100)"),
        RPC_PARAM_OPT("stepHeight", "number", "Step height (default 20)"),
        RPC_PARAM_OPT("stepDepth", "number", "Step depth (default 30)"),
        RPC_PARAM_OPT("numSteps", "number", "Number of steps (default 8). The engine floors it at 1 and says nothing, so the op clamps first: 0 or negative reads as unset and takes the default 8, and anything above 400 caps there. Either way it warns and the response echoes the EFFECTIVE count. 400 is the stairs' own ceiling, not the 256 segment ceiling: a solid staircase costs 2*n^2 + 10*n triangles, so 400 steps is 324,000 and 512 would breach the module's 500,000-triangle limit."),
        RPC_PARAM_OPT("floating", "boolean", "Floating stairs (default false)")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedStairs"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FStairsParams Params;
    Params.StepWidth = Ctx.GetNumber(TEXT("stepWidth"), 100.0f);
    Params.StepHeight = Ctx.GetNumber(TEXT("stepHeight"), 20.0f);
    Params.StepDepth = Ctx.GetNumber(TEXT("stepDepth"), 30.0f);
    Params.NumSteps = Ctx.GetInt(TEXT("numSteps"), 8);
    Params.bFloating = Ctx.GetBool(TEXT("floating"), false);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateStairs(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    // Params.NumSteps is the EFFECTIVE count: GenerateStairs clamps through the Params&
    // reference, so this echo no longer reports the 0 the caller passed for a staircase the
    // engine floored to 1 step.
    Result->SetNumberField(TEXT("numSteps"), Params.NumSteps);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_spiral_stairs
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_spiral_stairs", "geometry", "Create a curved/spiral staircase dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("stepWidth", "number", "Step width (default 100)"),
        RPC_PARAM_OPT("stepHeight", "number", "Step height (default 20)"),
        RPC_PARAM_OPT("innerRadius", "number", "Inner radius (default 150)"),
        RPC_PARAM_OPT("curveAngle", "number", "Total curve angle (default 90)"),
        RPC_PARAM_OPT("numSteps", "number", "Number of steps (default 8). The engine floors it at 1 and says nothing, so the op clamps first: 0 or negative reads as unset and takes the default 8, and anything above 400 caps there. Either way it warns and the response echoes the EFFECTIVE count. 400 is the stairs' own ceiling, not the 256 segment ceiling: a solid staircase costs 2*n^2 + 10*n triangles, so 400 steps is 324,000 and 512 would breach the module's 500,000-triangle limit."),
        RPC_PARAM_OPT("floating", "boolean", "Floating stairs (default false)")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedSpiralStairs"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FSpiralStairsParams Params;
    Params.StepWidth = Ctx.GetNumber(TEXT("stepWidth"), 100.0f);
    Params.StepHeight = Ctx.GetNumber(TEXT("stepHeight"), 20.0f);
    Params.InnerRadius = Ctx.GetNumber(TEXT("innerRadius"), 150.0f);
    Params.CurveAngle = Ctx.GetNumber(TEXT("curveAngle"), 90.0f);
    Params.NumSteps = Ctx.GetInt(TEXT("numSteps"), 8);
    Params.bFloating = Ctx.GetBool(TEXT("floating"), false);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateSpiralStairs(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    // Effective, not requested — see create_stairs.
    Result->SetNumberField(TEXT("numSteps"), Params.NumSteps);
    Result->SetNumberField(TEXT("curveAngle"), Params.CurveAngle);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_ring
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_ring", "geometry", "Create a ring (disc with hole) dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("outerRadius", "number", "Outer radius (default 50)"),
        RPC_PARAM_OPT("innerRadius", "number", "Inner radius (default 25)"),
        RPC_PARAM_OPT("segments", "number", "Segments around the rim (default 32). Minimum 3 - below that it is one quad, so the op clamps up and warns.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedRing"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FRingParams Params;
    Params.OuterRadius = Ctx.GetNumber(TEXT("outerRadius"), 50.0);
    Params.InnerRadius = Ctx.GetNumber(TEXT("innerRadius"), 25.0);
    Params.AngleSteps = Ctx.GetInt(TEXT("segments"), 32);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateRing(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetNumberField(TEXT("outerRadius"), Params.OuterRadius);
    Result->SetNumberField(TEXT("innerRadius"), Params.InnerRadius);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_arch
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_arch", "geometry", "Create an arch (partial torus) dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("majorRadius", "number", "Major radius (default 100)"),
        RPC_PARAM_OPT("minorRadius", "number", "Minor radius (default 25)"),
        RPC_PARAM_OPT("angle", "number", "Arch angle in degrees (default 180)"),
        RPC_PARAM_OPT("majorSteps", "number", "Steps along the sweep (default 16). Minimum 2 below angle 360 - a half-arch at 2 is sound - and minimum 3 from 360 up, where the sweep closes and 2 leaves an open shell. Below the floor it clamps up and warns."),
        RPC_PARAM_OPT("minorSteps", "number", "Steps around the tube profile (default 8). Minimum 3 - NOT the same floor as majorSteps. Either below its floor clamps up and warns.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedArch"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FArchParams Params;
    Params.MajorRadius = Ctx.GetNumber(TEXT("majorRadius"), 100.0);
    Params.MinorRadius = Ctx.GetNumber(TEXT("minorRadius"), 25.0);
    Params.Angle = Ctx.GetNumber(TEXT("angle"), 180.0);
    Params.MajorSteps = Ctx.GetInt(TEXT("majorSteps"), 16);
    Params.MinorSteps = Ctx.GetInt(TEXT("minorSteps"), 8);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateArch(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetNumberField(TEXT("majorRadius"), Params.MajorRadius);
    Result->SetNumberField(TEXT("angle"), Params.Angle);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_pipe
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_pipe", "geometry", "Create a hollow pipe dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("outerRadius", "number", "Outer radius (default 50). Must be greater than innerRadius."),
        RPC_PARAM_OPT("innerRadius", "number", "Inner radius (default 40). Must be greater than 0 and less than outerRadius, or the call fails INVALID_PARAMS - the annular cross-section cannot be swept otherwise, and there is no non-arbitrary value to clamp to."),
        RPC_PARAM_OPT("height", "number", "Pipe height (default 100). The pipe is CENTRE-placed like every other create_* primitive: height 100 spans local z -50..+50, exactly as create_cylinder height 100 does. It was base-placed (z 0..100) until this was fixed, so a recipe written against the old behaviour now sits half a height too LOW - raise its location.z by height/2 to restore it."),
        RPC_PARAM_OPT("radialSteps", "number", "Radial steps (default 24). Minimum 3 - below that the sweep collapses to a prism, so the op clamps up and warns."),
        RPC_PARAM_OPT("heightSteps", "number", "Height steps (default 1). ADDITIONAL wall loops, not the total, the same reading create_cylinder gives it. Minimum 0 (no intermediate loop); a negative value clamps up and warns.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedPipe"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FPipeParams Params;
    Params.OuterRadius = Ctx.GetNumber(TEXT("outerRadius"), 50.0);
    Params.InnerRadius = Ctx.GetNumber(TEXT("innerRadius"), 40.0);
    Params.Height = Ctx.GetNumber(TEXT("height"), 100.0);
    Params.RadialSteps = Ctx.GetInt(TEXT("radialSteps"), 24);
    Params.HeightSteps = Ctx.GetInt(TEXT("heightSteps"), 1);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GeneratePipe(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetNumberField(TEXT("outerRadius"), Params.OuterRadius);
    Result->SetNumberField(TEXT("innerRadius"), Params.InnerRadius);
    Result->SetNumberField(TEXT("height"), Params.Height);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_ramp
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_ramp", "geometry", "Create a ramp (wedge) dynamic mesh actor",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("width", "number", "Ramp width (default 100)"),
        RPC_PARAM_OPT("length", "number", "Ramp length (default 200)"),
        RPC_PARAM_OPT("height", "number", "Ramp height (default 50)")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedRamp"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FRampParams Params;
    Params.Width = Ctx.GetNumber(TEXT("width"), 100.0);
    Params.Length = Ctx.GetNumber(TEXT("length"), 200.0);
    Params.Height = Ctx.GetNumber(TEXT("height"), 50.0);

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateRamp(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetNumberField(TEXT("width"), Params.Width);
    Result->SetNumberField(TEXT("length"), Params.Length);
    Result->SetNumberField(TEXT("height"), Params.Height);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// revolve
// ============================================================================
REGISTER_RPC_HANDLER("geometry.revolve", "geometry", "Create a dynamic mesh actor by revolving an open profile path around the local Z axis. Named without the create_ prefix but a full member of that family: the same optional name slot, the same spawn, and the same response shape - `class` is \"DynamicMeshActor\" and the actor verification block (actorPath, actorObjectName, actorGuid, existsAfter) rides along. Fewer than two profile points substitutes a 6-point default and warns.",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("angle", "number", "Revolution angle in degrees (default 360)"),
        RPC_PARAM_OPT("steps", "number", "Revolution steps (default 16). The floor follows `angle`: minimum 2 below 360 (the engine sweeps the same two-step surface below that and says nothing), minimum 3 from 360 up - the default - where the sweep closes and 2 gives 8 triangles with 10 boundary edges and 6 degenerate ones. 0 or negative reads as unset and takes the default 16, and anything above 256 caps there. Either way it warns and the response echoes the EFFECTIVE count."),
        RPC_PARAM_OPT("capped", "boolean", "Close an OPEN profile by capping its two ends to the REVOLVE AXIS (default true) - what turns a lathed silhouette into a solid, and at angle=360 the flag that decides whether a bore stays open. A CLOSED SECTION - a ring, tube, rim or flange, written by repeating the first point as the last - needs no axis cap: the surface closes on itself, and the verb detects the repeated endpoint, drops it, sweeps the section closed and warns that it did. On such a profile `capped` then means only the two ends of a partial sweep, and does nothing at angle=360."),
        RPC_PARAM_OPT("profile", "array", "Profile points [{x,y}, ...]. An OPEN path by default. Repeat the first point as the last to declare a closed section; `profilePoints` then echoes the deduplicated count.")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("GeneratedRevolve"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);

    GeometryOps::FRevolveParams Params;
    Params.Angle = Ctx.GetNumber(TEXT("angle"), 360.0);
    Params.Steps = Ctx.GetInt(TEXT("steps"), 16);
    Params.bCapped = Ctx.GetBool(TEXT("capped"), true);

    // Get profile points from payload
    if (Payload->HasField(TEXT("profile")))
    {
        const TArray<TSharedPtr<FJsonValue>>& PointsArray = Payload->GetArrayField(TEXT("profile"));
        for (const TSharedPtr<FJsonValue>& PointValue : PointsArray)
        {
            const TSharedPtr<FJsonObject>& PointObj = PointValue->AsObject();
            if (PointObj.IsValid())
            {
                double X = GetNumberFieldGeomNew(PointObj, TEXT("x"), 0.0);
                double Y = GetNumberFieldGeomNew(PointObj, TEXT("y"), 0.0);
                Params.Profile.Add(FVector2D(X, Y));
            }
        }
    }

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    // Fewer than two points is not a path; GenerateRevolve substitutes its default profile into
    // Params.Profile, which is why the profilePoints echo below reads back off the struct.
    const GeometryOps::FOpResult Op = GeometryOps::GenerateRevolve(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetNumberField(TEXT("angle"), Params.Angle);
    // Effective steps: GenerateRevolve clamps to the AppendRevolvePath floor of 2 through the
    // Params& reference, so steps=1 now echoes 2 and says so in `warnings`.
    Result->SetNumberField(TEXT("steps"), Params.Steps);
    Result->SetNumberField(TEXT("profilePoints"), Params.Profile.Num());
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// create_procedural_mesh
// ============================================================================
REGISTER_RPC_HANDLER("geometry.create_procedural_mesh", "geometry", "Create an empty DynamicMeshActor for procedural mesh building",
    RPC_PARAMS(
        GeometryNameParamUtils::CreateNameParamOpt(),
        RPC_PARAM_OPT("location", "object", "Spawn location"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale"),
        RPC_PARAM_OPT("enableCollision", "boolean", "Enable collision (default false): sets QueryAndPhysics + complex-as-simple, so the geometry appended later is the collision")
    ))
{
    FString Name = GeometryNameParamUtils::ResolveCreateName(Ctx, TEXT("ProceduralMesh"));
    auto Payload = Ctx.GetRawPayload();
    FTransform Transform = GeometryUtils::ReadTransformFromPayload(Payload);
    bool bEnableCollision = Ctx.GetBool(TEXT("enableCollision"), false);

    GeometryOps::FEmptyMeshParams Params;

    UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

    const GeometryOps::FOpResult Op = GeometryOps::GenerateEmpty(DynMesh, Params, FTransform::Identity);
    if (!Op.bSuccess) { Ctx.SendError(Op.ErrorCode, Op.ErrorMessage); return true; }

    // GeometryTarget::Spawn returns the concrete type, so no Cast is needed here. The Cast that
    // used to wrap this block was unconditionally true and only served to hide the collision
    // setup behind a branch that could never be taken.
    ADynamicMeshActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, Name);
    if (!NewActor) return true;

    if (UDynamicMeshComponent* DMComp = NewActor->GetDynamicMeshComponent())
    {
        // SetCollisionEnabled is what actually turns collision on/off — SetGenerateOverlapEvents
        // (the only thing this used to do with enableCollision) merely toggles overlap-event
        // dispatch and leaves CollisionEnabled untouched, so enableCollision=true produced an
        // actor with no collision at all.
        //
        // The actor spawns with an EMPTY mesh and gets its geometry from later edits, so there
        // is nothing to build simple collision primitives from here: complex-as-simple makes the
        // mesh itself the collision, which keeps tracking whatever is appended afterwards.
        if (bEnableCollision)
        {
            DMComp->SetComplexAsSimpleCollisionEnabled(true, true);
            DMComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        }
        else
        {
            DMComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }
        DMComp->SetGenerateOverlapEvents(bEnableCollision);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
    Result->SetBoolField(TEXT("enableCollision"), bEnableCollision);
    AddActorVerification(Result, NewActor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}
