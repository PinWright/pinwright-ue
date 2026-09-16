// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Model/PwModelParser.h"

#include "Templates/Function.h"

#include "Handlers/Geometry/GeometryOps_Modeling.h"
// Read-only: the parser derives the `complexity` and `auto method=` spellings from the
// collision layer's enum-derived name lists rather than repeating them, so a value added to
// ECollisionTraceFlag or EGeometryScriptCollisionGenerationMethod cannot be accepted by one
// half of the pipeline and rejected by the other.
#include "Handlers/Geometry/GeometryClampDomains.h"
#include "Model/PwModelCollision.h"
#include "PwSource/PwParseCursor.h"
#include "PwSource/PwToken.h"
#include "PwSource/PwSuggest.h"
#include "PwSource/PwTokenizer.h"

// ============================================================================
// Op table
// ============================================================================
//
// Hand-written, not derived from the RPC registry. Deriving was evaluated per the plan:
// FRpcDispatcher::GetAutoRegisteredHandlers() is reachable, but FParamSpec::Type is a
// free-form string in which every vector-valued parameter is the shapeless "object", so a
// derived table would need a convention layer guessing which "object" is an FVector, which
// is an FVector2D and which is a transform - and the format's parameter names deliberately
// differ from the handlers' published ones anyway (size=(x,y,z) for width/height/depth).
// A half-derived table is worse than either end of that choice, so this is the whole table
// in one place, and model.describe_ops emits it verbatim.
//
// Defaults below are the shipped handler defaults, so a .pwmodel op with no parameters
// produces what the matching geometry.* verb produces with no parameters.

namespace
{
FPwModelParamSpec MakeParam(const TCHAR* Name, EPwModelParamType Type, const TCHAR* Default, const TCHAR* Description)
{
    FPwModelParamSpec Spec;
    Spec.Name = Name;
    Spec.Type = Type;
    Spec.bRequired = false;
    Spec.Default = Default;
    Spec.Description = Description;
    return Spec;
}

FPwModelParamSpec MakeRequired(const TCHAR* Name, EPwModelParamType Type, const TCHAR* Description)
{
    FPwModelParamSpec Spec = MakeParam(Name, Type, TEXT(""), Description);
    Spec.bRequired = true;
    return Spec;
}

FPwModelParamSpec MakeEnum(const TCHAR* Name, const TCHAR* Default, const TCHAR* Description, TArray<FString> Allowed)
{
    FPwModelParamSpec Spec = MakeParam(Name, EPwModelParamType::Enum, Default, Description);
    Spec.AllowedValues = MoveTemp(Allowed);
    return Spec;
}

FPwModelParamSpec MakeRequiredEnum(const TCHAR* Name, const TCHAR* Description, TArray<FString> Allowed)
{
    FPwModelParamSpec Spec = MakeEnum(Name, TEXT(""), Description, MoveTemp(Allowed));
    Spec.bRequired = true;
    return Spec;
}

// A Number / Integer parameter with an inclusive domain. The bounds are data on the spec
// rather than a check keyed on the op's name, so an op that gains a bounded parameter gets
// the check by declaring it here and nothing has to remember to widen a name list.
FPwModelParamSpec MakeRanged(const TCHAR* Name, EPwModelParamType Type, const TCHAR* Default,
                             double Min, double Max, const TCHAR* Description)
{
    FPwModelParamSpec Spec = MakeParam(Name, Type, Default, Description);
    Spec.bHasRange = true;
    Spec.MinValue = Min;
    Spec.MaxValue = Max;
    return Spec;
}

// Channels are 0-7 everywhere, matching Unreal's UV set limit: SetNumUVSets rejects above 8
// and the bake agrees. One spec so all four channel parameters cannot drift apart.
FPwModelParamSpec MakeUvChannelParam(const TCHAR* Description)
{
    return MakeRanged(TEXT("channel"), EPwModelParamType::Integer, TEXT("0"), 0.0, 7.0, Description);
}

FPwModelParamSpec MakeUVTextureResolutionParam(const TCHAR* Description)
{
    FPwModelParamSpec Spec = MakeRanged(TEXT("texture_resolution"), EPwModelParamType::Integer,
        TEXT(""), GeometryOps::LayoutUVTextureResolutionMin,
        GeometryOps::LayoutUVTextureResolutionMax, Description);
    Spec.Default = FString::FromInt(GeometryOps::LayoutUVTextureResolutionDefault);
    return Spec;
}

// at / rotate / scale on a geometry-producing op are part-local and compose into the
// primitive's Append* call. The RPC verbs put the same values on the spawned actor
// instead; applying them in both places double-applies location and squares scale, which
// is why the compiler composes exactly one transform per op. PrimitiveHandler.cpp's
// "Spawn-transform convention (load-bearing)" comment documents the same trap on the actor
// side, where the full failure mode lives on GeometryTarget::Spawn. Cited by symbol rather
// than by line number: a line citation into another file goes stale on its next edit, and
// this one already had.
void AppendLocalTransformParams(TArray<FPwModelParamSpec>& Params)
{
    Params.Add(MakeParam(TEXT("at"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
        TEXT("Part-local translation of this op's geometry, in Unreal units.")));
    Params.Add(MakeParam(TEXT("rotate"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
        TEXT("Part-local rotation (roll, pitch, yaw) in degrees.")));
    Params.Add(MakeParam(TEXT("scale"), EPwModelParamType::Vector3, TEXT("(1, 1, 1)"),
        TEXT("Part-local per-axis scale. Bakes into vertex positions, so non-uniform scale is safe here.")));
}

// What `from`/`to` do to THIS op's size, as one sentence built from the op's own aim kind. Built
// rather than written per op so an op cannot be registered with an aim kind its published
// description contradicts - the two would then disagree exactly where an author looks first.
FString DescribeAimExtent(EPwModelAimExtent Extent, const TCHAR* ExtentParam)
{
    switch (Extent)
    {
    case EPwModelAimExtent::Scalar:
        return FString::Printf(
            TEXT(" '%s' is then the DISTANCE between the two points and must not also be written."),
            ExtentParam);
    case EPwModelAimExtent::SizeZ:
        return FString::Printf(
            TEXT(" The Z component of '%s' is then the DISTANCE between the two points; write the cross-section as ")
            TEXT("%s=(x, y, 0) to say so, because any other Z would be a second answer for the same length."),
            ExtentParam, ExtentParam);
    case EPwModelAimExtent::CapsuleLength:
        return FString::Printf(
            TEXT(" '%s' is then DISTANCE - 2*radius, because it measures the cylindrical section only and a ")
            TEXT("hemisphere is added at each end; it must not also be written, and a distance no greater than ")
            TEXT("2*radius names no capsule."),
            ExtentParam);
    default:
        return FString(
            TEXT(" This op has no extent along its local Z, so the DISTANCE between the two points is not used - ")
            TEXT("only their midpoint and their direction. Size the op with its own parameters."));
    }
}

// The aiming parameters, on the same ops that take at/rotate/scale, and set together with the
// op's aim kind so a generator cannot advertise `from` without the compiler knowing what to do
// with the distance.
//
// WHY THIS EXISTS. Every centre-placed generator here is built along its own local +Z, so placing
// one BETWEEN two known points - two joints of a skeleton, two ends of a strut, a socket and its
// target - had to be spelled at=(A+B)/2 plus a length of |B-A| plus a rotation the author derived
// by hand, per op. That derivation is outside the format, it is where the arithmetic errors go,
// and a rig of a hundred limbs needs a hundred of them.
//
// `up` is not decoration: aiming fixes only two of the three degrees of freedom, and the third -
// the twist about the aimed axis - decides which world direction a non-uniform `scale=` flattens
// towards. Invisible on a round primitive, and the reason a fan of flattened cards comes out half
// flat and half on edge when the twist is left implicit. PwValueRead::MakeAimRotator owns the two
// rules and the reason they differ on a vertical axis.
void AppendAimParams(FPwModelOpSpec& Spec)
{
    const FString ExtentClause = DescribeAimExtent(Spec.AimExtent, *Spec.AimExtentParam);

    // Held in locals rather than passed inline: MakeParam takes a raw TCHAR*, and a description
    // assembled per op has to outlive the call that copies it.
    const FString FromText = FString::Printf(
        TEXT("Part-local start point. With 'to', the op is centred on the midpoint of the two and its local ")
        TEXT("+Z is aimed from 'from' towards 'to', replacing 'at' and 'rotate' - which are then refused, ")
        TEXT("because they would be a second answer for the same placement.%s"),
        *ExtentClause);

    const FString ToText = FString::Printf(
        TEXT("Part-local end point. Required with 'from' and meaningless without it. The two points must ")
        TEXT("differ: a zero-length aim names no direction.%s"),
        *ExtentClause);

    Spec.Params.Add(MakeParam(TEXT("from"), EPwModelParamType::Vector3, TEXT(""), *FromText));
    Spec.Params.Add(MakeParam(TEXT("to"), EPwModelParamType::Vector3, TEXT(""), *ToText));
    Spec.Params.Add(MakeParam(TEXT("up"), EPwModelParamType::Vector3, TEXT("(0, 0, 1)"),
        TEXT("Twist reference for 'from'/'to', pinning the one degree of freedom aiming leaves free. Local +Y is ")
        TEXT("set perpendicular to both 'up' and the aim direction, so a flattening scale=(1, k, 1) squashes the ")
        TEXT("op TOWARDS 'up' whichever way it points. Only meaningful with 'from'/'to'. It must not be parallel ")
        TEXT("to the aim direction, which names no frame and is refused. The default is world up and is NOT ")
        TEXT("refused on a vertical aim - it falls back to roll=0, which puts local +Y on world +Y.")));
}

// The `material=` slot tag, and the bAcceptsMaterial flag that advertises it, applied as one
// step. They MUST travel together: the parser's unknown-param gate reads the PARAM list while
// model.describe_ops publishes the FLAG, so an op given one without the other either rejects a
// tag it claims to take or accepts one it does not report. Its own function rather than a line
// inside MakeGenerator because `append_buffers` needs the tag and is not a shape primitive.
//
// `color=` is deliberately not part of this, which is why the pair was split: it is separately
// applied by the ops that want both.
//
// Description is overridable because the ops that produce NEW FACES rather than a whole shape -
// the four booleans and `bevel` - need to say WHICH faces the tag reaches. "the geometry this op
// produces" is true and useless on a `subtract`, whose output is the walls the cut opened.
void AcceptMaterialSlot(FPwModelOpSpec& Spec, const TCHAR* Description = nullptr)
{
    Spec.bAcceptsMaterial = true;
    Spec.Params.Add(MakeParam(TEXT("material"), EPwModelParamType::String, TEXT(""),
        Description ? Description
            : TEXT("Material slot name for the geometry this op produces. Slot identity is the name, model-wide.")));
}

// The per-corner vertex color overlay. Separable from the slot tag above, and separated
// because `append_buffers` takes the tag and must NOT take this: it already carries a
// per-vertex `colors` buffer, and the compiler's `color=` path runs SetVertexColor with
// bSetAll, so a scalar `color=` on the same op would silently overwrite every color the
// author supplied in `colors=`.
void AppendVertexColorParam(TArray<FPwModelParamSpec>& Params)
{
    Params.Add(MakeParam(TEXT("color"), EPwModelParamType::Vector4, TEXT("(1, 1, 1, 1)"),
        TEXT("Per-corner vertex color overlay. For masking and tinting, not for carrying precise data.")));
}

FPwModelOpSpec MakeOp(const TCHAR* Name, EPwModelOpContext Context, const TCHAR* Description,
                      TArray<FPwModelParamSpec> Params)
{
    FPwModelOpSpec Spec;
    Spec.Name = Name;
    Spec.Context = Context;
    Spec.Description = Description;
    Spec.Params = MoveTemp(Params);
    return Spec;
}

// A shape primitive: legal as a part's first op, and the only kind that carries a local
// transform. It takes both tagging parameters - the material slot and the vertex color.
FPwModelOpSpec MakeGenerator(const TCHAR* Name, const TCHAR* Description, TArray<FPwModelParamSpec> Params,
                            EPwModelAimExtent AimExtent = EPwModelAimExtent::None,
                            const TCHAR* AimExtentParam = TEXT(""))
{
    FPwModelOpSpec Spec = MakeOp(Name, EPwModelOpContext::Part, Description, MoveTemp(Params));
    Spec.bGenerator = true;
    Spec.AimExtent = AimExtent;
    Spec.AimExtentParam = AimExtentParam;
    AppendLocalTransformParams(Spec.Params);
    AppendAimParams(Spec);
    AcceptMaterialSlot(Spec);
    AppendVertexColorParam(Spec.Params);
    return Spec;
}

FPwModelOpSpec MakeModifier(const TCHAR* Name, const TCHAR* Description, TArray<FPwModelParamSpec> Params)
{
    return MakeOp(Name, EPwModelOpContext::Part, Description, MoveTemp(Params));
}

// An op that builds geometry from nothing but is not a shape primitive: it may open a part, yet
// carries no local transform, because its vertex positions are already explicit. It gets no
// tagging parameters here either - `append_buffers` and `append_triangle` each add the material
// slot with AcceptMaterialSlot at their registration, and neither takes `color=`.
FPwModelOpSpec MakeBareGenerator(const TCHAR* Name, const TCHAR* Description, TArray<FPwModelParamSpec> Params)
{
    FPwModelOpSpec Spec = MakeOp(Name, EPwModelOpContext::Part, Description, MoveTemp(Params));
    Spec.bGenerator = true;
    return Spec;
}

FPwModelOpSpec MakeBoolean(const TCHAR* Name, const TCHAR* Description, TArray<FPwModelParamSpec> Params)
{
    FPwModelOpSpec Spec = MakeOp(Name, EPwModelOpContext::Part, Description, MoveTemp(Params));
    Spec.bAcceptsBlock = true;
    Spec.bBoolean = true;
    // A boolean DOES produce faces, and they are the ones nothing else in the document can name:
    // the walls a `subtract` opens, the tool surface a `union` keeps, the caps a `trim` fills.
    // Untagged they inherit the slot of the geometry the op was applied to; this is how an author
    // says otherwise. `color=` is still refused - see the OnParam gate in ValidateSourceParams.
    AcceptMaterialSlot(Spec,
        TEXT("Material slot for the faces this operation CREATES - the walls a subtract opens, the tool surface a union keeps, the caps fill_holes closes. It does NOT recolour geometry that already exists: triangles the operation keeps carry the slot they arrived on. Untagged, new faces inherit the slot of the geometry the boolean was applied to, and geometry carrying more than one slot has no single answer, so the op takes the slot most of it is on and warns PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS. A generator INSIDE the block may also carry its own material=, which wins for the faces that generator's geometry produces."));
    return Spec;
}

FPwModelOpSpec MakeCollisionElement(const TCHAR* Name, const TCHAR* Description, TArray<FPwModelParamSpec> Params)
{
    return MakeOp(Name, EPwModelOpContext::Collision, Description, MoveTemp(Params));
}

TArray<FString> AxisValues()
{
    return TArray<FString>{ TEXT("x"), TEXT("y"), TEXT("z") };
}

// GeometryOps::EHarmonicTarget, in declaration order.
TArray<FString> HarmonicTargetValues()
{
    return TArray<FString>{ TEXT("radial"), TEXT("axial") };
}

// Enum vocabularies that mirror a Geometry Script enum. Each spelling is the snake_case of the
// engine ENUMERATOR minus its type prefix, which is the convention `collision`'s `complexity`
// set against ECollisionTraceFlag; the compiler maps them back onto the locally-spelled enum in
// GeometryOps_Modeling.h. Written out here rather than derived because the parser must not pull
// GeometryScripting headers in - PwModelCollision.h can export its lists precisely because the
// collision layer already links them, and this file has no equivalent.
//
// The engine's DisplayName for AttributeAware reads "Normals Aware, Volume Preserving" and for
// AttributeAwareV2 "Attribute Aware, Volume Preserving" - the labels and the enumerator names are
// one step out of phase after a 5.8 rename. These follow the ENUMERATOR, because that is what the
// engine header calls the value and what an author reading MeshSimplifyFunctions.h will see.
TArray<FString> SimplifyMethodValues()
{
    return TArray<FString>{
        TEXT("standard_qem"), TEXT("volume_preserving"),
        TEXT("attribute_aware"), TEXT("attribute_aware_v2") };
}

TArray<FString> SimplifyQuadricVariantValues()
{
    return TArray<FString>{ TEXT("plane_quadric"), TEXT("triangle_quadric") };
}

TArray<FString> RemeshTargetTypeValues()
{
    return TArray<FString>{ TEXT("triangle_count"), TEXT("target_edge_length") };
}

TArray<FString> RemeshSmoothingTypeValues()
{
    return TArray<FString>{ TEXT("uniform"), TEXT("uv_preserving"), TEXT("mixed") };
}

TArray<FString> RemeshEdgeConstraintValues()
{
    return TArray<FString>{ TEXT("fixed"), TEXT("refine"), TEXT("free"), TEXT("ignore") };
}

TArray<FString> UVLayoutTypeValues()
{
    return TArray<FString>{ TEXT("transform"), TEXT("stack"), TEXT("repack"), TEXT("normalize") };
}

TArray<FString> PolyOperationAreaValues()
{
    return TArray<FString>{ TEXT("entire_selection"), TEXT("per_polygroup"), TEXT("per_triangle") };
}

TArray<FString> EditPolygroupModeValues()
{
    return TArray<FString>{ TEXT("preserve_existing"), TEXT("auto_generate_new"), TEXT("set_constant") };
}

TArray<FString> OffsetFacesTypeValues()
{
    return TArray<FString>{ TEXT("vertex_normal"), TEXT("face_normal"), TEXT("parallel_face_offset") };
}

TArray<FString> LinearExtrudeDirectionValues()
{
    return TArray<FString>{ TEXT("fixed_direction"), TEXT("average_face_normal") };
}

TArray<FString> FillHolesMethodValues()
{
    return TArray<FString>{
        TEXT("automatic"), TEXT("minimal_fill"), TEXT("polygon_triangulation"),
        TEXT("triangle_fan"), TEXT("planar_projection") };
}

TArray<FString> RepairMeshModeValues()
{
    return TArray<FString>{ TEXT("delete_only"), TEXT("repair_or_delete"), TEXT("repair_or_skip") };
}

TArray<FString> TangentTypeValues()
{
    return TArray<FString>{ TEXT("fast_mikkt"), TEXT("per_triangle"), TEXT("standard_mikkt") };
}

TArray<FString> FlareTypeValues()
{
    return TArray<FString>{ TEXT("sin_mode"), TEXT("sin_squared_mode"), TEXT("triangle_mode") };
}

// The four parameters GeometryOps::FFaceOpCommonSpec carries, appended by every face op that
// takes one - extrude, inset, outset, offset_faces, poke. One appender rather than five copies
// so the five cannot end up with five spellings of AreaMode, and so the nested engine struct
// FGeometryScriptMeshEditPolygroupOptions has exactly ONE flattening in the whole format.
//
// Flattened, not nested: FPwValue has no map or sub-object member (PwModelAst.h), so
// `group { mode=… }` is not expressible. Two prefixed scalars is the shape the format already
// uses for every other nested engine struct it reaches.
void AppendFaceOpCommonParams(TArray<FPwModelParamSpec>& Params)
{
    Params.Add(MakeEnum(TEXT("area_mode"), TEXT("entire_selection"),
        TEXT("How the affected faces are partitioned before the op runs. entire_selection treats them as ONE region, so a set of disjoint faces moves as a single slab; per_polygroup and per_triangle run the op once per region, which is what makes each face move along its own normal."),
        PolyOperationAreaValues()));
    Params.Add(MakeEnum(TEXT("group_mode"), TEXT("preserve_existing"),
        TEXT("Polygroup assignment for the NEW faces the op creates. auto_generate_new gives them their own group, which is what a later `bevel` needs in order to see their boundary as an edge at all."),
        EditPolygroupModeValues()));
    Params.Add(MakeParam(TEXT("group_id"), EPwModelParamType::Integer, TEXT("0"),
        TEXT("Polygroup ID for the new faces. Read only when group_mode=set_constant.")));
    Params.Add(MakeParam(TEXT("uv_scale"), EPwModelParamType::Number, TEXT("1"),
        TEXT("UV scale for the new faces only. The UVs already on the mesh are not touched.")));
}

// MakeModifier plus the four shared face-op parameters, so a face op declares only what is its
// own and cannot forget the common four.
FPwModelOpSpec MakeFaceModifier(const TCHAR* Name, const TCHAR* Description, TArray<FPwModelParamSpec> Params)
{
    FPwModelOpSpec Spec = MakeModifier(Name, Description, MoveTemp(Params));
    AppendFaceOpCommonParams(Spec.Params);
    return Spec;
}

// The model-level `key=value` lists that are not part or collision ops. They are built here,
// from the same MakeParam helpers as every op, so they are validated and published from one
// parameter vocabulary.
TArray<FPwModelParamSpec> BuildUVLayoutParams()
{
    FPwModelParamSpec Channel = MakeUvChannelParam(
        TEXT("UV channel to pack after every part has been merged."));
    Channel.bRequired = true;
    Channel.Default.Reset();

    TArray<FPwModelParamSpec> Params;
    Params.Add(MoveTemp(Channel));
    Params.Add(MakeUVTextureResolutionParam(
        TEXT("Expected output texture resolution. Sets the gutter left between islands; it writes nothing at that size.")));
    return Params;
}

TArray<FPwModelParamSpec> BuildLightmapParams()
{
    FPwModelParamSpec Channel = MakeUvChannelParam(
        TEXT("UV channel the lightmap is baked into. Model-level rather than per-op: UStaticMesh has one LightMapCoordinateIndex."));
    Channel.bRequired = true;
    Channel.Default.Reset();

    // The asset's lightmap TEXTURE resolution. Deliberately NOT derived from
    // `uv ... mode=layout texture_resolution=`, and the two are not wired together:
    // texture_resolution is a UV PACKER hint that only sizes the gutter left between islands
    // (GeometryOps::FLayoutUVParams::TextureResolution, "not the size of anything written"),
    // it may be given on any channel, and a document may run several `uv` ops with different
    // values - while a UStaticMesh has exactly ONE LightMapResolution. Auto-wiring would let
    // whichever `uv` op happened to run last silently decide an asset property, on a channel
    // that need not even be the lightmap one. `mode=patch_builder` already carries the same
    // separation for its own packer's packing_target_image_width.
    //
    // Optional, and deliberately carrying NO Default text. FPwModelParamSpec::Default is
    // display-only - ValidateParams never writes it into FPwOp::Params - so a "4" here
    // could not force a value, but it would advertise a default this statement does not apply:
    // omitting `resolution` leaves the asset at whatever UStaticMesh's constructor set
    // (StaticMesh.cpp:4738, SetLightMapResolution(4)), which the compiler has to be able to
    // tell apart from an explicit request. Absent stays absent, so GetInt(..., INDEX_NONE)
    // answers INDEX_NONE.
    //
    // Range [4, 4096] is the engine's own: EnforceLightmapRestrictions raises anything smaller
    // back to 4 during the build (StaticMesh.cpp:9732,
    // `SetLightMapResolution(FMath::Max(GetLightMapResolution(), 4))`), and the UPROPERTY
    // declares `ClampMax = 4096` (StaticMesh.h:1176). The engine wants a MULTIPLE OF 4, not a
    // power of two - the same UPROPERTY declares `FixedIncrement="4.0"` and the LOD-group
    // default is rounded with `(x + 3) & (~3)` (StaticMesh.cpp:3306) - but no code path rejects
    // or rounds an author-set value, so an off-step number is accepted rather than refused or
    // warned about: the parser must not be stricter than the engine it front-ends.
    FPwModelParamSpec Resolution = MakeRanged(TEXT("resolution"), EPwModelParamType::Integer,
        TEXT(""), 4.0, 4096.0,
        TEXT("Lightmap TEXTURE resolution for the asset, in texels per side: UStaticMesh::LightMapResolution plus the matching MinLightmapResolution build setting. Independent of `uv ... mode=layout texture_resolution=`, which only sizes the packer's gutter on one UV channel and writes nothing at that size. Omitted, the asset keeps the UStaticMesh default of 4, so authored lightmap UVs bake at 4x4."));

    TArray<FPwModelParamSpec> Params;
    Params.Add(MoveTemp(Channel));
    Params.Add(MoveTemp(Resolution));
    return Params;
}

// Spelled out rather than routed through AppendLocalTransformParams: the header names and
// types match an op's, but a part header's transform maps part-local space to MESH space
// and is applied once after the part's ops have run, so the descriptions differ.
TArray<FPwModelParamSpec> BuildPartHeaderParams()
{
    TArray<FPwModelParamSpec> Params;
    Params.Add(MakeParam(TEXT("bone"), EPwModelParamType::String, TEXT(""),
        TEXT("Reference-skeleton bone name for rigidly binding this part. The name is resolved against the skeleton named by 'use skeleton'.")));
    Params.Add(MakeParam(TEXT("allow_floating"), EPwModelParamType::Bool, TEXT("false"),
        TEXT("Declare that this named part may be spatially isolated. The compiler still reports its measured distance; only this part's warning is suppressed.")));
    Params.Add(MakeParam(TEXT("at"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
        TEXT("Translation from part-local space into mesh space, applied once after the part's ops have run.")));
    Params.Add(MakeParam(TEXT("rotate"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
        TEXT("Rotation (roll, pitch, yaw) in degrees, applied once after the part's ops have run.")));
    Params.Add(MakeParam(TEXT("scale"), EPwModelParamType::Vector3, TEXT("(1, 1, 1)"),
        TEXT("Per-axis scale. Bakes into vertex positions, so non-uniform scale is safe here.")));
    return Params;
}

TArray<FPwModelOpSpec> BuildOpTable()
{
    TArray<FPwModelOpSpec> Ops;

    // ---- Generators (geometry.create_* minus the create_ prefix) ----------------

    Ops.Add(MakeGenerator(TEXT("box"), TEXT("An axis-aligned box, centred on the part-local origin."), {
        MakeParam(TEXT("size"), EPwModelParamType::Vector3, TEXT("(100, 100, 100)"),
            TEXT("Full extents along local X, Y and Z. Z is the vertical extent.")),
        MakeParam(TEXT("segments"), EPwModelParamType::Vector3, TEXT("(1, 1, 1)"),
            TEXT("Subdivisions along each axis. Clamped to 0-256 per axis with a warning. The engine counts VERTICES per edge and raises them to 2, so 0, 1 and 2 all build the same unsubdivided box and 3 is the first value that adds a quad along an edge.")),
    }, EPwModelAimExtent::SizeZ, TEXT("size")));

    Ops.Add(MakeGenerator(TEXT("sphere"), TEXT("A box-topology sphere: a rounded cube, with no poles and no seam. Not a lat/long sphere - the polyhedron and the triangle count both differ from append_sphere_lat_long at the same subdivisions."), {
        MakeParam(TEXT("radius"), EPwModelParamType::Number, TEXT("50"), TEXT("Sphere radius.")),
        MakeParam(TEXT("subdivisions"), EPwModelParamType::Integer, TEXT("16"), TEXT("Quad steps per cube face, the same count on all three axes. Triangle count is 12*(subdivisions-1)^2. Minimum 2: 1 draws the same 12-triangle cube as 2 and is clamped up with a warning. At 2 the half-extent is radius/sqrt(3) = 0.5774*radius on every axis, NOT radius: the only vertices are the 8 cube corners, and a corner projects to sqrt(1/3) per axis. From 3 up, edge and face-centre vertices reach radius and the bounding box matches it. At the floor, place neighbouring geometry against the reported bounds rather than against radius.")),
    }));

    Ops.Add(MakeGenerator(TEXT("cylinder"), TEXT("A cylinder along local Z."), {
        MakeParam(TEXT("radius"), EPwModelParamType::Number, TEXT("50"), TEXT("Cylinder radius.")),
        MakeParam(TEXT("height"), EPwModelParamType::Number, TEXT("100"), TEXT("Height along local Z.")),
        MakeParam(TEXT("segments"), EPwModelParamType::Integer, TEXT("16"), TEXT("Radial segments. Clamped to 3-256 with a warning; 0 or less reads as unset and takes the default 16.")),
        MakeParam(TEXT("height_steps"), EPwModelParamType::Integer, TEXT("1"),
            TEXT("ADDITIONAL wall loops, not the total, so 0 is legal and means one wall segment. Raise it so a later twist/bend/taper has side-wall loops to displace. Clamped to 0-256 with a warning.")),
    }, EPwModelAimExtent::Scalar, TEXT("height")));

    Ops.Add(MakeGenerator(TEXT("cone"), TEXT("A cone or truncated cone along local Z."), {
        MakeParam(TEXT("base_radius"), EPwModelParamType::Number, TEXT("50"), TEXT("Radius at the base.")),
        MakeParam(TEXT("top_radius"), EPwModelParamType::Number, TEXT("0"), TEXT("Radius at the top; non-zero truncates.")),
        MakeParam(TEXT("height"), EPwModelParamType::Number, TEXT("100"), TEXT("Height along local Z.")),
        MakeParam(TEXT("segments"), EPwModelParamType::Integer, TEXT("16"), TEXT("Radial segments. Clamped to 3-256 with a warning; 0 or less reads as unset and takes the default 16.")),
        MakeParam(TEXT("height_steps"), EPwModelParamType::Integer, TEXT("1"), TEXT("ADDITIONAL wall loops, not the total, so 0 is legal and means one wall segment. Clamped to 0-256 with a warning.")),
    }, EPwModelAimExtent::Scalar, TEXT("height")));

    Ops.Add(MakeGenerator(TEXT("capsule"), TEXT("A capsule along local Z."), {
        MakeParam(TEXT("radius"), EPwModelParamType::Number, TEXT("50"), TEXT("Capsule radius.")),
        MakeParam(TEXT("length"), EPwModelParamType::Number, TEXT("100"),
            TEXT("Length of the cylindrical section, excluding the hemispherical caps.")),
        MakeParam(TEXT("hemisphere_steps"), EPwModelParamType::Integer, TEXT("4"), TEXT("Steps per cap. Clamped to 2-256 with a warning - a lower floor than the radial counts, because it steps a half-arc profile rather than a closed loop; 0 or less takes the default 4.")),
        MakeParam(TEXT("segments"), EPwModelParamType::Integer, TEXT("16"), TEXT("Radial segments. Clamped to 3-256 with a warning; 0 or less reads as unset and takes the default 16.")),
    }, EPwModelAimExtent::CapsuleLength, TEXT("length")));

    Ops.Add(MakeGenerator(TEXT("torus"), TEXT("A torus."), {
        MakeParam(TEXT("major_radius"), EPwModelParamType::Number, TEXT("50"), TEXT("Radius of the ring.")),
        MakeParam(TEXT("minor_radius"), EPwModelParamType::Number, TEXT("20"), TEXT("Radius of the tube.")),
        MakeParam(TEXT("major_segments"), EPwModelParamType::Integer, TEXT("16"), TEXT("Segments around the ring. Clamped to 3-256 with a warning: a torus always closes and 2 cannot close it (16 triangles, 16 boundary edges, isClosed false). 0 or less takes the default 16.")),
        MakeParam(TEXT("minor_segments"), EPwModelParamType::Integer, TEXT("8"), TEXT("Segments around the tube. Clamped to 3-256 with a warning; 0 or less takes the default 8.")),
    }));

    Ops.Add(MakeGenerator(TEXT("plane"), TEXT("A flat rectangle in the local XY plane."), {
        MakeParam(TEXT("size"), EPwModelParamType::Vector2, TEXT("(100, 100)"), TEXT("Extents along local X and Y.")),
        MakeParam(TEXT("subdivisions"), EPwModelParamType::Vector2, TEXT("(1, 1)"), TEXT("Subdivisions along local X and Y. Clamped to 0-256 per axis with a warning. The engine counts VERTICES per edge and raises them to 2, so 0, 1 and 2 all build the same single quad.")),
    }));

    Ops.Add(MakeGenerator(TEXT("disc"), TEXT("A filled disc in the local XY plane."), {
        MakeParam(TEXT("radius"), EPwModelParamType::Number, TEXT("50"), TEXT("Disc radius.")),
        MakeParam(TEXT("segments"), EPwModelParamType::Integer, TEXT("16"), TEXT("Radial segments. Clamped to 3-256 with a warning; 0 or less reads as unset and takes the default 16.")),
    }));

    Ops.Add(MakeGenerator(TEXT("stairs"), TEXT("A linear staircase."), {
        MakeParam(TEXT("step_size"), EPwModelParamType::Vector3, TEXT("(100, 20, 30)"),
            TEXT("Per-step width, rise and depth along local X, Z and Y.")),
        MakeParam(TEXT("num_steps"), EPwModelParamType::Integer, TEXT("8"), TEXT("Number of steps. Clamped to 1-400 with a warning; 0 or less takes the default 8. A step count is not a segment count, so the ceiling is GEOM_MAX_STAIR_STEPS rather than the 256 every radial count takes.")),
        MakeParam(TEXT("floating"), EPwModelParamType::Bool, TEXT("false"), TEXT("Omit the solid under-structure.")),
    }));

    Ops.Add(MakeGenerator(TEXT("spiral_stairs"), TEXT("A curved staircase."), {
        MakeParam(TEXT("step_width"), EPwModelParamType::Number, TEXT("100"), TEXT("Tread width.")),
        MakeParam(TEXT("step_height"), EPwModelParamType::Number, TEXT("20"), TEXT("Rise per step.")),
        MakeParam(TEXT("inner_radius"), EPwModelParamType::Number, TEXT("150"), TEXT("Radius of the inner edge.")),
        MakeParam(TEXT("curve_angle"), EPwModelParamType::Number, TEXT("90"), TEXT("Total sweep in degrees.")),
        MakeParam(TEXT("num_steps"), EPwModelParamType::Integer, TEXT("8"), TEXT("Number of steps. Clamped to 1-400 with a warning; 0 or less takes the default 8. A step count is not a segment count, so the ceiling is GEOM_MAX_STAIR_STEPS rather than the 256 every radial count takes.")),
        MakeParam(TEXT("floating"), EPwModelParamType::Bool, TEXT("false"), TEXT("Omit the solid under-structure.")),
    }));

    Ops.Add(MakeGenerator(TEXT("ring"), TEXT("A disc with a concentric hole."), {
        MakeParam(TEXT("outer_radius"), EPwModelParamType::Number, TEXT("50"), TEXT("Outer radius.")),
        MakeParam(TEXT("inner_radius"), EPwModelParamType::Number, TEXT("25"), TEXT("Hole radius.")),
        MakeParam(TEXT("segments"), EPwModelParamType::Integer, TEXT("32"), TEXT("Radial segments. Clamped to 3-256 with a warning; 0 or less reads as unset and takes the default 32.")),
    }));

    Ops.Add(MakeGenerator(TEXT("arch"), TEXT("A partial torus."), {
        MakeParam(TEXT("major_radius"), EPwModelParamType::Number, TEXT("100"), TEXT("Radius of the arc.")),
        MakeParam(TEXT("minor_radius"), EPwModelParamType::Number, TEXT("25"), TEXT("Radius of the tube.")),
        MakeParam(TEXT("angle"), EPwModelParamType::Number, TEXT("180"), TEXT("Arc sweep in degrees.")),
        MakeParam(TEXT("major_steps"), EPwModelParamType::Integer, TEXT("16"), TEXT("Steps along the arc. Clamped to 2-256 with a warning, or to 3-256 at angle=360, where the sweep closes and 2 cannot close it. 0 or less takes the default 16.")),
        MakeParam(TEXT("minor_steps"), EPwModelParamType::Integer, TEXT("8"), TEXT("Steps around the tube. Clamped to 3-256 with a warning; 0 or less takes the default 8.")),
    }));

    Ops.Add(MakeGenerator(TEXT("pipe"),
        TEXT("A hollow tube along local Z: a closed solid with an outer wall, a bore and both annular end caps. Centred like every other generator - height=H spans z -H/2..+H/2, the same span as cylinder height=H."), {
        MakeParam(TEXT("outer_radius"), EPwModelParamType::Number, TEXT("50"), TEXT("Outer radius. Must be greater than inner_radius.")),
        MakeParam(TEXT("inner_radius"), EPwModelParamType::Number, TEXT("40"), TEXT("Bore radius. Must be greater than 0 and less than outer_radius; neither is clamped, because every substitute would be a different pipe.")),
        MakeParam(TEXT("height"), EPwModelParamType::Number, TEXT("100"), TEXT("Height along local Z, spanning -height/2..+height/2.")),
        MakeParam(TEXT("radial_steps"), EPwModelParamType::Integer, TEXT("24"), TEXT("Radial segments. Clamped to 3-256 with a warning; 0 or less reads as unset and takes the default 24.")),
        MakeParam(TEXT("height_steps"), EPwModelParamType::Integer, TEXT("1"), TEXT("ADDITIONAL wall loops, not the total - same reading as cylinder height_steps, so 0 is legal and means one wall segment. Clamped to 0-256 with a warning.")),
    }, EPwModelAimExtent::Scalar, TEXT("height")));

    Ops.Add(MakeGenerator(TEXT("ramp"), TEXT("A wedge."), {
        MakeParam(TEXT("size"), EPwModelParamType::Vector3, TEXT("(100, 200, 50)"),
            TEXT("Extents along local X, Y and Z; the slope rises along Y.")),
    }));

    Ops.Add(MakeGenerator(TEXT("revolve"), TEXT("A surface of revolution swept from a 2D profile."), {
        MakeRequired(TEXT("profile"), EPwModelParamType::PointList2,
            TEXT("Profile points [(x, y), …] revolved around the local Z axis: x is the RADIUS, y is the height. THE DIRECTION YOU WALK IT DECIDES THE FACING. The sweep keeps the winding the point order gives it, so the section must be traversed COUNTER-CLOCKWISE in that (x, y) plane - up the OUTER face, over the top, back down the INNER face, which is the enclosed material staying on your left. Walked the other way the solid comes out uniformly inside out and renders identically from every angle: isClosed, boundaryEdges, orientationConsistent, degenerates, the triangle count and the bounds are all unchanged and only health.signedVolume goes negative, which is what PWMODEL_REVOLVE_PROFILE_REVERSED reports at this line. The lathed silhouette that starts ON the axis obeys the same rule, but its figure looks nothing like a closed off-axis section, so do not read the direction off one to write the other.")),
        MakeParam(TEXT("angle"), EPwModelParamType::Number, TEXT("360"), TEXT("Revolution sweep in degrees.")),
        MakeParam(TEXT("steps"), EPwModelParamType::Integer, TEXT("16"), TEXT("Revolution steps. Clamped to 2-256 with a warning, or to 3-256 at angle=360 - its default - where the sweep closes and 2 cannot close it (8 triangles, 10 boundary edges, 6 degenerates). 0 or less takes the default 16.")),
        MakeParam(TEXT("capped"), EPwModelParamType::Bool, TEXT("true"), TEXT("Close an OPEN profile by capping its two ends to the REVOLVE AXIS - what turns a lathed silhouette into a solid. It is not about the ends of a partial sweep, and at angle=360 it is the flag that decides whether a bore stays open. A CLOSED SECTION - a ring, tube, rim or flange, written by repeating the first point as the last - needs no axis cap at all: the surface closes on itself, and the op detects the repeated endpoint, drops it, sweeps the section closed and warns that it did. On such a profile `capped` then means only the two ends of a partial sweep, and does nothing at angle=360.")),
    }));

    Ops.Add(MakeGenerator(TEXT("procedural_mesh"), TEXT("An explicitly empty mesh, to be built up with append_vertex / append_triangle / append_buffers."), {}));

    // ---- Modelling, deformers, repair ------------------------------------------

    // No `split_angle`: this op re-averages normals WITHIN the hard edges the mesh already has
    // and cannot create one, so the parameter it used to publish reached no engine call and was
    // silently discarded. `split_normals split_angle=` is the op that makes hard edges.
    Ops.Add(MakeModifier(TEXT("recalculate_normals"), TEXT("Recompute normals, preserving existing hard edges."), {
        MakeParam(TEXT("area_weighted"), EPwModelParamType::Bool, TEXT("true"), TEXT("Weight by triangle area.")),
        MakeParam(TEXT("angle_weighted"), EPwModelParamType::Bool, TEXT("true"), TEXT("Weight by corner angle.")),
    }));

    Ops.Add(MakeModifier(TEXT("flip_normals"), TEXT("Flip every normal and reverse triangle winding."), {}));

    Ops.Add(MakeModifier(TEXT("simplify_mesh"), TEXT("Reduce to a percentage of the current triangle count."), {
        MakeParam(TEXT("target_percentage"), EPwModelParamType::Number, TEXT("50"), TEXT("Target percentage of triangles.")),
        MakeEnum(TEXT("method"), TEXT("attribute_aware"),
            TEXT("Simplification metric. attribute_aware is the engine default and preserves volume and vertex normals; standard_qem is the crudest and ignores both. This op used to force standard_qem with no way to ask for anything else."),
            SimplifyMethodValues()),
        MakeParam(TEXT("allow_seam_collapse"), EPwModelParamType::Bool, TEXT("true"), TEXT("Let the simplifier collapse edges that lie on a UV / normal / material seam.")),
        MakeParam(TEXT("allow_seam_smoothing"), EPwModelParamType::Bool, TEXT("true"), TEXT("Let seam vertices move along the seam.")),
        MakeParam(TEXT("allow_seam_splits"), EPwModelParamType::Bool, TEXT("true"), TEXT("Let the simplifier split a seam edge.")),
        MakeParam(TEXT("preserve_vertex_positions"), EPwModelParamType::Bool, TEXT("false"), TEXT("Keep surviving vertices where they started instead of moving them to the quadric optimum.")),
        MakeParam(TEXT("retain_quadric_memory"), EPwModelParamType::Bool, TEXT("false"), TEXT("Trade memory for speed by keeping the quadric cache alive.")),
        MakeParam(TEXT("regularize_weight"), EPwModelParamType::Number, TEXT("0.000001"), TEXT("Small non-zero values improve triangle quality in flat regions.")),
        MakeParam(TEXT("auto_compact"), EPwModelParamType::Bool, TEXT("true"), TEXT("Compact the index space afterwards. Expensive; off leaves gaps.")),
        MakeEnum(TEXT("quadric_variant"), TEXT("plane_quadric"), TEXT("Which quadric the error metric is built from."),
            SimplifyQuadricVariantValues()),
        MakeParam(TEXT("normal_attribute_weight"), EPwModelParamType::Number, TEXT("16"), TEXT("Influence of normals on the metric. attribute-aware methods only.")),
        MakeParam(TEXT("tangent_attribute_weight"), EPwModelParamType::Number, TEXT("0.1"), TEXT("Influence of tangents and bitangents. attribute-aware methods only.")),
        MakeParam(TEXT("color_attribute_weight"), EPwModelParamType::Number, TEXT("0.1"), TEXT("Influence of vertex color. attribute-aware methods only.")),
        MakeParam(TEXT("texcoord_attribute_weight"), EPwModelParamType::Number, TEXT("0.5"), TEXT("Influence of UVs, all channels. attribute-aware methods only.")),
        MakeParam(TEXT("scale_correction"), EPwModelParamType::Number, TEXT("1"),
            TEXT("Rebalances geometry error against attribute error for a model built at a different scale than the weights were calibrated at. attribute-aware methods only.")),
        // The engine's three FGeometryScriptWeightMapDensity fields are NOT here: each needs an
        // FGeometryScriptWeightMapHandle naming a weight map on the mesh, and the format's only
        // identifier->resource binding is the material slot table. See FSimplifyMeshParams.
    }));

    Ops.Add(MakeModifier(TEXT("subdivide"), TEXT("PN tessellation."), {
        MakeParam(TEXT("iterations"), EPwModelParamType::Integer, TEXT("1"), TEXT("Subdivision iterations.")),
        MakeParam(TEXT("recompute_normals"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Recompute normals from the curved PN patch. This is what makes the result read as smooth; off keeps the faceted normals of the coarse mesh, which is what you want before a `split_normals` or your own bake.")),
    }));

    Ops.Add(MakeFaceModifier(TEXT("extrude"),
        TEXT("Extrude faces along a direction. With no face_direction the whole mesh is selected, which DUPLICATES a closed solid rather than thickening it."), {
        MakeParam(TEXT("distance"), EPwModelParamType::Number, TEXT("10"), TEXT("Extrude distance.")),
        MakeParam(TEXT("direction"), EPwModelParamType::Vector3, TEXT("(0, 0, 1)"), TEXT("Extrude direction. Ignored when direction_mode=average_face_normal.")),
        MakeEnum(TEXT("direction_mode"), TEXT("fixed_direction"),
            TEXT("fixed_direction pushes every face along `direction`. average_face_normal ignores `direction` entirely and pushes each region along its OWN averaged normal, which is what 'extrude these faces outward' means on a curved surface."),
            LinearExtrudeDirectionValues()),
        MakeParam(TEXT("solids_to_shells"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Turn a closed solid into a shell when the extrusion would otherwise fold it inside out.")),
        MakeParam(TEXT("face_direction"), EPwModelParamType::Vector3, TEXT(""),
            TEXT("Select only faces whose normal points this way. Omit to select the whole mesh.")),
        MakeParam(TEXT("face_angle_tolerance"), EPwModelParamType::Number, TEXT("45"),
            TEXT("Maximum normal deviation in degrees for face_direction selection.")),
    }));

    Ops.Add(MakeFaceModifier(TEXT("inset"), TEXT("Shrink faces inward in their own plane."), {
        MakeParam(TEXT("distance"), EPwModelParamType::Number, TEXT("5"), TEXT("Inset distance.")),
        MakeParam(TEXT("reproject"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Reproject the inset ring back onto the original surface, so it follows curvature. Off leaves it on the plane of the region - visible on a sphere, invisible on a box.")),
        MakeParam(TEXT("boundary_only"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("Inset only the boundary loop of the region rather than every face in it.")),
        MakeParam(TEXT("softness"), EPwModelParamType::Number, TEXT("0"),
            TEXT("Blends the inset ring toward its neighbours; 0 is a hard inset.")),
        MakeParam(TEXT("area_scale"), EPwModelParamType::Number, TEXT("1"),
            TEXT("Scales the distance by the region's area, so large and small faces inset proportionally instead of by the same absolute amount.")),
        MakeParam(TEXT("face_direction"), EPwModelParamType::Vector3, TEXT(""), TEXT("Face-normal filter.")),
        MakeParam(TEXT("face_angle_tolerance"), EPwModelParamType::Number, TEXT("45"), TEXT("Filter tolerance in degrees.")),
    }));

    // No `reproject` on outset, deliberately: the engine honours reprojection only for a
    // POSITIVE inset distance and outset always passes a negative one, so the parameter would
    // reach the engine and provably change nothing. `inset reproject=` is the one that works.
    Ops.Add(MakeFaceModifier(TEXT("outset"), TEXT("Expand faces outward in their own plane."), {
        MakeParam(TEXT("distance"), EPwModelParamType::Number, TEXT("5"), TEXT("Outset distance.")),
        MakeParam(TEXT("boundary_only"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("Outset only the boundary loop of the region rather than every face in it.")),
        MakeParam(TEXT("softness"), EPwModelParamType::Number, TEXT("0"),
            TEXT("Blends the outset ring toward its neighbours; 0 is a hard outset.")),
        MakeParam(TEXT("area_scale"), EPwModelParamType::Number, TEXT("1"),
            TEXT("Scales the distance by the region's area.")),
        MakeParam(TEXT("face_direction"), EPwModelParamType::Vector3, TEXT(""), TEXT("Face-normal filter.")),
        MakeParam(TEXT("face_angle_tolerance"), EPwModelParamType::Number, TEXT("45"), TEXT("Filter tolerance in degrees.")),
    }));

    // `material=` and the two ID parameters below carry the format's defaults, NOT the engine
    // struct's. GeometryOps::FBevelParams stays at the engine's `bInferMaterialID = false,
    // SetMaterialID = 0` because a parity test pins it there and the geometry.* RPC publishes it;
    // the DOCUMENT's defaults are decided here, and `material_id = 0` is the wrong one for a
    // document: slots are a model-wide table in first-use order, so 0 is whichever slot the first
    // part tagged, and an unqualified bevel therefore shipped every new face in another part's
    // material. Measured across one project: 38 of 38 bevels written by three independent teams
    // omitted infer_material_id, which is a default set the wrong way round rather than a choice.
    FPwModelOpSpec Bevel = MakeModifier(TEXT("bevel"),
        TEXT("Bevel polygroup edges. The mesh's polygroups ARE the edge selection, so both extremes bite: a mesh with no polygroups is returned unchanged, and a mesh with one polygroup per quad gets every interior quad boundary chamfered - a grid of notches that reads as surface damage, at roughly 3x the triangle cost. torus, arch and revolve are the second case: the engine's revolve generators group per quad and cannot be told otherwise. Both cases warn. filter_box_min / filter_box_max are the way out of the second one: they restrict the bevel to the edges inside a box. Before the engine runs, each selected edge is validated by exact topology ID and ordered span; invalid spans, spans containing a mesh-boundary edge the engine drops, and output already touched by an earlier bevel are skipped and reported at this bevel's line. Internal open spans remain eligible, including those ending at a mesh-boundary vertex. If the bevel adds self-intersections, its output is discarded and the input mesh restored."), {
        MakeParam(TEXT("distance"), EPwModelParamType::Number, TEXT("5"), TEXT("Bevel width. Applied to every safe polygroup edge the filter box leaves in; unsafe edges are skipped with a warning.")),
        MakeParam(TEXT("segments"), EPwModelParamType::Integer, TEXT("0"), TEXT("Rounding subdivisions across the bevel. Multiplies the cost of an already-dense polygroup layout.")),
        MakeParam(TEXT("round_weight"), EPwModelParamType::Number, TEXT("1"),
            TEXT("Roundness of the bevel profile. IGNORED at segments=0 - there is no interior loop to place - and the op warns if you set one without the other.")),
        MakeParam(TEXT("infer_material_id"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Take each new face's material from the two faces either side of the bevelled edge when they agree; material_id is used where they do not. On by default, which is the format's default and not the engine's: a chamfer belongs to the surfaces it joins, and the alternative put every new face on one fixed slot. Writing material= turns it OFF unless you also write infer_material_id=true, which then means 'the two sides where they agree, the named slot where they do not'.")),
        MakeParam(TEXT("material_id"), EPwModelParamType::Integer, TEXT(""),
            TEXT("RAW material index for the new bevel faces, bypassing the model-wide slot table - the fallback used wherever the two faces either side of an edge disagree, or everywhere when infer_material_id=false. Defaults to the slot MOST of the geometry being bevelled is on, so an unqualified bevel cannot land on another part's material. Prefer material=, which names a slot; both on one op is PWMODEL_MATERIAL_ID_CONFLICT.")),
        MakeParam(TEXT("filter_box_min"), EPwModelParamType::Vector3, TEXT(""),
            TEXT("Lower corner of a mesh-space box that restricts which polygroup edges are bevelled. Both corners must be given for the filter to apply; this is the ONLY edge filter the engine offers.")),
        MakeParam(TEXT("filter_box_max"), EPwModelParamType::Vector3, TEXT(""),
            TEXT("Upper corner of the filter box.")),
        MakeParam(TEXT("fully_contained"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("True bevels only edges entirely inside the filter box; false bevels any edge with a vertex in it.")),
    });
    AcceptMaterialSlot(Bevel,
        TEXT("Material slot for the CHAMFER FACES this op creates. Written, it turns infer_material_id off - the author named the faces - and every new face takes the named slot. Omitted, the two faces either side of each bevelled edge decide, falling back to the slot most of the bevelled geometry is on. Existing triangles are never recoloured either way."));
    // The engine tags bevel's output itself, per edge; the compiler's generic append-and-retag
    // path would flatten that to one slot. See FPwModelOpSpec::bSelfTagsMaterial.
    Bevel.bSelfTagsMaterial = true;
    Ops.Add(MoveTemp(Bevel));

    Ops.Add(MakeFaceModifier(TEXT("offset_faces"), TEXT("Offset faces along their normals."), {
        MakeParam(TEXT("distance"), EPwModelParamType::Number, TEXT("5"), TEXT("Offset distance.")),
        MakeEnum(TEXT("offset_type"), TEXT("parallel_face_offset"),
            TEXT("How the per-vertex offset direction is derived. parallel_face_offset moves each face along its own normal by exactly distance, so a box stays a box; vertex_normal averages at the corners and rounds them off."),
            OffsetFacesTypeValues()),
        MakeParam(TEXT("solids_to_shells"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Turn a closed solid into a shell when the offset would otherwise fold it inside out.")),
        MakeParam(TEXT("face_direction"), EPwModelParamType::Vector3, TEXT(""), TEXT("Face-normal filter.")),
        MakeParam(TEXT("face_angle_tolerance"), EPwModelParamType::Number, TEXT("45"), TEXT("Filter tolerance in degrees.")),
    }));

    Ops.Add(MakeModifier(TEXT("shell"), TEXT("Solidify: add an inner wall at a fixed thickness. The offset is an iterative SOLVE, not a rigid translation, so a wall that self-intersects on a concave mesh is usually fixed with solve_steps / smooth_alpha rather than with thickness."), {
        MakeParam(TEXT("thickness"), EPwModelParamType::Number, TEXT("5"), TEXT("Wall thickness.")),
        MakeParam(TEXT("fixed_boundary"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("Pin the boundary loops in place instead of offsetting them.")),
        MakeParam(TEXT("solve_steps"), EPwModelParamType::Integer, TEXT("5"),
            TEXT("Iterations of the offset solve. More is slower and closer to a true uniform offset.")),
        MakeParam(TEXT("smooth_alpha"), EPwModelParamType::Number, TEXT("0.1"),
            TEXT("Smoothing weight per solve step.")),
        MakeParam(TEXT("reproject_during_smoothing"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("Re-project onto the source surface between smoothing steps.")),
        MakeRanged(TEXT("boundary_alpha"), EPwModelParamType::Number, TEXT("0.2"), 0.0, 0.9,
            TEXT("Smoothing weight at the boundary. The engine's own note is 'should not be > 0.9', which is the ceiling here.")),
    }));

    Ops.Add(MakeModifier(TEXT("bend"), TEXT("Bend deformer."), {
        MakeParam(TEXT("angle"), EPwModelParamType::Number, TEXT("45"), TEXT("Bend angle in degrees.")),
        MakeParam(TEXT("extent"), EPwModelParamType::Number, TEXT("50"),
            TEXT("SYMMETRIC HALF-EXTENT measured ALONG axis FROM center: the deform spans [-extent, +extent] about that point, so a mesh of height H centred on it is covered at extent = H/2. Set symmetric_extents=false to give the lower half its own extent instead.")),
        MakeParam(TEXT("symmetric_extents"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("False makes the deform span [-lower_extent, +extent] instead of [-extent, +extent]. This is the answer to a mesh that does not straddle the origin, where a symmetric extent always covers the wrong half.")),
        MakeParam(TEXT("lower_extent"), EPwModelParamType::Number, TEXT("10"),
            TEXT("Lower half-extent. Read only when symmetric_extents=false.")),
        MakeEnum(TEXT("axis"), TEXT("z"),
            TEXT("The axis the extent is measured along. Defaults to z, which is what this op always used. Spelled the same as harmonic_deform's, because a deformer family whose members name their axis differently is one an author has to learn twice."),
            AxisValues()),
        MakeParam(TEXT("center"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
            TEXT("A point on that axis, in part-local space. The extent is measured from HERE, so a limb built away from the origin is deformed about itself rather than about a line off in space - which no combination of extent and lower_extent can express, since both are measured ALONG the axis. Deliberately not a bounding-box centre: that would move whenever an earlier op changed the mesh's extent, so the same document would deform differently depending on what ran before it.")),
        MakeParam(TEXT("bidirectional"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("True centres the bend on the origin and rigidly transforms both regions outside the extents. False starts the bend at the lower extent and leaves everything below it untouched.")),
    }));

    Ops.Add(MakeModifier(TEXT("twist"), TEXT("Twist deformer."), {
        MakeParam(TEXT("angle"), EPwModelParamType::Number, TEXT("45"), TEXT("Twist angle in degrees.")),
        MakeParam(TEXT("extent"), EPwModelParamType::Number, TEXT("50"),
            TEXT("SYMMETRIC HALF-EXTENT measured ALONG axis FROM center; see bend.")),
        MakeParam(TEXT("symmetric_extents"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("False makes the deform span [-lower_extent, +extent]; see bend.")),
        MakeParam(TEXT("lower_extent"), EPwModelParamType::Number, TEXT("10"),
            TEXT("Lower half-extent. Read only when symmetric_extents=false.")),
        MakeEnum(TEXT("axis"), TEXT("z"),
            TEXT("The axis the extent is measured along, and the axis the twist turns about. Defaults to z, which is what this op always used. Spelled the same as harmonic_deform's, because a deformer family whose members name their axis differently is one an author has to learn twice."),
            AxisValues()),
        MakeParam(TEXT("center"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
            TEXT("A point on that axis, in part-local space. The extent is measured from HERE, so a limb built away from the origin is deformed about itself rather than about a line off in space - which no combination of extent and lower_extent can express, since both are measured ALONG the axis. Deliberately not a bounding-box centre: that would move whenever an earlier op changed the mesh's extent, so the same document would deform differently depending on what ran before it.")),
        MakeParam(TEXT("bidirectional"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("False starts the twist at the lower extent and leaves everything below it untouched.")),
    }));

    Ops.Add(MakeModifier(TEXT("taper"), TEXT("Taper / flare deformer."), {
        MakeParam(TEXT("flare"), EPwModelParamType::Vector2, TEXT("(50, 50)"), TEXT("Flare percentage along local X and Y.")),
        MakeParam(TEXT("extent"), EPwModelParamType::Number, TEXT("50"),
            TEXT("SYMMETRIC HALF-EXTENT measured ALONG axis FROM center; see bend.")),
        MakeParam(TEXT("symmetric_extents"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("False makes the deform span [-lower_extent, +extent]; see bend.")),
        MakeParam(TEXT("lower_extent"), EPwModelParamType::Number, TEXT("10"),
            TEXT("Lower half-extent. Read only when symmetric_extents=false.")),
        MakeEnum(TEXT("axis"), TEXT("z"),
            TEXT("The axis the extent is measured along, and the axis the flare is perpendicular to. Defaults to z, which is what this op always used. Spelled the same as harmonic_deform's, because a deformer family whose members name their axis differently is one an author has to learn twice."),
            AxisValues()),
        MakeParam(TEXT("center"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
            TEXT("A point on that axis, in part-local space. The extent is measured from HERE, so a limb built away from the origin is deformed about itself rather than about a line off in space - which no combination of extent and lower_extent can express, since both are measured ALONG the axis. Deliberately not a bounding-box centre: that would move whenever an earlier op changed the mesh's extent, so the same document would deform differently depending on what ran before it.")),
        MakeEnum(TEXT("flare_type"), TEXT("sin_mode"),
            TEXT("Displacement profile swept over the extent. sin_squared_mode is the one to reach for when the flare has to blend into unwarped geometry: its normal derivative is continuous at both ends and sin_mode's is not, so a visible crease at the extent boundary is this parameter rather than the mesh."),
            FlareTypeValues()),
    }));

    Ops.Add(MakeModifier(TEXT("noise_deform"), TEXT("Perlin noise displacement."), {
        MakeParam(TEXT("magnitude"), EPwModelParamType::Number, TEXT("5"), TEXT("Displacement magnitude.")),
        MakeParam(TEXT("frequency"), EPwModelParamType::Number, TEXT("0.25"), TEXT("Noise frequency.")),
        MakeParam(TEXT("seed"), EPwModelParamType::Integer, TEXT("0"),
            TEXT("Selects the noise field. Perlin is spatial, so one seed gives one displacement per position: parts at different `at=` already differ, but two MODELS built from the same source differ only if their seeds do. There is one layer, so multi-octave noise is two noise_deform lines at different frequency and seed.")),
        MakeParam(TEXT("frequency_shift"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
            TEXT("Per-axis offset added to position before the noise lookup. Slides the sampling window continuously where seed jumps to an unrelated region.")),
        MakeParam(TEXT("apply_along_normal"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Displace along the vertex normal; false displaces by the 3D noise vector.")),
        MakeEnum(TEXT("normal_source"), TEXT("computed"),
            TEXT("Normals the displacement rides; only read when apply_along_normal."),
            { TEXT("computed"), TEXT("average_from_overlay") }),
        MakeEnum(TEXT("magnitude_mode"), TEXT("absolute"),
            TEXT("What `magnitude` MEASURES - the same noise field read with a different ruler, not a second field. absolute is a displacement in world units, identical everywhere, and is the default so an existing document is unchanged. relative makes it a FRACTION OF THE VERTEX'S OWN MEAN ONE-RING EDGE LENGTH, so one pass covers a model whose feature sizes differ instead of erasing the thin forms or leaving the thick ones smooth. Its proxy holds while the tessellation tracks the form and does NOT hold on a uniformly remeshed mesh, where every vertex reports the same number and relative degenerates into absolute with a rescaled magnitude. It also opens a hard seam whose two sides are tessellated differently, since each split vertex gets its own mean - merge_vertices first, or use absolute. Cannot be combined with apply_along_normal=false."),
            { TEXT("absolute"), TEXT("relative") }),
    }));

    // Sits beside noise_deform because it is the other displacement modifier, and reads as its
    // opposite: noise is a spatial field with no period, this is a period with no field.
    Ops.Add(MakeModifier(TEXT("harmonic_deform"),
        TEXT("Periodic displacement about an axis, driven by the AZIMUTH alone. A scalloped conifer rim is one term at order 2; a lumpy crown is orders 3 and 5 summed. Nothing here varies along the axis or with height - a shape that needs both is this op plus a warp deformer, not a parameter. It moves vertices and creates none, so a revolve's own winding survives it: the radial map is a strictly positive scale of the perpendicular radius, which preserves every triangle's facing normal, and the op REFUSES the amplitudes that would make it non-positive rather than clamping them."), {
        MakeRequired(TEXT("terms"), EPwModelParamType::HarmonicList,
            TEXT("The harmonic sum, as [(order, amplitude, phase), …]. `order` is cycles per revolution and must be a whole number of at least 1. `amplitude` is a FRACTION of the vertex's own perpendicular radius when target=radial - so a scallop stays proportional as a revolve profile's radius changes along the axis - and UNREAL UNITS when target=axial, which has no local scale to be a fraction of. `phase` is degrees, like every other angle in the format. An empty list is a no-op and says so.")),
        MakeEnum(TEXT("axis"), TEXT("z"),
            TEXT("The axis the azimuth is measured about. About z it is atan2(y, x); x and y follow cyclically."),
            AxisValues()),
        MakeEnum(TEXT("target"), TEXT("radial"),
            TEXT("What the sum displaces. radial scales the perpendicular radius by (1 + sum) and is the scalloped-rim and lumpy-crown case; axial adds the sum to the axis coordinate, which is how a rim alternates in HEIGHT. A rim that alternates in both is two lines, not one op."),
            HarmonicTargetValues()),
        MakeParam(TEXT("center"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"),
            TEXT("A point on the axis, in part-local space. Deliberately NOT the bounding-box centre spherify and cylindrify fit to: this axis is the one the geometry was revolved about, which is the local origin, and a box-derived centre would move whenever an earlier op changed the mesh's extent - so the same document would deform differently depending on what ran before it.")),
    }));

    Ops.Add(MakeModifier(TEXT("smooth"), TEXT("Iterative smoothing."), {
        MakeParam(TEXT("iterations"), EPwModelParamType::Integer, TEXT("10"), TEXT("Smoothing iterations.")),
        MakeParam(TEXT("alpha"), EPwModelParamType::Number, TEXT("0.2"), TEXT("Per-iteration strength.")),
    }));

    Ops.Add(MakeModifier(TEXT("relax"), TEXT("Laplacian relaxation."), {
        MakeParam(TEXT("iterations"), EPwModelParamType::Integer, TEXT("3"), TEXT("Relax iterations.")),
        MakeParam(TEXT("strength"), EPwModelParamType::Number, TEXT("0.5"), TEXT("Per-iteration strength.")),
    }));

    Ops.Add(MakeModifier(TEXT("stretch"), TEXT("Scale along one axis."), {
        MakeEnum(TEXT("axis"), TEXT("z"), TEXT("Axis to stretch."), AxisValues()),
        MakeParam(TEXT("factor"), EPwModelParamType::Number, TEXT("1.5"), TEXT("Stretch factor.")),
    }));

    Ops.Add(MakeModifier(TEXT("spherify"), TEXT("Project vertices toward a sphere."), {
        MakeParam(TEXT("factor"), EPwModelParamType::Number, TEXT("1"), TEXT("Blend 0-1 toward the sphere.")),
    }));

    Ops.Add(MakeModifier(TEXT("cylindrify"), TEXT("Project vertices toward a cylinder."), {
        MakeEnum(TEXT("axis"), TEXT("z"), TEXT("Cylinder axis."), AxisValues()),
        MakeParam(TEXT("factor"), EPwModelParamType::Number, TEXT("1"), TEXT("Blend 0-1 toward the cylinder.")),
    }));

    Ops.Add(MakeModifier(TEXT("weld_vertices"), TEXT("Weld edges whose vertices coincide."), {
        MakeParam(TEXT("tolerance"), EPwModelParamType::Number, TEXT("0.0001"),
            TEXT("Weld distance tolerance. Note this is NOT the engine's default of 1e-06, and `mirror` welds its seam at a looser 0.001 again - three tolerances for three jobs, all now settable.")),
        MakeParam(TEXT("only_unique_pairs"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Merge only unambiguous pairs - edges with exactly one duplicate match. Off lets the weld resolve ambiguous matches, which closes a seam where several edges coincide per vertex and can leave a non-manifold edge when it guesses wrong.")),
    }));

    Ops.Add(MakeModifier(TEXT("fill_holes"), TEXT("Fill every boundary loop."), {
        MakeEnum(TEXT("method"), TEXT("automatic"),
            TEXT("How each hole is triangulated. automatic picks per hole; the explicit methods are the escape hatch for the ones it picks badly - a large planar hole comes back as a fan of slivers under the automatic minimal fill, where polygon_triangulation or planar_projection produce a usable triangulation."),
            FillHolesMethodValues()),
        MakeParam(TEXT("delete_isolated_triangles"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Floating disconnected triangles bound a 'hole' that cannot be filled, so the engine deletes them first. Off keeps them and lets them be reported as failed fills instead.")),
    }));

    Ops.Add(MakeModifier(TEXT("remove_degenerates"), TEXT("Remove zero-area triangles and duplicate vertices. Both thresholds are ABSOLUTE world units, so on a mesh authored in metres rather than centimetres the defaults are effectively zero and the op looks like it does nothing."), {
        MakeEnum(TEXT("mode"), TEXT("repair_or_delete"),
            TEXT("What happens to a triangle found degenerate. repair_or_delete collapses it if it can and deletes it otherwise; delete_only skips the collapse; repair_or_skip never deletes, so the triangle count cannot drop."),
            RepairMeshModeValues()),
        MakeParam(TEXT("min_triangle_area"), EPwModelParamType::Number, TEXT("0.001"),
            TEXT("A triangle below this area is degenerate.")),
        MakeParam(TEXT("min_edge_length"), EPwModelParamType::Number, TEXT("0.0001"),
            TEXT("An edge below this length is degenerate.")),
        MakeParam(TEXT("compact_on_completion"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Compact the vertex and triangle ID lists afterwards. Off preserves existing IDs, which matters when a later op addresses elements by index.")),
    }));

    Ops.Add(MakeModifier(TEXT("remesh_uniform"), TEXT("Uniform remesh to a target triangle count."), {
        MakeParam(TEXT("target_triangle_count"), EPwModelParamType::Integer, TEXT("5000"),
            TEXT("Target triangle count. Read only when target_type=triangle_count, and APPROXIMATE either way: the remesher converts it to an edge length.")),
        MakeEnum(TEXT("target_type"), TEXT("triangle_count"), TEXT("Which goal drives the remesh."),
            RemeshTargetTypeValues()),
        MakeParam(TEXT("target_edge_length"), EPwModelParamType::Number, TEXT("1"),
            TEXT("Desired edge length in Unreal units. Read only when target_type=target_edge_length; 1 uu is very dense for primitive-scale geometry.")),
        MakeParam(TEXT("discard_attributes"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("Drop every mesh attribute first, so UV and normal seams stop constraining the remesh. The fastest way to a regular mesh and the fastest way to lose a UV layout.")),
        MakeParam(TEXT("reproject_to_input_mesh"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Project vertices back onto the input surface as the remesh runs, preserving shape.")),
        MakeEnum(TEXT("smoothing_type"), TEXT("mixed"), TEXT("3D smoothing applied during the remesh."),
            RemeshSmoothingTypeValues()),
        MakeRanged(TEXT("smoothing_rate"), EPwModelParamType::Number, TEXT("0.25"), 0.0, 1.0,
            TEXT("Smoothing speed. 0 disables smoothing entirely.")),
        MakeEnum(TEXT("mesh_boundary_constraint"), TEXT("free"), TEXT("Constraint on open boundary edges."),
            RemeshEdgeConstraintValues()),
        MakeEnum(TEXT("group_boundary_constraint"), TEXT("free"), TEXT("Constraint on polygroup boundary edges."),
            RemeshEdgeConstraintValues()),
        MakeEnum(TEXT("material_boundary_constraint"), TEXT("free"), TEXT("Constraint on material boundary edges."),
            RemeshEdgeConstraintValues()),
        MakeParam(TEXT("allow_flips"), EPwModelParamType::Bool, TEXT("true"), TEXT("Allow edge flips. Off markedly lowers output quality.")),
        MakeParam(TEXT("allow_splits"), EPwModelParamType::Bool, TEXT("true"), TEXT("Allow edge splits, i.e. let density rise.")),
        MakeParam(TEXT("allow_collapses"), EPwModelParamType::Bool, TEXT("true"), TEXT("Allow edge collapses, i.e. let density fall. Clearing this and allow_splits together makes the target unreachable by construction.")),
        MakeParam(TEXT("prevent_normal_flips"), EPwModelParamType::Bool, TEXT("true"), TEXT("Skip any flip or collapse that would flip a face normal.")),
        MakeParam(TEXT("prevent_tiny_triangles"), EPwModelParamType::Bool, TEXT("true"), TEXT("Skip any flip or collapse that would create a degenerate triangle.")),
        MakeParam(TEXT("use_full_remesh_passes"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("Use the expensive full-pass strategy instead of the edge queue: higher quality, and it multiplies the cost of iterations.")),
        MakeParam(TEXT("iterations"), EPwModelParamType::Integer, TEXT("20"), TEXT("Remesh iterations.")),
        MakeParam(TEXT("auto_compact"), EPwModelParamType::Bool, TEXT("true"), TEXT("Compact the index space afterwards. Expensive; off leaves gaps.")),
    }));

    Ops.Add(MakeModifier(TEXT("merge_vertices"), TEXT("Merge vertices within a tolerance."), {
        MakeParam(TEXT("tolerance"), EPwModelParamType::Number, TEXT("0.001"), TEXT("Merge distance tolerance.")),
        MakeParam(TEXT("compact"), EPwModelParamType::Bool, TEXT("true"), TEXT("Compact index buffers afterwards.")),
    }));

    Ops.Add(MakeFaceModifier(TEXT("poke"), TEXT("Poke faces: offset them, then insert a centre vertex and fan the triangle. It is offset_faces followed by ONE PN tessellation, so it carries both sets of options."), {
        MakeParam(TEXT("offset"), EPwModelParamType::Number, TEXT("0"), TEXT("Displace the inserted vertex along the face normal.")),
        MakeEnum(TEXT("offset_type"), TEXT("parallel_face_offset"),
            TEXT("How the offset direction is derived; see offset_faces."), OffsetFacesTypeValues()),
        MakeParam(TEXT("solids_to_shells"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Turn a closed solid into a shell when the offset would otherwise fold it inside out.")),
        MakeParam(TEXT("recompute_normals"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Recompute normals from the curved PN patch after the tessellation.")),
    }));

    Ops.Add(MakeModifier(TEXT("recompute_tangents"), TEXT("Recompute tangents. A tangent basis only means anything relative to a UV unwrap, so uv_layer has to name the channel the normal map was authored against - point it at the wrong one and the bake reads inverted along one axis."), {
        MakeEnum(TEXT("type"), TEXT("fast_mikkt"),
            TEXT("Tangent algorithm. standard_mikkt is the reference implementation and matches what most bakers assume; per_triangle skips the smoothing entirely."),
            TangentTypeValues()),
        MakeRanged(TEXT("uv_layer"), EPwModelParamType::Integer, TEXT("0"), 0.0, 7.0,
            TEXT("UV channel the tangent basis is built from.")),
    }));

    Ops.Add(MakeModifier(TEXT("split_normals"), TEXT("Split normals at edges over an angle threshold, at polygroup boundaries, or both."), {
        MakeParam(TEXT("split_angle"), EPwModelParamType::Number, TEXT("60"),
            TEXT("Split threshold in degrees. NOT the engine's default of 15: this surface has published 60 since it shipped, and adopting the engine value would put a hard edge on every mesh that currently comes out smooth. ZERO IS THE MAXIMUM this op splits and is the spelling of per-face normals; the engine compares through a cosine of this value, so a NEGATIVE angle has its sign discarded and splits less than the default rather than more. It warns.")),
        MakeParam(TEXT("split_by_opening_angle"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Split where the dihedral angle exceeds split_angle. With this and split_by_face_group both off there is no split predicate left, so the op recomputes normals with NO hard edges - the mesh comes back fully smoothed rather than unchanged, and it warns.")),
        MakeParam(TEXT("split_by_face_group"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("Also split along polygroup boundaries. Independent of the angle test - the engine ORs the two.")),
        MakeParam(TEXT("use_default_group_layer"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Use the standard polygroup layer. Read only when split_by_face_group.")),
        MakeParam(TEXT("group_layer_index"), EPwModelParamType::Integer, TEXT("0"),
            TEXT("Index of an extended polygroup layer. Read only when split_by_face_group and use_default_group_layer=false.")),
    }));

    Ops.Add(MakeModifier(TEXT("transform"),
        TEXT("Transform the geometry accumulated in the part so far, in part-local space. rotate and scale PIVOT ABOUT THE PART-LOCAL ORIGIN, not about the geometry, so anything authored away from that origin is DISPLACED as well as turned - a lever arm of distance * sin(angle). To turn a shape in place put rotate= on its own generator, which composes into the primitive before its at=. scale bakes into vertex positions."), {
        MakeParam(TEXT("at"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("Translation in Unreal units.")),
        MakeParam(TEXT("rotate"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("Rotation (roll, pitch, yaw) in degrees, about the PART-LOCAL ORIGIN. Geometry sitting a distance d from that origin is swung distance * sin(angle) sideways as well as turned: a boolean tool at z=1790 tilted 7 degrees lands 218 uu off in X, and nothing in the health gate reports it. The generator's own rotate= is the in-place alternative.")),
        MakeParam(TEXT("scale"), EPwModelParamType::Vector3, TEXT("(1, 1, 1)"), TEXT("Per-axis scale, about the PART-LOCAL ORIGIN - the same lever arm as rotate=, so geometry a distance d from the origin is moved (factor - 1) * d along each axis.")),
    }));

    Ops.Add(MakeModifier(TEXT("translate_mesh"), TEXT("Translate the accumulated geometry. The translation-only case of transform."), {
        MakeRequired(TEXT("translation"), EPwModelParamType::Vector3, TEXT("Translation in Unreal units.")),
    }));

    // One `uv` op rather than four verbs: geometry.project_uv, geometry.auto_uv,
    // geometry.unwrap_uv and geometry.pack_uv_islands all write the same UV channel and
    // differ only in the projection they use, so they are one op with a mode. `layout`
    // additionally reaches LayoutMeshUVs, which no RPC verb exposes today.
    Ops.Add(MakeModifier(TEXT("uv"), TEXT("Generate UVs into a channel."), {
        MakeUvChannelParam(TEXT("UV channel to write.")),
        MakeRequiredEnum(TEXT("mode"), TEXT("Projection to use."),
            { TEXT("box"), TEXT("planar"), TEXT("cylindrical"), TEXT("xatlas"), TEXT("patch_builder"), TEXT("layout") }),
        MakeParam(TEXT("scale"), EPwModelParamType::Vector2, TEXT("(1, 1)"), TEXT("Projection scale. box / planar / cylindrical only.")),
        MakeParam(TEXT("split_angle"), EPwModelParamType::Number, TEXT("45"), TEXT("Seam angle for mode=cylindrical.")),
        MakeParam(TEXT("max_iterations"), EPwModelParamType::Integer, TEXT("2"), TEXT("Solver iterations for mode=xatlas.")),

        // mode=layout. Prefixed `layout_` only where the engine field would collide with a name
        // this op already publishes: FGeometryScriptLayoutUVsOptions::Scale is a UNIFORM
        // post-pack scale and `scale` here is the 2D projection frame, so the two cannot share a
        // name. `texture_resolution` keeps its unprefixed spelling because it shipped that way.
        MakeUVTextureResolutionParam(
            TEXT("mode=layout: expected output texture resolution. Sets the gutter left between islands; it writes nothing at that size.")),
        MakeEnum(TEXT("layout_type"), TEXT("repack"),
            TEXT("mode=layout: repack fits the islands into the unit square with no overlap, stack fits each one individually (so they overlap), normalize equalizes texel density, transform only applies layout_scale and translation."),
            UVLayoutTypeValues()),
        MakeParam(TEXT("layout_scale"), EPwModelParamType::Number, TEXT("1"), TEXT("mode=layout: uniform scale applied to the UVs after packing.")),
        MakeParam(TEXT("translation"), EPwModelParamType::Vector2, TEXT("(0, 0)"), TEXT("mode=layout: translation applied after packing and scaling.")),
        MakeParam(TEXT("preserve_scale"), EPwModelParamType::Bool, TEXT("false"), TEXT("mode=layout, repack only: keep each island's existing scale. Can push the packing outside the unit square.")),
        MakeParam(TEXT("preserve_rotation"), EPwModelParamType::Bool, TEXT("false"), TEXT("mode=layout, repack only: keep each island's existing rotation. Costs packing efficiency.")),
        MakeParam(TEXT("allow_flips"), EPwModelParamType::Bool, TEXT("false"), TEXT("mode=layout: let the packer mirror an island to save space. Off by default because a flipped island breaks anything downstream that assumes winding.")),
        MakeParam(TEXT("enable_udim_layout"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("mode=layout: keep islands inside their originating UDIM tile. The engine's per-tile resolution map is not expressible in this format, so every tile uses texture_resolution.")),

        // mode=patch_builder. Unprefixed snake_case of the engine field, except the two nested
        // option structs, which are flattened behind their sub-struct's name: FPwValue has
        // no sub-object member so a nested block has no literal.
        MakeParam(TEXT("initial_patch_count"), EPwModelParamType::Integer, TEXT("100"),
            TEXT("mode=patch_builder: seed patch count for the clustering pass, not the island count of the result - merging collapses patches.")),
        MakeParam(TEXT("min_patch_size"), EPwModelParamType::Integer, TEXT("2"), TEXT("mode=patch_builder: smallest patch, in triangles.")),
        MakeParam(TEXT("patch_curvature_alignment_weight"), EPwModelParamType::Number, TEXT("1"), TEXT("mode=patch_builder: how strongly patches align to surface curvature.")),
        MakeParam(TEXT("patch_merging_metric_thresh"), EPwModelParamType::Number, TEXT("1.5"), TEXT("mode=patch_builder: distortion threshold above which two patches stop merging.")),
        MakeParam(TEXT("patch_merging_angle_thresh"), EPwModelParamType::Number, TEXT("45"), TEXT("mode=patch_builder: normal-angle threshold above which two patches stop merging, in degrees.")),
        MakeParam(TEXT("exp_map_normal_smoothing_rounds"), EPwModelParamType::Integer, TEXT("0"), TEXT("mode=patch_builder: normal smoothing rounds before the ExpMap flatten. Rounder patches, less faithful to the surface.")),
        MakeParam(TEXT("exp_map_normal_smoothing_alpha"), EPwModelParamType::Number, TEXT("0.25"), TEXT("mode=patch_builder: per-round smoothing strength for the above.")),
        MakeParam(TEXT("respect_input_groups"), EPwModelParamType::Bool, TEXT("false"), TEXT("mode=patch_builder: start from the mesh's polygroups instead of clustering from scratch.")),
        MakeParam(TEXT("auto_pack"), EPwModelParamType::Bool, TEXT("true"), TEXT("mode=patch_builder: pack the patches into the unit square. Off, the two packing_* parameters are read by nothing.")),
        MakeParam(TEXT("packing_target_image_width"), EPwModelParamType::Integer, TEXT("512"),
            TEXT("mode=patch_builder: gutter resolution for its own packer. NOT texture_resolution - different engine struct, different default, and matching them up would be inventing one.")),
        MakeParam(TEXT("packing_optimize_island_rotation"), EPwModelParamType::Bool, TEXT("true"), TEXT("mode=patch_builder: let the packer rotate islands to save space.")),
        // FGeometryScriptPatchBuilderOptions::GroupLayer is NOT published: it selects a named
        // polygroup layer that must already exist, and nothing in this format creates one. See
        // GeometryOps::FPatchBuilderUVParams.
    }));

    Ops.Add(MakeModifier(TEXT("transform_uvs"), TEXT("Translate, scale and rotate an existing UV channel."), {
        MakeUvChannelParam(TEXT("UV channel to transform.")),
        MakeParam(TEXT("translate"), EPwModelParamType::Vector2, TEXT("(0, 0)"), TEXT("UV translation.")),
        MakeParam(TEXT("scale"), EPwModelParamType::Vector2, TEXT("(1, 1)"), TEXT("UV scale.")),
        MakeParam(TEXT("rotate"), EPwModelParamType::Number, TEXT("0"), TEXT("UV rotation in degrees.")),
    }));

    // ---- Element edits ----------------------------------------------------------

    Ops.Add(MakeModifier(TEXT("set_vertex_position"), TEXT("Move one vertex."), {
        MakeRequired(TEXT("index"), EPwModelParamType::Integer, TEXT("Vertex ID.")),
        MakeRequired(TEXT("position"), EPwModelParamType::Vector3, TEXT("New position.")),
    }));

    Ops.Add(MakeModifier(TEXT("append_vertex"), TEXT("Add one vertex."), {
        MakeRequired(TEXT("position"), EPwModelParamType::Vector3, TEXT("Vertex position.")),
    }));

    Ops.Add(MakeModifier(TEXT("delete_vertex"), TEXT("Remove one vertex and every triangle using it."), {
        MakeRequired(TEXT("index"), EPwModelParamType::Integer, TEXT("Vertex ID.")),
    }));

    // Takes the slot tag, for the same reason `append_buffers` does and against the same
    // measured failure. Without it a patch had NO way to name a slot, and the compiler's
    // untagged path allocated `Default` for it: a part carrying `material="Cast"` on its
    // `procedural_mesh` and its `append_buffers`, patched with twelve `append_triangle` ops,
    // compiled to materialSlots 2 and shipped the twelve patch triangles rendering in
    // WorldGridMaterial while the rest of the part rendered in the bound one. That is not a
    // slot-allocation quirk to document around - it is the only op in the format that produces
    // geometry and cannot say what it is made of.
    //
    // No local transform (`v0`/`v1`/`v2` are already explicit positions) and no `color=`,
    // which is what keeps it a bare generator with one parameter added rather than a
    // MakeGenerator. `color=` is withheld only because nothing has needed it; unlike
    // `append_buffers`, this op carries no per-vertex `colors=` buffer for it to overwrite.
    FPwModelOpSpec AppendTriangle =
        MakeBareGenerator(TEXT("append_triangle"), TEXT("Add one triangle from three explicit positions."), {
            MakeParam(TEXT("v0"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("First vertex.")),
            MakeParam(TEXT("v1"), EPwModelParamType::Vector3, TEXT("(100, 0, 0)"), TEXT("Second vertex.")),
            MakeParam(TEXT("v2"), EPwModelParamType::Vector3, TEXT("(50, 100, 0)"), TEXT("Third vertex.")),
            MakeParam(TEXT("group_id"), EPwModelParamType::Integer, TEXT("0"), TEXT("Polygroup ID.")),
        });
    AcceptMaterialSlot(AppendTriangle);
    Ops.Add(MoveTemp(AppendTriangle));

    Ops.Add(MakeModifier(TEXT("delete_triangle"), TEXT("Remove one triangle."), {
        MakeRequired(TEXT("index"), EPwModelParamType::Integer, TEXT("Triangle ID.")),
    }));

    Ops.Add(MakeModifier(TEXT("set_vertex_color"), TEXT("Write the per-corner color overlay."), {
        MakeParam(TEXT("index"), EPwModelParamType::Integer, TEXT(""), TEXT("Vertex ID. Required unless set_all is true.")),
        MakeParam(TEXT("color"), EPwModelParamType::Vector4, TEXT("(1, 1, 1, 1)"), TEXT("Color as (r, g, b, a).")),
        MakeParam(TEXT("set_all"), EPwModelParamType::Bool, TEXT("false"), TEXT("Write every vertex instead of one.")),
        MakeParam(TEXT("channels"), EPwModelParamType::String, TEXT("rgba"), TEXT("Which components of color actually land: any combination of r, g, b, a. \"a\" writes only alpha and leaves an RGB tint intact.")),
    }));

    // The one op that MEASURES the accumulated geometry instead of moving it. It has to live in
    // the model source rather than only on the RPC surface: a part built from interpenetrating
    // primitives has no contact shading at any junction, and a post-compile RPC call would have
    // to be re-run by hand on every model edit.
    Ops.Add(MakeModifier(TEXT("bake_ao"), TEXT("Ray-cast the part against itself and write ambient occlusion into vertex-colour channels."), {
        MakeRequired(TEXT("radius"), EPwModelParamType::Number,
            TEXT("Maximum ray length in the model's own units. No default: it is what makes a part junction occlude while the far side of the same mesh does not.")),
        MakeRequired(TEXT("channels"), EPwModelParamType::String,
            TEXT("Which vertex-colour channels receive the bake: any combination of r, g, b, a. \"a\" leaves a generator-written RGB tint intact.")),
        MakeParam(TEXT("samples"), EPwModelParamType::Integer, TEXT("64"),
            TEXT("Rays cast per colour element. Trades noise for time linearly.")),
        MakeParam(TEXT("bias_angle"), EPwModelParamType::Number, TEXT("15"),
            TEXT("Degrees. Rays arriving within this angle of the surface's own tangent plane have their weight rolled off, which stops a faceted surface reading its neighbouring facet as an occluder. An ANGLE, not a distance offset.")),
        MakeEnum(TEXT("blend"), TEXT("replace"),
            TEXT("replace writes the bake into the masked channels; multiply scales what they already carry by it."),
            { TEXT("replace"), TEXT("multiply") }),
        MakeParam(TEXT("strength"), EPwModelParamType::Number, TEXT("1"),
            TEXT("0-1, lerps the bake toward fully exposed; 0 writes white and darkens nothing.")),
    }));

    Ops.Add(MakeModifier(TEXT("set_uvs"), TEXT("Write one vertex's UV in a channel."), {
        MakeRequired(TEXT("index"), EPwModelParamType::Integer, TEXT("Vertex ID.")),
        MakeRequired(TEXT("uv"), EPwModelParamType::Vector2, TEXT("UV coordinate.")),
        MakeUvChannelParam(TEXT("UV channel to write.")),
    }));

    // ---- Booleans and merge-transforms ------------------------------------------

    // The three symmetric booleans share one parameter list with `trim`, built per call
    // because FPwModelOpSpec owns its params by value. The bSimplifyOutputDefault parameter
    // survives from when the two surfaces disagreed - `union`/`subtract`/`intersection` were
    // pinned FALSE and `trim` ran the engine's TRUE. All four are now TRUE
    // (GeometryOps::FBooleanParams::bSimplifyOutput carries the flip and its audit), so every
    // caller below passes true. Kept parameterised rather than inlined so the next surface
    // that genuinely needs the other default cannot silently change these four.
    const auto BooleanOptionParams = [](bool bSimplifyOutputDefault)
    {
        return TArray<FPwModelParamSpec>{
            MakeParam(TEXT("fill_holes"), EPwModelParamType::Bool, TEXT("true"),
                TEXT("Fill the holes the cut opens.")),
            MakeParam(TEXT("simplify_output"), EPwModelParamType::Bool,
                bSimplifyOutputDefault ? TEXT("true") : TEXT("false"),
                TEXT("Collapse the small coplanar triangles the boolean itself generates along the new cut edges. On - the engine default. It only touches triangles the CUT created, at 0.1 degrees from coplanar, and is barred from distorting polygroups, UVs and normals, so it removes fan debris rather than geometry you authored. Off keeps every triangle the cut produced: one measured union came back at 135,324 triangles against 2,608 appended.")),
            // SimplifyPlanarTolerance is NOT published: UE 5.8's ApplyMeshBoolean never
            // forwards it to the operation (MeshBooleanFunctions.cpp:87), so it would be a
            // parameter that does nothing. `self_union` publishes its own because
            // ApplyMeshSelfUnion does forward it.
            MakeParam(TEXT("allow_empty_result"), EPwModelParamType::Bool, TEXT("false"),
                TEXT("Let the boolean produce an empty mesh instead of failing. The empty part is then reported by PWMODEL_EMPTY_MESH one stage later, so this moves where the failure is named rather than removing it.")),
            // FGeometryScriptMeshBooleanOptions::OutputTransformSpace is NOT published here. It
            // chooses which operand's local space the result lands in, and this compiler passes
            // IDENTITY for both operand transforms (every op bakes its at/rotate/scale into the
            // vertices first), so all three of its values produce the same geometry. It is
            // published on the geometry.boolean_* RPC verbs instead, where the two transforms
            // are two different actors' and the choice is observable.
        };
    };

    Ops.Add(MakeBoolean(TEXT("union"), TEXT("Union with the mesh built by the block."),
        BooleanOptionParams(/*bSimplifyOutputDefault=*/true)));
    Ops.Add(MakeBoolean(TEXT("subtract"), TEXT("Subtract the mesh built by the block."),
        BooleanOptionParams(/*bSimplifyOutputDefault=*/true)));
    Ops.Add(MakeBoolean(TEXT("intersection"), TEXT("Keep only what is inside both meshes."),
        BooleanOptionParams(/*bSimplifyOutputDefault=*/true)));

    {
        TArray<FPwModelParamSpec> TrimParams;
        TrimParams.Add(MakeParam(TEXT("keep_inside"), EPwModelParamType::Bool, TEXT("false"),
            TEXT("Keep the inside rather than the outside. Selects the operation - intersection or subtraction - rather than tuning it.")));
        TrimParams.Append(BooleanOptionParams(/*bSimplifyOutputDefault=*/true));
        Ops.Add(MakeBoolean(TEXT("trim"), TEXT("Trim against the mesh built by the block, keeping one side."),
            MoveTemp(TrimParams)));
    }

    Ops.Add(MakeModifier(TEXT("self_union"), TEXT("Resolve self-intersections in the accumulated mesh."), {
        MakeParam(TEXT("fill_holes"), EPwModelParamType::Bool, TEXT("true"), TEXT("Fill holes left by the resolve.")),
        MakeParam(TEXT("trim_flaps"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Drop the open half-faces a self-intersection leaves bounding nothing. Off keeps them, which is what a SURFACE model wants - there the flaps are the geometry, not debris.")),
        MakeParam(TEXT("simplify_output"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Collapse coplanar triangles in the result. On by default here, unlike the four boolean ops, which is the engine's own default and what this op has always run.")),
        MakeParam(TEXT("simplify_planar_tolerance"), EPwModelParamType::Number, TEXT("0.01"),
            TEXT("Coplanarity tolerance for the simplification pass. Ignored when simplify_output=false.")),
        MakeParam(TEXT("winding_threshold"), EPwModelParamType::Number, TEXT("0.5"),
            TEXT("Winding-number isovalue separating inside from outside. 0.5 keeps anything inside at least one shell; raising it toward 1.5 keeps only what is covered twice, turning the resolve into an intersection.")),
    }));

    Ops.Add(MakeModifier(TEXT("mirror"),
        TEXT("Mirror-and-MERGE: appends an axis-negated copy to the existing geometry. It does not replace the mesh with its reflection. The copy's triangle winding is reversed to match the original, which is what a reflection requires: negating one axis has determinant -1 and turns every triangle inside out, so a copy appended without the reversal renders backface-culled (black) and acts as an anti-solid inside a boolean tool. Note it APPENDS rather than unions - two halves that overlap keep the faces buried between them, so wrap the op in a union block when the sides meet rather than merely touch."), {
        MakeEnum(TEXT("axis"), TEXT("x"), TEXT("Mirror axis."), AxisValues()),
        MakeParam(TEXT("weld"), EPwModelParamType::Bool, TEXT("true"), TEXT("Weld vertices at the mirror plane.")),
        MakeParam(TEXT("weld_tolerance"), EPwModelParamType::Number, TEXT("0.001"),
            TEXT("Seam weld tolerance. Deliberately 10x weld_vertices' 0.001 -> 0.0001 default and 1000x the engine's 1e-06: mirror welds ONE plane whose two sides it just generated, so the only vertices in range are the ones meant to meet, and they miss by whatever floating-point residue the generator left. Tightening it leaves a crack that shows up as a lighting split rather than in the triangle count.")),
        MakeParam(TEXT("only_unique_pairs"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Merge only unambiguous edge pairs during the seam weld; see weld_vertices.")),
    }));

    Ops.Add(MakeModifier(TEXT("array_linear"), TEXT("Merge offset copies of the accumulated mesh into itself."), {
        MakeParam(TEXT("count"), EPwModelParamType::Integer, TEXT("3"), TEXT("Total copies including the original.")),
        MakeParam(TEXT("offset"), EPwModelParamType::Vector3, TEXT("(100, 0, 0)"), TEXT("Offset between copies.")),
    }));

    Ops.Add(MakeModifier(TEXT("array_radial"), TEXT("Merge rotated copies of the accumulated mesh into itself."), {
        MakeParam(TEXT("count"), EPwModelParamType::Integer, TEXT("6"), TEXT("Total copies including the original.")),
        MakeParam(TEXT("center"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("Centre of rotation.")),
        MakeEnum(TEXT("axis"), TEXT("z"), TEXT("Rotation axis."), AxisValues()),
        MakeParam(TEXT("angle"), EPwModelParamType::Number, TEXT("360"), TEXT("Total sweep in degrees.")),
    }));

    Ops.Add(MakeModifier(TEXT("array_along_path"),
        TEXT("Merge copies of the accumulated mesh into itself, one at each frame of path. The frame list IS the count - N frames produce exactly N copies, so a leading (0, 0, 0, 0, 0, 0) frame keeps a copy where the original stood, and the original is NOT additionally left in place. That is the same counting rule as array_linear, whose original occupies the first of its `count` positions."), {
        MakeRequired(TEXT("path"), EPwModelParamType::FrameList,
            TEXT("Placements [(x, y, z, roll, pitch, yaw), …], part-local. Absolute frames, not offsets between copies. 1 to 100 of them.")),
    }));

    // ---- Path-driven -------------------------------------------------------------
    //
    // Modifiers, not generators, and both APPEND: the swept surface is added to the geometry
    // already in the part rather than replacing it. Without `profile=` that accumulated geometry
    // is also what SIZES the cross-section, which is the reason neither op may open a part.
    //
    // BOTH TAKE `material=`, and they are the only modifiers that do - the same shape
    // `append_triangle` and `append_buffers` take it in, and for the identical reason. Because
    // they APPEND, they produce triangles of their own, and triangles need a slot; the engine
    // call stamps every new one with material ID 0, which is not a neutral default but the
    // model-wide table's FIRST-USE index - whichever slot the first part in the document tagged.
    // A rod swept in part `b` therefore shipped on part `a`'s material, on a green compile, with
    // the slot count unchanged. The compiler now gives untagged output the slot of the geometry
    // it extends (PwModelCompiler.cpp ApplyModifierMaterialTag); this parameter is how an author
    // says something else. The other modifiers deform, delete or copy triangles that already
    // carry a slot, so tagging them would RECOLOUR geometry rather than colour new geometry -
    // which is why the tag is on these two and not on MakeModifier.
    FPwModelOpSpec Sweep = MakeModifier(TEXT("sweep"),
        TEXT("Sweep a cross-section along a path and APPEND the result to the accumulated geometry. Without `profile=` the cross-section is a CIRCLE sized from the accumulated mesh's bounding box - the op never traces the mesh's outline. Without `path=` it sweeps vertically through the mesh's own bounding box instead, which is the linear fallback geometry.sweep runs when its spline actor resolves to nothing. The swept surface takes `material=` when written; untagged, it INHERITS the material slot of the geometry it was appended to, and geometry carrying more than one slot has no single answer to inherit, so the op takes the slot most of that geometry is on and warns PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS."), {
        MakeParam(TEXT("path"), EPwModelParamType::FrameList, TEXT(""),
            TEXT("Path frames [(x, y, z, roll, pitch, yaw), …], part-local. At least 2; omit it entirely for the vertical fallback.")),
        MakeParam(TEXT("profile"), EPwModelParamType::PointList2, TEXT(""),
            TEXT("Cross-section [(u, v), …] swept along the path, as an OPEN polygon in the same form revolve takes. At least 3 points. Omitted: the bounding-box circle.")),
        MakeParam(TEXT("steps"), EPwModelParamType::Integer, TEXT("16"),
            TEXT("Sides of the fallback circular cross-section, as clamp(steps/2, 4, 32), AND the step count of the vertical fallback path. It does not resample `path=`, which is taken as written.")),
        MakeParam(TEXT("twist"), EPwModelParamType::Number, TEXT("0"), TEXT("Total twist in degrees, applied linearly from the first frame to the last.")),
        MakeParam(TEXT("scale_start"), EPwModelParamType::Number, TEXT("1"), TEXT("Cross-section scale at the first frame. Refused together with `scales`, which supersedes it.")),
        MakeParam(TEXT("scale_end"), EPwModelParamType::Number, TEXT("1"), TEXT("Cross-section scale at the last frame. Refused together with `scales`, which supersedes it.")),
        MakeParam(TEXT("scales"), EPwModelParamType::PointList2, TEXT(""),
            TEXT("Cross-section scale as a LAW along the path, [(alpha, scale), …], where alpha is the normalised position from the first frame (0) to the last (1). Piecewise-linear between knots and HELD outside the outermost pair, never extrapolated. This is what `scale_start` / `scale_end` cannot say: those interpolate LINEARLY, so a swept tube could taper only straight, and any radius law that is not a straight line - a power-law taper, an exponential flare, a bulge - had to be split into one sweep per span with the radii matched by hand at every join. At least 2 knots; alpha strictly ascending within [0, 1]; every scale positive, because 0 collapses the section onto the path and a negative one reflects it and reverses the tube's facing normals. It does not resample `path=`: alpha is evaluated at the frames you wrote.")),
        MakeParam(TEXT("cap"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Cap both ends of the sweep. sweep never loops: a path that returns to its start is still swept OPEN and still capped, unlike extrude_along_spline.")),
    });
    AcceptMaterialSlot(Sweep);
    Ops.Add(MoveTemp(Sweep));

    FPwModelOpSpec ExtrudeAlongSpline = MakeModifier(TEXT("extrude_along_spline"),
        TEXT("The same construction as sweep, and there is no fallback: `path=` is required because with no frames the engine call produces nothing at all. OPEN OR CLOSED IS READ OFF THE PATH, not fixed: a path of at least 4 frames whose last frame returns within 0.01 uu of the first is swept as a CLOSED loop - the repeated frame is dropped and the engine wraps the ring itself - and every other path is swept OPEN. `cap` is honoured on an open path and does nothing on a loop, which has no ends; on a returning path the op warns `cap has no effect on a path that returns to its start` rather than leaving it to be discovered. It used to loop unconditionally, which made `cap` unreachable at every path length and ran an open path back from its last frame to its first. The name is the geometry.* verb's; the op needs no spline and no actor. `material=` and the untagged inheritance rule are sweep's, unchanged."), {
        MakeRequired(TEXT("path"), EPwModelParamType::FrameList,
            TEXT("Path frames [(x, y, z, roll, pitch, yaw), …], part-local. At least 2.")),
        MakeParam(TEXT("profile"), EPwModelParamType::PointList2, TEXT(""),
            TEXT("Cross-section [(u, v), …]. At least 3 points. Omitted: the bounding-box circle.")),
        MakeParam(TEXT("segments"), EPwModelParamType::Integer, TEXT("16"),
            TEXT("Sides of the fallback circular cross-section, as clamp(segments/2, 4, 32). It does not resample `path=`.")),
        MakeParam(TEXT("twist"), EPwModelParamType::Number, TEXT("0"), TEXT("Total twist in degrees across the path.")),
        MakeParam(TEXT("scale_start"), EPwModelParamType::Number, TEXT("1"), TEXT("Cross-section scale at the first frame. Refused together with `scales`, which supersedes it.")),
        MakeParam(TEXT("scale_end"), EPwModelParamType::Number, TEXT("1"), TEXT("Cross-section scale at the last frame. Refused together with `scales`, which supersedes it.")),
        MakeParam(TEXT("scales"), EPwModelParamType::PointList2, TEXT(""),
            TEXT("Cross-section scale as a LAW along the path, [(alpha, scale), …], alpha normalised from the first frame (0) to the last (1). Piecewise-linear between knots, held outside them. Same rules and same reason as sweep's: at least 2 knots, alpha strictly ascending within [0, 1], every scale positive. Dropped WITH `cap`, `scale_start` and `scale_end` on a path that returns to its start - the engine's loop branch gates path scaling off entirely - and the op warns rather than leaving it to be found.")),
        MakeParam(TEXT("cap"), EPwModelParamType::Bool, TEXT("true"),
            TEXT("Cap both ends. Honoured on an OPEN path only: a path that returns to its start is swept as a loop, which has no ends, and setting it there warns instead of capping.")),
    });
    AcceptMaterialSlot(ExtrudeAlongSpline);
    Ops.Add(MoveTemp(ExtrudeAlongSpline));

    // ---- Topology and bulk ------------------------------------------------------

    // No `subdivisions`, and its removal is the point rather than an omission. The parameter was
    // published by both front-ends and consumed by neither: FBridgeParams has never carried a
    // field for it and the strip builder emits exactly one quad per loop vertex pair, so every
    // value an author wrote produced the identical mesh. It follows `recalculate_normals`'s
    // `split_angle` out of the vocabulary for the same reason - a parameter that is accepted and
    // silently discarded is worse than one that does not exist, because the author spends a
    // cycle believing it did something. bridge is hand-rolled over FDynamicMesh3::AppendTriangle
    // rather than an engine call, so there is no engine option to wire it to; giving it real
    // behaviour means intermediate interpolated rings, which is a feature and not a fix.
    Ops.Add(MakeModifier(TEXT("bridge"), TEXT("Bridge two boundary loops with a single triangle strip."), {
        MakeParam(TEXT("edge_group_a"), EPwModelParamType::Integer, TEXT("0"), TEXT("Index of the first boundary loop.")),
        MakeParam(TEXT("edge_group_b"), EPwModelParamType::Integer, TEXT("1"), TEXT("Index of the second boundary loop.")),
    }));

    Ops.Add(MakeModifier(TEXT("edge_split"), TEXT("Split edges by inserting midpoint vertices."), {
        MakeParam(TEXT("edges"), EPwModelParamType::NumberList, TEXT(""), TEXT("Edge IDs to split, as (a, b, c, …).")),
        MakeParam(TEXT("edge_index"), EPwModelParamType::Integer, TEXT(""), TEXT("A single edge ID, as an alternative to edges.")),
        MakeParam(TEXT("split_factor"), EPwModelParamType::Number, TEXT("0.5"), TEXT("Position along the edge, 0-1.")),
        MakeParam(TEXT("weld_vertices"), EPwModelParamType::Bool, TEXT("true"), TEXT("Weld afterwards.")),
        MakeParam(TEXT("weld_tolerance"), EPwModelParamType::Number, TEXT("0.0001"), TEXT("Weld distance tolerance.")),
    }));

    // The one op that can name a material slot two ways, and the only reason it is built into a
    // local instead of added inline. `material=` resolves through the model-wide slot table like
    // every shape primitive's; `material_id=` writes a raw index, which is what a generator that
    // already computed IDs hands over. Both on one op is PWMODEL_MATERIAL_ID_CONFLICT (ValidateOp).
    //
    // It gets the slot tag WITHOUT the local transform (`vertices=` are already explicit
    // positions) and WITHOUT `color=` (see AppendVertexColorParam), which is why it stays a bare
    // generator with one parameter added rather than becoming a MakeGenerator.
    FPwModelOpSpec AppendBuffers =
        MakeBareGenerator(TEXT("append_buffers"), TEXT("Append explicit vertex and triangle buffers."), {
            MakeRequired(TEXT("vertices"), EPwModelParamType::PointList3, TEXT("Vertex positions [(x, y, z), …].")),
            MakeRequired(TEXT("triangles"), EPwModelParamType::PointList3, TEXT("Triangle vertex indices [(a, b, c), …].")),
            MakeParam(TEXT("normals"), EPwModelParamType::PointList3, TEXT(""), TEXT("Per-vertex normals.")),
            MakeParam(TEXT("uvs"), EPwModelParamType::PointList2, TEXT(""), TEXT("Per-vertex UVs.")),
            MakeParam(TEXT("colors"), EPwModelParamType::PointList4, TEXT(""), TEXT("Per-vertex colors.")),
            MakeParam(TEXT("group_id"), EPwModelParamType::Integer, TEXT("0"), TEXT("Polygroup ID for the appended triangles.")),
            MakeParam(TEXT("material_id"), EPwModelParamType::Integer, TEXT("0"),
                TEXT("Raw material ID for the appended triangles, bypassing the slot table. Mutually exclusive with material=.")),
        });
    AcceptMaterialSlot(AppendBuffers);
    Ops.Add(MoveTemp(AppendBuffers));

    // ---- Collision elements -----------------------------------------------------
    //
    // at / rotate here are MESH space - the merged mesh, after every part transform -
    // not part-local, because UBodySetup is per-asset.

    Ops.Add(MakeCollisionElement(TEXT("box"), TEXT("A box collision element (FKBoxElem)."), {
        MakeRequired(TEXT("size"), EPwModelParamType::Vector3, TEXT("Full extents, not half-extents.")),
        MakeParam(TEXT("at"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("Centre, in mesh space.")),
        MakeParam(TEXT("rotate"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("Rotation in degrees, in mesh space.")),
    }));

    Ops.Add(MakeCollisionElement(TEXT("sphere"), TEXT("A sphere collision element (FKSphereElem)."), {
        MakeRequired(TEXT("radius"), EPwModelParamType::Number, TEXT("Sphere radius.")),
        MakeParam(TEXT("at"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("Centre, in mesh space.")),
    }));

    Ops.Add(MakeCollisionElement(TEXT("capsule"), TEXT("A capsule collision element (FKSphylElem)."), {
        MakeRequired(TEXT("radius"), EPwModelParamType::Number, TEXT("Capsule radius.")),
        MakeRequired(TEXT("height"), EPwModelParamType::Number,
            TEXT("TOTAL height including both caps. Converted to FKSphylElem::Length as max(0, height - 2*radius).")),
        MakeParam(TEXT("at"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("Centre, in mesh space.")),
        MakeParam(TEXT("rotate"), EPwModelParamType::Vector3, TEXT("(0, 0, 0)"), TEXT("Rotation in degrees, in mesh space.")),
    }));

    FPwModelOpSpec Hull = MakeCollisionElement(TEXT("hull"),
        TEXT("One convex hull built from the block's geometry. EVERY CONCAVE FEATURE IS DISCARDED - the name says what happens to the input, not what is stored."), {});
    Hull.bAcceptsBlock = true;
    Ops.Add(MoveTemp(Hull));

    Ops.Add(MakeCollisionElement(TEXT("convex"),
        TEXT("A convex hull from explicit vertices. Machine-generated only; not the authoring path - use hull { … }."), {
        MakeRequired(TEXT("points"), EPwModelParamType::PointList3, TEXT("Hull vertices [(x, y, z), …]; at least 4.")),
    }));

    Ops.Add(MakeCollisionElement(TEXT("auto"), TEXT("Generated simple collision. Exclusive with explicit elements."), {
        // Taken from the collision layer's enum-derived list rather than retyped: the parser
        // accepting a spelling the builder rejects (or the reverse) is exactly the drift
        // PwModelCollisionNames exists to prevent, and it had no caller until now.
        MakeEnum(TEXT("method"), TEXT("convex_hulls"), TEXT("Generation method."),
            PwModelCollisionNames::AutoMethodNames()),
        MakeParam(TEXT("max_hulls_per_component"), EPwModelParamType::Integer, TEXT("8"),
            TEXT("Maximum convex hulls PER CONNECTED COMPONENT of the merged mesh, not per asset, and a ceiling rather than a quota. The engine splits the mesh into its disconnected pieces first, then spends the budget only where a split reduces hull error, so a mesh in 11 pieces at 16 produces up to 176 elements but may produce far fewer - measured 37 on ships_wheel. The count is the decomposer's: not proportional to this number, not monotonic in it, and not reproducible run to run. Read collisionElements off the response instead of predicting it. The compile warns with the piece count whenever the total exceeds this number.")),
        MakeParam(TEXT("max_hulls"), EPwModelParamType::Integer, TEXT("8"),
            TEXT("Old spelling of max_hulls_per_component, still accepted and warned. It read as an asset-wide budget and never was one.")),
        MakeParam(TEXT("simplify_to"), EPwModelParamType::Integer, TEXT(""), TEXT("Simplify the source mesh to this triangle count first.")),
        MakeParam(TEXT("detect_boxes"), EPwModelParamType::Bool, TEXT("false"), TEXT("Emit boxes where the shape is boxy.")),
        MakeParam(TEXT("detect_spheres"), EPwModelParamType::Bool, TEXT("false"), TEXT("Emit spheres where the shape is spherical.")),
        MakeParam(TEXT("detect_capsules"), EPwModelParamType::Bool, TEXT("false"), TEXT("Emit capsules where the shape is capsular.")),
    }));

    // ---- Skin rules (model-level) ------------------------------------------

    Ops.Add(MakeOp(TEXT("smooth"), EPwModelOpContext::Skin,
        TEXT("Compute smooth base skin weights for every vertex from the referenced skeleton."), {
        MakeRanged(TEXT("max_influences"), EPwModelParamType::Integer, TEXT("4"), 1.0, 12.0,
            TEXT("Maximum number of bone influences per vertex. The engine's inline influence ceiling is 12.")),
        MakeRanged(TEXT("stiffness"), EPwModelParamType::Number, TEXT("0.2"), 0.0, 1.0,
            TEXT("Weight-solve stiffness, from fully flexible (0) to fully stiff (1).")),
        MakeEnum(TEXT("method"), TEXT("direct_distance"),
            TEXT("Bone-weight solve method."),
            { TEXT("direct_distance"), TEXT("geodesic_voxel") }),
        MakeRanged(TEXT("voxel_resolution"), EPwModelParamType::Integer, TEXT("128"), 8.0, 1024.0,
            TEXT("Voxel resolution used by the geodesic_voxel solve.")),
    }));

    return Ops;
}
}

const TCHAR* PwModelParamTypeToString(EPwModelParamType Type)
{
    switch (Type)
    {
    case EPwModelParamType::Number:     return TEXT("number");
    case EPwModelParamType::Integer:    return TEXT("integer");
    case EPwModelParamType::Bool:       return TEXT("boolean");
    case EPwModelParamType::String:     return TEXT("string");
    case EPwModelParamType::Enum:       return TEXT("enum");
    case EPwModelParamType::Vector2:    return TEXT("vector2");
    case EPwModelParamType::Vector3:    return TEXT("vector3");
    case EPwModelParamType::Vector4:    return TEXT("vector4");
    case EPwModelParamType::NumberList: return TEXT("number_list");
    case EPwModelParamType::PointList2: return TEXT("point_list2");
    case EPwModelParamType::PointList3: return TEXT("point_list3");
    case EPwModelParamType::PointList4: return TEXT("point_list4");
    case EPwModelParamType::FrameList:  return TEXT("frame_list");
    case EPwModelParamType::HarmonicList: return TEXT("harmonic_list");
    default:                            return TEXT("unknown");
    }
}

// The component names of one entry, for the arity diagnostic. Empty for every type whose ordering
// is either obvious or already spelled in the parameter's own description - which is why this
// leaves the existing messages byte-identical and only lengthens the one that needed it: a
// 6-tuple's halves carry different units, so "expects 6 components" alone is not actionable.
const TCHAR* PwModelParamTypeTupleShape(EPwModelParamType Type)
{
    switch (Type)
    {
    case EPwModelParamType::FrameList:    return TEXT(" (x, y, z, roll, pitch, yaw)");
    case EPwModelParamType::HarmonicList: return TEXT(" (order, amplitude, phase)");
    default:                              return TEXT("");
    }
}

const TCHAR* PwModelOpContextToString(EPwModelOpContext Context)
{
    switch (Context)
    {
    case EPwModelOpContext::Part:      return TEXT("part");
    case EPwModelOpContext::Collision: return TEXT("collision");
    case EPwModelOpContext::Skin:      return TEXT("skin");
    default:                           return TEXT("unknown");
    }
}

const TCHAR* PwModelAimExtentToString(EPwModelAimExtent Extent)
{
    switch (Extent)
    {
    case EPwModelAimExtent::Scalar:        return TEXT("scalar");
    case EPwModelAimExtent::SizeZ:         return TEXT("size_z");
    case EPwModelAimExtent::CapsuleLength: return TEXT("capsule_length");
    case EPwModelAimExtent::None:          return TEXT("none");
    default:                               return TEXT("unknown");
    }
}

const FPwModelParamSpec* FPwModelOpSpec::FindParam(const FString& ParamName) const
{
    for (const FPwModelParamSpec& Param : Params)
    {
        if (Param.Name == ParamName)
        {
            return &Param;
        }
    }
    return nullptr;
}

// The .pwmodel op that publishes each generator, so a clamped domain declared against an RPC
// method can be stamped onto the parameter this format spells it with.
//
// The mapping lives HERE rather than in GeometryClampDomains.h deliberately, and it is the same
// rule PwModelWarningNames exists to serve: the ops layer stays ignorant of its callers'
// spellings, so the layer that knows both names is the front end. This is that front end.
const TCHAR* ModelOpForRpcMethod(const FString& RpcMethod)
{
    static const TMap<FString, const TCHAR*> Map = {
        { TEXT("geometry.create_box"),           TEXT("box") },
        { TEXT("geometry.create_sphere"),        TEXT("sphere") },
        { TEXT("geometry.create_cylinder"),      TEXT("cylinder") },
        { TEXT("geometry.create_cone"),          TEXT("cone") },
        { TEXT("geometry.create_capsule"),       TEXT("capsule") },
        { TEXT("geometry.create_torus"),         TEXT("torus") },
        { TEXT("geometry.create_plane"),         TEXT("plane") },
        { TEXT("geometry.create_disc"),          TEXT("disc") },
        { TEXT("geometry.create_stairs"),        TEXT("stairs") },
        { TEXT("geometry.create_spiral_stairs"), TEXT("spiral_stairs") },
        { TEXT("geometry.create_ring"),          TEXT("ring") },
        { TEXT("geometry.create_arch"),          TEXT("arch") },
        { TEXT("geometry.create_pipe"),          TEXT("pipe") },
        { TEXT("geometry.revolve"),              TEXT("revolve") },
    };
    const TCHAR* const* Found = Map.Find(RpcMethod);
    return Found ? *Found : nullptr;
}

// The .pwmodel PARAMETER a clamp's warning label lands on. PwModelWarningNames maps the RPC label
// to the document's WARNING label, which for a vector-valued parameter carries a component suffix
// ("segments.x") the parameter itself does not have - so the suffix is dropped.
FString ModelParamForRpcLabel(const TCHAR* ModelOp, const TCHAR* RpcLabel)
{
    FString Name = RpcLabel;
    for (const PwModelWarningNames::FEntry& Entry : PwModelWarningNames::Entries())
    {
        if (FCString::Strcmp(Entry.OpName, ModelOp) == 0
            && FCString::Strcmp(Entry.RpcLabel, RpcLabel) == 0)
        {
            Name = Entry.ModelLabel;
            break;
        }
    }
    int32 Dot = INDEX_NONE;
    if (Name.FindChar(TEXT('.'), Dot))
    {
        Name = Name.Left(Dot);
    }
    return Name;
}

// Copies every declared clamped domain onto the parameter that publishes it, once, as the table
// is built. A post-pass rather than an argument to each MakeParam call: the domains are declared
// against RPC methods and there are 23 of them, so threading them through the registrations would
// put the same fact in two places and let them disagree - which is the staleness the ops-layer
// table was created to end, reproduced one layer up.
//
// A row whose op or parameter this format does not publish is skipped in silence here. It is not
// silent overall: PinWright.Geometry.ClampDomains.EveryClampedCountPublishesItsDomainUnenforced
// walks the same table and fails on exactly that case, and it is the right place for it, because
// a missing mapping is a test-visible gap rather than something to fail an editor launch over.
void StampClampDomains(TArray<FPwModelOpSpec>& Ops)
{
    for (const GeometryOps::ClampDomains::FClampDomain& Domain : GeometryOps::ClampDomains::Entries())
    {
        const TCHAR* ModelOp = ModelOpForRpcMethod(FString(Domain.RpcMethod));
        if (!ModelOp)
        {
            continue;
        }

        FPwModelOpSpec* Spec = Ops.FindByPredicate([ModelOp](const FPwModelOpSpec& Candidate)
        {
            return Candidate.Context == EPwModelOpContext::Part && Candidate.Name == ModelOp;
        });
        if (!Spec)
        {
            continue;
        }

        const FString ParamName = ModelParamForRpcLabel(ModelOp, Domain.Label);
        FPwModelParamSpec* Param = Spec->Params.FindByPredicate(
            [&ParamName](const FPwModelParamSpec& Candidate) { return Candidate.Name == ParamName; });
        if (!Param)
        {
            continue;
        }

        Param->bHasClampDomain = true;
        // The floor an author who writes no angle actually meets, so the headline number is the
        // one that applies to the line they are most likely to write.
        Param->ClampMin = Domain.FloorAtOpDefaults();
        Param->ClampMinClosedSweep = Domain.ClampMinClosedSweep;
        Param->ClampMax = Domain.ClampMax;
        Param->bClampZeroMeansUnset = Domain.bZeroMeansUnset;
        Param->ClampUnsetDefault = Domain.Default;
    }
}

const TArray<FPwModelOpSpec>& PwModelOpTable::Get()
{
    static const TArray<FPwModelOpSpec> Table = []()
    {
        TArray<FPwModelOpSpec> Built = BuildOpTable();
        StampClampDomains(Built);
        return Built;
    }();
    return Table;
}

const FPwModelOpSpec* PwModelOpTable::Find(const FString& OpName, EPwModelOpContext Context)
{
    for (const FPwModelOpSpec& Spec : Get())
    {
        if (Spec.Context == Context && Spec.Name == OpName)
        {
            return &Spec;
        }
    }
    return nullptr;
}

const FPwModelOpSpec* PwModelOpTable::FindInAnyContext(const FString& OpName)
{
    for (const FPwModelOpSpec& Spec : Get())
    {
        if (Spec.Name == OpName)
        {
            return &Spec;
        }
    }
    return nullptr;
}

TArray<FString> PwModelOpTable::NamesInContext(EPwModelOpContext Context)
{
    TArray<FString> Names;
    for (const FPwModelOpSpec& Spec : Get())
    {
        if (Spec.Context == Context)
        {
            Names.Add(Spec.Name);
        }
    }
    Names.Sort();
    return Names;
}

TArrayView<const FPwModelParamSpec> PwModelOpTable::LightmapParams()
{
    static const TArray<FPwModelParamSpec> Params = BuildLightmapParams();
    return Params;
}

TArrayView<const FPwModelParamSpec> PwModelOpTable::UVLayoutParams()
{
    static const TArray<FPwModelParamSpec> Params = BuildUVLayoutParams();
    return Params;
}

TArrayView<const FPwModelParamSpec> PwModelOpTable::PartHeaderParams()
{
    static const TArray<FPwModelParamSpec> Params = BuildPartHeaderParams();
    return Params;
}

// ============================================================================
// Warning label translation
// ============================================================================

namespace
{
// One row per divergence, ordered by op and then by the order that op's own clamps run, so a row
// lines up with GeometryOps_Primitives.cpp / GeometryOps_Advanced.cpp by reading down.
//
// Absent by design: every op whose two surfaces already spell the value the same way. `sphere
// subdivisions`, `cylinder`/`cone`/`capsule`/`disc`/`ring` `segments`, `revolve steps`, `sweep
// steps`, `spherify`/`cylindrify` `factor`, `subdivide iterations`, `revolve profile` all pass
// through untouched, and a row for them would be a second place to keep in sync for no gain.
const PwModelWarningNames::FEntry PwModelWarningNameTable[] =
{
    // `box` is the widest fork: ONE `.pwmodel` Vector3 becomes THREE RPC scalars, and it happens
    // twice - once for the extents, once for the subdivision counts. There is no single name that
    // is both `size` and the axis that moved, so the translation keeps the document's parameter
    // and appends the component: an author greps `size` and finds it, and still learns it was Y.
    { TEXT("box"),            TEXT("width"),              TEXT("size.x") },
    { TEXT("box"),            TEXT("height"),             TEXT("size.y") },
    { TEXT("box"),            TEXT("depth"),              TEXT("size.z") },
    { TEXT("box"),            TEXT("widthSegments"),      TEXT("segments.x") },
    { TEXT("box"),            TEXT("heightSegments"),     TEXT("segments.y") },
    { TEXT("box"),            TEXT("depthSegments"),      TEXT("segments.z") },

    { TEXT("cylinder"),       TEXT("heightSteps"),        TEXT("height_steps") },
    { TEXT("cone"),           TEXT("heightSteps"),        TEXT("height_steps") },
    { TEXT("capsule"),        TEXT("hemisphereSteps"),    TEXT("hemisphere_steps") },
    { TEXT("torus"),          TEXT("majorSegments"),      TEXT("major_segments") },
    { TEXT("torus"),          TEXT("minorSegments"),      TEXT("minor_segments") },

    // `plane` is `box`'s fork one dimension down: a Vector2 `subdivisions` against the RPC's
    // widthSubdivisions / depthSubdivisions. The RPC calls the second axis `depth`; the document
    // calls it Y, and the document's name is the one that has to survive here.
    { TEXT("plane"),          TEXT("widthSubdivisions"),  TEXT("subdivisions.x") },
    { TEXT("plane"),          TEXT("depthSubdivisions"),  TEXT("subdivisions.y") },

    { TEXT("stairs"),         TEXT("numSteps"),           TEXT("num_steps") },
    { TEXT("spiral_stairs"),  TEXT("numSteps"),           TEXT("num_steps") },
    { TEXT("arch"),           TEXT("majorSteps"),         TEXT("major_steps") },
    { TEXT("arch"),           TEXT("minorSteps"),         TEXT("minor_steps") },
    { TEXT("pipe"),           TEXT("radialSteps"),        TEXT("radial_steps") },
    { TEXT("pipe"),           TEXT("heightSteps"),        TEXT("height_steps") },

    // The first two rows that exist for a FAILURE rather than for a clamp, and the reason
    // TranslateMessage below exists at all. GeneratePipe refuses an unordered pair outright -
    // "pipe requires 0 < innerRadius < outerRadius; got innerRadius=%g, outerRadius=%g" - and
    // both names sit MID-SENTENCE, where the leading-token rule cannot reach them. That text
    // reached authors verbatim, naming two parameters `.pwmodel` does not publish.
    { TEXT("pipe"),           TEXT("innerRadius"),        TEXT("inner_radius") },
    { TEXT("pipe"),           TEXT("outerRadius"),        TEXT("outer_radius") },

    // Every mode of `uv` routes to one of GeometryOps' four UV builders, and all four refuse an
    // unusable channel with "uvChannel %d is out of range: …" - mid-sentence again. Unreachable
    // from a PARSED document, because `channel` is declared 0-7 and the parser rejects the rest
    // before the compiler runs; it still decides a hand-built AST, which is the same reason the
    // material_id tie-break in PwModelCompiler.cpp stays.
    { TEXT("uv"),             TEXT("uvChannel"),          TEXT("channel") },

    { TEXT("bridge"),         TEXT("edgeGroupA"),         TEXT("edge_group_a") },
};

// An identifier body character. The word-boundary test TranslateMessage runs on both sides of a
// candidate match, and what makes `width` decline to rewrite the `width` inside `widthSegments`
// while the longer row still matches it.
bool PwModelWarningNames_IsLabelChar(TCHAR Character)
{
    return FChar::IsAlnum(Character) || Character == TEXT('_');
}
}

TArrayView<const PwModelWarningNames::FEntry> PwModelWarningNames::Entries()
{
    return MakeArrayView(PwModelWarningNameTable);
}

FString PwModelWarningNames::Translate(const FString& OpName, const FString& Warning)
{
    for (const FEntry& Entry : PwModelWarningNameTable)
    {
        if (OpName != Entry.OpName)
        {
            continue;
        }

        // The label is always the FIRST token of a Clamp*Warn message ("%s clamped from …"), so
        // requiring the match at position 0 and requiring a space after it is exact: it cannot
        // fire inside a prose warning that happens to mention the same word, and it cannot
        // rewrite a longer name that merely starts with a mapped one.
        const int32 LabelLen = FCString::Strlen(Entry.RpcLabel);
        if (Warning.Len() > LabelLen
            && Warning[LabelLen] == TEXT(' ')
            && FCString::Strncmp(*Warning, Entry.RpcLabel, LabelLen) == 0)
        {
            return FString(Entry.ModelLabel) + Warning.RightChop(LabelLen);
        }
    }

    return Warning;
}

FString PwModelWarningNames::TranslateMessage(const FString& OpName, const FString& Message)
{
    // The leading-token rule first and unchanged, so a clamp label that reaches here through a
    // failure keeps exactly the rewrite Translate would have given it.
    const FString Leading = Translate(OpName, Message);

    FString Out;
    Out.Reserve(Leading.Len());

    // Scanned once over the INPUT, emitting into a separate buffer, rather than as a sequence of
    // ReplaceInline passes over a string that is already partly rewritten. A pass over its own
    // output can rewrite a ModelLabel it just wrote if some row's RpcLabel happens to spell it -
    // no row does today, and this is what means none ever has to.
    const TCHAR* const Chars = *Leading;
    const int32 Length = Leading.Len();

    for (int32 Position = 0; Position < Length; /* advanced below */)
    {
        const bool bAtWordStart =
            (Position == 0) || !PwModelWarningNames_IsLabelChar(Chars[Position - 1]);

        int32 Consumed = 0;
        if (bAtWordStart)
        {
            for (const FEntry& Entry : PwModelWarningNameTable)
            {
                if (OpName != Entry.OpName)
                {
                    continue;
                }

                const int32 LabelLen = FCString::Strlen(Entry.RpcLabel);
                if (LabelLen > Length - Position
                    || FCString::Strncmp(Chars + Position, Entry.RpcLabel, LabelLen) != 0)
                {
                    continue;
                }

                // Whole word on the trailing side too. Without it `width` would eat the head of
                // `widthSegments` and leave `size.xSegments`, and row order would decide which.
                if (Position + LabelLen < Length
                    && PwModelWarningNames_IsLabelChar(Chars[Position + LabelLen]))
                {
                    continue;
                }

                Out.Append(Entry.ModelLabel);
                Consumed = LabelLen;
                break;
            }
        }

        if (Consumed > 0)
        {
            Position += Consumed;
        }
        else
        {
            Out.AppendChar(Chars[Position]);
            ++Position;
        }
    }

    return Out;
}

// ============================================================================
// Parser
// ============================================================================

namespace
{
// Every version this build accepts. `pwmodel 0` carries no compatibility promise, so this
// is a single entry and there is deliberately no migration machinery behind it.
const TCHAR* PwModelAcceptedVersions = TEXT("0");

bool PwModelIsAcceptedVersion(int32 Version)
{
    return Version == 0;
}

EPwParamType PwModelToSourceParamType(EPwModelParamType Type)
{
    switch (Type)
    {
    case EPwModelParamType::Number:     return EPwParamType::Number;
    case EPwModelParamType::Integer:    return EPwParamType::Integer;
    case EPwModelParamType::Bool:       return EPwParamType::Bool;
    case EPwModelParamType::String:     return EPwParamType::String;
    case EPwModelParamType::Enum:       return EPwParamType::Enum;
    case EPwModelParamType::Vector2:    return EPwParamType::Vector2;
    case EPwModelParamType::Vector3:    return EPwParamType::Vector3;
    case EPwModelParamType::Vector4:    return EPwParamType::Vector4;
    case EPwModelParamType::NumberList: return EPwParamType::NumberList;
    case EPwModelParamType::PointList2: return EPwParamType::PointList2;
    case EPwModelParamType::PointList3: return EPwParamType::PointList3;
    case EPwModelParamType::PointList4: return EPwParamType::PointList4;
    case EPwModelParamType::FrameList:  return EPwParamType::FrameList;
    case EPwModelParamType::HarmonicList: return EPwParamType::HarmonicList;
    default:                            return EPwParamType::Number;
    }
}

struct FPwModelParserImpl : public FPwParseCursor
{
    FPwModelDocument& Doc;

    struct FStatementSink final : IPwStatementSink
    {
        FPwModelParserImpl& Owner;
        EPwModelOpContext Context;

        FStatementSink(FPwModelParserImpl& InOwner, EPwModelOpContext InContext)
            : Owner(InOwner)
            , Context(InContext)
        {
        }

        virtual void OnStatement(const FPwOp& Op, bool bIsFirstInList, bool bHadBlock) override
        {
            Owner.ValidateOp(Op, Context, bIsFirstInList, bHadBlock);
        }

        virtual IPwStatementSink& NestedSink() override
        {
            // A collision hull contains part geometry. Returning the collision sink here
            // would make `collision { hull { box ... } }` validate the inner box against
            // the collision vocabulary instead of the part vocabulary.
            return Context == EPwModelOpContext::Collision ? Owner.PartSink : *this;
        }
    };

    FStatementSink PartSink;
    FStatementSink CollisionSink;
    FStatementSink SkinSink;

    // Set while inside a part so every diagnostic raised there carries its name.
    FString CurrentPart;

    // Model-level blocks are at-most-one. Tracked separately from the document fields
    // because a duplicate is still parsed - to keep braces balanced and keep reporting
    // real errors inside it - and simply not stored.
    bool bSawMaterials = false;
    bool bSawCollision = false;
    bool bSawLightmap = false;
    bool bSawSkin = false;

    FPwModelParserImpl(const TArray<FPwToken>& InTokens, FPwModelDocument& InDoc,
                       TArray<FPwDiagnostic>& InDiags)
        : FPwParseCursor(InTokens, InDiags)
        , Doc(InDoc)
        , PartSink(*this, EPwModelOpContext::Part)
        , CollisionSink(*this, EPwModelOpContext::Collision)
        , SkinSink(*this, EPwModelOpContext::Skin)
    {
        ScopeLabel = TEXT("part");
    }

    // ---- diagnostics --------------------------------------------------------

    void SyncScope()
    {
        ScopeLabel = TEXT("part");
        ScopeName = CurrentPart;
    }

    void Emit(EPwSeverity Severity, const TCHAR* Code, const FPwToken& At, FString Message,
              TArray<FString> Suggestions = {})
    {
        SyncScope();
        FPwParseCursor::Emit(Severity, Code, At, MoveTemp(Message), MoveTemp(Suggestions));
    }

    void Error(const TCHAR* Code, const FPwToken& At, FString Message, TArray<FString> Suggestions = {})
    {
        SyncScope();
        FPwParseCursor::Error(Code, At, MoveTemp(Message), MoveTemp(Suggestions));
    }

    void Warn(const TCHAR* Code, const FPwToken& At, FString Message)
    {
        SyncScope();
        FPwParseCursor::Warn(Code, At, MoveTemp(Message));
    }

    void ErrorAtLine(const TCHAR* Code, int32 Line, int32 Column, FString Message)
    {
        SyncScope();
        FPwParseCursor::ErrorAtLine(Code, Line, Column, MoveTemp(Message));
    }

    void ValidateHarmonicOrdersFromSource(const FPwToken& At, const FString& Owner,
                                          const FPwParamSpec& Spec, const FPwValue& Value)
    {
        if (Spec.Type != EPwParamType::HarmonicList || Value.Type != EPwValueType::TupleList)
        {
            return;
        }

        for (int32 Index = 0; Index < Value.TupleList.Num(); ++Index)
        {
            const TArray<double>& Entry = Value.TupleList[Index];
            if (Entry.Num() == 0)
            {
                continue;
            }

            const double Order = Entry[0];
            if (Order == FMath::TruncToDouble(Order) && Order >= 1.0)
            {
                continue;
            }

            Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, At,
                FString::Printf(TEXT(
                    "Entry %d of '%s' on '%s' has order %g. The first component is cycles per "
                    "revolution: it must be a whole number and at least 1. A fractional order "
                    "does not close on itself after a full turn and tears the surface at the "
                    "wrap; order 0 is a constant offset rather than a harmonic."),
                    Index, *Spec.Name, *Owner, Order));
        }
    }

    void ValidateSourceParams(const FPwToken& MissingAt, const FString& Owner,
                              TArrayView<const FPwModelParamSpec> Specs,
                              const TMap<FString, FPwValue>& Params,
                              bool bBooleanOp = false, const TCHAR* UnknownHint = nullptr)
    {
        TArray<FPwParamSpec> SourceSpecs;
        SourceSpecs.Reserve(Specs.Num());
        for (const FPwModelParamSpec& ModelSpec : Specs)
        {
            FPwParamSpec& SourceSpec = SourceSpecs.AddDefaulted_GetRef();
            SourceSpec.Name = ModelSpec.Name;
            SourceSpec.Type = PwModelToSourceParamType(ModelSpec.Type);
            SourceSpec.bRequired = ModelSpec.bRequired;
            SourceSpec.Default = ModelSpec.Default;
            SourceSpec.Description = ModelSpec.Description;
            SourceSpec.AllowedValues = ModelSpec.AllowedValues;
            SourceSpec.bHasRange = ModelSpec.bHasRange;
            SourceSpec.MinValue = ModelSpec.MinValue;
            SourceSpec.MaxValue = ModelSpec.MaxValue;
        }

        FPwParamPolicy Policy;
        Policy.UnknownHint = UnknownHint;
        Policy.OnParam = [this, bBooleanOp](const FString& ParamOwner, const FString& Key,
                                             const FPwValue& Value)
        {
            // `material=` is accepted on a boolean and names the faces the operation CREATES.
            // `color=` is not, and the asymmetry is the point rather than an oversight: a boolean
            // creates FACES but no VERTICES - every vertex in the result comes from one of the two
            // operands - so a scalar colour here could only overwrite colours the generators
            // already wrote.
            if (bBooleanOp && Key == TEXT("color"))
            {
                FPwToken At;
                At.Line = Value.Line;
                At.Column = Value.Column;
                Error(PwModelDiagnosticCodes::PWMODEL_MATERIAL_ON_BOOLEAN, At,
                    FString::Printf(TEXT("'%s' cannot take 'color': a boolean creates faces but no vertices of its own - every vertex in the result comes from one of the two operands and already carries the colour its generator gave it - so this could only recolour geometry you already coloured. Write color= on the generators inside the block, or set_vertex_color after the op. 'material=' IS accepted here and names the faces the operation creates."),
                        *ParamOwner));
                return EPwParamAction::Handled;
            }
            return EPwParamAction::Continue;
        };
        Policy.OnValue = [this](const FString& ValueOwner, const FPwParamSpec& Spec,
                                const FPwValue& Value)
        {
            if (ValueOwner == TEXT("uv") && Spec.Name == TEXT("mode")
                && Value.Type == EPwValueType::Identifier && Value.Text == TEXT("spherical"))
            {
                FPwToken At;
                At.Line = Value.Line;
                At.Column = Value.Column;
                Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, At,
                    TEXT("UV mode 'spherical' does not exist: UE 5.8 ships no spherical UV projection. Use mode=cylindrical, or mode=xatlas for an automatic unwrap."));
                return EPwParamAction::Handled;
            }
            return EPwParamAction::Continue;
        };
        Policy.OnValueChecked = [this](const FString& ValueOwner, const FPwParamSpec& Spec,
                                       const FPwValue& Value)
        {
            FPwToken At;
            At.Line = Value.Line;
            At.Column = Value.Column;
            ValidateHarmonicOrdersFromSource(At, ValueOwner, Spec, Value);
        };

        FPwParseCursor::ValidateParams(MissingAt, Owner, SourceSpecs, Params, Policy);
    }

    // ---- aiming -------------------------------------------------------------
    //
    // Everything `from`/`to` can get wrong is visible WITHOUT A MESH, so it is refused here
    // rather than in the compiler: two endpoints, a twist reference and a duplicate extent are
    // all literal values on the line. That puts every one of these on model.validate, which is
    // the surface an author iterates on, and it keeps the compiler's aim step arithmetic with no
    // branches for malformed input.
    //
    // The one aim failure that is NOT here is PWMODEL_AIM_TOO_SHORT: it compares the distance
    // against the capsule's radius, whose default lives in GeometryOps' own params struct. The
    // parser would have to re-type that number to check it, and a re-typed default is a second
    // source of truth for exactly the kind of value that drifts.
    static bool AimVectorFrom(const FPwValue* Value, FVector& OutVector)
    {
        if (!Value || Value->Type != EPwValueType::Tuple || Value->Tuple.Num() < 3)
        {
            return false;
        }
        OutVector = FVector(Value->Tuple[0], Value->Tuple[1], Value->Tuple[2]);
        return true;
    }

    static FPwToken AimTokenAt(const FPwValue& Value)
    {
        FPwToken At;
        At.Line = Value.Line;
        At.Column = Value.Column;
        return At;
    }

    void ValidateAimParams(const FPwOp& Op, const FPwModelOpSpec& Spec, const FPwToken& OpAt)
    {
        const FPwValue* From = Op.Params.Find(TEXT("from"));
        const FPwValue* To = Op.Params.Find(TEXT("to"));
        const FPwValue* Up = Op.Params.Find(TEXT("up"));

        if (!From && !To && !Up)
        {
            return;
        }

        // An op that does not declare these has already been told the parameter is unknown by
        // ValidateSourceParams; adding an aim diagnostic on top would report one mistake twice.
        if (Spec.FindParam(FString(TEXT("from"))) == nullptr)
        {
            return;
        }

        if (!From || !To)
        {
            const FPwValue* Present = From ? From : (To ? To : Up);
            const TCHAR* PresentName = From ? TEXT("from") : (To ? TEXT("to") : TEXT("up"));
            const TCHAR* MissingName = From ? TEXT("to") : TEXT("from");
            Error(PwModelDiagnosticCodes::PWMODEL_AIM_INCOMPLETE, AimTokenAt(*Present),
                (From || To)
                    ? FString::Printf(
                        TEXT("'%s' carries '%s' but not '%s'. Aiming needs both endpoints - one point names a ")
                        TEXT("position, not a direction. Add '%s=(x, y, z)', or use 'at=' to place the op without ")
                        TEXT("aiming it."),
                        *Op.OpName, PresentName, MissingName, MissingName)
                    : FString::Printf(
                        TEXT("'%s' carries 'up' but neither 'from' nor 'to'. 'up' only pins the twist of an aim; ")
                        TEXT("with nothing aimed it does nothing. Add 'from=' and 'to=', or use 'rotate=' to set ")
                        TEXT("the rotation directly."),
                        *Op.OpName));
            return;
        }

        // Both endpoints are present from here on. A value of the wrong kind was already reported
        // by ValidateSourceParams, so anything below that cannot read a vector simply stops
        // rather than reporting a second, less specific problem about the same token.
        const FPwValue* AtParam = Op.Params.Find(TEXT("at"));
        const FPwValue* RotateParam = Op.Params.Find(TEXT("rotate"));
        if (AtParam || RotateParam)
        {
            const FPwValue& Anchor = AtParam ? *AtParam : *RotateParam;
            const FString Both = (AtParam && RotateParam)
                ? FString(TEXT("'at' and 'rotate'"))
                : FString(AtParam ? TEXT("'at'") : TEXT("'rotate'"));
            Error(PwModelDiagnosticCodes::PWMODEL_AIM_CONFLICT, AimTokenAt(Anchor),
                FString::Printf(
                    TEXT("'%s' carries 'from'/'to' as well as %s. Aiming COMPUTES both: 'at' becomes the midpoint ")
                    TEXT("of the two points and 'rotate' the rotation that carries local +Z from one to the other, ")
                    TEXT("so keeping %s would silently discard one of the two answers. Drop %s, or drop 'from'/'to' ")
                    TEXT("and place the op by hand."),
                    *Op.OpName, *Both, *Both, *Both));
        }

        FVector FromPoint;
        FVector ToPoint;
        if (!AimVectorFrom(From, FromPoint) || !AimVectorFrom(To, ToPoint))
        {
            return;
        }

        const FVector Direction = ToPoint - FromPoint;
        if (Direction.IsNearlyZero())
        {
            Error(PwModelDiagnosticCodes::PWMODEL_AIM_DEGENERATE, AimTokenAt(*To),
                FString::Printf(
                    TEXT("'%s' aims from (%g, %g, %g) to the same point. A zero-length aim names no direction and ")
                    TEXT("no length. Move one endpoint, or use 'at=' to place the op without aiming it."),
                    *Op.OpName, FromPoint.X, FromPoint.Y, FromPoint.Z));
            return;
        }

        if (Up)
        {
            FVector UpVector;
            if (AimVectorFrom(Up, UpVector))
            {
                const FVector Cross = FVector::CrossProduct(UpVector.GetSafeNormal(), Direction.GetSafeNormal());
                if (UpVector.IsNearlyZero() || Cross.SizeSquared() < 1e-6)
                {
                    Error(PwModelDiagnosticCodes::PWMODEL_AIM_UP_PARALLEL, AimTokenAt(*Up),
                        FString::Printf(
                            TEXT("'%s' has up=(%g, %g, %g), which lies along the aim direction (%g, %g, %g) or is ")
                            TEXT("zero. 'up' exists to pin the twist about that direction, and one parallel to it ")
                            TEXT("pins nothing. Pick any direction across the aim - for a vertical aim, (1, 0, 0) ")
                            TEXT("- or drop 'up' to take the default frame."),
                            *Op.OpName, UpVector.X, UpVector.Y, UpVector.Z,
                            Direction.X, Direction.Y, Direction.Z));
                }
            }
        }

        const double Distance = Direction.Size();

        if (Spec.AimExtent == EPwModelAimExtent::None)
        {
            Warn(PwModelDiagnosticCodes::PWMODEL_AIM_LENGTH_UNUSED, OpAt,
                FString::Printf(
                    TEXT("'%s' is aimed from/to, but it has no extent along its own local Z - so the %g uu between ")
                    TEXT("the two points sets only the midpoint and the direction, and the op keeps whatever size ")
                    TEXT("its own parameters give it. Size it explicitly, or use an op whose length the aim can ")
                    TEXT("set (cylinder, cone, capsule, pipe, box)."),
                    *Op.OpName, Distance));
        }
        else if (const FPwValue* Extent = Op.Params.Find(Spec.AimExtentParam))
        {
            // `size` is the one extent parameter whose other components are still the author's,
            // so only its Z is a second answer. Zero is the spelling that says "take it from the
            // aim" and is the value the message asks for; any other Z is refused.
            const bool bSizeZ = Spec.AimExtent == EPwModelAimExtent::SizeZ;
            const bool bHasZ = Extent->Type == EPwValueType::Tuple && Extent->Tuple.Num() >= 3;
            const bool bConflicts = !bSizeZ || (bHasZ && !FMath::IsNearlyZero(Extent->Tuple[2]));
            if (bConflicts)
            {
                Error(PwModelDiagnosticCodes::PWMODEL_AIM_EXTENT_CONFLICT, AimTokenAt(*Extent),
                    bSizeZ
                        ? FString::Printf(
                            TEXT("'%s' is aimed from/to, so the Z of '%s' is the %g uu between the two points - but ")
                            TEXT("this line also gives it as %g. Write the cross-section as %s=(x, y, 0) to take ")
                            TEXT("the length from the aim, or drop 'from'/'to'."),
                            *Op.OpName, *Spec.AimExtentParam, Distance,
                            bHasZ ? Extent->Tuple[2] : 0.0, *Spec.AimExtentParam)
                        : FString::Printf(
                            TEXT("'%s' is aimed from/to, which already sets '%s' from the %g uu between the two ")
                            TEXT("points, so writing '%s' as well is a second answer for the same length. Drop ")
                            TEXT("'%s', or drop 'from'/'to' and place the op by hand."),
                            *Op.OpName, *Spec.AimExtentParam, Distance,
                            *Spec.AimExtentParam, *Spec.AimExtentParam));
            }
        }

        if (Spec.AimExtent != EPwModelAimExtent::None)
        {
            FVector ScaleVector;
            if (AimVectorFrom(Op.Params.Find(TEXT("scale")), ScaleVector)
                && !FMath::IsNearlyEqual(ScaleVector.Z, 1.0))
            {
                Warn(PwModelDiagnosticCodes::PWMODEL_AIM_AXIAL_SCALE, OpAt,
                    FString::Printf(
                        TEXT("'%s' is aimed across %g uu and also carries scale=(%g, %g, %g). Scale bakes into the ")
                        TEXT("vertices after the aim sets the length, so this op spans %g uu and stops short of - ")
                        TEXT("or past - 'to'. Use scale's X and Y to shape the cross-section and leave Z at 1 if ")
                        TEXT("the geometry is meant to reach the second point."),
                        *Op.OpName, Distance, ScaleVector.X, ScaleVector.Y, ScaleVector.Z,
                        Distance * ScaleVector.Z));
            }
        }
    }

    // ---- op validation ------------------------------------------------------

    void ValidateOp(const FPwOp& Op, EPwModelOpContext Context, bool bIsFirstInList, bool bHadBlock)
    {
        FPwToken At;
        At.Line = Op.Line;
        At.Column = Op.Column;

        const FPwModelOpSpec* Spec = PwModelOpTable::Find(Op.OpName, Context);
        if (!Spec)
        {
            if (const FPwModelOpSpec* Elsewhere = PwModelOpTable::FindInAnyContext(Op.OpName))
            {
                Error(PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP, At,
                    FString::Printf(TEXT("Op '%s' is not valid in a %s block; it is a %s op."),
                        *Op.OpName, PwModelOpContextToString(Context), PwModelOpContextToString(Elsewhere->Context)));
                return;
            }

            const TArray<FString> Names = PwModelOpTable::NamesInContext(Context);
            TArray<FString> Suggestions;
            const FString Guess = PwSuggest::Closest(Op.OpName, Names);
            if (!Guess.IsEmpty())
            {
                Suggestions.Add(Guess);
            }
            Error(PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP, At,
                FString::Printf(TEXT("Unknown op '%s' in a %s block. Call model.describe_ops for the full vocabulary."),
                    *Op.OpName, PwModelOpContextToString(Context)),
                MoveTemp(Suggestions));
            return;
        }

        if (bIsFirstInList && Context == EPwModelOpContext::Part && !Spec->bGenerator)
        {
            Error(PwModelDiagnosticCodes::PWMODEL_PART_NEEDS_PRIMITIVE, At,
                FString::Printf(TEXT("'%s' modifies existing geometry, so it cannot be the first op - there is nothing to modify yet. Start with a generator such as box, sphere or cylinder."),
                    *Op.OpName));
        }

        if (bHadBlock && !Spec->bAcceptsBlock)
        {
            Error(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, At,
                FString::Printf(TEXT("Op '%s' does not take a { … } block."), *Op.OpName));
        }
        else if (!bHadBlock && Spec->bAcceptsBlock)
        {
            Error(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, At,
                FString::Printf(TEXT("Op '%s' requires a { … } block holding the geometry it operates on."), *Op.OpName));
        }

        ValidateSourceParams(At, Op.OpName, Spec->Params, Op.Params, Spec->bBoolean);

        ValidateAimParams(Op, *Spec, At);

        // Both spellings of "which material do these triangles use" on one op. Only one can be
        // honoured: `material=` allocates a slot from the model-wide table and rewrites every
        // material ID the op produced, while `material_id=` writes a raw index and must not be
        // rewritten. The compiler's tie-break lets material_id win in SILENCE
        // (PwModelCompiler.cpp RunGenerator), so without this the named slot vanishes and the
        // author is told the binding was dropped instead of that it was overridden.
        //
        // Keyed on the two parameter NAMES rather than on `append_buffers`, so a second op that
        // ever gains material_id inherits the check instead of needing to be remembered here.
        // Every other op has one of the pair at most, so the test cannot fire spuriously.
        const FPwValue* SlotTag = Op.Params.Find(TEXT("material"));
        const FPwValue* RawMaterialId = Op.Params.Find(TEXT("material_id"));
        if (SlotTag && RawMaterialId)
        {
            // Anchored on material_id, which is the one to delete: the named slot is what the
            // format wants an author reaching for, and it is the half the compiler discards.
            FPwToken ConflictAt;
            ConflictAt.Line = RawMaterialId->Line;
            ConflictAt.Column = RawMaterialId->Column;

            // Both values are printed from what the AST holds rather than from a re-parse: a
            // value of the wrong kind was already reported by ValidateParams above, and this
            // message must still name something rather than crash or print a stale default.
            const FString SlotText = (SlotTag->Type == EPwValueType::String)
                ? SlotTag->Text : FString(TEXT("(not a string)"));
            const FString IdText = (RawMaterialId->Type == EPwValueType::Number)
                ? FString::FromInt(static_cast<int32>(FMath::RoundToDouble(RawMaterialId->Number)))
                : FString(TEXT("(not a number)"));

            Error(PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_CONFLICT, ConflictAt,
                FString::Printf(
                    TEXT("'%s' carries both material=\"%s\" and material_id=%s, which name the same triangles' material two different ways. ")
                    TEXT("Keep material=\"%s\" to resolve the slot through the model-wide table, or keep material_id=%s to write the raw index - not both."),
                    *Op.OpName, *SlotText, *IdText, *SlotText, *IdText));
        }

        // `magnitude_mode=relative` with `apply_along_normal=false`. The op refuses the pair, so
        // without this the author gets PWMODEL_OP_FAILED carrying INVALID_ARGUMENT and no line of
        // their own - and this is visible without a mesh, which is where the parser's contract
        // says it belongs. Keyed on the two parameter names rather than on `noise_deform`, so a
        // second op that ever gains the pair inherits the check.
        const FPwValue* MagnitudeMode = Op.Params.Find(TEXT("magnitude_mode"));
        const FPwValue* AlongNormal = Op.Params.Find(TEXT("apply_along_normal"));
        if (MagnitudeMode && AlongNormal
            && MagnitudeMode->Type == EPwValueType::Identifier
            && MagnitudeMode->Text == TEXT("relative")
            && AlongNormal->Type == EPwValueType::Identifier
            && AlongNormal->Text == TEXT("false"))
        {
            FPwToken ModeAt;
            ModeAt.Line = MagnitudeMode->Line;
            ModeAt.Column = MagnitudeMode->Column;
            Error(PwModelDiagnosticCodes::PWMODEL_NOISE_MODE_CONFLICT, ModeAt,
                FString::Printf(
                    TEXT("'%s' asks for magnitude_mode=relative together with apply_along_normal=false, which name ")
                    TEXT("two rulers that cannot both apply. Relative scales the displacement by each vertex's own ")
                    TEXT("mean one-ring edge length - one SCALAR per vertex - while apply_along_normal=false ")
                    TEXT("displaces by the noise VECTOR, three decorrelated fields with no single length to scale. ")
                    TEXT("Drop magnitude_mode to displace along the vector in world units, or drop ")
                    TEXT("apply_along_normal to scale along the normal."),
                    *Op.OpName));
        }

        // A hull whose body is empty produces no vertices, which the compiler would only
        // discover after building a mesh. The empty-block case is visible here.
        if (Context == EPwModelOpContext::Collision && Op.OpName == TEXT("hull") && bHadBlock && Op.Children.Num() == 0)
        {
            Error(PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL, At,
                TEXT("A 'hull' block with no ops produces no geometry to hull."));
        }

        if (Context == EPwModelOpContext::Collision && Op.OpName == TEXT("convex"))
        {
            const FPwValue* Points = Op.Params.Find(TEXT("points"));
            if (Points && Points->Type == EPwValueType::TupleList && Points->TupleList.Num() < 4)
            {
                Error(PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL, At,
                    FString::Printf(TEXT("'convex' needs at least 4 points to bound a volume, but %d were given."),
                        Points->TupleList.Num()));
            }
        }
    }

    void ParsePart()
    {
        const FPwToken Keyword = Advance(); // 'part'

        FPwModelPart Part;
        Part.Line = Keyword.Line;
        Part.Column = Keyword.Column;

        if (Check(EPwTokenType::Identifier) && !Check(EPwTokenType::Equals, 1))
        {
            Part.Name = Advance().Text;
        }
        else
        {
            UnexpectedToken(TEXT("a part name"));
        }

        CurrentPart = Part.Name;
        SyncScope();

        const int32 HeaderStart = Pos;
        FPwToken BraceToken;

        if (ParseParams(Part.Transform, FString::Printf(TEXT("part %s"), *Part.Name)))
        {
            // The part header is the one place the grammar is not `keyword params` on a line:
            // the brace closes the header rather than opening a line of its own.
            if (!Check(EPwTokenType::OpenBrace))
            {
                UnexpectedToken(TEXT("'{' to open the part body"));
                SkipToNextLine();
                CurrentPart.Reset();
                SyncScope();
                return;
            }
            BraceToken = Advance();
        }
        else
        {
            // ParseParams recovered by consuming the rest of the line - and the part's '{'
            // went with it, because the brace closes the header instead of standing on its
            // own line. Rewind over the skipped span and resynchronise on that brace.
            //
            // Without this, `part body at= {` reported the real error, then a spurious
            // "Expected '{' to open the part body", then every op line and the closing '}'
            // again at model level: N + 2 diagnostics for one malformed header parameter,
            // with the author's actual mistake buried at the top of the pile.
            int32 Scan = HeaderStart;
            while (Scan < Pos && Tokens[Scan].Type != EPwTokenType::OpenBrace)
            {
                ++Scan;
            }
            if (Scan >= Pos)
            {
                CurrentPart.Reset();
                SyncScope();
                return;
            }
            BraceToken = Tokens[Scan];
        }

        FPwParseCursor::ParseOpList(Part.Ops, BraceToken, PartSink);

        ValidatePartTransform(Part);

        // `bone=` is a binding, not a transform. Validate it through the same published
        // part-header table, then move the decoded String value out so the compiler's
        // transform reader can never accidentally treat it as geometry state.
        if (const FPwValue* Bone = Part.Transform.Find(TEXT("bone")))
        {
            if (Bone->Type == EPwValueType::String)
            {
                Part.BoneBinding = Bone->Text;
            }
            Part.Transform.Remove(TEXT("bone"));
        }
        if (const FPwValue* AllowFloating = Part.Transform.Find(TEXT("allow_floating")))
        {
            Part.bAllowFloating = AllowFloating->Type == EPwValueType::Identifier
                && AllowFloating->Text.Equals(TEXT("true"), ESearchCase::IgnoreCase);
            Part.Transform.Remove(TEXT("allow_floating"));
        }

        for (const FPwModelPart& Existing : Doc.Parts)
        {
            if (Existing.Name == Part.Name && !Part.Name.IsEmpty())
            {
                FPwToken At;
                At.Line = Part.Line;
                At.Column = Part.Column;
                Error(PwModelDiagnosticCodes::PWMODEL_DUPLICATE_PART, At,
                    FString::Printf(TEXT("A part named '%s' is already declared on line %d. Part names identify a region and must be unique."),
                        *Part.Name, Existing.Line));
                break;
            }
        }

        if (Part.Ops.Num() == 0)
        {
            FPwToken At;
            At.Line = Part.Line;
            At.Column = Part.Column;
            Warn(PwModelDiagnosticCodes::PWMODEL_EMPTY_PART, At,
                FString::Printf(TEXT("Part '%s' has no ops and contributes no geometry."), *Part.Name));
        }

        Doc.Parts.Add(MoveTemp(Part));
        CurrentPart.Reset();
        SyncScope();
    }

    void ValidatePartTransform(const FPwModelPart& Part)
    {
        FPwToken At;
        At.Line = Part.Line;
        At.Column = Part.Column;

        ValidateSourceParams(At, FString::Printf(TEXT("part %s"), *Part.Name),
            PwModelOpTable::PartHeaderParams(), Part.Transform, /*bBooleanOp=*/false,
            TEXT("Geometry parameters belong on ops inside the part."));
    }

    void ParseMaterials()
    {
        CurrentPart.Reset();
        SyncScope();
        const FPwToken Keyword = Advance(); // 'materials'

        const bool bDuplicate = bSawMaterials;
        if (bDuplicate)
        {
            Error(PwModelDiagnosticCodes::PWMODEL_DUPLICATE_MATERIALS, Keyword,
                TEXT("A model has at most one 'materials' block; merge the slot bindings into the first one."));
        }
        bSawMaterials = true;

        if (!Check(EPwTokenType::OpenBrace))
        {
            UnexpectedToken(TEXT("'{' to open the materials block"));
            SkipToNextLine();
            return;
        }

        const FPwToken BraceToken = Advance();
        TArray<FPwModelMaterialBinding> Bindings;

        FPwParseCursor::ForEachBlockEntry(BraceToken, [this, &Bindings]()
        {
            if (!Check(EPwTokenType::Identifier) || !Check(EPwTokenType::Equals, 1))
            {
                UnexpectedToken(TEXT("a slot binding of the form Slot = \"/Game/…\""));
                SkipToNextLine();
                return;
            }

            const FPwToken SlotToken = Advance();
            Advance(); // '='

            if (!Check(EPwTokenType::String))
            {
                UnexpectedToken(TEXT("a quoted material asset path"));
                SkipToNextLine();
                return;
            }

            const FPwToken PathToken = Advance();

            for (const FPwModelMaterialBinding& Existing : Bindings)
            {
                if (Existing.Slot == SlotToken.Text)
                {
                    Error(PwModelDiagnosticCodes::PWMODEL_DUPLICATE_SLOT, SlotToken,
                        FString::Printf(TEXT("Slot '%s' is already bound on line %d."), *SlotToken.Text, Existing.Line));
                    return;
                }
            }

            FPwModelMaterialBinding Binding;
            Binding.Slot = SlotToken.Text;
            Binding.AssetPath = PathToken.Text;
            Binding.Line = SlotToken.Line;
            Binding.Column = SlotToken.Column;
            Bindings.Add(MoveTemp(Binding));
        });

        if (!bDuplicate)
        {
            Doc.Materials = MoveTemp(Bindings);
        }
    }

    void ParseCollision()
    {
        CurrentPart.Reset();
        SyncScope();
        const FPwToken Keyword = Advance(); // 'collision'

        const bool bDuplicate = bSawCollision;
        if (bDuplicate)
        {
            Error(PwModelDiagnosticCodes::PWMODEL_DUPLICATE_COLLISION, Keyword,
                TEXT("A model has at most one 'collision' block; UBodySetup is per-asset."));
        }
        bSawCollision = true;

        if (!Check(EPwTokenType::OpenBrace))
        {
            UnexpectedToken(TEXT("'{' to open the collision block"));
            SkipToNextLine();
            return;
        }

        const FPwToken BraceToken = Advance();

        FPwModelCollision Collision;
        Collision.Line = Keyword.Line;
        Collision.Column = Keyword.Column;

        FPwParseCursor::ForEachBlockEntry(BraceToken, [this, &Collision]()
        {
            if (!Check(EPwTokenType::Identifier))
            {
                UnexpectedToken(TEXT("a collision entry"));
                SkipToNextLine();
                return;
            }

            // `complexity = <flag>` is an assignment rather than an op, so it is the one
            // entry recognised by its '=' instead of by the op table.
            if (Check(EPwTokenType::Equals, 1))
            {
                const FPwToken KeyToken = Advance();
                Advance(); // '='

                if (KeyToken.Text != TEXT("complexity"))
                {
                    // Not a token error: every token here is legal and in a legal position.
                    // The name is the thing that is wrong, which is what UNKNOWN_PARAM says.
                    Error(PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM, KeyToken,
                        FString::Printf(TEXT("'%s = …' is not a collision entry. The only assignment here is 'complexity = <flag>'."),
                            *KeyToken.Text));
                    SkipToNextLine();
                    return;
                }

                FPwValue Value;
                if (!ParseValue(Value))
                {
                    return;
                }

                if (Collision.Complexity.Type != EPwValueType::None)
                {
                    Error(PwSourceDiagnosticCodes::PWSRC_DUPLICATE_PARAM, KeyToken,
                        TEXT("'complexity' is set more than once in this collision block."));
                    return;
                }

                // The accepted spellings come from the collision layer's enum-derived list,
                // not from a literal here: a flag added to ECollisionTraceFlag must not be
                // parseable by one half of the pipeline and unknown to the other.
                FPwModelParamSpec Spec;
                Spec.Name = TEXT("complexity");
                Spec.Type = EPwModelParamType::Enum;
                Spec.AllowedValues = PwModelCollisionNames::ComplexityNames();
                TArray<FPwModelParamSpec> Specs;
                Specs.Add(Spec);
                TMap<FString, FPwValue> Params;
                Params.Add(Spec.Name, Value);
                ValidateSourceParams(KeyToken, TEXT("collision"), Specs, Params);
                Collision.Complexity = MoveTemp(Value);
                return;
            }

            FPwOp Element;
            if (FPwParseCursor::ParseOpStatement(Element, /*bIsFirstInList=*/false, CollisionSink))
            {
                Collision.Elements.Add(MoveTemp(Element));
            }
        });

        ValidateCollisionExclusivity(Collision);

        if (!bDuplicate)
        {
            Doc.Collision = MoveTemp(Collision);
        }
    }

    // `auto` generates the whole simple-collision set, so it cannot coexist with hand-authored
    // elements or with a second `auto` - each would silently win over the other.
    void ValidateCollisionExclusivity(const FPwModelCollision& Collision)
    {
        int32 AutoCount = 0;
        int32 ExplicitCount = 0;
        const FPwOp* FirstAuto = nullptr;

        for (const FPwOp& Element : Collision.Elements)
        {
            if (Element.OpName == TEXT("auto"))
            {
                ++AutoCount;
                if (!FirstAuto)
                {
                    FirstAuto = &Element;
                }
            }
            else if (PwModelOpTable::Find(Element.OpName, EPwModelOpContext::Collision))
            {
                ++ExplicitCount;
            }
        }

        if (!FirstAuto)
        {
            return;
        }

        FPwToken At;
        At.Line = FirstAuto->Line;
        At.Column = FirstAuto->Column;

        if (AutoCount > 1)
        {
            Error(PwModelDiagnosticCodes::PWMODEL_COLLISION_CONFLICT, At,
                FString::Printf(TEXT("A collision block carries at most one 'auto' rule, but %d were given."), AutoCount));
        }
        else if (ExplicitCount > 0)
        {
            Error(PwModelDiagnosticCodes::PWMODEL_COLLISION_CONFLICT, At,
                FString::Printf(TEXT("A collision block carries either explicit elements or one 'auto' rule, not both; %d explicit element(s) are also declared."),
                    ExplicitCount));
        }
    }

    void ParseLightmap()
    {
        CurrentPart.Reset();
        SyncScope();
        const FPwToken Keyword = Advance(); // 'lightmap'

        const bool bDuplicate = bSawLightmap;
        if (bDuplicate)
        {
            Error(PwModelDiagnosticCodes::PWMODEL_DUPLICATE_LIGHTMAP, Keyword,
                TEXT("A model has at most one 'lightmap' statement; UStaticMesh has one LightMapCoordinateIndex."));
        }
        bSawLightmap = true;

        FPwOp Op;
        Op.OpName = TEXT("lightmap");
        Op.Line = Keyword.Line;
        Op.Column = Keyword.Column;

        if (!ParseParams(Op.Params, TEXT("lightmap")))
        {
            return;
        }

        ValidateSourceParams(Keyword, TEXT("lightmap"), PwModelOpTable::LightmapParams(), Op.Params);

        if (!bDuplicate)
        {
            Doc.Lightmap = MoveTemp(Op);
        }
    }

    void ParseUVLayout()
    {
        CurrentPart.Reset();
        SyncScope();
        const FPwToken Keyword = Advance(); // 'uv_layout'

        FPwOp Op;
        Op.OpName = TEXT("uv_layout");
        Op.Line = Keyword.Line;
        Op.Column = Keyword.Column;

        if (!ParseParams(Op.Params, TEXT("uv_layout")))
        {
            return;
        }

        ValidateSourceParams(Keyword, TEXT("uv_layout"), PwModelOpTable::UVLayoutParams(), Op.Params);
        Doc.UVLayouts.Add(MoveTemp(Op));
    }

    void ParseUse()
    {
        CurrentPart.Reset();
        SyncScope();
        static const TArray<FString> ValidKinds = {
            TEXT("skeleton") };

        FPwUse Use;
        const bool bParsed = FPwParseCursor::ParseUse(ValidKinds, Use);
        if (bParsed && ValidKinds.Contains(Use.Kind))
        {
            Doc.Uses.Add(MoveTemp(Use));
        }
    }

    void ParseSkin()
    {
        CurrentPart.Reset();
        SyncScope();
        const FPwToken Keyword = Advance(); // 'skin'

        const bool bDuplicate = bSawSkin;
        if (bDuplicate)
        {
            Error(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, Keyword,
                TEXT("A model has at most one 'skin' block; put the smooth rule in the first one."));
        }
        bSawSkin = true;

        FPwOp Skin;
        Skin.OpName = TEXT("skin");
        Skin.Line = Keyword.Line;
        Skin.Column = Keyword.Column;

        if (!Check(EPwTokenType::OpenBrace))
        {
            UnexpectedToken(TEXT("'{' to open the skin block"));
            SkipToNextLine();
            return;
        }

        const FPwToken BraceToken = Advance();
        FPwParseCursor::ParseOpList(Skin.Children, BraceToken, SkinSink);

        int32 SmoothCount = 0;
        const FPwOp* FirstSmooth = nullptr;
        for (const FPwOp& Rule : Skin.Children)
        {
            if (Rule.OpName == TEXT("smooth"))
            {
                ++SmoothCount;
                if (!FirstSmooth)
                {
                    FirstSmooth = &Rule;
                }
            }
        }

        if (SmoothCount == 0)
        {
            Error(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, Keyword,
                TEXT("A 'skin' block requires one 'smooth' rule."));
        }
        else if (SmoothCount > 1 && FirstSmooth)
        {
            FPwToken At;
            At.Line = FirstSmooth->Line;
            At.Column = FirstSmooth->Column;
            Error(PwSourceDiagnosticCodes::PWSRC_DUPLICATE_PARAM, At,
                TEXT("A 'skin' block accepts at most one 'smooth' rule."));
        }

        if (!bDuplicate)
        {
            Doc.Skin = MoveTemp(Skin);
        }
    }

    void ParseReservedBlock()
    {
        CurrentPart.Reset();
        SyncScope();
        const FPwToken Keyword = Advance(); // skeleton | animation

        FPwModelReservedBlock Block;
        Block.Keyword = Keyword.Text;
        Block.Line = Keyword.Line;
        Block.Column = Keyword.Column;

        if (Keyword.Text == TEXT("animation"))
        {
            if (Check(EPwTokenType::Identifier))
            {
                Block.Name = Advance().Text;
            }
            else
            {
                UnexpectedToken(TEXT("an animation name"));
            }
        }

        if (!Check(EPwTokenType::OpenBrace))
        {
            UnexpectedToken(FString::Printf(TEXT("'{' to open the %s block"), *Keyword.Text));
            SkipToNextLine();
            return;
        }

        // Brace-checked but uninterpreted: the body is discarded, and the compiler rejects
        // the reserved construct with PWMODEL_CONSTRUCT_IN_WRONG_FORMAT. Reserving the keyword
        // exists to produce that diagnostic instead of PWSRC_UNKNOWN_OP.
        const FPwToken BraceToken = Advance();
        FPwParseCursor::SkipBalancedBlock(BraceToken);

        Doc.ReservedBlocks.Add(MoveTemp(Block));
    }

    void ParseDocument()
    {
        CurrentPart.Reset();
        SyncScope();
        FPwParseCursor::ParseVersionHeader(TEXT("pwmodel"), PwModelAcceptedVersions,
            PwModelIsAcceptedVersion, Doc.Header);

        // The model level is not a `{ … }` block - it ends at end of file, not at a brace -
        // so it cannot use ForEachBlockEntry. It still needs the same forward-progress
        // guarantee, spelled once here around a dispatch that has several exits.
        while (true)
        {
            SkipNewlines();
            if (AtEnd())
            {
                break;
            }

            const int32 Before = Pos;
            ParseModelLevelConstruct();
            if (Pos == Before)
            {
                Advance();
            }
        }

        ValidateDocument();
    }

    void ParseModelLevelConstruct()
    {
        CurrentPart.Reset();
        SyncScope();
        static const TArray<FString> ModelKeywords = {
            TEXT("part"), TEXT("materials"), TEXT("collision"), TEXT("uv_layout"), TEXT("lightmap"),
            TEXT("use"), TEXT("skeleton"), TEXT("skin"), TEXT("animation") };
        const FString ModelKeywordVocabulary = FString::Join(ModelKeywords, TEXT(", "));

        if (!Check(EPwTokenType::Identifier))
        {
            UnexpectedToken(FString::Printf(TEXT("a model-level construct: %s"), *ModelKeywordVocabulary));
            FPwParseCursor::SkipUnknownConstruct();
            return;
        }

        const FString Keyword = Peek().Text;

        if (Keyword == TEXT("part"))
        {
            ParsePart();
        }
        else if (Keyword == TEXT("materials"))
        {
            ParseMaterials();
        }
        else if (Keyword == TEXT("collision"))
        {
            ParseCollision();
        }
        else if (Keyword == TEXT("lightmap"))
        {
            ParseLightmap();
        }
        else if (Keyword == TEXT("uv_layout"))
        {
            ParseUVLayout();
        }
        else if (Keyword == TEXT("use"))
        {
            ParseUse();
        }
        else if (Keyword == TEXT("skin"))
        {
            ParseSkin();
        }
        else if (Keyword == TEXT("skeleton") || Keyword == TEXT("animation"))
        {
            ParseReservedBlock();
        }
        else
        {
            TArray<FString> Suggestions;
            const FString Guess = PwSuggest::Closest(Keyword, ModelKeywords);
            if (!Guess.IsEmpty())
            {
                Suggestions.Add(Guess);
            }
            Error(PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN, Peek(),
                FString::Printf(TEXT("'%s' is not a model-level construct. Ops belong inside a part; valid here: %s."),
                    *Keyword, *ModelKeywordVocabulary),
                MoveTemp(Suggestions));
            FPwParseCursor::SkipUnknownConstruct();
        }
    }

    void ValidateDocument()
    {
        if (Doc.Parts.Num() == 0)
        {
            FPwToken At;
            At.Line = Doc.Header.VersionLine > 0 ? Doc.Header.VersionLine : 1;
            At.Column = 1;
            Error(PwModelDiagnosticCodes::PWMODEL_NO_PARTS, At,
                TEXT("A model declares at least one 'part'; there is nothing to compile."));
        }

        TSet<int32> UVLayoutChannels;
        for (const FPwOp& UVLayout : Doc.UVLayouts)
        {
            const FPwValue* Channel = UVLayout.Params.Find(TEXT("channel"));
            if (Channel == nullptr || Channel->Type != EPwValueType::Number)
            {
                continue;
            }

            const int32 ChannelIndex = FMath::RoundToInt(Channel->Number);
            if (UVLayoutChannels.Contains(ChannelIndex))
            {
                FPwToken At;
                At.Line = UVLayout.Line;
                At.Column = UVLayout.Column;
                Error(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, At,
                    FString::Printf(TEXT("A model has at most one 'uv_layout' statement for channel %d."), ChannelIndex));
            }
            else
            {
                UVLayoutChannels.Add(ChannelIndex);
            }
        }

        ValidateMaterialSlots();
    }

    // Slot identity is the name, model-wide. Part-level ops AND the ops nested inside their
    // blocks are counted, but the two are not counted the same way and the asymmetry is load
    // bearing:
    //
    //   PART LEVEL decides both questions - which slots a tag names, and whether an UNTAGGED
    //     generator opened the implicit `Default` slot.
    //   NESTED decides only the first. A boolean tool's `material=` does reach the asset - the
    //     compiler resolves it through the same model-wide table and the faces it produces (the
    //     walls a cut opens, the tool surface a union keeps) carry it - so a binding for it is
    //     used and must not be reported dropped. But a block never OPENS the `Default` slot, so an
    //     untagged op inside one must not set bUsesImplicitDefaultSlot.
    //
    // Hull bodies are not reachable from here at all: they live under the model-level `collision`
    // block rather than in Part.Ops, and their ops are collision-context ops, which take no
    // `material=` in the first place.
    //
    // A slot can be referenced without any `material=` naming it: the compiler allocates
    // the implicit `Default` slot for untagged part-level geometry (PwModelAst.h
    // PwModelDefaultSlotName). That is why the two warnings below read different sets -
    // see the comment on bUsesImplicitDefaultSlot.
    void ValidateMaterialSlots()
    {
        // The part name travels with the token. This runs from ValidateDocument, long after
        // CurrentPart was reset, so an unbound-slot warning anchored on a line inside a part
        // used to ship with an empty PartName - the one field of the diagnostic that says
        // WHERE among several parts to look.
        struct FSlotReference
        {
            FPwToken At;
            FString PartName;
        };

        TMap<FString, FSlotReference> ReferencedSlots;

        // The implicit slot, tracked apart from ReferencedSlots on purpose. It is REFERENCED
        // (the compiler allocates it and untagged triangles carry its ID), so a
        // `materials { Default = "…" }` binding is used and must not be reported as dropped -
        // that report was false, and static_mesh.describe on the written asset showed the slot
        // present and bound. But it is never TAGGED by a `material=` the author wrote, so
        // feeding it into ReferencedSlots would fire PWMODEL_UNBOUND_MATERIAL on every ordinary
        // document that simply has no materials block, which is the normal case and not a
        // mistake. Hence one bool used in exactly one of the two loops below.
        bool bUsesImplicitDefaultSlot = false;

        // One tagged reference, recorded the first time the name is seen. Shared by the part-level
        // loop and the nested walk below it so the two cannot record it differently.
        auto RecordSlotReference = [&ReferencedSlots](const FPwValue& Material, const FString& PartName)
        {
            if (ReferencedSlots.Contains(Material.Text))
            {
                return;
            }
            FSlotReference Reference;
            Reference.At.Line = Material.Line;
            Reference.At.Column = Material.Column;
            Reference.PartName = PartName;
            ReferencedSlots.Add(Material.Text, MoveTemp(Reference));
        };

        for (const FPwModelPart& Part : Doc.Parts)
        {
            // Ops nested inside this part's blocks, walked breadth-first for their tags alone.
            TArray<const FPwOp*> NestedOps;

            for (const FPwOp& Op : Part.Ops)
            {
                for (const FPwOp& Child : Op.Children)
                {
                    NestedOps.Add(&Child);
                }

                const FPwValue* Material = Op.Params.Find(TEXT("material"));
                const bool bTagged =
                    Material && Material->Type == EPwValueType::String && !Material->Text.IsEmpty();

                if (!bTagged)
                {
                    // Mirrors PwModelCompiler.cpp RunGenerator, which is the only path that
                    // OPENS the implicit slot: part-level (never inside a boolean tool or a hull
                    // body - an untagged op there takes the enclosing boolean's fallback, which is
                    // why the nested walk below collects tags only), a GENERATOR rather than a
                    // modifier, and not an `append_buffers` carrying an explicit raw
                    // `material_id=` - that one writes the ID itself and the compiler leaves it
                    // alone.
                    //
                    // Deliberately not mirrored: the compiler's `Scratch->GetTriangleCount() > 0`
                    // test, which the parser cannot evaluate. The only op that fails it is a
                    // `procedural_mesh` that appends nothing, so a document made ONLY of those
                    // loses this warning for a binding that really is dropped. Over-suppressing
                    // one degenerate case beats the shipped alternative of contradicting the
                    // asset on every ordinary one.
                    const FPwModelOpSpec* Spec = PwModelOpTable::Find(Op.OpName, EPwModelOpContext::Part);
                    const bool bExplicitMaterialId =
                        Op.OpName == TEXT("append_buffers") && Op.Params.Contains(TEXT("material_id"));
                    if (Spec && Spec->bGenerator && !bExplicitMaterialId)
                    {
                        bUsesImplicitDefaultSlot = true;
                    }
                    continue;
                }

                RecordSlotReference(*Material, Part.Name);
            }

            for (int32 Index = 0; Index < NestedOps.Num(); ++Index)
            {
                const FPwOp& Op = *NestedOps[Index];
                for (const FPwOp& Child : Op.Children)
                {
                    NestedOps.Add(&Child);
                }

                const FPwValue* Material = Op.Params.Find(TEXT("material"));
                if (Material && Material->Type == EPwValueType::String && !Material->Text.IsEmpty())
                {
                    RecordSlotReference(*Material, Part.Name);
                }
            }
        }

        for (const TPair<FString, FSlotReference>& Pair : ReferencedSlots)
        {
            bool bBound = false;
            for (const FPwModelMaterialBinding& Binding : Doc.Materials)
            {
                if (Binding.Slot == Pair.Key)
                {
                    bBound = true;
                    break;
                }
            }
            if (!bBound)
            {
                CurrentPart = Pair.Value.PartName;
                Warn(PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL, Pair.Value.At,
                    FString::Printf(TEXT("Slot '%s' is tagged by geometry but bound to no material; the asset gets an empty slot."),
                        *Pair.Key));
                CurrentPart.Reset();
            }
        }

        for (const FPwModelMaterialBinding& Binding : Doc.Materials)
        {
            const bool bIsUsedImplicitDefault =
                bUsesImplicitDefaultSlot && Binding.Slot == PwModelDefaultSlotName;
            if (!ReferencedSlots.Contains(Binding.Slot) && !bIsUsedImplicitDefault)
            {
                // No part name here, and that is correct rather than an oversight: this
                // warning is anchored on the binding line inside `materials { … }`, which is
                // model level. The dropped binding is the mistake, not any one part.
                //
                // "Dropped" is a checked claim, not a figure of speech: PwModelCompiler.cpp
                // CreateAsset copies a binding into FStaticMeshCreateSpec::MaterialBindings
                // only when SlotNames contains its slot, so a slot no geometry allocated never
                // reaches the asset at all. The one case where that used to be false - the
                // implicit `Default` slot - is excluded above.
                FPwToken At;
                At.Line = Binding.Line;
                At.Column = Binding.Column;
                Warn(PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL, At,
                    FString::Printf(TEXT("Slot '%s' is bound but no geometry tags it, so the asset has no such slot and the binding is dropped. ")
                                    TEXT("Tag geometry with material=\"%s\", or drop the binding."),
                        *Binding.Slot, *Binding.Slot));
            }
        }
    }
};
}

bool FPwModelParser::Parse(FStringView Source, FPwModelDocument& OutDocument,
                           TArray<FPwDiagnostic>& OutDiagnostics)
{
    OutDocument = FPwModelDocument();
    OutDiagnostics.Reset();

    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> LexDiagnostics;
    FPwTokenizer::Tokenize(Source, Tokens, LexDiagnostics);
    OutDiagnostics.Append(MoveTemp(LexDiagnostics));

    FPwModelParserImpl Impl(Tokens, OutDocument, OutDiagnostics);
    Impl.ParseDocument();

    return !PwDiagnosticsHaveError(OutDiagnostics);
}
