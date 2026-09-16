// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Model/PwModelCompiler.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelCollision.h"
#include "Model/PwModelParser.h"
#include "PwSource/PwSkeletonRef.h"
#include "PwSource/PwSuggest.h"
#include "PwSource/PwValueRead.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryAssetCreate.h"
#include "Handlers/Geometry/GeometrySkeletalAssetCreate.h"
#include "Handlers/Geometry/GeometryOps.h"
#include "Handlers/Geometry/GeometryOps_Advanced.h"
#include "Handlers/Geometry/GeometryOps_Boolean.h"
#include "Handlers/Geometry/GeometryOps_Elements.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/GeometryScriptDebugSink.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "Materials/MaterialInterface.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMeshEditor.h"
#include "Intersection/IntrTriangle3Triangle3.h"
#include "Parameterization/MeshUVPacking.h"
#include "Polygon2.h"
#include "Selections/MeshConnectedComponents.h"

#include "Animation/Skeleton.h"
#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshBoneWeightFunctions.h"
#include "GeometryScript/MeshMaterialFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"

// Prefixed rather than anonymous: this module builds with bUseUnity = true, so a helper
// with a common name in an anonymous namespace collides with a sibling TU when Unity
// merges them. GeometryOps_Elements.cpp's ElementsPrivate is the same idiom.
namespace PwModelCompilerPrivate
{

// The slot every untagged piece of geometry lands in. Allocated lazily, so a model whose
// geometry is fully tagged never grows one, and placed LAST among the slots the build
// allocates - see FCompiler::MoveImplicitDefaultSlotLast for why this one slot is exempt
// from the first-use rule that orders every other. A model whose geometry is untagged
// throughout still has it at ID 0, being the only slot there is.
//
// The name itself lives in PwModelAst.h, shared with the parser: it is a bindable slot,
// so PWMODEL_UNUSED_MATERIAL has to know that `materials { Default = "…" }` is used.
const TCHAR* const DefaultSlotName = PwModelDefaultSlotName;

// `.pwmodel` dimensions are already Unreal units (centimetres); there is no per-document unit
// declaration to read. Cleanup still derives its physical window from the actual operands so a
// tiny model is not judged with a large-model absolute threshold. The area term is quadratic in
// the characteristic length because it measures a surface, not a distance.
constexpr double PwModelDocumentToUnrealUnitScale = 1.0;
constexpr double PwModelBooleanCleanupRelativeLength = 1e-6;
constexpr double PwModelBooleanCleanupRelativeArea = 1e-9;
constexpr double PwModelBooleanCleanupMinLength = 1e-6;
constexpr double PwModelBooleanCleanupMaxLength = 1e-3;
constexpr double PwModelBooleanCleanupMinArea = 1e-12;
constexpr double PwModelBooleanCleanupMaxArea = 1e-3;

// ---------------------------------------------------------------------------
// Reading FPwValue
//
// The six accessors, the (roll, pitch, yaw) -> FRotator reordering and the at/rotate/scale
// transform reader all live in Model/PwValueRead.h, shared with PwModelCollision.cpp. They
// were duplicated across the two files, with the rotator convention spelled twice in two
// different-looking forms - flip one and a rotated collision element silently disagrees with the
// part transform it was authored against, with nothing to fail.
//
// Imported by using-declarations rather than qualified at ~120 call sites. Unity-safe because
// PwModelCompilerPrivate is a named namespace this translation unit alone opens, so the names are
// synonyms inside it and are never visible to a sibling TU merged into the same blob - which is
// the hazard the prefixing convention exists for, and is not what a using-declaration creates.
// ---------------------------------------------------------------------------

using PwValueRead::FindValue;
using PwValueRead::GetBool;
using PwValueRead::GetColor;
using PwValueRead::GetFrameList;
using PwValueRead::GetIdentifier;
using PwValueRead::GetIndexList;
using PwValueRead::GetInt;
using PwValueRead::GetNumber;
using PwValueRead::GetPointList2;
using PwValueRead::GetPointList3;
using PwValueRead::GetPointList4;
using PwValueRead::GetString;
using PwValueRead::GetVector2;
using PwValueRead::GetVector3;
using PwValueRead::HasValue;
using PwValueRead::ReadTransformParams;

GeometryOps::EMeshAxis ReadMeshAxis(const TMap<FString, FPwValue>& Params, const TCHAR* Default)
{
    const FString Axis = GetIdentifier(Params, TEXT("axis"), Default);
    if (Axis == TEXT("x")) return GeometryOps::EMeshAxis::X;
    if (Axis == TEXT("y")) return GeometryOps::EMeshAxis::Y;
    return GeometryOps::EMeshAxis::Z;
}

// `terms=[(order, amplitude, phase), …]` as harmonic terms.
//
// Lives here rather than in PwValueRead.h on purpose: that header is deliberately free of
// op types - it reads FPwValue and nothing else, and PwModelCollision.cpp includes it -
// while GeometryOps::FHarmonicTerm is an op type. Every other reader that speaks an op's
// vocabulary (ReadMeshAxis, ReadFlareType, ReadFillHolesMethod) is in this file for the same
// reason.
//
// The order is TRUNCATED, not rounded: the parser refuses any entry whose first component is not
// a whole number, so a fraction arriving here would be a parser bug, and rounding it would
// quietly deform the mesh into the shape of that bug instead of reproducing what was tested.
TArray<GeometryOps::FHarmonicTerm> ReadHarmonicTerms(
    const TMap<FString, FPwValue>& Params, const TCHAR* Name)
{
    TArray<GeometryOps::FHarmonicTerm> Terms;
    const FPwValue* Value = PwValueRead::FindValue(Params, Name);
    if (!Value || Value->Type != EPwValueType::TupleList)
    {
        return Terms;
    }
    Terms.Reserve(Value->TupleList.Num());
    for (const TArray<double>& Entry : Value->TupleList)
    {
        if (Entry.Num() >= 3)
        {
            GeometryOps::FHarmonicTerm Term;
            Term.Order = static_cast<int32>(FMath::TruncToDouble(Entry[0]));
            Term.Amplitude = Entry[1];
            Term.PhaseDegrees = Entry[2];
            Terms.Add(Term);
        }
    }
    return Terms;
}

// Enum readers. Each takes the CURRENT value as its fallback so the op's params struct stays the
// single source of every default - the same shape every GetInt / GetNumber call here uses - and
// each ends in a plain return rather than a diagnostic: the parser has already rejected any
// identifier outside FPwModelParamSpec::AllowedValues, so an unmatched string here would be a
// parser bug and inventing a second diagnostic for it would report that bug as the author's.
GeometryOps::ESimplifyMethod ReadSimplifyMethod(
    const TMap<FString, FPwValue>& Params, GeometryOps::ESimplifyMethod Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("method"), TEXT(""));
    if (Value == TEXT("standard_qem")) return GeometryOps::ESimplifyMethod::StandardQEM;
    if (Value == TEXT("volume_preserving")) return GeometryOps::ESimplifyMethod::VolumePreserving;
    if (Value == TEXT("attribute_aware")) return GeometryOps::ESimplifyMethod::AttributeAware;
    if (Value == TEXT("attribute_aware_v2")) return GeometryOps::ESimplifyMethod::AttributeAwareV2;
    return Fallback;
}

GeometryOps::ESimplifyQuadricVariant ReadSimplifyQuadricVariant(
    const TMap<FString, FPwValue>& Params, GeometryOps::ESimplifyQuadricVariant Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("quadric_variant"), TEXT(""));
    if (Value == TEXT("plane_quadric")) return GeometryOps::ESimplifyQuadricVariant::PlaneQuadric;
    if (Value == TEXT("triangle_quadric")) return GeometryOps::ESimplifyQuadricVariant::TriangleQuadric;
    return Fallback;
}

GeometryOps::ERemeshTargetType ReadRemeshTargetType(
    const TMap<FString, FPwValue>& Params, GeometryOps::ERemeshTargetType Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("target_type"), TEXT(""));
    if (Value == TEXT("triangle_count")) return GeometryOps::ERemeshTargetType::TriangleCount;
    if (Value == TEXT("target_edge_length")) return GeometryOps::ERemeshTargetType::TargetEdgeLength;
    return Fallback;
}

GeometryOps::ERemeshSmoothingType ReadRemeshSmoothingType(
    const TMap<FString, FPwValue>& Params, GeometryOps::ERemeshSmoothingType Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("smoothing_type"), TEXT(""));
    if (Value == TEXT("uniform")) return GeometryOps::ERemeshSmoothingType::Uniform;
    if (Value == TEXT("uv_preserving")) return GeometryOps::ERemeshSmoothingType::UVPreserving;
    if (Value == TEXT("mixed")) return GeometryOps::ERemeshSmoothingType::Mixed;
    return Fallback;
}

// Takes the parameter NAME because remesh publishes the same vocabulary three times, once per
// attribute boundary.
GeometryOps::ERemeshEdgeConstraint ReadRemeshEdgeConstraint(
    const TMap<FString, FPwValue>& Params, const TCHAR* Name,
    GeometryOps::ERemeshEdgeConstraint Fallback)
{
    const FString Value = GetIdentifier(Params, Name, TEXT(""));
    if (Value == TEXT("fixed")) return GeometryOps::ERemeshEdgeConstraint::Fixed;
    if (Value == TEXT("refine")) return GeometryOps::ERemeshEdgeConstraint::Refine;
    if (Value == TEXT("free")) return GeometryOps::ERemeshEdgeConstraint::Free;
    if (Value == TEXT("ignore")) return GeometryOps::ERemeshEdgeConstraint::Ignore;
    return Fallback;
}

GeometryOps::EUVLayoutType ReadUVLayoutType(
    const TMap<FString, FPwValue>& Params, GeometryOps::EUVLayoutType Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("layout_type"), TEXT(""));
    if (Value == TEXT("transform")) return GeometryOps::EUVLayoutType::Transform;
    if (Value == TEXT("stack")) return GeometryOps::EUVLayoutType::Stack;
    if (Value == TEXT("repack")) return GeometryOps::EUVLayoutType::Repack;
    if (Value == TEXT("normalize")) return GeometryOps::EUVLayoutType::Normalize;
    return Fallback;
}

GeometryOps::FFaceSelectionSpec ReadFaceSelection(const TMap<FString, FPwValue>& Params)
{
    GeometryOps::FFaceSelectionSpec Faces;
    if (const FPwValue* Direction = FindValue(Params, TEXT("face_direction")))
    {
        if (Direction->Type == EPwValueType::Tuple && Direction->Tuple.Num() >= 3)
        {
            Faces.bHasDirection = true;
            Faces.Direction = FVector(Direction->Tuple[0], Direction->Tuple[1], Direction->Tuple[2]);
        }
    }
    Faces.AngleTolerance = GetNumber(Params, TEXT("face_angle_tolerance"), Faces.AngleTolerance);
    return Faces;
}

GeometryOps::EPolyOperationArea ReadPolyOperationArea(
    const TMap<FString, FPwValue>& Params, GeometryOps::EPolyOperationArea Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("area_mode"), TEXT(""));
    if (Value == TEXT("entire_selection")) return GeometryOps::EPolyOperationArea::EntireSelection;
    if (Value == TEXT("per_polygroup")) return GeometryOps::EPolyOperationArea::PerPolygroup;
    if (Value == TEXT("per_triangle")) return GeometryOps::EPolyOperationArea::PerTriangle;
    return Fallback;
}

GeometryOps::EEditPolygroupMode ReadEditPolygroupMode(
    const TMap<FString, FPwValue>& Params, GeometryOps::EEditPolygroupMode Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("group_mode"), TEXT(""));
    if (Value == TEXT("preserve_existing")) return GeometryOps::EEditPolygroupMode::PreserveExisting;
    if (Value == TEXT("auto_generate_new")) return GeometryOps::EEditPolygroupMode::AutoGenerateNew;
    if (Value == TEXT("set_constant")) return GeometryOps::EEditPolygroupMode::SetConstant;
    return Fallback;
}

GeometryOps::EOffsetFacesType ReadOffsetFacesType(
    const TMap<FString, FPwValue>& Params, GeometryOps::EOffsetFacesType Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("offset_type"), TEXT(""));
    if (Value == TEXT("vertex_normal")) return GeometryOps::EOffsetFacesType::VertexNormal;
    if (Value == TEXT("face_normal")) return GeometryOps::EOffsetFacesType::FaceNormal;
    if (Value == TEXT("parallel_face_offset")) return GeometryOps::EOffsetFacesType::ParallelFaceOffset;
    return Fallback;
}

GeometryOps::ELinearExtrudeDirection ReadLinearExtrudeDirection(
    const TMap<FString, FPwValue>& Params, GeometryOps::ELinearExtrudeDirection Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("direction_mode"), TEXT(""));
    if (Value == TEXT("fixed_direction")) return GeometryOps::ELinearExtrudeDirection::FixedDirection;
    if (Value == TEXT("average_face_normal")) return GeometryOps::ELinearExtrudeDirection::AverageFaceNormal;
    return Fallback;
}

GeometryOps::EFillHolesMethod ReadFillHolesMethod(
    const TMap<FString, FPwValue>& Params, GeometryOps::EFillHolesMethod Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("method"), TEXT(""));
    if (Value == TEXT("automatic")) return GeometryOps::EFillHolesMethod::Automatic;
    if (Value == TEXT("minimal_fill")) return GeometryOps::EFillHolesMethod::MinimalFill;
    if (Value == TEXT("polygon_triangulation")) return GeometryOps::EFillHolesMethod::PolygonTriangulation;
    if (Value == TEXT("triangle_fan")) return GeometryOps::EFillHolesMethod::TriangleFan;
    if (Value == TEXT("planar_projection")) return GeometryOps::EFillHolesMethod::PlanarProjection;
    return Fallback;
}

GeometryOps::ERepairMeshMode ReadRepairMeshMode(
    const TMap<FString, FPwValue>& Params, GeometryOps::ERepairMeshMode Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("mode"), TEXT(""));
    if (Value == TEXT("delete_only")) return GeometryOps::ERepairMeshMode::DeleteOnly;
    if (Value == TEXT("repair_or_delete")) return GeometryOps::ERepairMeshMode::RepairOrDelete;
    if (Value == TEXT("repair_or_skip")) return GeometryOps::ERepairMeshMode::RepairOrSkip;
    return Fallback;
}

GeometryOps::ETangentType ReadTangentType(
    const TMap<FString, FPwValue>& Params, GeometryOps::ETangentType Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("type"), TEXT(""));
    if (Value == TEXT("fast_mikkt")) return GeometryOps::ETangentType::FastMikkT;
    if (Value == TEXT("per_triangle")) return GeometryOps::ETangentType::PerTriangle;
    if (Value == TEXT("standard_mikkt")) return GeometryOps::ETangentType::StandardMikkT;
    return Fallback;
}

GeometryOps::EFlareType ReadFlareType(
    const TMap<FString, FPwValue>& Params, GeometryOps::EFlareType Fallback)
{
    const FString Value = GetIdentifier(Params, TEXT("flare_type"), TEXT(""));
    if (Value == TEXT("sin_mode")) return GeometryOps::EFlareType::SinMode;
    if (Value == TEXT("sin_squared_mode")) return GeometryOps::EFlareType::SinSquaredMode;
    if (Value == TEXT("triangle_mode")) return GeometryOps::EFlareType::TriangleMode;
    return Fallback;
}

// The four parameters every face op shares, read in one place for the reason the parser
// publishes them in one place. Takes the spec BY VALUE as its own fallback source, so each
// field's default still comes from the op's params struct rather than from a literal here.
GeometryOps::FFaceOpCommonSpec ReadFaceOpCommon(
    const TMap<FString, FPwValue>& Params, GeometryOps::FFaceOpCommonSpec Common)
{
    Common.AreaMode = ReadPolyOperationArea(Params, Common.AreaMode);
    Common.Groups.GroupMode = ReadEditPolygroupMode(Params, Common.Groups.GroupMode);
    Common.Groups.ConstantGroup = GetInt(Params, TEXT("group_id"), Common.Groups.ConstantGroup);
    Common.UVScale = GetNumber(Params, TEXT("uv_scale"), Common.UVScale);
    return Common;
}

// The two extent fields bend / twist / taper share.
GeometryOps::FWarpExtentSpec ReadWarpExtents(
    const TMap<FString, FPwValue>& Params, GeometryOps::FWarpExtentSpec Extents)
{
    Extents.bSymmetricExtents = GetBool(Params, TEXT("symmetric_extents"), Extents.bSymmetricExtents);
    Extents.LowerExtent = GetNumber(Params, TEXT("lower_extent"), Extents.LowerExtent);
    return Extents;
}

// The warp frame, shared by bend / twist / taper for the same reason ReadWarpExtents is: the
// three publish an identical pair and reading it three times is three places for one of them to
// drift. Defaults are behaviour-preserving by ARITHMETIC rather than by a branch - axis z with
// centre (0, 0, 0) builds the identity basis and a zero translation, which is exactly the
// FTransform::Identity these ops passed before the frame existed.
GeometryOps::FWarpFrameSpec ReadWarpFrame(
    const TMap<FString, FPwValue>& Params, GeometryOps::FWarpFrameSpec Frame)
{
    Frame.Axis = ReadMeshAxis(Params, TEXT("z"));
    Frame.Center = GetVector3(Params, TEXT("center"), Frame.Center);
    return Frame;
}

// The upper bound on an authored path, read out of GeometryOps::SplinePathStepCount rather than
// re-typed. That function is the RPC front-end's buffer sizer and is exported so a second front
// end can size the same way - but this front end does not SAMPLE anything, because the frames are
// written literally in the document. What is left to share is the bound itself: clamping a step
// count of MAX_int32 answers the ceiling, and a path is steps+1 frames. Read through the call so
// widening the clamp widens this with it.
int32 MaxAuthoredPathFrames()
{
    return GeometryOps::SplinePathStepCount(TNumericLimits<int32>::Max()) + 1;
}

// The shape check the two SWEEPS need on a `path=`, as an FOpResult so it reports through
// ReportOpResult exactly like an engine failure - line-anchored, with an ERR_* code - rather
// than as a second, differently-shaped diagnostic.
//
// `sweep` and `extrude_along_spline` are its only callers. `array_along_path` deliberately does
// NOT share it: an array of copies is meaningful at one frame, so its bound is
// ValidateArrayCount's 1..100 rather than the 2..MaxAuthoredPathFrames() below, and a single
// frame there is a placement rather than the silent vertical-tube fallback described next.
//
// The lower bound is the load-bearing half. GeometryOps::Sweep selects its VERTICAL FALLBACK on
// `Samples.Num() < 2`, so a one-frame path would compile clean and quietly produce a vertical
// tube through the mesh's bounding box instead of the path the author wrote; ExtrudeAlongSpline
// has no fallback and would produce nothing at all. Both are silent, and both are the reason this
// runs before the op does.
GeometryOps::FOpResult ValidatePathFrames(const TArray<FTransform>& Frames, const TCHAR* ParamName)
{
    if (Frames.Num() < 2)
    {
        return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'%s' needs at least 2 frames to describe a path, but carries %d"),
                ParamName, Frames.Num()));
    }
    if (Frames.Num() > MaxAuthoredPathFrames())
    {
        return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'%s' carries %d frames; the sweep path is bounded at %d (GeometryOps::SplinePathStepCount)"),
                ParamName, Frames.Num(), MaxAuthoredPathFrames()));
    }
    return GeometryOps::FOpResult::Ok();
}

// `profile=` as the op's cross-section. Fewer than three points cannot bound a face, and the ops
// treat a short list as "no profile" and fall back to the bounding-box circle - which is a
// different shape from the one the author asked for, arriving with no diagnostic. Rejected here
// so the fallback is only ever reached by omitting the parameter.
GeometryOps::FOpResult ReadSweepProfile(const TMap<FString, FPwValue>& Params,
                                        GeometryOps::FSweepProfile& OutProfile)
{
    if (!HasValue(Params, TEXT("profile")))
    {
        return GeometryOps::FOpResult::Ok();
    }

    OutProfile.Vertices = GetPointList2(Params, TEXT("profile"));
    if (OutProfile.Vertices.Num() < 3)
    {
        return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'profile' needs at least 3 points to bound a cross-section, but carries %d"),
                OutProfile.Vertices.Num()));
    }
    return GeometryOps::FOpResult::Ok();
}

// `scales=[(alpha, scale), …]` for sweep / extrude_along_spline: the cross-section scale as a
// LAW along the path rather than a straight ramp between two scalars.
//
// Every arm below refuses rather than repairs, and each refusal is a shape whose repair would be
// a guess about which half the author meant:
//
//  - `scales` together with `scale_start` / `scale_end`. Only one can be honoured. Accepting the
//    pair would leave the scalars looking set while the curve silently won - the same defect
//    PWMODEL_MATERIAL_ID_CONFLICT exists to refuse on `append_buffers`.
//  - alpha outside [0, 1]. Alpha is the normalised position along the path, so 1.5 is not a
//    longer path, it is a knot the sweep never reaches; clamping it would move the knot.
//  - alpha not strictly ascending. Two knots at one alpha is a discontinuity the sweep cannot
//    represent (there is one frame there, and it has one scale), and a descending pair is almost
//    always a transposed line.
//  - scale <= 0. Zero collapses the cross-section to the path itself - zero-area triangles all
//    the way round - and negative REFLECTS it, which reverses the tube's facing normals while
//    leaving isClosed, boundaryEdges and the triangle count identical. That is a mesh that
//    renders correctly and lights inside-out, which is the failure this format spends
//    health.signedVolume to catch after the fact; here it can be refused at its own line.
GeometryOps::FOpResult ReadSweepScaleCurve(const TMap<FString, FPwValue>& Params,
                                           GeometryOps::FSweepScaleCurve& OutCurve)
{
    if (!HasValue(Params, TEXT("scales")))
    {
        return GeometryOps::FOpResult::Ok();
    }

    const bool bHasStart = HasValue(Params, TEXT("scale_start"));
    const bool bHasEnd = HasValue(Params, TEXT("scale_end"));
    if (bHasStart || bHasEnd)
    {
        return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'scales' and '%s' both name the cross-section scale and only one can be ")
                TEXT("honoured. 'scales' is the general form - write the endpoints as its first and last ")
                TEXT("knot, e.g. scales=[(0, 1), (1, 0.3)] for what scale_start=1 scale_end=0.3 said."),
                bHasStart ? TEXT("scale_start") : TEXT("scale_end")));
    }

    OutCurve.Knots = GetPointList2(Params, TEXT("scales"));
    if (OutCurve.Knots.Num() < 2)
    {
        return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'scales' needs at least 2 knots to be a law, but carries %d. One knot is a ")
                TEXT("constant scale, which is what omitting the parameter already gives."),
                OutCurve.Knots.Num()));
    }

    for (int32 i = 0; i < OutCurve.Knots.Num(); ++i)
    {
        const FVector2D& Knot = OutCurve.Knots[i];
        if (Knot.X < 0.0 || Knot.X > 1.0)
        {
            return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("'scales' knot %d has alpha %g, outside [0, 1]. Alpha is the NORMALISED ")
                    TEXT("position along the path - 0 is the first frame, 1 the last - not a distance in uu."),
                    i, Knot.X));
        }
        if (Knot.Y <= 0.0)
        {
            return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("'scales' knot %d has scale %g. A scale must be positive: 0 collapses the ")
                    TEXT("cross-section onto the path and a negative one reflects it, which reverses the swept ")
                    TEXT("tube's facing normals while every count and health field stays identical."),
                    i, Knot.Y));
        }
        if (i > 0 && Knot.X <= OutCurve.Knots[i - 1].X)
        {
            return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("'scales' knot %d has alpha %g, which does not come after knot %d's %g. ")
                    TEXT("Knots must ascend strictly: the sweep has one frame per alpha and therefore one ")
                    TEXT("scale there, so two knots at the same alpha describe a step it cannot make."),
                    i, Knot.X, i - 1, OutCurve.Knots[i - 1].X));
        }
    }
    return GeometryOps::FOpResult::Ok();
}

UDynamicMesh* NewTransientMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// Stamps the counts onto a result an op produced by calling the engine directly, so it reports
// the same before/after shape every GeometryOps op does. Two call sites had four identical lines
// each; a third would have been the one to drift.
//
// bChanged is unconditional because both callers are unconditional writes - the engine call has
// already happened by the time this runs. Before == after because neither adds or removes
// geometry: they rewrite UVs and vertex positions in place.
GeometryOps::FOpResult OkWithCounts(UDynamicMesh* Mesh)
{
    GeometryOps::FOpResult Result = GeometryOps::FOpResult::Ok();
    Result.bChanged = true;
    Result.TrianglesBefore = Result.TrianglesAfter = Mesh->GetTriangleCount();
    Result.VerticesBefore = Result.VerticesAfter = GeometryUtils::GetMeshVertexCount(Mesh);
    return Result;
}

// True when the mesh carries a UV layer at UVChannel that actually has elements. Element
// presence rather than layer presence is the whole point: SetNumUVSets creates an
// element-less layer, and it is the missing ELEMENTS that ship an untextured asset.
//
// One line, delegating: this used to be a hand-rolled copy of the attribute walk that
// GeometryUtils::MeshHasUsableUVs performs for channel 0. The shared predicate is now
// channel-parameterised (GeometryUtils::MeshHasUVElementsInChannel) and MeshHasUsableUVs is
// defined on it, so the compiler's merge and the bake's crash guard cannot answer differently
// about the same channel. The local name stays because the call sites read better with it.
bool MeshHasUVElements(UDynamicMesh* Mesh, int32 UVChannel)
{
    return GeometryUtils::MeshHasUVElementsInChannel(Mesh, UVChannel);
}

// The layer count, from the same accessor GeometryUtils::EnsureMeshHasUVChannel grows against.
// Reading it through the geometry-script query rather than walking the attribute set by hand is
// what keeps "how many layers are there" and "does channel N exist yet" the same question - a
// hand-walk that disagreed with the grow guard would loop over channels the guard refuses to
// create.
int32 MeshNumUVLayers(UDynamicMesh* Mesh)
{
    return Mesh ? UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Mesh) : 0;
}

// The same deterministic bounds-framed box projection GeometryUtils::EnsureMeshHasUVs
// applies to channel 0, for any channel. Deterministic and solver-free: an XAtlas unwrap
// can hang on a degenerate mesh, and this runs unattended inside a compile.
//
// This IS the projection body inside EnsureMeshHasUVs, minus that function's SetNumUVSets(1) and
// its MeshHasUsableUVs precondition - both channel-0-only, and both already decided by the caller
// here, which knows exactly which channels are missing and grows them through
// EnsureMeshHasUVChannel. Calling EnsureMeshHasUVs instead would truncate every higher UV set.
void BoxProjectChannel(UDynamicMesh* Mesh, int32 UVChannel)
{
    const UE::Geometry::FAxisAlignedBox3d Bounds = Mesh->GetMeshRef().GetBounds();
    const FVector Center = Bounds.Center();
    FVector BoxSize = Bounds.Diagonal();
    BoxSize.X = FMath::Max(FMath::Abs(BoxSize.X), 1.0);
    BoxSize.Y = FMath::Max(FMath::Abs(BoxSize.Y), 1.0);
    BoxSize.Z = FMath::Max(FMath::Abs(BoxSize.Z), 1.0);

    UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
        Mesh, UVChannel, FTransform(FQuat::Identity, Center, BoxSize),
        FGeometryScriptMeshSelection(), /*MinIslandTriCount=*/2, nullptr);
}

FString HashSource(FStringView Source)
{
    FMD5 Md5;

    if (Source.Len() > 0)
    {
        const FTCHARToUTF8 Utf8(Source.GetData(), Source.Len());
        Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    }

    uint8 Digest[16] = {};
    Md5.Final(Digest);

    return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

// The box the REPORTED bounds is measured over: the triangles that survive, not the vertex
// buffer.
//
// FDynamicMesh3::GetBounds is documented "Computes bounding box of all vertices" and loops
// VertexIndicesItr() with no reference to triangles, so any vertex still ALLOCATED votes whether
// or not a triangle uses it. A cut can leave one: the vertex is live, isolated, and part of no
// surface.
//
// MEASURED, and the reason this is not a theoretical tidy-up: an 8-gon prism (r=100, z 0..200)
// written as `revolve` and then cut at z=150 by a `subtract` reported bounds.max.z = 200, while
// its signedVolume was exactly the analytic volume of the same prism at height 150 and its
// health block read closed / 0 boundary edges / 0 degenerates / 0 bowties. The identical solid
// written with `cylinder` reported 150, and the BAKED asset - whose render data is built from
// triangles - spanned the cut height. Both meshes were the same cut solid; only the box
// differed, and nothing wrong ever reached disk.
//
// Only `revolve` produces it because only `revolve` takes an author-supplied profile that can
// have a point ON the revolution axis, and the engine's profile sweep welds such a point to a
// single shared vertex (SweepGenerator.h:268-270, `WeldedVertices`) - one apex closing the end
// fan, which the cut strands. WHY that apex outlives the cut is not traced: FMeshBoolean deletes
// with RemoveTriangle(TID, bRemoveIsolatedVertices=true, false) (MeshBoolean.cpp:532), so
// something later in the operation re-adds or preserves it. It does not need to be traced to fix
// the measurement - the baked asset proves no triangle reaches the reported extent.
//
// The failure mode is the dangerous one. 200 is exactly the extent the author asked the
// generator for, so the wrong number does not read as a bad measurement - it reads as "the
// boolean did nothing", and sends the author to debug the boolean. The docs prescribe this box
// as the check that lets a model be fitted to a size WITHOUT writing an asset per iteration, so
// it is the one number in the response that has to be measured rather than inferred.
//
// Two remedies were rejected. `weld_vertices` cannot clear an isolated vertex - it has no edge
// to weld along, and the repro confirmed the box unchanged after it. Compacting the mesh does
// not clear it either: compaction closes gaps in the index space and keeps every LIVE vertex,
// and an isolated vertex is live. Removing the vertices would work but mutates the geometry on
// the way to the asset for the sake of a report; measuring the triangles changes nothing that
// is written.
//
// Empty on a triangle-less mesh, which is the same answer GetBounds gives for a vertex-less
// one, and both call sites already gate on IsEmpty().
UE::Geometry::FAxisAlignedBox3d GetTriangleReferencedBounds(UDynamicMesh* Mesh)
{
    UE::Geometry::FAxisAlignedBox3d Box = UE::Geometry::FAxisAlignedBox3d::Empty();
    if (!Mesh)
    {
        return Box;
    }

    const UE::Geometry::FDynamicMesh3& MeshRef = Mesh->GetMeshRef();
    for (const int32 TriangleID : MeshRef.TriangleIndicesItr())
    {
        Box.Contain(MeshRef.GetTriBounds(TriangleID));
    }
    return Box;
}

// ---------------------------------------------------------------------------
// Material IDs on a whole mesh
//
// The three helpers below are the shared vocabulary for every "which slot should THESE triangles
// carry" question in this file - the appending modifiers, the booleans and `bevel`. They exist as
// free functions rather than FCompiler methods because none of them touches the slot TABLE; they
// only read and shift the ids already written onto triangles.
// ---------------------------------------------------------------------------

// Triangle count per distinct material id over the whole mesh.
//
// A mesh whose ops never reached ClearMaterialIDs has no material attribute at all. Its triangles
// are id 0 by the same engine default anything untagged lands on, and counting them as 0 is the
// honest answer rather than a special case.
void CountMaterialTriangles(UDynamicMesh* Mesh, TMap<int32, int32>& Out)
{
    if (!Mesh)
    {
        return;
    }

    Mesh->ProcessMesh([&Out](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        const UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs =
            ReadMesh.HasAttributes() ? ReadMesh.Attributes()->GetMaterialID() : nullptr;

        for (const int32 TriangleID : ReadMesh.TriangleIndicesItr())
        {
            ++Out.FindOrAdd(MaterialIDs ? MaterialIDs->GetValue(TriangleID) : 0);
        }
    });
}

// The id MOST of those triangles carry; ties go to the lower id, so the answer is stable across
// runs and across TMap iteration order. INDEX_NONE when there was nothing to count.
int32 DominantMaterialSlot(const TMap<int32, int32>& CountsByMaterial)
{
    int32 Best = INDEX_NONE;
    int32 BestCount = 0;
    for (const TPair<int32, int32>& Entry : CountsByMaterial)
    {
        if (Entry.Value > BestCount || (Entry.Value == BestCount && Entry.Key < Best))
        {
            Best = Entry.Key;
            BestCount = Entry.Value;
        }
    }
    return Best;
}

// Shift every material id up by one, so no triangle of this mesh carries 0. Enables the attribute
// on a mesh that has none - its triangles are 0 by the engine default and shift to 1 like any
// other - so both operands of a boolean come out of this in the same shape.
//
// WHY A BOOLEAN NEEDS IT. GeometryCore carries a SURVIVING triangle's material id across the
// operation, but a face the operation CREATES from neither operand - a hole fill, and `fill_holes`
// is on by default for all four boolean ops - gets the attribute's default value, which is 0. In a
// finished model 0 is not neutral: the slot table is model-wide and allocated in first-use order,
// so 0 is whichever slot the FIRST part in the document tagged, and a created face therefore ships
// silently in another part's material.
//
// Reserving 0 for the duration makes "still 0 afterwards" mean exactly "created by this operation,
// from neither operand". That is the only way to find those faces: a boolean renumbers triangles
// wholesale, so the snapshot-and-diff by triangle id that works for an appending modifier
// (SnapshotModifierMaterial) is unsound here, and the format has no face identity to fall back on.
void ReserveMaterialIDZero(UDynamicMesh* Mesh)
{
    if (!Mesh)
    {
        return;
    }

    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        if (!EditMesh.HasAttributes())
        {
            EditMesh.EnableAttributes();
        }
        if (!EditMesh.Attributes()->HasMaterialID())
        {
            EditMesh.Attributes()->EnableMaterialID();
        }
        UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = EditMesh.Attributes()->GetMaterialID();
        for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
        {
            MaterialIDs->SetValue(TriangleID, MaterialIDs->GetValue(TriangleID) + 1);
        }
    },
    // The same three arguments the engine's own material edits pass - see ApplyModifierMaterialTag.
    EDynamicMeshChangeType::GeneralEdit,
    EDynamicMeshAttributeChangeFlags::Unknown,
    /*bDeferChangeEvents=*/false);
}

// The inverse of the reservation, and the point of it: every triangle still on 0 was created by the
// operation and is given NewFaceSlot. INDEX_NONE leaves them on 0 - the caller had nothing to
// inherit and the op named nothing, which is the state RunBoolean reports rather than repairs.
//
// Fills OutCountsByMaterial with the RELEASED distribution, so the caller can tell whether a slot a
// `material=` opened ended up carrying anything without a second walk over the mesh.
void ReleaseMaterialIDZero(UDynamicMesh* Mesh, int32 NewFaceSlot,
                           TMap<int32, int32>& OutCountsByMaterial)
{
    if (!Mesh)
    {
        return;
    }

    Mesh->EditMesh([NewFaceSlot, &OutCountsByMaterial](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        if (!EditMesh.HasAttributes() || !EditMesh.Attributes()->HasMaterialID())
        {
            return;
        }
        UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = EditMesh.Attributes()->GetMaterialID();
        for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
        {
            const int32 Reserved = MaterialIDs->GetValue(TriangleID);
            const int32 Released = (Reserved >= 1)
                ? Reserved - 1
                : (NewFaceSlot != INDEX_NONE ? NewFaceSlot : 0);
            MaterialIDs->SetValue(TriangleID, Released);
            ++OutCountsByMaterial.FindOrAdd(Released);
        }
    },
    EDynamicMeshChangeType::GeneralEdit,
    EDynamicMeshAttributeChangeFlags::Unknown,
    /*bDeferChangeEvents=*/false);
}

// Return one mesh per edge-connected tool component. GeometryCore's boolean solver builds one
// global intersection arrangement for a multi-shell tool; a subtract of many disjoint shells can
// therefore over-tessellate the target and leave zero-area triangles. Sequential subtraction is
// equivalent for disconnected tool components and keeps each solver invocation local.
bool SplitToolIntoConnectedComponents(
    const UE::Geometry::FDynamicMesh3* SourceMesh,
    TArray<TUniquePtr<UE::Geometry::FDynamicMesh3>>& OutComponents)
{
    if (!SourceMesh)
    {
        return false;
    }

    UE::Geometry::FMeshConnectedComponents ConnectedComponents(SourceMesh);
    ConnectedComponents.FindConnectedTriangles();
    if (ConnectedComponents.Num() <= 1)
    {
        return false;
    }

    TArray<int32> TriangleToComponent;
    TriangleToComponent.Init(INDEX_NONE, SourceMesh->MaxTriangleID());
    for (int32 ComponentIndex = 0; ComponentIndex < ConnectedComponents.Num(); ++ComponentIndex)
    {
        for (const int32 TriangleID : ConnectedComponents.GetComponent(ComponentIndex).Indices)
        {
            if (TriangleToComponent.IsValidIndex(TriangleID))
            {
                TriangleToComponent[TriangleID] = ComponentIndex;
            }
        }
    }

    auto TriIDToMeshID = [&TriangleToComponent](const int32 TriangleID)
    {
        return TriangleToComponent.IsValidIndex(TriangleID)
            ? TriangleToComponent[TriangleID]
            : INDEX_NONE;
    };

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    return UE::Geometry::FDynamicMeshEditor::SplitMesh(
        SourceMesh, OutComponents, /*bSplitIfSingle=*/false, TriIDToMeshID,
        /*DeleteMeshID=*/-1, nullptr, /*bSortByMeshID=*/true);
#else
    // The TUniquePtr overload (and with it bSplitIfSingle) arrived in UE 5.8. The by-value
    // overload always splits, so the early return above on a single component already supplies
    // bSplitIfSingle=false; move the results into the caller's array.
    TArray<UE::Geometry::FDynamicMesh3> SplitMeshes;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    if (!UE::Geometry::FDynamicMeshEditor::SplitMesh(
            SourceMesh, SplitMeshes, TriIDToMeshID,
            /*DeleteMeshID=*/-1, nullptr, /*bSortByMeshID=*/true))
    {
        return false;
    }
#else
    // MeshIDPerSplitMesh / bSortByMeshID arrived in UE 5.6; before that the split emits one mesh
    // per mesh id in first-seen triangle order. The caller subtracts these components from the
    // target one after another and they are edge-disjoint by construction, so the emission order
    // does not change the result - only which component is subtracted first.
    if (!UE::Geometry::FDynamicMeshEditor::SplitMesh(
            SourceMesh, SplitMeshes, TriIDToMeshID, /*DeleteMeshID=*/-1))
    {
        return false;
    }
#endif
    OutComponents.Reserve(OutComponents.Num() + SplitMeshes.Num());
    for (UE::Geometry::FDynamicMesh3& SplitMesh : SplitMeshes)
    {
        OutComponents.Add(MakeUnique<UE::Geometry::FDynamicMesh3>(MoveTemp(SplitMesh)));
    }
    return true;
#endif
}

// ---------------------------------------------------------------------------
// The compiler
// ---------------------------------------------------------------------------

class FCompiler
{
public:
    FCompiler(const FPwModelCompileOptions& InOptions, FPwModelCompileResult& InResult)
        : Options(InOptions)
        , Result(InResult)
        , Sink(&InResult.Diagnostics)
    {
    }

    void Run(FStringView Source);

private:
    // ---- diagnostics ----
    void AddDiagnostic(EPwSeverity Severity, const TCHAR* Code, int32 Line, int32 Column,
                       FString Message, const FString& PartName,
                       TArray<FString> Suggestions = TArray<FString>(),
                       const TCHAR* ScopeLabel = TEXT("part"));
    void Error(const TCHAR* Code, const FPwOp& Op, FString Message);
    void Warn(const TCHAR* Code, const FPwOp& Op, FString Message);
    void ModelError(const TCHAR* Code, const FPwOp& Op, FString Message);
    void ModelWarn(const TCHAR* Code, const FPwOp& Op, FString Message);

    // ---- material slots ----
    int32 ResolveSlot(const FString& SlotName);

    // "'<Name>' (<index>)" for a slot in the table, "id <n>" for an index past its end. Named by
    // slot rather than by raw index because the name is what the author wrote; an id past the end
    // - only `append_buffers material_id=` can make one - has no name to give.
    FString DescribeSlot(int32 MaterialID) const;

    // The material picture of the geometry an APPENDING modifier was handed, captured before the
    // op runs so the triangles the op adds are separable from it afterwards.
    //
    // WHY IT EXISTS. `sweep` and `extrude_along_spline` write into the part mesh through an engine
    // call whose FGeometryScriptPrimitiveOptions::MaterialID is 0, and 0 is not a neutral default:
    // the slot table is model-wide and allocated in first-use order, so 0 is whichever slot the
    // FIRST part in the document tagged. A rod swept in part `b` therefore shipped rendering in
    // part `a`'s material - green compile, unchanged slot count, no diagnostic anywhere, and the
    // one part in two materials was visible only in a render of an asset whose two slots differ.
    //
    // The fix is NOT a blanket ClearMaterialIDs over the part mesh: that would recolour the
    // geometry the op was HANDED, which the author already tagged. Only the triangles the op ADDED
    // are retagged, and they are found by id difference rather than as "everything past the old
    // MaxTriangleID" - no Append* call promises to append contiguously, which is the same reason
    // RunGenerator builds into a scratch mesh instead of tagging a triangle range after the fact.
    struct FModifierMaterialSnapshot
    {
        // Triangle ids present before the op, indexed BY id. Deletes leave the id space sparse, so
        // a range cannot express this and a bit per id costs less than a set.
        TBitArray<> TrianglesBefore;

        // Triangle count per distinct material id among the geometry the op was handed. Empty when
        // the op was handed no geometry at all.
        TMap<int32, int32> IncomingByMaterial;

        // The slot MOST of that geometry is on; ties go to the lower id, so the answer is stable
        // across runs and across TMap iteration order. INDEX_NONE when there was nothing to
        // inherit from.
        int32 DominantMaterialID() const;
    };

    void SnapshotModifierMaterial(UDynamicMesh* Mesh, FModifierMaterialSnapshot& Out) const;

    // Gives the triangles Op appended - and only those - the slot they should carry: the one
    // `material=` names, otherwise the one inherited from the geometry the op extends. Raises
    // PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS when neither is available.
    void ApplyModifierMaterialTag(const FPwOp& Op, UDynamicMesh* Mesh,
                                  const FModifierMaterialSnapshot& Before);

    // The material rule in force while a BOOLEAN TOOL BLOCK is being built. Set by RunBoolean
    // around its RunOps call and restored on exit, so a boolean nested inside another boolean's
    // block gets its own; left inactive for a `collision { hull { … } }` body, whose geometry
    // carries no material at all and must not allocate slots.
    //
    // WHY THE BLOCK NEEDS A RULE. Every op inside a boolean block used to skip tagging outright,
    // so its triangles kept material ID 0 - and 0 is not a neutral default but whichever slot the
    // FIRST part in the document tagged. GeometryCore then carried that 0 into the result, which
    // put a `union` block's geometry and every wall a `subtract` opened on another part's
    // material, on a clean compile with nothing said. Tagging the OPERANDS before the operation is
    // what fixes it: the boolean preserves a surviving triangle's id, and both operands index the
    // same model-wide slot table, so there is no second id space to reconcile afterwards.
    struct FBooleanBlockMaterial
    {
        // Whether a boolean tool block is being built. False for a part and for a hull body.
        bool bActive = false;

        // The slot UNTAGGED geometry inside the block takes: the boolean op's own `material=` when
        // it carries one, otherwise the slot most of the geometry the boolean is being applied to
        // is on. INDEX_NONE when the op named nothing and there was nothing to inherit, which is
        // the one case block triangles keep material ID 0 - reported rather than repaired.
        int32 FallbackSlot = INDEX_NONE;

        // Whether any op in the block actually took FallbackSlot. A fallback that had to guess is
        // only worth reporting when something used it: a block whose every op carries its own
        // `material=` has nothing ambiguous about it.
        bool bFallbackUsed = false;
    };
    FBooleanBlockMaterial BooleanBlock;

    // Tags a generator's output inside a boolean tool block. See FBooleanBlockMaterial.
    void TagBooleanBlockGeometry(const FPwOp& Op, UDynamicMesh* Scratch);

    // ---- op execution ----
    bool RunOps(const TArray<FPwOp>& Ops, UDynamicMesh* Mesh, bool bNested);
    bool RunOp(const FPwOp& Op, UDynamicMesh* Mesh, bool bNested);
    bool RunGenerator(const FPwOp& Op, UDynamicMesh* Mesh, bool bNested);
    bool RunBoolean(const FPwOp& Op, UDynamicMesh* Mesh, bool bNested);

    // The format's biggest authoring trap: siblings in a part or a block are APPENDED, never
    // unioned. See the definition for the precision rule and why it is bounds and not solids.
    void WarnOnUnunionedOverlap(const FPwOp& Op, UDynamicMesh* Scratch);

    // One entry per generator already appended into the mesh currently being built.
    struct FAppendedFootprint
    {
        FBox Bounds = FBox(ForceInit);
        FString OpName;
        int32 Line = -1;

        // Whether this op's geometry ENCLOSES A VOLUME - FDynamicMesh3::IsClosed() on the
        // scratch mesh, measured before the append while the op's own geometry is still
        // separable. WarnOnUnunionedOverlap compares it on both operands; see the reasoning
        // there.
        bool bClosed = false;
    };

    // Scoped to ONE mesh by RunOps, which saves and clears it on entry and restores it on exit.
    // A part mesh, a boolean tool mesh and a hull body each get their own list, so a `subtract`
    // whose block holds two overlapping circles is checked exactly like a part that holds two.
    //
    // CLEARED by any modifier or boolean, because both invalidate the recorded boxes: a
    // `translate` moves geometry a footprint still claims to describe, and a boolean rewrites
    // all of it. Losing coverage there is the price of never making a false claim about where a
    // previous op's geometry currently is.
    TArray<FAppendedFootprint> AppendedFootprints;

    // Run immediately before a generator's scratch mesh is appended into Target, so neither
    // side can contribute triangles with no UVs to a channel the other populates. Records
    // what it padded in UVPaddedOnAppend.
    void ReconcileUVChannelsForAppend(UDynamicMesh* Target, UDynamicMesh* Scratch);

    // Fills OutResult for a generator writing into Scratch. Returns false when the op name
    // is not a generator this family knows.
    bool DispatchGenerator(const FPwOp& Op, UDynamicMesh* Scratch, const FTransform& Local,
                           GeometryOps::FOpResult& OutResult);
    // Same shape for the in-place modifiers.
    bool DispatchModifier(const FPwOp& Op, UDynamicMesh* Mesh, GeometryOps::FOpResult& OutResult);

    // Rewrites `from` / `to` / `up` into the op's own at / rotate / extent parameters, so every
    // generator is aimed without any of them knowing aiming exists. See the definition for the
    // extent rules and for the one refusal that has to live here rather than in the parser.
    //
    // Returns false only when an aimed op cannot be honoured, having added the diagnostic.
    // bOutAimed says whether OutOp was populated; when it is false the caller keeps using Op.
    bool ResolveAim(const FPwOp& Op, FPwOp& OutOp, bool& bOutAimed);

    // The whole-model half of the ununioned-overlap check: PARTS whose bounding boxes
    // interpenetrate. Runs once, after every part mesh exists, because that is the only point at
    // which parts are comparable - AppendedFootprints is scoped to one mesh by RunOps and can
    // therefore never see across a part boundary. See the definition for why it emits one
    // diagnostic rather than one per pair.
    void WarnOnInterpenetratingParts(const FPwModelDocument& Document);

    // The one authoring mistake that produces a shell nothing downstream can detect. See the
    // definition for the engine rule, the narrow case it fires on, and what it excludes.
    void WarnOnExtrudeFacingOpposed(const FPwOp& Op, UDynamicMesh* Mesh,
                                    const GeometryOps::FExtrudeParams& Params);

    // The `revolve` twin of the above: the same silent inversion, reached by walking the
    // profile the wrong way round. See the definition for why it measures the produced mesh
    // rather than the point list, and for the two cases it stays quiet on.
    void WarnOnRevolveProfileReversed(const FPwOp& Op, UDynamicMesh* Scratch,
                                      const FTransform& Local);

    GeometryOps::FOpResult ApplyUVOp(const FPwOp& Op, UDynamicMesh* Mesh);

    void ReportOpResult(const FPwOp& Op, const GeometryOps::FOpResult& OpResult,
                        bool bModelLevel = false);

    // A self-intersection is actionable only at the operation that first creates it. The check
    // runs after capable edits to a render part and to boolean tool geometry that can survive
    // into that part. Collision hull construction stays outside this diagnostic contract.
    bool ShouldCheckSelfIntersection(const FPwOp& Op) const;
    void CheckSelfIntersectionAfterOp(const FPwOp& Op, UDynamicMesh* Mesh);
    void RememberBevelOutputGroups(UDynamicMesh* Mesh, int32 FirstNewGroupID);

    // ---- stages ----
    bool RejectReservedConstructs(const FPwModelDocument& Document);
    bool ResolveAndValidateSkin(const FPwModelDocument& Document);
    bool BuildParts(const FPwModelDocument& Document, TArray<TStrongObjectPtr<UDynamicMesh>>& OutPartMeshes);
    bool BindRigidPartWeights(const FPwModelDocument& Document,
                              TArray<TStrongObjectPtr<UDynamicMesh>>& PartMeshes);
    // False when a part could not be given a channel another part populates - the one outcome
    // this stage cannot repair, and the one that would otherwise ship a merged mesh with the
    // channel populated on some triangles and absent on others.
    bool FillMissingUVChannels(const FPwModelDocument& Document,
                               TArray<TStrongObjectPtr<UDynamicMesh>>& PartMeshes);
    void MergeParts(TArray<TStrongObjectPtr<UDynamicMesh>>& PartMeshes, UDynamicMesh* Merged);
    bool ApplyModelUVLayouts(const FPwModelDocument& Document, UDynamicMesh* Merged);
    bool ValidateCrossPartUVChannel(const FPwModelDocument& Document, UDynamicMesh* Merged,
                                    int32 Channel, bool bLightmapChannel);
    bool ValidateCrossPartUVs(const FPwModelDocument& Document, UDynamicMesh* Merged);
    // Rotates the implicit `Default` slot to the end of the table, rewriting the merged mesh's
    // material IDs to match. Runs inside MergeParts, before the padding below it.
    void MoveImplicitDefaultSlotLast(UDynamicMesh* Merged);
    bool ValidateMergedMesh(UDynamicMesh* Merged);
    bool ApplySkinWeights(const FPwModelDocument& Document, UDynamicMesh* Merged);
    bool CheckSkinFreshness(UDynamicMesh* Merged, const TCHAR* StageName);
    bool BuildCollisionGeometry(const FPwModelDocument& Document, UDynamicMesh* Merged,
                                FPwModelCollisionResult& OutCollision);
    void CreateAsset(const FPwModelDocument& Document, UDynamicMesh* Merged,
                     const FPwModelCollisionResult& Collision, FStringView Source);
    // The one check CreateAsset performs that a bValidateOnly run would otherwise never reach.
    // Called only on the validate path; see the definition for why not on both.
    void WarnOnUnloadableMaterialBindings(const FPwModelDocument& Document);

    // The phantom slot an untagged generator inserts into a table the author wrote out in full.
    // Runs once, after the merge, because the index it took is the fact worth reporting and only
    // the finished table carries it. See the definition for the four conditions.
    void WarnOnImplicitDefaultSlot(const FPwModelDocument& Document);

    const FPwModelCompileOptions& Options;
    FPwModelCompileResult& Result;

    // Redirected while a hull body runs, so the collision builder's own diagnostic is the
    // one the caller sees rather than a duplicate pair.
    TArray<FPwDiagnostic>* Sink;

    // Model-wide, first-use order; the index IS the material ID written onto triangles. The one
    // departure is the implicit `Default`, rotated to the end after the merge by
    // MoveImplicitDefaultSlotLast, which rewrites the merged mesh's IDs to match.
    TArray<FString> SlotNames;

    // The generator that forced the implicit `Default` slot into existence - the FIRST untagged
    // one, since that is the op whose position an author has to change to remove the slot.
    // Recorded at the allocation site and read once after the merge; see
    // FCompiler::WarnOnImplicitDefaultSlot.
    struct FImplicitDefaultSlotOrigin
    {
        bool bAllocated = false;
        int32 Line = 0;
        int32 Column = 0;
        FString OpName;
        FString PartName;
    };
    FImplicitDefaultSlotOrigin ImplicitDefaultSlot;

    // Set when a part-level op writes `material="Default"` itself. That is a slot the author
    // NAMED, so it keeps its first-use index like any other name even if an untagged sibling
    // happened to open it first; only a slot nobody ever named is moved to the end.
    bool bDefaultSlotExplicitlyTagged = false;

    // One mapping per source part, in document order. The direct append below keeps this
    // authoritative vertex correspondence for the later skin stages; deriving ranges from
    // MaxVertexID would assume compact meshes and make a future non-compact append silently bind
    // the wrong vertices.
    TArray<UE::Geometry::FMeshIndexMappings> PartIndexMappings;
    TMap<int32, int32> MergedTriangleToPart;
    TArray<bool> AllowFloatingParts;

    USkeleton* Skeleton = nullptr;
    FString SkeletonPath;
    bool bSkeletalOutput = false;
    int32 SkinVertexCountAfterSolve = -1;
    FString SkinSolveStageName;

    // "<part>/channel N" for every channel ReconcileUVChannelsForAppend box-projected while a
    // part was being built. Accumulated rather than reported on the spot so the model still
    // emits exactly ONE PWMODEL_UV_CHANNEL_FILLED warning: FillMissingUVChannels folds this in.
    TArray<FString> UVPaddedOnAppend;

    // Group IDs allocated by a previous bevel in the mesh currently being built. RunOps scopes
    // this independently for a part, each boolean tool, and each collision hull; only render
    // parts and boolean tools consume it for safety diagnostics.
    TSet<int32> PriorBevelGroupIDs;

    // Parts are checked independently, so each part's first introducing op gets an attributed
    // diagnostic. ValidateMergedMesh keeps its model-wide line:-1 warning only as a fallback for
    // geometry that could not be attributed.
    TSet<FString> SelfIntersectionDiagnosticParts;
    bool bSelfIntersectionDiagnosticAttributed = false;

    FString CurrentPartName;
};

void FCompiler::AddDiagnostic(EPwSeverity Severity, const TCHAR* Code, int32 Line, int32 Column,
                              FString Message, const FString& PartName,
                              TArray<FString> Suggestions, const TCHAR* ScopeLabel)
{
    FPwDiagnostic Diagnostic = (Severity == EPwSeverity::Warning)
        ? FPwDiagnostic::MakeWarning(Code, Line, Column, MoveTemp(Message))
        : FPwDiagnostic::MakeError(Code, Line, Column, MoveTemp(Message));
    Diagnostic.ScopeLabel = ScopeLabel;
    Diagnostic.ScopeName = PartName;
    Diagnostic.Suggestions = MoveTemp(Suggestions);
    Sink->Add(MoveTemp(Diagnostic));
}

void FCompiler::Error(const TCHAR* Code, const FPwOp& Op, FString Message)
{
    AddDiagnostic(EPwSeverity::Error, Code, Op.Line, Op.Column, MoveTemp(Message), CurrentPartName);
}

void FCompiler::Warn(const TCHAR* Code, const FPwOp& Op, FString Message)
{
    AddDiagnostic(EPwSeverity::Warning, Code, Op.Line, Op.Column, MoveTemp(Message), CurrentPartName);
}

void FCompiler::ModelError(const TCHAR* Code, const FPwOp& Op, FString Message)
{
    AddDiagnostic(EPwSeverity::Error, Code, Op.Line, Op.Column, MoveTemp(Message), FString(),
        TArray<FString>(), TEXT("model"));
}

void FCompiler::ModelWarn(const TCHAR* Code, const FPwOp& Op, FString Message)
{
    AddDiagnostic(EPwSeverity::Warning, Code, Op.Line, Op.Column, MoveTemp(Message), FString(),
        TArray<FString>(), TEXT("model"));
}

int32 FCompiler::ResolveSlot(const FString& SlotName)
{
    const int32 Existing = SlotNames.IndexOfByKey(SlotName);
    if (Existing != INDEX_NONE)
    {
        return Existing;
    }
    return SlotNames.Add(SlotName);
}

FString FCompiler::DescribeSlot(int32 MaterialID) const
{
    return SlotNames.IsValidIndex(MaterialID)
        ? FString::Printf(TEXT("'%s' (%d)"), *SlotNames[MaterialID], MaterialID)
        : FString::Printf(TEXT("id %d"), MaterialID);
}

int32 FCompiler::FModifierMaterialSnapshot::DominantMaterialID() const
{
    // Strictly greater, then the lower id on a tie - the rule lives once, in DominantMaterialSlot,
    // shared with the boolean and bevel paths. TMap iteration order is not insertion order, so
    // without the tie-break the same document could pick either of two equal slots.
    return DominantMaterialSlot(IncomingByMaterial);
}

void FCompiler::SnapshotModifierMaterial(UDynamicMesh* Mesh, FModifierMaterialSnapshot& Out) const
{
    if (!Mesh)
    {
        return;
    }

    // Two passes over the same triangles rather than one, so the material half is the shared
    // CountMaterialTriangles the boolean and bevel paths call and cannot answer differently.
    CountMaterialTriangles(Mesh, Out.IncomingByMaterial);

    Mesh->ProcessMesh([&Out](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        Out.TrianglesBefore.Init(false, ReadMesh.MaxTriangleID());

        for (const int32 TriangleID : ReadMesh.TriangleIndicesItr())
        {
            Out.TrianglesBefore[TriangleID] = true;
        }
    });
}

void FCompiler::ApplyModifierMaterialTag(const FPwOp& Op, UDynamicMesh* Mesh,
                                         const FModifierMaterialSnapshot& Before)
{
    if (!Mesh)
    {
        return;
    }

    // The triangles this op added. An op that appended nothing - a deformer routed here by a
    // future op-table flag, or a sweep the engine declined without failing - has nothing to tag
    // and nothing to be ambiguous about, so it must not warn either.
    TArray<int32> AppendedTriangles;
    Mesh->ProcessMesh([&Before, &AppendedTriangles](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (const int32 TriangleID : ReadMesh.TriangleIndicesItr())
        {
            if (!Before.TrianglesBefore.IsValidIndex(TriangleID) || !Before.TrianglesBefore[TriangleID])
            {
                AppendedTriangles.Add(TriangleID);
            }
        }
    });

    if (AppendedTriangles.Num() == 0)
    {
        return;
    }

    const bool bTagged = HasValue(Op.Params, TEXT("material"))
        && !GetString(Op.Params, TEXT("material")).IsEmpty();

    int32 TargetID = 0;
    if (bTagged)
    {
        const FString SlotName = GetString(Op.Params, TEXT("material"));
        if (SlotName == DefaultSlotName)
        {
            // A name the author chose, exactly as on a generator: it keeps its first-use index
            // rather than being rotated to the end with the implicit slot.
            bDefaultSlotExplicitlyTagged = true;
        }
        TargetID = ResolveSlot(SlotName);
    }
    else if (Before.IncomingByMaterial.Num() == 1)
    {
        // The ordinary case, and the one this whole path exists to make ordinary: the geometry the
        // op extends speaks with one voice, so the swept surface joins it. No slot is allocated -
        // the id already exists in the table - so the slot count is unchanged, which is what makes
        // this safe to do silently.
        TargetID = Before.DominantMaterialID();
    }
    else if (Before.IncomingByMaterial.Num() > 1)
    {
        TargetID = Before.DominantMaterialID();

        TArray<int32> Candidates;
        Before.IncomingByMaterial.GetKeys(Candidates);
        Candidates.Sort();

        // Named by slot rather than by raw index: the index is what a referencer binds by, the
        // name is what the author wrote. See FCompiler::DescribeSlot.
        TArray<FString> Named;
        Named.Reserve(Candidates.Num());
        for (const int32 Candidate : Candidates)
        {
            Named.Add(DescribeSlot(Candidate));
        }
        const FString ChosenSlot = DescribeSlot(TargetID);

        Warn(PwModelDiagnosticCodes::PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS, Op,
            FString::Printf(
                TEXT("'%s' appended %d triangle(s) onto geometry carrying %d different material ")
                TEXT("slots (%s), so there is no single slot to inherit. The new geometry was put ")
                TEXT("on %s, the slot most of the geometry it extends is on. Write ")
                TEXT("material=\"<Slot>\" on the op to say which slot the appended surface ")
                TEXT("belongs to. Nothing downstream can catch this: the op allocates no slot, so ")
                TEXT("the slot count is unchanged and the mesh is valid - the only place the ")
                TEXT("choice shows is which section the new faces render in."),
                *Op.OpName, AppendedTriangles.Num(), Candidates.Num(),
                *FString::Join(Named, TEXT(", ")), *ChosenSlot));
    }
    else if (BooleanBlock.bActive && BooleanBlock.FallbackSlot != INDEX_NONE)
    {
        // Nothing to inherit LOCALLY, but this op is building a boolean's tool mesh and the
        // boolean already resolved what its untagged geometry should be - its own `material=`, or
        // the slot of the geometry it is being applied to. A sweep that opens a block therefore
        // joins the same slot as a generator that opens one, rather than warning about a mesh it
        // was never going to inherit from.
        TargetID = BooleanBlock.FallbackSlot;
        BooleanBlock.bFallbackUsed = true;
    }
    else
    {
        // Nothing to inherit from: the op was handed an empty mesh, which a `procedural_mesh` that
        // appends nothing in front of it produces. The triangles keep the engine's ID 0, and that
        // is precisely the cross-part accident - 0 is the first-use index of another part's slot -
        // so it is reported rather than left to a render to find.
        Warn(PwModelDiagnosticCodes::PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS, Op,
            FString::Printf(
                TEXT("'%s' appended %d triangle(s) with no geometry in front of it to inherit a ")
                TEXT("material slot from, so they keep material ID 0. That is not a neutral ")
                TEXT("default: slots are a MODEL-WIDE table in first-use order, so ID 0 is ")
                TEXT("whichever slot the first part in the document tagged and this geometry ")
                TEXT("ships in another part's material. Write material=\"<Slot>\" on the op."),
                *Op.OpName, AppendedTriangles.Num()));
        return;
    }

    if (TargetID == 0 && Before.IncomingByMaterial.Num() == 1 && !bTagged)
    {
        // Inheriting ID 0 is a no-op write; skip the edit rather than enabling a material
        // attribute on a mesh that has none.
        return;
    }

    Mesh->EditMesh([&AppendedTriangles, TargetID](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        if (!EditMesh.HasAttributes())
        {
            EditMesh.EnableAttributes();
        }
        if (!EditMesh.Attributes()->HasMaterialID())
        {
            EditMesh.Attributes()->EnableMaterialID();
        }
        UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = EditMesh.Attributes()->GetMaterialID();
        for (const int32 TriangleID : AppendedTriangles)
        {
            MaterialIDs->SetValue(TriangleID, TargetID);
        }
    },
    // The same three arguments the engine's own material edits pass
    // (MeshMaterialFunctions.cpp SimpleMeshMaterialEdit): there is no attribute-change flag for
    // material IDs, so a GeneralEdit with Unknown is what every writer of this attribute uses.
    EDynamicMeshChangeType::GeneralEdit,
    EDynamicMeshAttributeChangeFlags::Unknown,
    /*bDeferChangeEvents=*/false);
}

void FCompiler::TagBooleanBlockGeometry(const FPwOp& Op, UDynamicMesh* Scratch)
{
    // append_buffers writes raw ids of its own for machine-generated input; the part-level path
    // leaves those alone and so does this one. The parser refuses `material=` alongside them
    // (PWMODEL_MATERIAL_ID_CONFLICT), so this cannot silently discard a named slot.
    if (Op.OpName == TEXT("append_buffers") && HasValue(Op.Params, TEXT("material_id")))
    {
        return;
    }

    const FString SlotName = HasValue(Op.Params, TEXT("material"))
        ? GetString(Op.Params, TEXT("material"))
        : FString();

    if (!SlotName.IsEmpty())
    {
        // The SAME model-wide table the part level resolves against, which is the whole of the
        // id-space reconciliation a boolean needs: both operands come out of this indexing one
        // table, so the operation's own attribute transfer carries the ids across correctly and
        // nothing has to be remapped after it.
        if (SlotName == DefaultSlotName)
        {
            // A name the author chose keeps its first-use index rather than being rotated to the
            // end with the implicit slot - exactly as at part level.
            bDefaultSlotExplicitlyTagged = true;
        }
        UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs(
            Scratch, ResolveSlot(SlotName), nullptr);
        return;
    }

    // Untagged. Deliberately NOT the implicit `Default` slot the part level would open: a block's
    // geometry is a tool, not a part, and opening a model-wide slot for it would put a section in
    // the asset for something that may not survive the operation. It takes the boolean's fallback
    // instead - which is either what the op's own `material=` named, or the slot of the geometry
    // the boolean is being applied to.
    if (BooleanBlock.FallbackSlot != INDEX_NONE)
    {
        UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs(
            Scratch, BooleanBlock.FallbackSlot, nullptr);
        BooleanBlock.bFallbackUsed = true;
    }
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

bool FCompiler::DispatchGenerator(const FPwOp& Op, UDynamicMesh* Scratch, const FTransform& Local,
                                  GeometryOps::FOpResult& OutResult)
{
    const TMap<FString, FPwValue>& P = Op.Params;
    const FString& Name = Op.OpName;

    if (Name == TEXT("box"))
    {
        GeometryOps::FBoxParams Params;
        Params.Size = GetVector3(P, TEXT("size"), Params.Size);
        const FVector Segments = GetVector3(P, TEXT("segments"),
            FVector(Params.Steps.X, Params.Steps.Y, Params.Steps.Z));
        Params.Steps = FIntVector(
            FMath::RoundToInt(Segments.X), FMath::RoundToInt(Segments.Y), FMath::RoundToInt(Segments.Z));
        OutResult = GeometryOps::GenerateBox(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("sphere"))
    {
        GeometryOps::FSphereParams Params;
        Params.Radius = GetNumber(P, TEXT("radius"), Params.Radius);
        Params.Subdivisions = GetInt(P, TEXT("subdivisions"), Params.Subdivisions);
        OutResult = GeometryOps::GenerateSphere(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("cylinder"))
    {
        GeometryOps::FCylinderParams Params;
        Params.Radius = GetNumber(P, TEXT("radius"), Params.Radius);
        Params.Height = GetNumber(P, TEXT("height"), Params.Height);
        Params.RadialSteps = GetInt(P, TEXT("segments"), Params.RadialSteps);
        Params.HeightSteps = GetInt(P, TEXT("height_steps"), Params.HeightSteps);
        OutResult = GeometryOps::GenerateCylinder(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("cone"))
    {
        GeometryOps::FConeParams Params;
        Params.BaseRadius = GetNumber(P, TEXT("base_radius"), Params.BaseRadius);
        Params.TopRadius = GetNumber(P, TEXT("top_radius"), Params.TopRadius);
        Params.Height = GetNumber(P, TEXT("height"), Params.Height);
        Params.RadialSteps = GetInt(P, TEXT("segments"), Params.RadialSteps);
        Params.HeightSteps = GetInt(P, TEXT("height_steps"), Params.HeightSteps);
        OutResult = GeometryOps::GenerateCone(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("capsule"))
    {
        GeometryOps::FCapsuleParams Params;
        Params.Radius = GetNumber(P, TEXT("radius"), Params.Radius);
        Params.Length = GetNumber(P, TEXT("length"), Params.Length);
        Params.HemisphereSteps = GetInt(P, TEXT("hemisphere_steps"), Params.HemisphereSteps);
        Params.RadialSteps = GetInt(P, TEXT("segments"), Params.RadialSteps);
        OutResult = GeometryOps::GenerateCapsule(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("torus"))
    {
        GeometryOps::FTorusParams Params;
        Params.MajorRadius = GetNumber(P, TEXT("major_radius"), Params.MajorRadius);
        Params.MinorRadius = GetNumber(P, TEXT("minor_radius"), Params.MinorRadius);
        Params.MajorSteps = GetInt(P, TEXT("major_segments"), Params.MajorSteps);
        Params.MinorSteps = GetInt(P, TEXT("minor_segments"), Params.MinorSteps);
        OutResult = GeometryOps::GenerateTorus(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("plane"))
    {
        GeometryOps::FPlaneParams Params;
        Params.Size = GetVector2(P, TEXT("size"), Params.Size);
        const FVector2D Steps = GetVector2(P, TEXT("subdivisions"),
            FVector2D(Params.Steps.X, Params.Steps.Y));
        Params.Steps = FIntPoint(FMath::RoundToInt(Steps.X), FMath::RoundToInt(Steps.Y));
        OutResult = GeometryOps::GeneratePlane(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("disc"))
    {
        GeometryOps::FDiscParams Params;
        Params.Radius = GetNumber(P, TEXT("radius"), Params.Radius);
        Params.AngleSteps = GetInt(P, TEXT("segments"), Params.AngleSteps);
        OutResult = GeometryOps::GenerateDisc(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("stairs"))
    {
        // step_size is (width along X, rise along Z, depth along Y) - the axis triple the
        // op table documents, not an XYZ extent.
        GeometryOps::FStairsParams Params;
        const FVector StepSize = GetVector3(P, TEXT("step_size"),
            FVector(Params.StepWidth, Params.StepHeight, Params.StepDepth));
        Params.StepWidth = static_cast<float>(StepSize.X);
        Params.StepHeight = static_cast<float>(StepSize.Y);
        Params.StepDepth = static_cast<float>(StepSize.Z);
        Params.NumSteps = GetInt(P, TEXT("num_steps"), Params.NumSteps);
        Params.bFloating = GetBool(P, TEXT("floating"), Params.bFloating);
        OutResult = GeometryOps::GenerateStairs(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("spiral_stairs"))
    {
        GeometryOps::FSpiralStairsParams Params;
        Params.StepWidth = static_cast<float>(GetNumber(P, TEXT("step_width"), Params.StepWidth));
        Params.StepHeight = static_cast<float>(GetNumber(P, TEXT("step_height"), Params.StepHeight));
        Params.InnerRadius = static_cast<float>(GetNumber(P, TEXT("inner_radius"), Params.InnerRadius));
        Params.CurveAngle = static_cast<float>(GetNumber(P, TEXT("curve_angle"), Params.CurveAngle));
        Params.NumSteps = GetInt(P, TEXT("num_steps"), Params.NumSteps);
        Params.bFloating = GetBool(P, TEXT("floating"), Params.bFloating);
        OutResult = GeometryOps::GenerateSpiralStairs(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("ring"))
    {
        GeometryOps::FRingParams Params;
        Params.OuterRadius = GetNumber(P, TEXT("outer_radius"), Params.OuterRadius);
        Params.InnerRadius = GetNumber(P, TEXT("inner_radius"), Params.InnerRadius);
        Params.AngleSteps = GetInt(P, TEXT("segments"), Params.AngleSteps);
        OutResult = GeometryOps::GenerateRing(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("arch"))
    {
        GeometryOps::FArchParams Params;
        Params.MajorRadius = GetNumber(P, TEXT("major_radius"), Params.MajorRadius);
        Params.MinorRadius = GetNumber(P, TEXT("minor_radius"), Params.MinorRadius);
        Params.Angle = GetNumber(P, TEXT("angle"), Params.Angle);
        Params.MajorSteps = GetInt(P, TEXT("major_steps"), Params.MajorSteps);
        Params.MinorSteps = GetInt(P, TEXT("minor_steps"), Params.MinorSteps);
        OutResult = GeometryOps::GenerateArch(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("pipe"))
    {
        GeometryOps::FPipeParams Params;
        Params.OuterRadius = GetNumber(P, TEXT("outer_radius"), Params.OuterRadius);
        Params.InnerRadius = GetNumber(P, TEXT("inner_radius"), Params.InnerRadius);
        Params.Height = GetNumber(P, TEXT("height"), Params.Height);
        Params.RadialSteps = GetInt(P, TEXT("radial_steps"), Params.RadialSteps);
        Params.HeightSteps = GetInt(P, TEXT("height_steps"), Params.HeightSteps);
        OutResult = GeometryOps::GeneratePipe(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("ramp"))
    {
        GeometryOps::FRampParams Params;
        const FVector Size = GetVector3(P, TEXT("size"),
            FVector(Params.Width, Params.Length, Params.Height));
        Params.Width = Size.X;
        Params.Length = Size.Y;
        Params.Height = Size.Z;
        OutResult = GeometryOps::GenerateRamp(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("revolve"))
    {
        GeometryOps::FRevolveParams Params;
        Params.Profile = GetPointList2(P, TEXT("profile"));
        Params.Angle = GetNumber(P, TEXT("angle"), Params.Angle);
        Params.Steps = GetInt(P, TEXT("steps"), Params.Steps);
        Params.bCapped = GetBool(P, TEXT("capped"), Params.bCapped);
        OutResult = GeometryOps::GenerateRevolve(Scratch, Params, Local);
        // AFTER the op, unlike the extrude twin: the fact this check reads - the sign of the
        // volume the sweep encloses - exists only on the finished mesh.
        if (OutResult.bSuccess)
        {
            WarnOnRevolveProfileReversed(Op, Scratch, Local);
        }
        return true;
    }
    if (Name == TEXT("procedural_mesh"))
    {
        GeometryOps::FEmptyMeshParams Params;
        OutResult = GeometryOps::GenerateEmpty(Scratch, Params, Local);
        return true;
    }
    if (Name == TEXT("append_triangle"))
    {
        GeometryOps::FAppendTriangleParams Params;
        Params.V0 = GetVector3(P, TEXT("v0"), Params.V0);
        Params.V1 = GetVector3(P, TEXT("v1"), Params.V1);
        Params.V2 = GetVector3(P, TEXT("v2"), Params.V2);
        Params.GroupID = GetInt(P, TEXT("group_id"), Params.GroupID);
        GeometryOps::FAppendTriangleIndices Indices;
        OutResult = GeometryOps::AppendTriangle(Scratch, Params, Indices);
        return true;
    }
    if (Name == TEXT("append_buffers"))
    {
        GeometryOps::FAppendBuffersParams Params;
        Params.Vertices = GetPointList3(P, TEXT("vertices"));
        for (const FVector& Triangle : GetPointList3(P, TEXT("triangles")))
        {
            Params.Triangles.Add(FIntVector(
                FMath::RoundToInt(Triangle.X), FMath::RoundToInt(Triangle.Y), FMath::RoundToInt(Triangle.Z)));
        }
        Params.Normals = GetPointList3(P, TEXT("normals"));
        Params.UVs = GetPointList2(P, TEXT("uvs"));
        Params.Colors = GetPointList4(P, TEXT("colors"));
        Params.GroupId = GetInt(P, TEXT("group_id"), Params.GroupId);
        Params.MaterialId = GetInt(P, TEXT("material_id"), Params.MaterialId);

        OutResult = GeometryOps::ValidateAppendBuffers(Params);
        if (OutResult.bSuccess)
        {
            GeometryOps::FAppendBuffersOutputs Outputs;
            OutResult = GeometryOps::AppendBuffers(Scratch, MoveTemp(Params), Outputs);
        }
        return true;
    }

    return false;
}

// Keeps the invariant everything downstream depends on: at every append boundary a UV channel is
// either wholly set or wholly unset on the mesh that owns it. Without this a part that mixes a
// UV-carrying generator with a UV-less one ends PARTIALLY set - elements present, some triangles
// still at -1 - and that is the state the engine's boundary stitcher reads as Elements[-2]
// (GeometryUtils::MeshHasUVsOnEveryTriangle spells the chain out) while the rest of the pipeline
// sees a populated channel and ships the untextured triangles without a word.
//
// GeometryOps::AppendBuffers already pads incoming buffers to preserve the TARGET's UV layers,
// and that guard CANNOT fire from here: the compiler runs every generator into a FRESH SCRATCH
// mesh, so the target it inspects is empty and GeometryOpsAdvanced_UVLayersWorthPreserving
// answers 0. The scratch indirection is what routes around it, so the repair belongs at the
// scratch boundary rather than in the op.
//
// FDynamicMeshEditor::AppendMesh copies min(target, source) UV layers (DynamicMeshEditor.cpp:2002
// in UE 5.8) and sets a triangle only when the SOURCE had it set (AppendUVs, :2256). Note the
// geometry-script wrapper first runs EnableMatchingAttributes(Source, false, false), which grows
// the target's layer COUNT to max(target, source) and clears elements only in the layers it just
// added (DynamicMeshAttributeSet.cpp:477-482) - so growth alone never fills a triangle. Both
// asymmetries therefore leak, and both are repaired here:
//
//   target has elements in channel C, scratch does not -> the APPENDED triangles land unset
//   scratch has elements in channel C, target does not -> the ALREADY-BUILT triangles stay unset
//
// The element-presence test is deliberately the O(1) one and not the per-triangle sweep. It runs
// once per generator op, and while this invariant holds "has elements" and "has elements on every
// triangle" are the same answer. A part whose invariant an IN-PLACE modifier already broke
// (bridge, edge_split, and any future op that calls FDynamicMesh3::AppendTriangle directly) is
// caught at the merge by FillMissingUVChannels, which does sweep.
//
// Box projection, not the placeholder (0,0) GeometryOps::AppendBuffers pads with. The two
// disagree on purpose: that one is a published RPC where inventing UVs would make append_buffers
// silently contradict `uvs=`, whereas the compiler already owns a box-projection rescue for this
// exact condition one stage later, and (0,0) on all three corners is a degenerate zero-area UV
// triangle that MikkT can only inherit a neighbour's tangent frame for.
//
// procedural_mesh + append_buffers with no uvs= is untouched by construction: neither side has UV
// elements, so no channel matches, and the wholly-UV-less shape the shell and bevel crash guards
// in GeometryOps_Modeling.cpp are written and tested against still reaches them.
void FCompiler::ReconcileUVChannelsForAppend(UDynamicMesh* Target, UDynamicMesh* Scratch)
{
    const int32 NumChannels = FMath::Max(MeshNumUVLayers(Target), MeshNumUVLayers(Scratch));
    const bool bTargetHasGeometry = Target->GetTriangleCount() > 0;

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        const bool bTargetHas = MeshHasUVElements(Target, Channel);
        const bool bScratchHas = MeshHasUVElements(Scratch, Channel);
        if (bTargetHas == bScratchHas)
        {
            continue;
        }

        UDynamicMesh* const Missing = bTargetHas ? Scratch : Target;

        // The first generator in a part: the target is empty, so it contributes no triangles to
        // leave unset and AppendMesh's own EnableMatchingAttributes gives it the layer.
        if (Missing == Target && !bTargetHasGeometry)
        {
            continue;
        }

        if (!GeometryUtils::EnsureMeshHasUVChannel(Missing, Channel))
        {
            // Only a channel above 7 reaches this, and FillMissingUVChannels reports that per
            // part as an error with the part named. Adding a second diagnostic from inside a
            // generator op would double-report the same unrepairable condition.
            continue;
        }
        BoxProjectChannel(Missing, Channel);

        UVPaddedOnAppend.AddUnique(FString::Printf(TEXT("%s/channel %d"),
            CurrentPartName.IsEmpty() ? TEXT("(unnamed part)") : *CurrentPartName, Channel));
    }
}

// Two ops in one part, and the second one lands inside the first: warn, and name the fix.
//
// SIBLINGS ARE APPENDED, NOT UNIONED. Nothing in a part or a nested block merges two shapes -
// RunGenerator ends in AppendMesh, and mirror / array_linear / array_radial / array_along_path
// append their copies the same way. Two overlapping shapes therefore leave BURIED INTERIOR
// FACES, and hand the next boolean a self-intersecting mesh. It is what left a white
// cross-shaped shard standing inside the gothic window's trefoil: three overlapping circles
// appended, so the lobe partition walls survived inside the opening.
//
// The append is NOT changed to a union, here or anywhere. Disjoint parts and deliberate open
// shells are the majority of appends and are correct; unioning them would alter the geometry of
// every existing document and cost a boolean per op. The author gets a warning and an explicit
// `union { }` instead.
//
// ---- Precision, and why it is this and not tighter or looser -------------------------------
//
// The test is AXIS-ALIGNED BOUNDS INTERSECTION with a POSITIVE-THICKNESS REQUIREMENT ON ALL
// THREE AXES. Both halves are deliberate.
//
// Not tighter: an exact solid-solid intersection test IS a boolean. Running one to decide
// whether to recommend one would put a full boolean on every generator in every document and
// then throw the result away - the check would cost more than the fix it suggests.
//
// Not a plain FBox::Intersect: that fires on FLUSH-ADJACENT geometry, which is the single most
// common legitimate multi-generator pattern in the format - a column on a base, a floor meeting
// a wall, two boxes sharing a face. Those produce a zero-thickness intersection, and a warning
// that fires on every adjacent pair is worse than no warning at all, because it trains the
// author to ignore the one that matters. Requiring more than OverlapEpsilon of overlap on every
// axis removes that whole class and keeps every real interpenetration: two shapes an author
// meant to merge overlap by a modelling dimension, never by a float residue.
//
// KNOWN RESIDUAL FALSE POSITIVE, and the reason this is a warning that can never fail a
// compile: boxes can interpenetrate while the solids inside them do not. Petals arranged around
// a ring, an L of two boxes about a corner, and a diagonal strut beside a block all trip it. The
// message says so, so an author who knows the shapes are disjoint can move on.
//
// ---- BOTH OPERANDS MUST ENCLOSE A VOLUME ---------------------------------------------------
//
// A mesh with boundary edges bounds no interior, so there is no shared volume to keep buried
// faces in and `union { }` - the remedy this warning exists to name - is not a legal operation
// on it. Every clause of the message is false about such a pair, so the check does not run on
// one: the footprint still gets recorded, with its closedness, and any pair where either side
// is open is skipped.
//
// Measured, and the reason it is this rather than a name test on `append_triangle`: a
// twelve-triangle patch on Examples/pwmodel/mobius_band.pwmodel produced NINE of these
// warnings. Two triangles sharing an edge have interpenetrating boxes always - that is what a
// shared edge means in three dimensions - and the advice offered for it was to union two
// shapes that between them enclose nothing. A name test would have fixed exactly that op and
// left the identical false positive on `plane`, `disc`, an uncapped `revolve` and every
// `procedural_mesh` + `append_buffers` shell.
//
// The coverage this gives up is a warning about two overlapping OPEN shells, which is real
// (they still z-fight) and which this warning was never able to advise on. The positive cases
// are unchanged: every primitive the format calls solid - box, sphere, cylinder, capsule,
// torus, pipe, stairs - is closed, and it was two of those that left the white cross-shaped
// shard inside the gothic window's trefoil.
//
// Compared per PREVIOUS OP rather than against the accumulated mesh's single box: the
// accumulated box is the union of everything, so a large base primitive would make every
// subsequent op overlap and the warning would name nothing useful. Per-op boxes are also what
// lets the message name BOTH ops and both source lines, which is the actionable part.
void FCompiler::WarnOnUnunionedOverlap(const FPwOp& Op, UDynamicMesh* Scratch)
{
    // Well below any authored dimension and well above the residue a generator leaves on a ring
    // it did not align to the plane. Same order as FMirrorParams::WeldTolerance, for the same
    // reason: it is the scale at which 'these were meant to meet' stops being float noise.
    constexpr double OverlapEpsilon = 1e-3;

    const UE::Geometry::FAxisAlignedBox3d ScratchBox = Scratch->GetMeshRef().GetBounds();
    if (!ScratchBox.IsEmpty())
    {
        const FBox NewBounds(FVector(ScratchBox.Min), FVector(ScratchBox.Max));

        // Read once per generator, off the op's own scratch mesh rather than off the
        // accumulated part - the accumulated mesh's closedness answers nothing about THIS op.
        const bool bNewIsClosed = Scratch->GetMeshRef().IsClosed();

        for (const FAppendedFootprint& Previous : AppendedFootprints)
        {
            if (!bNewIsClosed || !Previous.bClosed)
            {
                continue;
            }

            const FVector OverlapMin = FVector::Max(Previous.Bounds.Min, NewBounds.Min);
            const FVector OverlapMax = FVector::Min(Previous.Bounds.Max, NewBounds.Max);
            const FVector Overlap = OverlapMax - OverlapMin;

            if (Overlap.X <= OverlapEpsilon || Overlap.Y <= OverlapEpsilon || Overlap.Z <= OverlapEpsilon)
            {
                continue;
            }

            Warn(PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP, Op,
                FString::Printf(
                    TEXT("'%s' overlaps '%s' (line %d) by %.3g x %.3g x %.3g uu, and siblings in a block are ")
                    TEXT("APPENDED, not unioned - so both surfaces survive and the shared volume keeps its buried ")
                    TEXT("interior faces. Resolve it with 'union { }', which keeps each solid's own material slot ")
                    TEXT("- a generator inside the block carries its own material= through the model-wide table - ")
                    TEXT("or move them apart, or keep the overlap deliberate. ")
                    TEXT("This compares bounding boxes, so it also fires on shapes whose boxes interpenetrate while ")
                    TEXT("the solids do not - petals around a ring, an L about a corner: ignore it in that case."),
                    *Op.OpName, *Previous.OpName, Previous.Line,
                    Overlap.X, Overlap.Y, Overlap.Z));

            // One diagnostic per op, not one per overlapping pair. An op that lands inside four
            // previous ones is one authoring mistake, and four warnings for it would bury the
            // next op's.
            break;
        }

        FAppendedFootprint Footprint;
        Footprint.Bounds = NewBounds;
        Footprint.OpName = Op.OpName;
        Footprint.Line = Op.Line;
        Footprint.bClosed = bNewIsClosed;
        AppendedFootprints.Add(MoveTemp(Footprint));
    }
}

// ---------------------------------------------------------------------------
// Aiming
// ---------------------------------------------------------------------------
//
// Every centre-placed generator in this format is built along its OWN LOCAL +Z, so placing one
// BETWEEN two known points had to be spelled by hand: at=(A+B)/2, a length of |B-A|, and a
// rotation the author derived off-format with two inverse trig calls. A rig of a hundred limbs
// needs a hundred of those, and the arithmetic is where the mistakes went.
//
// `from`/`to` are resolved into exactly those parameters HERE, before anything reads them, and
// the op is then dispatched as though the author had written them. That is deliberate and it is
// the whole design: no generator learns what aiming is, a generator added later inherits it by
// declaring an aim kind in the op table, and the derived values are the same values the format
// already had - so there is one placement model, not two.
//
// Everything an aim can get WRONG is refused by the parser, which can see all of it without a
// mesh (FPwModelParserImpl::ValidateAimParams). This function therefore assumes well-formed
// input and, on anything it still cannot read, declines to aim rather than inventing a frame -
// that path is reachable only from a hand-built AST, which no diagnostic would reach anyway.
//
// The single exception is PWMODEL_AIM_TOO_SHORT, which is raised from here. It compares the
// distance against the capsule's RADIUS, and the radius default lives in GeometryOps'
// FCapsuleParams. The parser would have to re-type that number to check it, and a re-typed
// default is a second source of truth for exactly the kind of value that drifts.
namespace
{
FPwValue MakeAimTuple(const FVector& Value, const FPwOp& Op)
{
    FPwValue Result;
    Result.Type = EPwValueType::Tuple;
    Result.Tuple = { Value.X, Value.Y, Value.Z };
    // Anchored on the op rather than left at zero: these values are synthesised, and a diagnostic
    // raised against one downstream must still name the line the author can edit.
    Result.Line = Op.Line;
    Result.Column = Op.Column;
    return Result;
}

FPwValue MakeAimNumber(double Value, const FPwOp& Op)
{
    FPwValue Result;
    Result.Type = EPwValueType::Number;
    Result.Number = Value;
    Result.Line = Op.Line;
    Result.Column = Op.Column;
    return Result;
}
}

bool FCompiler::ResolveAim(const FPwOp& Op, FPwOp& OutOp, bool& bOutAimed)
{
    bOutAimed = false;

    const FPwValue* From = Op.Params.Find(TEXT("from"));
    const FPwValue* To = Op.Params.Find(TEXT("to"));
    if (!From || !To)
    {
        return true;
    }

    const auto ReadTuple = [](const FPwValue* Value, FVector& OutVector)
    {
        if (!Value || Value->Type != EPwValueType::Tuple || Value->Tuple.Num() < 3)
        {
            return false;
        }
        OutVector = FVector(Value->Tuple[0], Value->Tuple[1], Value->Tuple[2]);
        return true;
    };

    FVector FromPoint;
    FVector ToPoint;
    if (!ReadTuple(From, FromPoint) || !ReadTuple(To, ToPoint))
    {
        return true;
    }

    const FVector Direction = ToPoint - FromPoint;
    if (Direction.IsNearlyZero())
    {
        return true;
    }

    // Two rules, and the difference between them is the reason `up` exists. Without `up` the
    // twist is pinned by the closed form (roll = 0); with it, local +Y is set perpendicular to
    // both `up` and the aim. PwValueRead::MakeAimRotator carries the derivation and states where
    // the two agree - which is everywhere the aim is not vertical.
    FRotator Aim;
    FVector UpVector;
    if (ReadTuple(Op.Params.Find(TEXT("up")), UpVector))
    {
        if (!PwValueRead::MakeAimRotator(Direction, UpVector, Aim))
        {
            return true;
        }
    }
    else
    {
        Aim = PwValueRead::MakeAimRotator(Direction);
    }

    const double Distance = Direction.Size();

    OutOp = Op;

    // `rotate` is (roll, pitch, yaw) in the source format, which is NOT FRotator's own component
    // order; PwValueRead::MakeRotator owns that conversion and this is its inverse, spelled the
    // same way round so the two cannot drift apart.
    OutOp.Params.Add(TEXT("at"), MakeAimTuple((FromPoint + ToPoint) * 0.5, Op));
    OutOp.Params.Add(TEXT("rotate"), MakeAimTuple(FVector(Aim.Roll, Aim.Pitch, Aim.Yaw), Op));

    const FPwModelOpSpec* Spec = PwModelOpTable::Find(Op.OpName, EPwModelOpContext::Part);
    const EPwModelAimExtent AimExtent = Spec ? Spec->AimExtent : EPwModelAimExtent::None;

    switch (AimExtent)
    {
    case EPwModelAimExtent::Scalar:
        OutOp.Params.Add(Spec->AimExtentParam, MakeAimNumber(Distance, Op));
        break;

    case EPwModelAimExtent::SizeZ:
    {
        // X and Y stay the author's; only the third component is the aim's. The defaults come
        // from the generator's own params struct, so an aimed op with no `size=` at all is the
        // same cross-section an unaimed one would have had.
        GeometryOps::FBoxParams Defaults;
        const FVector Size = GetVector3(Op.Params, TEXT("size"), Defaults.Size);
        OutOp.Params.Add(Spec->AimExtentParam, MakeAimTuple(FVector(Size.X, Size.Y, Distance), Op));
        break;
    }

    case EPwModelAimExtent::CapsuleLength:
    {
        // `length` is the CYLINDRICAL SECTION ONLY - a hemisphere of `radius` is added at each
        // end - so an aimed capsule needs the caps subtracted. Handing the raw distance over
        // instead overshoots `to` by exactly one radius at each end, silently, on the primitive
        // an author reaches for most when aiming between two joints.
        GeometryOps::FCapsuleParams Defaults;
        const double Radius = GetNumber(Op.Params, TEXT("radius"), Defaults.Radius);
        const double Length = Distance - 2.0 * Radius;
        if (Length <= 0.0)
        {
            Error(PwModelDiagnosticCodes::PWMODEL_AIM_TOO_SHORT, Op,
                FString::Printf(
                    TEXT("'%s' is aimed across %g uu, but its two hemispherical caps already span 2*radius = %g uu ")
                    TEXT("- so the cylindrical section it would need is %g uu, which is not a capsule. Reduce ")
                    TEXT("radius below %g, move the endpoints further apart, or use 'sphere' if the two points are ")
                    TEXT("this close together."),
                    *Op.OpName, Distance, 2.0 * Radius, Length, Distance * 0.5));
            return false;
        }
        OutOp.Params.Add(Spec->AimExtentParam, MakeAimNumber(Length, Op));
        break;
    }

    case EPwModelAimExtent::None:
    default:
        // Placement and direction only. The parser has already told the author the distance is
        // unused (PWMODEL_AIM_LENGTH_UNUSED), so nothing more is said here.
        break;
    }

    bOutAimed = true;
    return true;
}

bool FCompiler::RunGenerator(const FPwOp& Op, UDynamicMesh* Mesh, bool bNested)
{
    // Generators build into a scratch mesh rather than straight into the part, so the
    // geometry THIS op produced is separable from everything already accumulated. That is
    // what makes material and color tagging exact: the alternative - tagging by triangle
    // range after the fact - assumes the engine appends contiguously at the end, which no
    // Append* call promises.
    TStrongObjectPtr<UDynamicMesh> Scratch(NewTransientMesh());

    // `from`/`to` become at / rotate / an extent here, once, so everything below - the transform
    // read, the dispatch, and every generator behind it - sees one placement model rather than
    // two. AimedOp is a copy and is used only when the op actually carried an aim.
    FPwOp AimedOp;
    bool bAimed = false;
    if (!ResolveAim(Op, AimedOp, bAimed))
    {
        Scratch->MarkAsGarbage();
        return false;
    }
    const FPwOp& Effective = bAimed ? AimedOp : Op;

    const FTransform Local = ReadTransformParams(Effective.Params);

    GeometryOps::FOpResult OpResult;
    if (!DispatchGenerator(Effective, Scratch.Get(), Local, OpResult))
    {
        Error(PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP, Op,
            FString::Printf(TEXT("Op '%s' is in the op table but the compiler has no generator for it."), *Op.OpName));
        Scratch->MarkAsGarbage();
        return false;
    }

    ReportOpResult(Op, OpResult);
    if (!OpResult.bSuccess)
    {
        Scratch->MarkAsGarbage();
        return false;
    }

    // procedural_mesh appends nothing by design. Tagging an empty scratch would allocate a
    // material slot for geometry that does not exist, which is exactly the case the
    // "Default is created only if something is untagged" rule exists to avoid.
    if (Scratch->GetTriangleCount() > 0)
    {
        // Material tags are a MODEL-WIDE slot allocation. Three scopes, three rules:
        //
        //   PART LEVEL (below): `material=` resolves through the table, and an untagged generator
        //     opens the implicit `Default` slot.
        //   A BOOLEAN TOOL BLOCK: `material=` resolves through the SAME table - its geometry does
        //     reach the asset, as the tool surface a union keeps or the walls a subtract opens -
        //     but an untagged op takes the boolean's fallback rather than opening `Default`. See
        //     TagBooleanBlockGeometry, and FBooleanBlockMaterial for why the block needs a rule at
        //     all.
        //   A HULL BODY: no rule. `collision` ops take no `material=` and hull geometry carries
        //     none, so nothing here runs and no slot is allocated.
        //
        // color= is plain vertex data with no such bookkeeping, so it applies everywhere.
        if (!bNested)
        {
            const FString SlotName = HasValue(Op.Params, TEXT("material"))
                ? GetString(Op.Params, TEXT("material"))
                : FString(DefaultSlotName);

            // append_buffers carries an explicit raw material_id for machine-generated
            // input; honour it rather than overwriting it with a slot.
            //
            // This is a tie-break, and it is no longer reachable from a parsed document: an op
            // carrying both material= and material_id= is PWMODEL_MATERIAL_ID_CONFLICT
            // (PwModelParser.cpp ValidateOp) and the compile stops at stage 1. It still decides
            // a hand-built AST, which is why it stays - but it must never again be the thing
            // that makes `material=` on append_buffers do nothing, silently, which is what it
            // was while the parser rejected the parameter outright.
            const bool bExplicitMaterialId =
                Op.OpName == TEXT("append_buffers") && HasValue(Op.Params, TEXT("material_id"));

            if (!bExplicitMaterialId)
            {
                // Implicit means the COMPILER chose the slot: no `material=` at all, or one whose
                // text is empty. An explicit material="Default" is the author's own table entry
                // and is not this case.
                const FString EffectiveSlot = SlotName.IsEmpty() ? FString(DefaultSlotName) : SlotName;
                const bool bImplicitDefault =
                    !HasValue(Op.Params, TEXT("material")) || SlotName.IsEmpty();

                if (bImplicitDefault && !ImplicitDefaultSlot.bAllocated
                    && !SlotNames.Contains(EffectiveSlot))
                {
                    ImplicitDefaultSlot.bAllocated = true;
                    ImplicitDefaultSlot.Line = Op.Line;
                    ImplicitDefaultSlot.Column = Op.Column;
                    ImplicitDefaultSlot.OpName = Op.OpName;
                    ImplicitDefaultSlot.PartName = CurrentPartName;
                }
                else if (!bImplicitDefault && EffectiveSlot == DefaultSlotName)
                {
                    bDefaultSlotExplicitlyTagged = true;
                }

                UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs(
                    Scratch.Get(), ResolveSlot(EffectiveSlot), nullptr);
            }
        }
        else if (BooleanBlock.bActive)
        {
            TagBooleanBlockGeometry(Op, Scratch.Get());
        }

        if (HasValue(Op.Params, TEXT("color")))
        {
            GeometryOps::FSetVertexColorParams ColorParams;
            ColorParams.Color = GetColor(Op.Params, TEXT("color"), FLinearColor::White);
            ColorParams.bSetAll = true;

            int32 VerticesModified = 0;
            const GeometryOps::FOpResult ColorResult =
                GeometryOps::SetVertexColor(Scratch.Get(), ColorParams, VerticesModified);
            ReportOpResult(Op, ColorResult);
            if (!ColorResult.bSuccess)
            {
                Scratch->MarkAsGarbage();
                return false;
            }
        }

        // The scratch-mesh indirection's own hazard, closed here because here is the only
        // place both meshes are in hand. See ReconcileUVChannelsForAppend.
        ReconcileUVChannelsForAppend(Mesh, Scratch.Get());

        // Before the append, because it needs the two meshes separable - which is exactly what
        // the scratch indirection buys and what an after-the-fact bounds read cannot recover.
        WarnOnUnunionedOverlap(Op, Scratch.Get());

        // Identity transform: the op's at/rotate/scale were baked into the vertices by the
        // generator itself. Applying them here as well would double the location and
        // square the scale.
        UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
            Mesh, Scratch.Get(), FTransform::Identity, /*bDeferChangeNotifications=*/false,
            FGeometryScriptAppendMeshOptions(), nullptr);
    }

    Scratch->MarkAsGarbage();
    return true;
}

// ---------------------------------------------------------------------------
// Modifiers
// ---------------------------------------------------------------------------

// The `uv` op's mode vocabulary, READ FROM THE OP TABLE rather than spelled here. The parser
// validates `mode=` against exactly this list, so deriving it is what keeps the diagnostic below
// naming the modes that actually exist - a seventh added to the table and not to the dispatch
// chain now errors by name instead of silently falling through to a box projection.
const TArray<FString>& UVModeNames()
{
    static const TArray<FString> Names = []()
    {
        if (const FPwModelOpSpec* Spec = PwModelOpTable::Find(FString(TEXT("uv")), EPwModelOpContext::Part))
        {
            if (const FPwModelParamSpec* Mode = Spec->FindParam(FString(TEXT("mode"))))
            {
                return Mode->AllowedValues;
            }
        }
        return TArray<FString>();
    }();

    return Names;
}

GeometryOps::FOpResult FCompiler::ApplyUVOp(const FPwOp& Op, UDynamicMesh* Mesh)
{
    const TMap<FString, FPwValue>& P = Op.Params;

    // Defaults come from the params struct rather than being re-typed here: PwModelParser.h
    // documents the op table's copies as display text and the struct as the real source.
    GeometryOps::FProjectUVParams Params;
    Params.UVChannel = GetInt(P, TEXT("channel"), Params.UVChannel);
    Params.Scale = GetVector2(P, TEXT("scale"), Params.Scale);
    Params.SplitAngle = GetNumber(P, TEXT("split_angle"), Params.SplitAngle);

    const FString Mode = GetIdentifier(P, TEXT("mode"), TEXT("box"));

    GeometryOps::FOpResult UVResult;

    if (Mode == TEXT("box") || Mode == TEXT("planar") || Mode == TEXT("cylindrical"))
    {
        // Routed rather than re-implemented. This used to open-code GeometryOps::ProjectUV -
        // including its EnsureMeshHasUVChannel guard and INVALID_ARGUMENT failure - with
        // different wording, so one failure read two ways depending on which front-end reached
        // it, and the two projection frames could drift apart unnoticed.
        Params.Projection = (Mode == TEXT("planar")) ? GeometryOps::EUVProjectionMode::Planar
            : (Mode == TEXT("cylindrical")) ? GeometryOps::EUVProjectionMode::Cylindrical
            : GeometryOps::EUVProjectionMode::Box;

        UVResult = GeometryOps::ProjectUV(Mesh, Params);
    }
    else if (Mode == TEXT("patch_builder"))
    {
        // Routed through the extracted op, like the three projections above. It used to be
        // open-coded here with a DEFAULT-CONSTRUCTED FGeometryScriptPatchBuilderOptions, so all
        // ten of the engine's patch-builder settings were unreachable from any front-end.
        GeometryOps::FPatchBuilderUVParams PatchParams;
        PatchParams.UVChannel = Params.UVChannel;
        PatchParams.InitialPatchCount = GetInt(P, TEXT("initial_patch_count"), PatchParams.InitialPatchCount);
        PatchParams.MinPatchSize = GetInt(P, TEXT("min_patch_size"), PatchParams.MinPatchSize);
        PatchParams.PatchCurvatureAlignmentWeight =
            GetNumber(P, TEXT("patch_curvature_alignment_weight"), PatchParams.PatchCurvatureAlignmentWeight);
        PatchParams.PatchMergingMetricThresh =
            GetNumber(P, TEXT("patch_merging_metric_thresh"), PatchParams.PatchMergingMetricThresh);
        PatchParams.PatchMergingAngleThresh =
            GetNumber(P, TEXT("patch_merging_angle_thresh"), PatchParams.PatchMergingAngleThresh);
        PatchParams.ExpMapNormalSmoothingRounds =
            GetInt(P, TEXT("exp_map_normal_smoothing_rounds"), PatchParams.ExpMapNormalSmoothingRounds);
        PatchParams.ExpMapNormalSmoothingAlpha =
            GetNumber(P, TEXT("exp_map_normal_smoothing_alpha"), PatchParams.ExpMapNormalSmoothingAlpha);
        PatchParams.bRespectInputGroups = GetBool(P, TEXT("respect_input_groups"), PatchParams.bRespectInputGroups);
        PatchParams.bAutoPack = GetBool(P, TEXT("auto_pack"), PatchParams.bAutoPack);
        PatchParams.PackingTargetImageWidth =
            GetInt(P, TEXT("packing_target_image_width"), PatchParams.PackingTargetImageWidth);
        PatchParams.bPackingOptimizeIslandRotation =
            GetBool(P, TEXT("packing_optimize_island_rotation"), PatchParams.bPackingOptimizeIslandRotation);

        UVResult = GeometryOps::AutoUVPatchBuilder(Mesh, PatchParams);
    }
    else if (Mode == TEXT("layout"))
    {
        // Also routed now. This used to set TextureResolution and nothing else, leaving the other
        // seven FGeometryScriptLayoutUVsOptions fields - including LayoutType, which decides
        // whether the op repacks, stacks or normalizes - unreachable.
        GeometryOps::FLayoutUVParams LayoutParams;
        LayoutParams.UVChannel = Params.UVChannel;
        LayoutParams.LayoutType = ReadUVLayoutType(P, LayoutParams.LayoutType);
        LayoutParams.TextureResolution = GetInt(P, TEXT("texture_resolution"), LayoutParams.TextureResolution);
        LayoutParams.Scale = GetNumber(P, TEXT("layout_scale"), LayoutParams.Scale);
        LayoutParams.Translation = GetVector2(P, TEXT("translation"), LayoutParams.Translation);
        LayoutParams.bPreserveScale = GetBool(P, TEXT("preserve_scale"), LayoutParams.bPreserveScale);
        LayoutParams.bPreserveRotation = GetBool(P, TEXT("preserve_rotation"), LayoutParams.bPreserveRotation);
        LayoutParams.bAllowFlips = GetBool(P, TEXT("allow_flips"), LayoutParams.bAllowFlips);
        LayoutParams.bEnableUDIMLayout = GetBool(P, TEXT("enable_udim_layout"), LayoutParams.bEnableUDIMLayout);

        UVResult = GeometryOps::LayoutUV(Mesh, LayoutParams);
    }
    else if (Mode == TEXT("xatlas"))
    {
        // The one mode still open-coded: GeometryOps::UnwrapUVXAtlas takes a bare channel and
        // hard-codes FGeometryScriptXAtlasOptions(), so routing through it would silently drop
        // the published `max_iterations`. Behind ProjectUV's guard verbatim, so the one failure
        // has exactly one text no matter which mode or which front-end produced it.
        if (!GeometryUtils::EnsureMeshHasUVChannel(Mesh, Params.UVChannel))
        {
            return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("uvChannel %d is out of range: the mesh supports at most 8 UV channels (0-7), so the UV layer could not be created"),
                    Params.UVChannel));
        }

        FGeometryScriptXAtlasOptions XAtlasOptions;
        XAtlasOptions.MaxIterations = GetInt(P, TEXT("max_iterations"), XAtlasOptions.MaxIterations);

        // Compact first, because XAtlas REFUSES a non-compact mesh outright
        // (MeshUVFunctions.cpp:1143, `if (EditMesh.IsCompact() == false)` -> AppendError, return)
        // and a .pwmodel reaches this op non-compact in the ordinary case: `subtract`, `trim`,
        // `weld_vertices`, `remove_degenerates` and `simplify` all delete elements and leave gaps
        // in the id space, and the compiler runs them in the same part, before the uv stage, with
        // no way for the author to interleave anything. There is no `compact` op in the .pwmodel
        // op table, so without this the document has no expression that makes `uv mode=xatlas`
        // work after a boolean - the language could describe the model but not compile it.
        //
        // THIS IS THE FIX FOR A REAL, LONG-STANDING PRODUCT DEFECT, not a workaround for a noisy
        // diagnostic. The refusal predates the sink: `Saved/Logs/pw_full.log` (host project,
        // 2026-08-20 05:31 UTC) carries the engine's own
        // "AutoGenerateXAtlasMeshUVs: TargetMesh is non-Compact" twice while
        // Model.Compiler.RecompilingAnXAtlasUnwrapReproducesTheSameMesh reported Success - the
        // engine logs through MakeScriptError whether or not a Debug object is passed, so the
        // failure was visible in the log the whole time and invisible to every caller. The
        // compiles that "passed" shipped the box projection an earlier stage had left on channel
        // 0; xatlas had never run. Wiring the sink turned a silent wrong answer into a loud one,
        // which is what made this reachable at all.
        //
        // Deliberately NOT in GeometryOps::UnwrapUVXAtlas, and deliberately not after every op
        // that deletes elements:
        //   - the op layer must keep REPORTING the refusal - that is what
        //     Geometry.Ops.Modeling.XAtlasRefusesANonCompactMeshInsteadOfReportingSuccess pins,
        //     and geometry.unwrap_uv runs on a caller-owned mesh whose ids the caller may be
        //     holding, so silently renumbering them there would be the surprise;
        //   - compacting after each boolean/weld/simplify would pay a full rebuild per op to
        //     restore a property only this one engine call needs, and would renumber the ids
        //     `edge_split edges=[...]` addresses.
        // Here the compiler owns the mesh end to end within the part, and the compaction is
        // immediately upstream of the only call that cannot proceed without it.
        //
        // Guarded on IsCompact() so this is a no-op - not just cheap, but byte-identical - for
        // every document whose mesh is already compact. The only documents whose output changes
        // are the ones that hard-fail today.
        //
        // Safe for the reproducibility claim the format rests on: FDynamicMesh3::CompactInPlace
        // (DynamicMesh3_Edits.cpp:331) is a swap-from-the-end walk driven by the ref-count
        // bitmap, with no hashing and no parallelism, so the same input compacts the same way
        // every run; it remaps the attribute set through FCompactMaps, so material ids, groups
        // and the UV/normal overlays travel with their elements. It DOES move vertex and
        // triangle ids, which is why Model.Compiler's round-trip test asserts counts and bounds
        // rather than vertex order (see the fixture note in TestPwModelCompiler.cpp).
        if (!Mesh->GetMeshRef().IsCompact())
        {
            // Null Debug: the engine's only AppendError on this function is a null TargetMesh,
            // which cannot be reached here - ApplyUVOp's caller resolved the mesh already. See
            // docs/geometry-debug-sink-sweep.md, LEAVE (null-only).
            UGeometryScriptLibrary_MeshRepairFunctions::CompactMesh(Mesh, nullptr);
        }

        // The failure channel, wired here as well as in GeometryOps::UnwrapUVXAtlas because this
        // branch is open-coded rather than routed through that op (see the note above) - so it
        // would otherwise be the one xatlas call in the plugin still discarding the engine's
        // refusal, in the front-end that runs it in a loop over every part of a model.
        //
        // The non-compact refusal is now PREVENTED above rather than reported, so what is left
        // for this channel is XAtlas's other two: "UV Generation Failed" (XAtlasWrapper::ComputeUVs
        // returned false) and "UVSetIndex does not exist on TargetMesh" (pre-empted by the
        // EnsureMeshHasUVChannel guard, kept as a guard against an engine that grows a third
        // path). Whatever arrives is forwarded verbatim rather than paraphrased - the engine's
        // wording is the part an author can act on. The non-compact text reaching a caller again
        // would mean the compaction above stopped running, not that the message came back.
        //
        // The post-check below would catch some of these anyway, but only some, and only by
        // inference: it fires when the channel ends up with zero elements, which cannot
        // distinguish "xatlas refused" from "xatlas ran and produced nothing", and says nothing
        // at all when xatlas refuses a channel that already held UVs from an earlier stage.
        GeometryOps::FGeometryScriptDebugSink Debug;

        UGeometryScriptLibrary_MeshUVFunctions::AutoGenerateXAtlasMeshUVs(
            Mesh, Params.UVChannel, XAtlasOptions, Debug.Get());

        if (Debug.HasError())
        {
            return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_UV_GENERATION_FAILED,
                FString::Printf(TEXT("'uv mode=xatlas' failed - %s"), *Debug.ErrorText()));
        }

        UVResult = OkWithCounts(Mesh);
        Debug.DrainWarningsInto(UVResult);
    }
    else
    {
        // Not a fall-through to box projection: the parser's enum lists exactly six modes, and a
        // seventh added there but not here would otherwise compile to box-projected UVs with no
        // diagnostic anywhere.
        return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'uv mode=%s' is not a projection. Valid modes: %s"),
                *Mode, *FString::Join(UVModeNames(), TEXT(", "))));
    }

    // Every branch above can report success having written ZERO UV elements: xatlas bails on a
    // degenerate mesh, patch_builder on one it cannot partition, and layout only rearranges what
    // is already there - so on an element-less channel it packs nothing. All three send their
    // complaint to the GeometryScriptDebug argument, which is null at every call site, so the
    // only way to see it is to ask the mesh afterwards. Without this the op reports success and
    // the part ships untextured.
    //
    // Gated on there being geometry at all: a mesh with no triangles has no UVs to write, and
    // reporting that as a UV failure would name the wrong stage - the empty merged mesh is
    // PWMODEL_EMPTY_MESH's to report.
    if (UVResult.bSuccess && Mesh->GetTriangleCount() > 0 && !MeshHasUVElements(Mesh, Params.UVChannel))
    {
        return GeometryOps::FOpResult::Fail(ErrorCodes::ERR_NO_UV_ELEMENTS,
            FString::Printf(TEXT("'uv mode=%s' wrote no UV elements into channel %d: it reported success, but the channel is still empty."),
                *Mode, Params.UVChannel));
    }

    return UVResult;
}

// ---------------------------------------------------------------------------
// extrude: `direction` against the surface's own facing
// ---------------------------------------------------------------------------
//
// THE RULE, verified against UE 5.8 source. ApplyMeshLinearExtrudeFaces feeds
// `Direction * Distance` into FOffsetMeshRegion::OffsetPositionFunc as a plain
// `Position + UseDirection` closure (MeshModelingFunctions.cpp:580-600). FOffsetMeshRegion
// then displaces the ORIGINAL triangles by that vector, keeping their authored winding
// (OffsetMeshRegion.cpp:696-705, "The original RegionTriangles are the ones that are offset"
// at :613), and reverses the STATIONARY DUPLICATE (:734-744). So the authored surface ends up
// on the +direction face still facing the way it was authored, and outward requires
// `direction` to point ALONG the facing normal.
//
// The facing normal is the NEGATION of the right-hand rule: VectorUtil::Normal returns
// `(V2-V0) x (V1-V0)` because Unreal is left-handed (VectorUtil.h:80-87), and
// FDynamicMesh3::GetTriNormal is that function (DynamicMesh3_Queries.cpp:853-858). Reading the
// authored winding through GetTriInfo rather than re-deriving a cross product is what keeps
// this check on the same convention as the engine it is predicting.
//
// The sign of `distance` does NOT flip the rule. With a negative distance the engine reverses
// the originals instead of the duplicate (:743), and the slab also grows the other way; the two
// inversions cancel and `dot(direction, facing) > 0` stays the correct condition.
//
// WHAT IT FIRES ON - deliberately one narrow case, because that is the one where the sign
// decides the answer and nothing downstream can:
//
//   1. `direction_mode=fixed_direction`. average_face_normal ignores `direction` entirely
//      (MeshModelingFunctions.cpp:581-593) and pushes each region along its own averaged
//      normal, so the parameter cannot be wrong.
//   2. A non-zero distance. Zero moves nothing.
//   3. NO `face_direction` - the whole mesh is the selection. With a face filter the region is
//      a subset of its component, the engine takes a different path (Region.bIsSolid is
//      `selection == whole component`, OffsetMeshRegion.cpp:40-45), and pushing a selected face
//      INWARD is a legitimate way to author a recess. Warning there would be noise.
//   4. The accumulated mesh is OPEN. This is what excludes the already-warned case: whole-mesh
//      `extrude` on a CLOSED solid duplicates the solid rather than thickening a sheet, and
//      GeometryOps::Extrude already says so ("on a closed solid that duplicates it").
//   5. The surface has a COHERENT facing - the area-weighted mean normal must retain at least
//      half the total area. A closed solid integrates to zero and a deep cup to nearly zero;
//      on those the question "which way does this face" has no answer, and a warning derived
//      from a near-zero vector would be a coin toss. A flat sheet scores 1.0, a hemisphere
//      exactly 0.5.
//
// A mesh mixing a closed solid and an open sheet normally falls below (5) and stays quiet.
// That is the intended trade: narrow and silent beats broad and noisy on a warning nobody can
// verify from the response.
void FCompiler::WarnOnExtrudeFacingOpposed(const FPwOp& Op, UDynamicMesh* Mesh,
                                           const GeometryOps::FExtrudeParams& Params)
{
    if (!Mesh
        || Params.DirectionMode != GeometryOps::ELinearExtrudeDirection::FixedDirection
        || FMath::IsNearlyZero(Params.Distance)
        || Params.Faces.bHasDirection)
    {
        return;
    }

    FVector Direction = Params.Direction;
    if (!Direction.Normalize())
    {
        // A zero direction is the engine's problem, not this check's.
        return;
    }

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    if (EditMesh.TriangleCount() == 0)
    {
        return;
    }

    // (4): open only. Cheap enough to run before the per-triangle sweep, and it is the
    // condition that keeps this off the closed-solid case the op already warns about.
    bool bHasBoundary = false;
    for (int32 EdgeID : EditMesh.EdgeIndicesItr())
    {
        if (EditMesh.IsBoundaryEdge(EdgeID))
        {
            bHasBoundary = true;
            break;
        }
    }
    if (!bHasBoundary)
    {
        return;
    }

    FVector3d WeightedNormal = FVector3d::ZeroVector;
    double TotalArea = 0.0;
    for (int32 TriangleID : EditMesh.TriangleIndicesItr())
    {
        FVector3d Normal;
        FVector3d Centroid;
        double Area = 0.0;
        EditMesh.GetTriInfo(TriangleID, Normal, Area, Centroid);
        WeightedNormal += Area * Normal;
        TotalArea += Area;
    }

    if (TotalArea <= 0.0)
    {
        return;
    }

    // (5): coherence. |sum(A_i * N_i)| / sum(A_i) - 1.0 for a flat sheet, 0.5 for a
    // hemisphere, 0 for anything closed.
    const double Coherence = WeightedNormal.Length() / TotalArea;
    if (Coherence < 0.5)
    {
        return;
    }

    const FVector3d Facing = WeightedNormal / WeightedNormal.Length();
    const double Alignment = FVector3d(Direction).Dot(Facing);
    if (Alignment >= 0.0)
    {
        return;
    }

    Warn(PwModelDiagnosticCodes::PWMODEL_EXTRUDE_FACING_OPPOSED, Op,
        FString::Printf(
            TEXT("'extrude direction=(%g, %g, %g)' points AGAINST this surface's facing normal ")
            TEXT("(%.3f, %.3f, %.3f) - dot %.3f - so the resulting shell will be inside out. The op ")
            TEXT("displaces the authored triangles along 'direction' keeping their winding and reverses ")
            TEXT("the copy left behind, so 'direction' has to point the way the surface already faces. ")
            TEXT("Negate 'direction'. Nothing downstream can catch this: the slab comes out closed, ")
            TEXT("manifold, 0 boundary edges, and renders identically to a correct one - but its mesh ")
            TEXT("distance field is inverted, so Lumen and DFAO light it as if the camera were inside. ")
            TEXT("Check health.signedVolume: it is negative on an inverted closed shell."),
            Params.Direction.X, Params.Direction.Y, Params.Direction.Z,
            Facing.X, Facing.Y, Facing.Z, Alignment));
}

// The `revolve` twin of the check above, and the same argument for its existence: a profile
// walked the wrong way round sweeps a solid whose every triangle faces inwards, and NOTHING
// downstream separates it from the correct one. Measured on the repro this was written for -
// a 16-point closed off-axis dome section at steps=36, revolved both ways - isClosed,
// boundaryEdges, degenerateTriangles, nonManifoldVertices, orientationConsistent,
// floatingCount, bounds and the triangle count were identical, and signedVolume alone flipped
// from +1.73e9 to -1.73e9. Backface culling then makes the two render identically from every
// angle, so no capture finds it, while the mesh distance field inverts and Lumen / DFAO light
// the part as though the camera were inside it.
//
// The test is the SIGN OF THE ENCLOSED VOLUME of the mesh the op produced, not a walk over the
// point list. That is one predicate for all three shapes `revolve` can build - the closed
// off-axis section, the axis-capped lathe, and a partial sweep closed by `capped` - where a
// profile-space winding test would need a different rule for each and would still be wrong on
// a self-crossing outline.
//
// Two exclusions, both there so the message cannot name the wrong cause:
//
//   1. An OPEN result. Signed volume on an unclosed surface is the integral of an unclosed
//      boundary and means nothing; an uncapped partial sweep is not inside out, it is open.
//      FMeshOrientation::IsInverted() already encodes exactly that gate.
//   2. A negative-determinant `scale=` on the op, which reverses winding on its own. The
//      author asked for that, the profile may be perfectly correct, and blaming it here would
//      send them to edit the one thing that is not wrong.
void FCompiler::WarnOnRevolveProfileReversed(const FPwOp& Op, UDynamicMesh* Scratch,
                                             const FTransform& Local)
{
    if (!Scratch || Scratch->GetTriangleCount() == 0)
    {
        return;
    }

    const FVector Scale = Local.GetScale3D();
    if (Scale.X * Scale.Y * Scale.Z < 0.0)
    {
        return;
    }

    const GeometryUtils::FMeshOrientation Orientation =
        GeometryUtils::MeasureMeshOrientation(Scratch);
    if (!Orientation.IsInverted())
    {
        return;
    }

    Warn(PwModelDiagnosticCodes::PWMODEL_REVOLVE_PROFILE_REVERSED, Op,
        FString::Printf(
            TEXT("'revolve' swept a closed solid enclosing NEGATIVE volume (%g), so this profile is ")
            TEXT("walked the wrong way round and the whole surface faces inwards. The sweep keeps the ")
            TEXT("winding the point order gives it, so in the profile's own (x = radius, y = height) ")
            TEXT("plane the section has to be traversed COUNTER-CLOCKWISE - up the OUTER face, over the ")
            TEXT("top, back down the INNER face, which is the enclosed material staying on your left. ")
            TEXT("Reverse the point list. Nothing downstream can catch this: the solid comes out closed, ")
            TEXT("manifold, 0 boundary edges, orientationConsistent, with the same triangle count and the ")
            TEXT("same bounds as the correct one, and it renders identically from every angle - but its ")
            TEXT("mesh distance field is inverted, so Lumen and DFAO light it as if the camera were ")
            TEXT("inside. Check health.signedVolume PER PART: the model-wide sum averages one inverted ")
            TEXT("part away."),
            Orientation.SignedVolume));
}

bool FCompiler::DispatchModifier(const FPwOp& Op, UDynamicMesh* Mesh, GeometryOps::FOpResult& OutResult)
{
    const TMap<FString, FPwValue>& P = Op.Params;
    const FString& Name = Op.OpName;

    // ---- normals and tangents ----
    if (Name == TEXT("recalculate_normals"))
    {
        // Both keys reach the engine call. `split_angle` used to sit in the op table alongside
        // them, copied over from the RPC verb's declaration, and reached nothing at all - the
        // verb neither read it nor echoed it. It is gone from both front-ends now; hard edges
        // are `split_normals`, whose SplitAngle does go through.
        GeometryOps::FRecalculateNormalsParams Params;
        Params.bAreaWeighted = GetBool(P, TEXT("area_weighted"), Params.bAreaWeighted);
        Params.bAngleWeighted = GetBool(P, TEXT("angle_weighted"), Params.bAngleWeighted);
        OutResult = GeometryOps::RecalculateNormals(Mesh, Params);
        return true;
    }
    if (Name == TEXT("flip_normals"))
    {
        OutResult = GeometryOps::FlipNormals(Mesh, GeometryOps::FFlipNormalsParams());
        return true;
    }
    if (Name == TEXT("recompute_tangents"))
    {
        GeometryOps::FRecomputeTangentsParams Params;
        Params.Type = ReadTangentType(P, Params.Type);
        Params.UVLayer = GetInt(P, TEXT("uv_layer"), Params.UVLayer);
        OutResult = GeometryOps::RecomputeTangents(Mesh, Params);
        return true;
    }
    if (Name == TEXT("split_normals"))
    {
        GeometryOps::FSplitNormalsParams Params;
        Params.SplitAngle = GetNumber(P, TEXT("split_angle"), Params.SplitAngle);
        Params.bSplitByOpeningAngle = GetBool(P, TEXT("split_by_opening_angle"), Params.bSplitByOpeningAngle);
        Params.bSplitByFaceGroup = GetBool(P, TEXT("split_by_face_group"), Params.bSplitByFaceGroup);
        Params.bUseDefaultGroupLayer = GetBool(P, TEXT("use_default_group_layer"), Params.bUseDefaultGroupLayer);
        Params.ExtendedGroupLayerIndex = GetInt(P, TEXT("group_layer_index"), Params.ExtendedGroupLayerIndex);
        OutResult = GeometryOps::SplitNormals(Mesh, Params);
        return true;
    }

    // ---- topology budget ----
    if (Name == TEXT("simplify_mesh"))
    {
        GeometryOps::FSimplifyMeshParams Params;
        Params.TargetPercentage = GetNumber(P, TEXT("target_percentage"), Params.TargetPercentage);
        Params.Method = ReadSimplifyMethod(P, Params.Method);
        Params.bAllowSeamCollapse = GetBool(P, TEXT("allow_seam_collapse"), Params.bAllowSeamCollapse);
        Params.bAllowSeamSmoothing = GetBool(P, TEXT("allow_seam_smoothing"), Params.bAllowSeamSmoothing);
        Params.bAllowSeamSplits = GetBool(P, TEXT("allow_seam_splits"), Params.bAllowSeamSplits);
        Params.bPreserveVertexPositions = GetBool(P, TEXT("preserve_vertex_positions"), Params.bPreserveVertexPositions);
        Params.bRetainQuadricMemory = GetBool(P, TEXT("retain_quadric_memory"), Params.bRetainQuadricMemory);
        Params.RegularizeWeight = GetNumber(P, TEXT("regularize_weight"), Params.RegularizeWeight);
        Params.bAutoCompact = GetBool(P, TEXT("auto_compact"), Params.bAutoCompact);
        Params.QuadricVariant = ReadSimplifyQuadricVariant(P, Params.QuadricVariant);
        Params.NormalAttributeWeight = GetNumber(P, TEXT("normal_attribute_weight"), Params.NormalAttributeWeight);
        Params.TangentAttributeWeight = GetNumber(P, TEXT("tangent_attribute_weight"), Params.TangentAttributeWeight);
        Params.ColorAttributeWeight = GetNumber(P, TEXT("color_attribute_weight"), Params.ColorAttributeWeight);
        Params.TexCoordAttributeWeight = GetNumber(P, TEXT("texcoord_attribute_weight"), Params.TexCoordAttributeWeight);
        Params.ScaleCorrection = GetNumber(P, TEXT("scale_correction"), Params.ScaleCorrection);
        OutResult = GeometryOps::SimplifyMesh(Mesh, Params);
        return true;
    }
    if (Name == TEXT("subdivide"))
    {
        GeometryOps::FSubdivideParams Params;
        Params.Iterations = GetInt(P, TEXT("iterations"), Params.Iterations);
        Params.bRecomputeNormals = GetBool(P, TEXT("recompute_normals"), Params.bRecomputeNormals);
        OutResult = GeometryOps::Subdivide(Mesh, Params);
        return true;
    }
    if (Name == TEXT("remesh_uniform"))
    {
        GeometryOps::FRemeshUniformParams Params;
        Params.TargetTriangleCount = GetInt(P, TEXT("target_triangle_count"), Params.TargetTriangleCount);
        Params.TargetType = ReadRemeshTargetType(P, Params.TargetType);
        Params.TargetEdgeLength = GetNumber(P, TEXT("target_edge_length"), Params.TargetEdgeLength);
        Params.bDiscardAttributes = GetBool(P, TEXT("discard_attributes"), Params.bDiscardAttributes);
        Params.bReprojectToInputMesh = GetBool(P, TEXT("reproject_to_input_mesh"), Params.bReprojectToInputMesh);
        Params.SmoothingType = ReadRemeshSmoothingType(P, Params.SmoothingType);
        Params.SmoothingRate = GetNumber(P, TEXT("smoothing_rate"), Params.SmoothingRate);
        Params.MeshBoundaryConstraint =
            ReadRemeshEdgeConstraint(P, TEXT("mesh_boundary_constraint"), Params.MeshBoundaryConstraint);
        Params.GroupBoundaryConstraint =
            ReadRemeshEdgeConstraint(P, TEXT("group_boundary_constraint"), Params.GroupBoundaryConstraint);
        Params.MaterialBoundaryConstraint =
            ReadRemeshEdgeConstraint(P, TEXT("material_boundary_constraint"), Params.MaterialBoundaryConstraint);
        Params.bAllowFlips = GetBool(P, TEXT("allow_flips"), Params.bAllowFlips);
        Params.bAllowSplits = GetBool(P, TEXT("allow_splits"), Params.bAllowSplits);
        Params.bAllowCollapses = GetBool(P, TEXT("allow_collapses"), Params.bAllowCollapses);
        Params.bPreventNormalFlips = GetBool(P, TEXT("prevent_normal_flips"), Params.bPreventNormalFlips);
        Params.bPreventTinyTriangles = GetBool(P, TEXT("prevent_tiny_triangles"), Params.bPreventTinyTriangles);
        Params.bUseFullRemeshPasses = GetBool(P, TEXT("use_full_remesh_passes"), Params.bUseFullRemeshPasses);
        Params.RemeshIterations = GetInt(P, TEXT("iterations"), Params.RemeshIterations);
        Params.bAutoCompact = GetBool(P, TEXT("auto_compact"), Params.bAutoCompact);
        OutResult = GeometryOps::RemeshUniform(Mesh, Params);
        return true;
    }
    if (Name == TEXT("poke"))
    {
        GeometryOps::FPokeParams Params;
        Params.Offset = GetNumber(P, TEXT("offset"), Params.Offset);
        Params.OffsetType = ReadOffsetFacesType(P, Params.OffsetType);
        Params.Common = ReadFaceOpCommon(P, Params.Common);
        Params.bSolidsToShells = GetBool(P, TEXT("solids_to_shells"), Params.bSolidsToShells);
        Params.bRecomputeNormals = GetBool(P, TEXT("recompute_normals"), Params.bRecomputeNormals);
        OutResult = GeometryOps::Poke(Mesh, Params);
        return true;
    }

    // ---- face modeling ----
    if (Name == TEXT("extrude"))
    {
        GeometryOps::FExtrudeParams Params;
        Params.Distance = GetNumber(P, TEXT("distance"), Params.Distance);
        Params.Direction = GetVector3(P, TEXT("direction"), Params.Direction);
        Params.DirectionMode = ReadLinearExtrudeDirection(P, Params.DirectionMode);
        Params.bSolidsToShells = GetBool(P, TEXT("solids_to_shells"), Params.bSolidsToShells);
        Params.Common = ReadFaceOpCommon(P, Params.Common);
        Params.Faces = ReadFaceSelection(P);
        // BEFORE the op: the check reads the surface the author authored, and the op replaces
        // it with the slab.
        WarnOnExtrudeFacingOpposed(Op, Mesh, Params);
        OutResult = GeometryOps::Extrude(Mesh, Params);
        return true;
    }
    if (Name == TEXT("inset"))
    {
        GeometryOps::FInsetParams Params;
        Params.Distance = GetNumber(P, TEXT("distance"), Params.Distance);
        Params.bReproject = GetBool(P, TEXT("reproject"), Params.bReproject);
        Params.bBoundaryOnly = GetBool(P, TEXT("boundary_only"), Params.bBoundaryOnly);
        Params.Softness = GetNumber(P, TEXT("softness"), Params.Softness);
        Params.AreaScale = GetNumber(P, TEXT("area_scale"), Params.AreaScale);
        Params.Common = ReadFaceOpCommon(P, Params.Common);
        Params.Faces = ReadFaceSelection(P);
        OutResult = GeometryOps::Inset(Mesh, Params);
        return true;
    }
    if (Name == TEXT("outset"))
    {
        // No `reproject` read here and no parameter for it in the op table: the engine honours
        // reprojection only for a positive inset distance, and outset always passes a negative
        // one. See FOutsetParams.
        GeometryOps::FOutsetParams Params;
        Params.Distance = GetNumber(P, TEXT("distance"), Params.Distance);
        Params.bBoundaryOnly = GetBool(P, TEXT("boundary_only"), Params.bBoundaryOnly);
        Params.Softness = GetNumber(P, TEXT("softness"), Params.Softness);
        Params.AreaScale = GetNumber(P, TEXT("area_scale"), Params.AreaScale);
        Params.Common = ReadFaceOpCommon(P, Params.Common);
        Params.Faces = ReadFaceSelection(P);
        OutResult = GeometryOps::Outset(Mesh, Params);
        return true;
    }
    if (Name == TEXT("offset_faces"))
    {
        GeometryOps::FOffsetFacesParams Params;
        Params.Distance = GetNumber(P, TEXT("distance"), Params.Distance);
        Params.OffsetType = ReadOffsetFacesType(P, Params.OffsetType);
        Params.bSolidsToShells = GetBool(P, TEXT("solids_to_shells"), Params.bSolidsToShells);
        Params.Common = ReadFaceOpCommon(P, Params.Common);
        Params.Faces = ReadFaceSelection(P);
        OutResult = GeometryOps::OffsetFaces(Mesh, Params);
        return true;
    }
    if (Name == TEXT("bevel"))
    {
        GeometryOps::FBevelParams Params;
        Params.PriorBevelGroupIDs = PriorBevelGroupIDs;
        Params.Distance = GetNumber(P, TEXT("distance"), Params.Distance);
        Params.Subdivisions = GetInt(P, TEXT("segments"), Params.Subdivisions);
        Params.RoundWeight = GetNumber(P, TEXT("round_weight"), Params.RoundWeight);
        // ---- the material of the chamfer faces --------------------------------------------
        //
        // The DOCUMENT's defaults, not the engine struct's, and the divergence is deliberate:
        // GeometryOps::FBevelParams stays pinned to `bInferMaterialID = false, SetMaterialID = 0`
        // because a parity test holds it against FGeometryScriptMeshBevelOptions and the
        // geometry.bevel RPC publishes it. Inside a document, 0 is not a neutral index - the slot
        // table is model-wide and allocated in first-use order, so it is whichever slot the FIRST
        // part tagged, and an unqualified bevel therefore put every new face in another part's
        // material. Measured across one project: 38 of 38 bevels written by three teams omitted
        // infer_material_id, which is a default set the wrong way round rather than a choice.
        //
        // Bevel is the one op that tags its own output (FPwModelOpSpec::bSelfTagsMaterial), so it
        // is deliberately kept off the generic append-and-retag path in RunOp: the engine takes
        // each new face from the two faces either side of ITS edge, which is per-edge correct on a
        // multi-slot part and strictly better than the one-slot-for-everything answer a retag can
        // give.
        TMap<int32, int32> IncomingByMaterial;
        CountMaterialTriangles(Mesh, IncomingByMaterial);
        const int32 Dominant = DominantMaterialSlot(IncomingByMaterial);

        // The fallback the engine uses wherever the two sides of an edge disagree: the slot most
        // of the geometry being bevelled is on, never 0-by-accident.
        int32 FallbackMaterialID = (Dominant != INDEX_NONE) ? Dominant : 0;

        // `material=` names the faces outright, so inference goes OFF unless the author asks for
        // it back - `material=` with `infer_material_id=true` is the coherent pair meaning "the
        // two sides where they agree, the named slot where they do not".
        bool bInferByDefault = true;
        if (HasValue(P, TEXT("material")))
        {
            const FString SlotName = GetString(P, TEXT("material"));
            if (!SlotName.IsEmpty())
            {
                if (SlotName == DefaultSlotName)
                {
                    bDefaultSlotExplicitlyTagged = true;
                }
                FallbackMaterialID = ResolveSlot(SlotName);
                bInferByDefault = false;
            }
        }

        Params.bInferMaterialID = GetBool(P, TEXT("infer_material_id"), bInferByDefault);
        Params.SetMaterialID = GetInt(P, TEXT("material_id"), FallbackMaterialID);
        // The filter is derived from the two corners being present rather than from a separate
        // `apply_filter_box` flag: a flag with no box, or a box with the flag off, are two
        // spellings of a request that silently does nothing, and this shape has neither.
        if (HasValue(P, TEXT("filter_box_min")) && HasValue(P, TEXT("filter_box_max")))
        {
            Params.bApplyFilterBox = true;
            Params.FilterBoxMin = GetVector3(P, TEXT("filter_box_min"), Params.FilterBoxMin);
            Params.FilterBoxMax = GetVector3(P, TEXT("filter_box_max"), Params.FilterBoxMax);
        }
        Params.bFullyContained = GetBool(P, TEXT("fully_contained"), Params.bFullyContained);
        OutResult = GeometryOps::Bevel(Mesh, Params);
        return true;
    }
    if (Name == TEXT("shell"))
    {
        GeometryOps::FShellParams Params;
        Params.Thickness = GetNumber(P, TEXT("thickness"), Params.Thickness);
        Params.bFixedBoundary = GetBool(P, TEXT("fixed_boundary"), Params.bFixedBoundary);
        Params.SolveSteps = GetInt(P, TEXT("solve_steps"), Params.SolveSteps);
        Params.SmoothAlpha = GetNumber(P, TEXT("smooth_alpha"), Params.SmoothAlpha);
        Params.bReprojectDuringSmoothing =
            GetBool(P, TEXT("reproject_during_smoothing"), Params.bReprojectDuringSmoothing);
        Params.BoundaryAlpha = GetNumber(P, TEXT("boundary_alpha"), Params.BoundaryAlpha);
        OutResult = GeometryOps::Shell(Mesh, Params);
        return true;
    }

    // ---- warp deformers ----
    if (Name == TEXT("bend"))
    {
        GeometryOps::FBendParams Params;
        Params.Angle = GetNumber(P, TEXT("angle"), Params.Angle);
        Params.Extent = GetNumber(P, TEXT("extent"), Params.Extent);
        Params.Extents = ReadWarpExtents(P, Params.Extents);
        Params.Frame = ReadWarpFrame(P, Params.Frame);
        Params.bBidirectional = GetBool(P, TEXT("bidirectional"), Params.bBidirectional);
        OutResult = GeometryOps::Bend(Mesh, Params);
        return true;
    }
    if (Name == TEXT("twist"))
    {
        GeometryOps::FTwistParams Params;
        Params.Angle = GetNumber(P, TEXT("angle"), Params.Angle);
        Params.Extent = GetNumber(P, TEXT("extent"), Params.Extent);
        Params.Extents = ReadWarpExtents(P, Params.Extents);
        Params.Frame = ReadWarpFrame(P, Params.Frame);
        Params.bBidirectional = GetBool(P, TEXT("bidirectional"), Params.bBidirectional);
        OutResult = GeometryOps::Twist(Mesh, Params);
        return true;
    }
    if (Name == TEXT("taper"))
    {
        GeometryOps::FTaperParams Params;
        const FVector2D Flare = GetVector2(P, TEXT("flare"), FVector2D(Params.FlareX, Params.FlareY));
        Params.FlareX = Flare.X;
        Params.FlareY = Flare.Y;
        Params.Extent = GetNumber(P, TEXT("extent"), Params.Extent);
        Params.Extents = ReadWarpExtents(P, Params.Extents);
        Params.Frame = ReadWarpFrame(P, Params.Frame);
        Params.FlareType = ReadFlareType(P, Params.FlareType);
        OutResult = GeometryOps::Taper(Mesh, Params);
        return true;
    }
    if (Name == TEXT("noise_deform"))
    {
        GeometryOps::FNoiseDeformParams Params;
        Params.Magnitude = GetNumber(P, TEXT("magnitude"), Params.Magnitude);
        Params.Frequency = GetNumber(P, TEXT("frequency"), Params.Frequency);
        Params.FrequencyShift = GetVector3(P, TEXT("frequency_shift"), Params.FrequencyShift);
        Params.Seed = GetInt(P, TEXT("seed"), Params.Seed);
        Params.bApplyAlongNormal = GetBool(P, TEXT("apply_along_normal"), Params.bApplyAlongNormal);
        Params.NormalSource = GetIdentifier(P, TEXT("normal_source"), TEXT("computed")) == TEXT("average_from_overlay")
            ? GeometryOps::ENoiseNormalSource::AverageFromOverlay
            : GeometryOps::ENoiseNormalSource::Computed;
        Params.MagnitudeMode = GetIdentifier(P, TEXT("magnitude_mode"), TEXT("absolute")) == TEXT("relative")
            ? GeometryOps::ENoiseMagnitudeMode::Relative
            : GeometryOps::ENoiseMagnitudeMode::Absolute;
        OutResult = GeometryOps::NoiseDeform(Mesh, Params);
        return true;
    }
    if (Name == TEXT("harmonic_deform"))
    {
        GeometryOps::FHarmonicDeformParams Params;
        Params.Axis = ReadMeshAxis(P, TEXT("z"));
        Params.Center = GetVector3(P, TEXT("center"), Params.Center);
        Params.Target = GetIdentifier(P, TEXT("target"), TEXT("radial")) == TEXT("axial")
            ? GeometryOps::EHarmonicTarget::Axial
            : GeometryOps::EHarmonicTarget::Radial;
        Params.Terms = ReadHarmonicTerms(P, TEXT("terms"));
        OutResult = GeometryOps::HarmonicDeform(Mesh, Params);
        return true;
    }
    if (Name == TEXT("smooth"))
    {
        GeometryOps::FSmoothParams Params;
        Params.Iterations = GetInt(P, TEXT("iterations"), Params.Iterations);
        Params.Alpha = GetNumber(P, TEXT("alpha"), Params.Alpha);
        OutResult = GeometryOps::Smooth(Mesh, Params);
        return true;
    }
    if (Name == TEXT("relax"))
    {
        GeometryOps::FRelaxParams Params;
        Params.Iterations = GetInt(P, TEXT("iterations"), Params.Iterations);
        Params.Strength = GetNumber(P, TEXT("strength"), Params.Strength);
        OutResult = GeometryOps::Relax(Mesh, Params);
        return true;
    }
    if (Name == TEXT("stretch"))
    {
        GeometryOps::FStretchParams Params;
        Params.Axis = ReadMeshAxis(P, TEXT("z"));
        Params.Factor = GetNumber(P, TEXT("factor"), Params.Factor);
        OutResult = GeometryOps::Stretch(Mesh, Params);
        return true;
    }
    if (Name == TEXT("spherify"))
    {
        GeometryOps::FSpherifyParams Params;
        Params.Factor = GetNumber(P, TEXT("factor"), Params.Factor);
        OutResult = GeometryOps::Spherify(Mesh, Params);
        return true;
    }
    if (Name == TEXT("cylindrify"))
    {
        GeometryOps::FCylindrifyParams Params;
        Params.Axis = ReadMeshAxis(P, TEXT("z"));
        Params.Factor = GetNumber(P, TEXT("factor"), Params.Factor);
        OutResult = GeometryOps::Cylindrify(Mesh, Params);
        return true;
    }

    // ---- repair ----
    if (Name == TEXT("weld_vertices"))
    {
        GeometryOps::FWeldVerticesParams Params;
        Params.Tolerance = GetNumber(P, TEXT("tolerance"), Params.Tolerance);
        Params.bOnlyUniquePairs = GetBool(P, TEXT("only_unique_pairs"), Params.bOnlyUniquePairs);
        OutResult = GeometryOps::WeldVertices(Mesh, Params);
        return true;
    }
    if (Name == TEXT("fill_holes"))
    {
        GeometryOps::FFillHolesParams Params;
        Params.FillMethod = ReadFillHolesMethod(P, Params.FillMethod);
        Params.bDeleteIsolatedTriangles =
            GetBool(P, TEXT("delete_isolated_triangles"), Params.bDeleteIsolatedTriangles);
        OutResult = GeometryOps::FillHoles(Mesh, Params);
        return true;
    }
    if (Name == TEXT("remove_degenerates"))
    {
        GeometryOps::FRemoveDegeneratesParams Params;
        Params.Mode = ReadRepairMeshMode(P, Params.Mode);
        Params.MinTriangleArea = GetNumber(P, TEXT("min_triangle_area"), Params.MinTriangleArea);
        Params.MinEdgeLength = GetNumber(P, TEXT("min_edge_length"), Params.MinEdgeLength);
        Params.bCompactOnCompletion =
            GetBool(P, TEXT("compact_on_completion"), Params.bCompactOnCompletion);
        OutResult = GeometryOps::RemoveDegenerates(Mesh, Params);
        return true;
    }
    if (Name == TEXT("merge_vertices"))
    {
        GeometryOps::FMergeVerticesParams Params;
        Params.Tolerance = GetNumber(P, TEXT("tolerance"), Params.Tolerance);
        Params.bCompact = GetBool(P, TEXT("compact"), Params.bCompact);
        OutResult = GeometryOps::MergeVertices(Mesh, Params);
        return true;
    }

    // ---- UV ----
    if (Name == TEXT("uv"))
    {
        OutResult = ApplyUVOp(Op, Mesh);
        return true;
    }
    if (Name == TEXT("transform_uvs"))
    {
        GeometryOps::FTransformUVsParams Params;
        Params.UVChannel = GetInt(P, TEXT("channel"), Params.UVChannel);
        Params.Translate = GetVector2(P, TEXT("translate"), Params.Translate);
        Params.Scale = GetVector2(P, TEXT("scale"), Params.Scale);
        Params.Rotation = GetNumber(P, TEXT("rotate"), Params.Rotation);
        OutResult = GeometryOps::TransformUVs(Mesh, Params);
        return true;
    }

    // ---- element edits ----
    if (Name == TEXT("set_vertex_position"))
    {
        GeometryOps::FSetVertexPositionParams Params;
        Params.VertexIndex = GetInt(P, TEXT("index"), Params.VertexIndex);
        Params.Position = GetVector3(P, TEXT("position"), Params.Position);
        OutResult = GeometryOps::SetVertexPosition(Mesh, Params);
        return true;
    }
    if (Name == TEXT("append_vertex"))
    {
        GeometryOps::FAppendVertexParams Params;
        Params.Position = GetVector3(P, TEXT("position"), Params.Position);
        int32 VertexIndex = INDEX_NONE;
        OutResult = GeometryOps::AppendVertex(Mesh, Params, VertexIndex);
        return true;
    }
    if (Name == TEXT("delete_vertex"))
    {
        GeometryOps::FDeleteVertexParams Params;
        Params.VertexIndex = GetInt(P, TEXT("index"), Params.VertexIndex);

        // Sparse-but-present is a NO-OP here, not a failure, and the split is made in the
        // compiler rather than in the op because the two front ends need different answers.
        // geometry.delete_vertex is one call about one vertex, and INVALID_VERTEX is the right
        // reply to "remove a vertex that is not there". A document is a SEQUENCE, and the
        // engine removes vertices the document never named: FDynamicMesh3::RemoveTriangle drops
        // the vertices its triangle leaves isolated, so `delete_triangle` followed by
        // `delete_vertex` on the interior vertex it stranded - the natural cut-then-clean-up
        // pairing - aborted the whole compile over work the engine had already done. Nothing in
        // the document says which vertices that was, so the author cannot write around it.
        //
        // MaxVertexID is what keeps this from swallowing a real mistake: deletes leave the id
        // space SPARSE rather than renumbering it (docs/pwmodel-format.md, "Ids come from the
        // emission order"), so every id that was ever valid stays below the bound and every id
        // that never existed is at or above it. Out-of-bound and negative ids fall through to
        // GeometryOps::DeleteVertex and its INVALID_VERTEX failure, unchanged.
        //
        // Null falls through rather than being handled here: GeometryOps::BeginOp answers a null
        // mesh with MESH_NOT_FOUND, and a second spelling of that would be a second thing to
        // keep in step.
        if (Mesh != nullptr
            && Params.VertexIndex >= 0
            && Params.VertexIndex < Mesh->GetMeshRef().MaxVertexID()
            && !Mesh->GetMeshRef().IsVertex(Params.VertexIndex))
        {
            Warn(PwModelDiagnosticCodes::PWMODEL_VERTEX_ALREADY_REMOVED, Op,
                FString::Printf(
                    TEXT("'delete_vertex' names vertex %d, which the mesh no longer carries - a previous ")
                    TEXT("'delete_triangle' removed it along with the triangle that was its last user. ")
                    TEXT("The op is skipped; the mesh already holds the state it asked for. Delete the ")
                    TEXT("statement to silence this. An id the mesh NEVER carried is still an error."),
                    Params.VertexIndex));

            OutResult = GeometryOps::FOpResult::Ok();
            OutResult.bChanged = false;
            return true;
        }

        OutResult = GeometryOps::DeleteVertex(Mesh, Params);
        return true;
    }
    if (Name == TEXT("delete_triangle"))
    {
        GeometryOps::FDeleteTriangleParams Params;
        Params.TriangleIndex = GetInt(P, TEXT("index"), Params.TriangleIndex);
        OutResult = GeometryOps::DeleteTriangle(Mesh, Params);
        return true;
    }
    if (Name == TEXT("set_vertex_color"))
    {
        GeometryOps::FSetVertexColorParams Params;
        Params.VertexIndex = GetInt(P, TEXT("index"), Params.VertexIndex);
        Params.Color = GetColor(P, TEXT("color"), Params.Color);
        Params.bSetAll = GetBool(P, TEXT("set_all"), Params.bSetAll);

        // A MISSING key means all four, so a document written before the mask existed compiles
        // unchanged. A key that is PRESENT is always parsed, including `channels=""` - gating on
        // IsEmpty() instead would have made the empty mask silently write all four here while the
        // RPC refused it, which is the worse of the two ways for two front-ends to disagree.
        // A spelling this cannot read is an error rather than a fallback to all: the author named
        // something specific, and guessing all four would overwrite the channels they meant to
        // keep - which is the reason the parameter exists.
        if (HasValue(P, TEXT("channels")))
        {
            FString ChannelError;
            if (!GeometryOps::ParseColorChannels(GetString(P, TEXT("channels")), Params.Channels, ChannelError))
            {
                OutResult = GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_ARGUMENT, ChannelError);
                return true;
            }
        }

        int32 VerticesModified = 0;
        OutResult = GeometryOps::SetVertexColor(Mesh, Params, VerticesModified);
        return true;
    }
    if (Name == TEXT("bake_ao"))
    {
        GeometryOps::FBakeAmbientOcclusionParams Params;
        Params.OcclusionRadius = GetNumber(P, TEXT("radius"), Params.OcclusionRadius);
        Params.Samples = GetInt(P, TEXT("samples"), Params.Samples);
        Params.BiasAngleDegrees = GetNumber(P, TEXT("bias_angle"), Params.BiasAngleDegrees);
        Params.Strength = GetNumber(P, TEXT("strength"), Params.Strength);

        // `channels` is required by the op table, so an absent key never reaches here; an
        // unreadable one still has to fail rather than widen to all four.
        FString ChannelError;
        if (!GeometryOps::ParseColorChannels(GetString(P, TEXT("channels")), Params.Channels, ChannelError))
        {
            OutResult = GeometryOps::FOpResult::Fail(ErrorCodes::ERR_INVALID_ARGUMENT, ChannelError);
            return true;
        }

        // The parser's enum domain already refuses anything but these two, so this reads the
        // choice rather than re-validating it.
        Params.bMultiply = GetString(P, TEXT("blend")).Equals(TEXT("multiply"), ESearchCase::IgnoreCase);

        GeometryOps::FBakeAmbientOcclusionOutputs Outputs;
        OutResult = GeometryOps::BakeAmbientOcclusion(Mesh, Params, Outputs);
        return true;
    }
    if (Name == TEXT("set_uvs"))
    {
        GeometryOps::FSetUVsParams Params;
        Params.VertexIndex = GetInt(P, TEXT("index"), Params.VertexIndex);
        Params.UV = GetVector2(P, TEXT("uv"), Params.UV);
        Params.UVChannel = GetInt(P, TEXT("channel"), Params.UVChannel);
        int32 ElementsModified = 0;
        OutResult = GeometryOps::SetUVs(Mesh, Params, ElementsModified);
        return true;
    }
    if (Name == TEXT("translate_mesh"))
    {
        GeometryOps::FTranslateMeshParams Params;
        Params.Translation = GetVector3(P, TEXT("translation"), Params.Translation);
        OutResult = GeometryOps::TranslateMesh(Mesh, Params);
        return true;
    }

    // ---- merge-transforms ----
    if (Name == TEXT("self_union"))
    {
        GeometryOps::FSelfUnionParams Params;
        Params.bFillHoles = GetBool(P, TEXT("fill_holes"), Params.bFillHoles);
        Params.bTrimFlaps = GetBool(P, TEXT("trim_flaps"), Params.bTrimFlaps);
        Params.bSimplifyOutput = GetBool(P, TEXT("simplify_output"), Params.bSimplifyOutput);
        Params.SimplifyPlanarTolerance =
            GetNumber(P, TEXT("simplify_planar_tolerance"), Params.SimplifyPlanarTolerance);
        Params.WindingThreshold = GetNumber(P, TEXT("winding_threshold"), Params.WindingThreshold);
        OutResult = GeometryOps::SelfUnion(Mesh, Params);
        return true;
    }
    if (Name == TEXT("mirror"))
    {
        GeometryOps::FMirrorParams Params;
        Params.Axis = ReadMeshAxis(P, TEXT("x"));
        Params.bWeld = GetBool(P, TEXT("weld"), Params.bWeld);
        Params.WeldTolerance = GetNumber(P, TEXT("weld_tolerance"), Params.WeldTolerance);
        Params.bOnlyUniquePairs = GetBool(P, TEXT("only_unique_pairs"), Params.bOnlyUniquePairs);
        OutResult = GeometryOps::Mirror(Mesh, Params);
        return true;
    }
    if (Name == TEXT("array_linear"))
    {
        GeometryOps::FArrayLinearParams Params;
        Params.Count = GetInt(P, TEXT("count"), Params.Count);
        Params.Offset = GetVector3(P, TEXT("offset"), Params.Offset);
        OutResult = GeometryOps::ArrayLinear(Mesh, Params);
        return true;
    }
    if (Name == TEXT("array_radial"))
    {
        GeometryOps::FArrayRadialParams Params;
        Params.Count = GetInt(P, TEXT("count"), Params.Count);
        Params.Center = GetVector3(P, TEXT("center"), Params.Center);
        Params.Axis = ReadMeshAxis(P, TEXT("z"));
        Params.TotalAngleDegrees = GetNumber(P, TEXT("angle"), Params.TotalAngleDegrees);
        OutResult = GeometryOps::ArrayRadial(Mesh, Params);
        return true;
    }
    if (Name == TEXT("array_along_path"))
    {
        // No 2-frame floor here, unlike the two sweeps: one frame is a legitimate single
        // placement of the mesh, and ArrayAlongPath's own ValidateArrayCount rejects zero.
        GeometryOps::FArrayAlongPathParams Params;
        Params.Frames = GetFrameList(P, TEXT("path"));
        OutResult = GeometryOps::ArrayAlongPath(Mesh, Params);
        return true;
    }

    // ---- path-driven ----
    if (Name == TEXT("sweep"))
    {
        GeometryOps::FSweepParams Params;
        Params.Steps = GetInt(P, TEXT("steps"), Params.Steps);
        Params.Twist = GetNumber(P, TEXT("twist"), Params.Twist);
        Params.ScaleStart = GetNumber(P, TEXT("scale_start"), Params.ScaleStart);
        Params.ScaleEnd = GetNumber(P, TEXT("scale_end"), Params.ScaleEnd);
        Params.bCap = GetBool(P, TEXT("cap"), Params.bCap);

        OutResult = ReadSweepProfile(P, Params.Profile);
        if (!OutResult.bSuccess)
        {
            return true;
        }

        OutResult = ReadSweepScaleCurve(P, Params.ScaleCurve);
        if (!OutResult.bSuccess)
        {
            return true;
        }

        // `path=` omitted is the documented vertical fallback and reaches the op with no samples.
        // `path=` PRESENT but too short is not - it would take the same fallback silently, which
        // is a different shape from the one the author wrote.
        GeometryOps::FSweepPath Path;
        if (HasValue(P, TEXT("path")))
        {
            Path.Samples = GetFrameList(P, TEXT("path"));
            OutResult = ValidatePathFrames(Path.Samples, TEXT("path"));
            if (!OutResult.bSuccess)
            {
                return true;
            }
        }

        GeometryOps::FSweepOutputs Outputs;
        OutResult = GeometryOps::Sweep(Mesh, Params, Path, Outputs);
        return true;
    }
    if (Name == TEXT("extrude_along_spline"))
    {
        GeometryOps::FExtrudeAlongSplineParams Params;
        Params.Segments = GetInt(P, TEXT("segments"), Params.Segments);
        Params.Twist = GetNumber(P, TEXT("twist"), Params.Twist);
        Params.ScaleStart = GetNumber(P, TEXT("scale_start"), Params.ScaleStart);
        Params.ScaleEnd = GetNumber(P, TEXT("scale_end"), Params.ScaleEnd);
        Params.bCap = GetBool(P, TEXT("cap"), Params.bCap);

        OutResult = ReadSweepProfile(P, Params.Profile);
        if (!OutResult.bSuccess)
        {
            return true;
        }

        OutResult = ReadSweepScaleCurve(P, Params.ScaleCurve);
        if (!OutResult.bSuccess)
        {
            return true;
        }

        const TArray<FTransform> PathSamples = GetFrameList(P, TEXT("path"));
        OutResult = ValidatePathFrames(PathSamples, TEXT("path"));
        if (!OutResult.bSuccess)
        {
            return true;
        }

        OutResult = GeometryOps::ExtrudeAlongSpline(Mesh, Params, PathSamples);
        return true;
    }

    // ---- topology ----
    if (Name == TEXT("bridge"))
    {
        // `subdivisions` is gone from both front-ends. It was declared by geometry.bridge and by
        // this op table and consumed by neither - FBridgeParams has never carried a field for it
        // and the strip builder emits one quad per loop vertex pair regardless - so every value
        // an author wrote produced the identical mesh. geometry.bridge still ECHOES a
        // `subdivisions` response field, because removing an echoed field is a response change
        // and this is not one; it can no longer be set, so it echoes its former default.
        GeometryOps::FBridgeParams Params;
        Params.EdgeGroupA = GetInt(P, TEXT("edge_group_a"), Params.EdgeGroupA);
        Params.EdgeGroupB = GetInt(P, TEXT("edge_group_b"), Params.EdgeGroupB);
        GeometryOps::FBridgeOutputs Outputs;
        OutResult = GeometryOps::Bridge(Mesh, Params, Outputs);
        return true;
    }
    if (Name == TEXT("edge_split"))
    {
        GeometryOps::FEdgeSplitParams Params;
        Params.EdgeIndices = GetIndexList(P, TEXT("edges"));
        if (Params.EdgeIndices.Num() == 0 && HasValue(P, TEXT("edge_index")))
        {
            Params.EdgeIndices.Add(GetInt(P, TEXT("edge_index"), 0));
        }
        Params.SplitFactor = GetNumber(P, TEXT("split_factor"), Params.SplitFactor);
        Params.bWeldVertices = GetBool(P, TEXT("weld_vertices"), Params.bWeldVertices);
        Params.WeldTolerance = GetNumber(P, TEXT("weld_tolerance"), Params.WeldTolerance);
        GeometryOps::FEdgeSplitOutputs Outputs;
        OutResult = GeometryOps::EdgeSplit(Mesh, Params, Outputs);
        return true;
    }

    // ---- part-local transform ----
    if (Name == TEXT("transform"))
    {
        UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh(
            Mesh, ReadTransformParams(P), /*bFixOrientationForNegativeScale=*/true, nullptr);
        OutResult = OkWithCounts(Mesh);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Booleans
// ---------------------------------------------------------------------------

// The helpers below exist for ONE diagnostic - PWMODEL_BOOLEAN_NO_EFFECT - and for the test that
// decides whether it fires. Both need the two operands MEASURED, and the tool has to be measured
// before RunBoolean marks it garbage.

// Axis-aligned extent of a mesh. Invalid (IsValid == 0) rather than zero-sized when the mesh
// carries no geometry: 0 is a real coordinate and "nothing to measure" is not. Same conversion
// the per-part and model-wide bounds use.
FBox BooleanOperandBounds(UDynamicMesh* Mesh)
{
    if (!Mesh)
    {
        return FBox(ForceInit);
    }

    const UE::Geometry::FAxisAlignedBox3d Box = Mesh->GetMeshRef().GetBounds();
    if (Box.IsEmpty())
    {
        return FBox(ForceInit);
    }
    return FBox(FVector(Box.Min), FVector(Box.Max));
}

FString DescribeBooleanOperandBounds(const FBox& Box)
{
    if (Box.IsValid == 0)
    {
        return TEXT("nothing - it produced no geometry");
    }
    return FString::Printf(TEXT("(%.4g, %.4g, %.4g)..(%.4g, %.4g, %.4g)"),
        Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z);
}

// The clause that says where the two operands sit RELATIVE TO EACH OTHER, and the reason this
// is a function rather than a constant sentence: the diagnostic used to assert unconditionally
// that the operands do not intersect, which is strictly stronger than the test it was made
// from and is false whenever they do. Three cases, one true sentence each.
FString DescribeBooleanOperandMiss(const FBox& AccumulatedBounds, const FBox& ToolBounds)
{
    if (AccumulatedBounds.IsValid == 0 || ToolBounds.IsValid == 0)
    {
        return TEXT("One of the two carries no geometry at all, so there was nothing to cut, or nothing to cut with. ");
    }

    // Positive per axis where the boxes overlap, negative by the clear distance between them
    // where they do not - so one vector answers both branches, and all three positive is
    // exactly FBox::Intersect.
    const FVector Overlap = FVector::Min(AccumulatedBounds.Max, ToolBounds.Max)
                          - FVector::Max(AccumulatedBounds.Min, ToolBounds.Min);

    if (Overlap.X <= 0.0 || Overlap.Y <= 0.0 || Overlap.Z <= 0.0)
    {
        const FVector Gap(FMath::Max(0.0, -Overlap.X),
                          FMath::Max(0.0, -Overlap.Y),
                          FMath::Max(0.0, -Overlap.Z));
        return FString::Printf(
            TEXT("Their bounding boxes do not meet - the clear gap between them is (%.4g, %.4g, %.4g) uu - so the ")
            TEXT("two solids cannot touch, and that gap is how far the block has to move. "),
            Gap.X, Gap.Y, Gap.Z);
    }

    return FString::Printf(
        TEXT("Their bounding boxes DO overlap, by %.4g x %.4g x %.4g uu, so this is not a miss by distance: the ")
        TEXT("solids inside those boxes share no volume. A cutter that only grazes a face, one that sits in the ")
        TEXT("hollow of a shell, and one whose box interpenetrates the target's around a corner all look like this. "),
        Overlap.X, Overlap.Y, Overlap.Z);
}

// Did the boolean move any geometry?
//
// FOpResult::bChanged ALONE cannot answer that. It is a TRIANGLE-COUNT DELTA and nothing else -
// the assignment at the end of GeometryOps::Boolean, which is deliberate and pinned there
// because bChanged is the value the four geometry.* boolean verbs publish as `changed`, so it
// cannot be widened at the source. A THROUGH-CUT turns a box into a smaller box: 12 triangles
// before, 12 after, 40% of the material gone and a delta of zero. That aborted the part with a
// diagnostic saying the operands did not intersect, on a cut that had worked perfectly.
//
// The engine was never involved in the refusal. An engine boolean that declines fails
// bSuccess, which is checked ABOVE this and reports PWMODEL_OP_FAILED with the engine's own
// text; reaching here means ApplyMeshBoolean ran and succeeded. Giving the TARGET a `segments=`
// "fixed" the same document only because a subdivided target lands on a different triangle
// count, and giving the TOOL one never did because the tool's count is not in this test.
//
// Enclosed volume is the measure that matches what the diagnostic claims: material removed or
// added moves it, retriangulating the same solid does not. Signed, and on an open mesh it is
// the integral of an unclosed surface - meaningless as a volume, still valid as the change
// detector that is all it is used for here.
//
// OR, not AND: every op that passed on the triangle delta still passes, so this only ever
// weakens the test. The disjoint-boolean abort that twelve green-but-broken examples exist to
// justify is untouched - a tool that misses moves neither count nor volume.
bool BooleanMovedGeometry(const GeometryOps::FOpResult& OpResult, double VolumeBefore, double VolumeAfter)
{
    if (OpResult.bChanged)
    {
        return true;
    }

    // Relative, so it means the same thing on a 10 uu part and a 10000 uu one, with an absolute
    // floor for a part whose enclosed volume is near zero. Re-summing the same solid in a
    // different triangle order moves the last few bits, nothing near this.
    const double Scale = FMath::Max(FMath::Abs(VolumeBefore), FMath::Abs(VolumeAfter));
    const double Epsilon = FMath::Max(1e-6 * Scale, UE_DOUBLE_KINDA_SMALL_NUMBER);
    return FMath::Abs(VolumeAfter - VolumeBefore) > Epsilon;
}

bool FCompiler::RunBoolean(const FPwOp& Op, UDynamicMesh* Mesh, bool bNested)
{
    // ---- the slot the faces this operation CREATES will carry -------------------------------
    //
    // Decided BEFORE the block runs, because the block's untagged ops need it: they tag the tool
    // mesh with it, and the operation then carries those ids into the result. Two sources, in
    // order:
    //
    //   `material=` ON THE OP. The author naming the new faces - the walls a subtract opens, the
    //     tool surface a union keeps. This is the only spelling for a cut's walls, which have no
    //     op of their own to tag.
    //   THE GEOMETRY THE BOOLEAN IS APPLIED TO. Untagged, new faces join the surface they were cut
    //     into or merged onto. Unambiguous where that geometry speaks with one voice; where it
    //     does not, the slot most of it is on, reported by
    //     PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS below.
    //
    // Neither available leaves INDEX_NONE, and the new faces keep material ID 0 - which is
    // whichever slot the first part in the document tagged, and is reported rather than repaired.
    //
    // NONE OF IT APPLIES to a boolean building COLLISION geometry. A `collision { hull { … } }`
    // body is run through the part vocabulary (PwModelParser.cpp NestedSink), so a boolean inside one
    // reaches here with the same ops - but its geometry becomes a convex hull and never a render
    // section, so resolving a slot from it would put an empty section in the asset for something
    // no triangle carries. Told apart by the two flags together: a boolean nested inside another
    // BOOLEAN's block arrives with BooleanBlock.bActive already set, a hull-nested one does not.
    const bool bTagsBlockGeometry = !bNested || BooleanBlock.bActive;

    TMap<int32, int32> TargetMaterials;
    if (bTagsBlockGeometry)
    {
        CountMaterialTriangles(Mesh, TargetMaterials);
    }

    const bool bTagged = bTagsBlockGeometry
        && HasValue(Op.Params, TEXT("material"))
        && !GetString(Op.Params, TEXT("material")).IsEmpty();

    // Slots allocated from here on were opened by this op or by its block. Used after the
    // operation to find a tag that reached no triangle at all.
    const int32 SlotMark = SlotNames.Num();

    int32 NewFaceSlot = INDEX_NONE;
    if (bTagged)
    {
        const FString SlotName = GetString(Op.Params, TEXT("material"));
        if (SlotName == DefaultSlotName)
        {
            bDefaultSlotExplicitlyTagged = true;
        }
        NewFaceSlot = ResolveSlot(SlotName);
    }
    else if (bTagsBlockGeometry)
    {
        NewFaceSlot = DominantMaterialSlot(TargetMaterials);
    }

    // The tool mesh is built by the block's own ops, recursively - a nested boolean inside
    // a boolean is just another block, and gets its own rule. Save-and-restore rather than
    // clear-on-exit for exactly that reason.
    TStrongObjectPtr<UDynamicMesh> Tool(NewTransientMesh());

    const FBooleanBlockMaterial OuterBlockMaterial = BooleanBlock;
    BooleanBlock.bActive = bTagsBlockGeometry;
    BooleanBlock.FallbackSlot = NewFaceSlot;
    BooleanBlock.bFallbackUsed = false;

    const bool bBlockOk = RunOps(Op.Children, Tool.Get(), /*bNested=*/true);
    const bool bFallbackUsed = BooleanBlock.bFallbackUsed;
    BooleanBlock = OuterBlockMaterial;

    if (!bBlockOk)
    {
        Tool->MarkAsGarbage();
        return false;
    }

    // Measured HERE and not at the diagnostic below, because neither value is readable from
    // there: `Tool->MarkAsGarbage()` runs before it, and the op mutates Mesh in place. Two
    // cheap passes over meshes a boolean is about to walk anyway.
    const FBox ToolBounds = BooleanOperandBounds(Tool.Get());
    const double VolumeBefore = GeometryUtils::MeasureMeshOrientation(Mesh).SignedVolume;

    // Both operands out of material ID 0 for the duration, so a face the operation creates from
    // NEITHER of them - a hole fill, and `fill_holes` is on by default on all four ops - is
    // identifiable afterwards as the only thing still on 0. See ReserveMaterialIDZero for why a
    // triangle-id diff cannot do this job on a boolean. Skipped for collision geometry, which has
    // no material to encode and must not be touched at all.
    TMap<int32, int32> ResultMaterials;
    GeometryOps::FOpResult OpResult;

    if (Op.OpName == TEXT("trim"))
    {
        GeometryOps::FTrimParams Params;
        Params.bKeepInside = GetBool(Op.Params, TEXT("keep_inside"), Params.bKeepInside);
        Params.bFillHoles = GetBool(Op.Params, TEXT("fill_holes"), Params.bFillHoles);
        Params.bSimplifyOutput = GetBool(Op.Params, TEXT("simplify_output"), Params.bSimplifyOutput);
        // simplify_planar_tolerance is not published on these ops - the engine ignores it for
        // booleans. See GeometryOps::FBooleanParams.
        Params.bAllowEmptyResult = GetBool(Op.Params, TEXT("allow_empty_result"), Params.bAllowEmptyResult);
        // OutputTransformSpace is left at its default: both transforms below are identity, so
        // all three of its values name the same space. See the op table entry.
        if (bTagsBlockGeometry)
        {
            ReserveMaterialIDZero(Mesh);
            ReserveMaterialIDZero(Tool.Get());
        }
        OpResult = GeometryOps::Trim(Mesh, FTransform::Identity, Tool.Get(), FTransform::Identity, Params);
        if (bTagsBlockGeometry)
        {
            ReleaseMaterialIDZero(Mesh, NewFaceSlot, ResultMaterials);
        }
    }
    else
    {
        EGeometryScriptBooleanOperation Operation = EGeometryScriptBooleanOperation::Union;
        if (Op.OpName == TEXT("subtract"))
        {
            Operation = EGeometryScriptBooleanOperation::Subtract;
        }
        else if (Op.OpName == TEXT("intersection"))
        {
            Operation = EGeometryScriptBooleanOperation::Intersection;
        }

        GeometryOps::FBooleanParams Params;
        Params.bFillHoles = GetBool(Op.Params, TEXT("fill_holes"), Params.bFillHoles);
        Params.bSimplifyOutput = GetBool(Op.Params, TEXT("simplify_output"), Params.bSimplifyOutput);
        // simplify_planar_tolerance is not published on these ops - the engine ignores it for
        // booleans. See GeometryOps::FBooleanParams.
        Params.bAllowEmptyResult = GetBool(Op.Params, TEXT("allow_empty_result"), Params.bAllowEmptyResult);
        // OutputTransformSpace is left at its default - see the trim branch above.

        // Both transforms are identity: every op's at/rotate/scale is already baked into
        // the vertices of the mesh it produced.
        TArray<TUniquePtr<UE::Geometry::FDynamicMesh3>> ToolComponents;
        const bool bSequentialSubtract = Op.OpName == TEXT("subtract")
            && SplitToolIntoConnectedComponents(Tool->GetMeshPtr(), ToolComponents);
        if (bSequentialSubtract)
        {
            // A disconnected subtract tool is set subtraction, so applying each shell in turn
            // preserves the shape while avoiding one global cut arrangement for every shell.
            const int32 TargetTrianglesBefore = Mesh->GetTriangleCount();
            const int32 TargetVerticesBefore = GeometryUtils::GetMeshVertexCount(Mesh);
            TArray<FString> ComponentWarnings;
            TStrongObjectPtr<UDynamicMesh> ComponentTool(NewTransientMesh());
            bool bComponentsSucceeded = true;

            for (int32 ComponentIndex = 0; ComponentIndex < ToolComponents.Num(); ++ComponentIndex)
            {
                TUniquePtr<UE::Geometry::FDynamicMesh3>& ToolComponent = ToolComponents[ComponentIndex];
                ComponentTool->SetMesh(MoveTemp(*ToolComponent));
                if (bTagsBlockGeometry)
                {
                    ReserveMaterialIDZero(Mesh);
                    ReserveMaterialIDZero(ComponentTool.Get());
                }

                const GeometryOps::FOpResult ComponentResult = GeometryOps::Boolean(
                    Mesh, FTransform::Identity, ComponentTool.Get(), FTransform::Identity,
                    Operation, Params);

                TMap<int32, int32> ComponentMaterials;
                if (bTagsBlockGeometry)
                {
                    ReleaseMaterialIDZero(Mesh, NewFaceSlot, ComponentMaterials);
                }
                ResultMaterials = MoveTemp(ComponentMaterials);

                for (const FString& Warning : ComponentResult.Warnings)
                {
                    ComponentWarnings.Add(FString::Printf(
                        TEXT("%s (subtract component %d of %d)"),
                        *Warning, ComponentIndex + 1, ToolComponents.Num()));
                }

                if (!ComponentResult.bSuccess)
                {
                    OpResult = ComponentResult;
                    OpResult.Warnings = MoveTemp(ComponentWarnings);
                    OpResult.ErrorMessage = FString::Printf(
                        TEXT("%s (subtract component %d of %d failed)"),
                        *OpResult.ErrorMessage, ComponentIndex + 1, ToolComponents.Num());
                    bComponentsSucceeded = false;
                    break;
                }
            }

            ComponentTool->MarkAsGarbage();
            if (bComponentsSucceeded)
            {
                OpResult = GeometryOps::FOpResult::Ok();
                OpResult.Warnings = MoveTemp(ComponentWarnings);
                OpResult.TrianglesBefore = TargetTrianglesBefore;
                OpResult.TrianglesAfter = Mesh->GetTriangleCount();
                OpResult.VerticesBefore = TargetVerticesBefore;
                OpResult.VerticesAfter = GeometryUtils::GetMeshVertexCount(Mesh);
                OpResult.bChanged = OpResult.TrianglesAfter != OpResult.TrianglesBefore;
            }
        }
        else
        {
            if (bTagsBlockGeometry)
            {
                ReserveMaterialIDZero(Mesh);
                ReserveMaterialIDZero(Tool.Get());
            }
            OpResult = GeometryOps::Boolean(
                Mesh, FTransform::Identity, Tool.Get(), FTransform::Identity, Operation, Params);
            if (bTagsBlockGeometry)
            {
                ReleaseMaterialIDZero(Mesh, NewFaceSlot, ResultMaterials);
            }
        }
    }

    Tool->MarkAsGarbage();

    ReportOpResult(Op, OpResult);
    if (!OpResult.bSuccess)
    {
        return false;
    }

    // The engine returns the target unmodified when the two meshes are disjoint. What makes
    // that observable is BooleanMovedGeometry above, not FOpResult::bChanged on its own - see
    // its comment for why the triangle delta alone aborted parts over cuts that had worked.
    //
    // ERROR, not a warning, and it aborts the part. This shipped as a warning and twelve
    // examples went out green because of it: pipe_junction's two `subtract` bores both
    // reported "changed nothing", model.compile still answered success:true, and the asset
    // is a pipe with no hole through it. A boolean is not advice - the author asked for
    // material to be removed, none was, and the geometry that reaches the asset is not the
    // geometry the document describes. There is no repair downstream and no reading of the
    // result that is still correct, which is the line between this and the warnings around
    // it (PWMODEL_UV_CHANNEL_FILLED repairs the mesh; PWMODEL_MATERIAL_ID_OUT_OF_RANGE pads
    // the slot list; both leave a valid asset).
    //
    // `return false` rather than Error()-and-continue: Error() alone would still let
    // Run reach CreateAsset and only fail the RESULT afterwards, writing the broken
    // .uasset and then reporting failure - and FPwModelCompiler's header promises the
    // opposite ("Never partially creates: every failure short-circuits before
    // CreateStaticMesh"). Returning false unwinds RunOps -> BuildParts -> Run, so nothing
    // is created, exactly like an op that failed outright.
    //
    // Scope note: this is also the path a `collision { hull { … } }` body takes (RunOps
    // with bNested), so a no-effect boolean inside a hull fails the hull the same way,
    // which is the same defect one level down.
    const double VolumeAfter = GeometryUtils::MeasureMeshOrientation(Mesh).SignedVolume;
    if (!BooleanMovedGeometry(OpResult, VolumeBefore, VolumeAfter))
    {
        // Both boxes, and their separation. The message used to name only the op, and the one
        // remedy it offered - move the block - could not be acted on from it: the part is
        // aborted, so the response's parts[] holds only the parts that already succeeded, and
        // the tool never becomes geometry an author can measure at all. Recovering where the
        // accumulated geometry had got to cost a delete-the-op-and-revalidate cycle per
        // attempt. It is in hand here; it is now in the sentence.
        const FBox AccumulatedBounds = BooleanOperandBounds(Mesh);

        Error(PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT, Op,
            FString::Printf(TEXT("'%s' changed nothing: the geometry accumulated so far still spans %s, with %d triangles ")
                            TEXT("and an enclosed volume of %.6g uu3, unchanged by the op - and the block's geometry spans %s. %s")
                            TEXT("Move or resize the block's geometry so it overlaps, or delete the op - a boolean that removes ")
                            TEXT("nothing leaves the model without the feature it describes."),
                *Op.OpName,
                *DescribeBooleanOperandBounds(AccumulatedBounds),
                Mesh->GetTriangleCount(),
                VolumeAfter,
                *DescribeBooleanOperandBounds(ToolBounds),
                *DescribeBooleanOperandMiss(AccumulatedBounds, ToolBounds)));
        return false;
    }

    // Boolean solvers can leave coincident boundary edges and zero-area triangles at the cut
    // seams. Repair only the target produced by this boolean: DeleteOnly avoids the modeling
    // repair pass changing valid nearby geometry, while the weld reconnects coincident seam
    // vertices before the final degenerate scan. Keep the window visible to model.compile and
    // model.validate; the ordinary meshTriangleCount remains the post-repair mesh count.
    const int32 CleanupTrianglesBefore = Mesh->GetTriangleCount();

    const FBox CleanupTargetBounds = BooleanOperandBounds(Mesh);
    const double TargetExtent = CleanupTargetBounds.IsValid != 0
        ? CleanupTargetBounds.GetSize().GetAbsMax() : 0.0;
    const double ToolExtent = ToolBounds.IsValid != 0 ? ToolBounds.GetSize().GetAbsMax() : 0.0;
    const double CharacteristicExtent = FMath::Max(
        FMath::Max(TargetExtent, ToolExtent) * PwModelDocumentToUnrealUnitScale,
        PwModelBooleanCleanupMinLength);
    const double CleanupLength = FMath::Clamp(
        CharacteristicExtent * PwModelBooleanCleanupRelativeLength,
        PwModelBooleanCleanupMinLength,
        PwModelBooleanCleanupMaxLength);
    const double CleanupArea = FMath::Clamp(
        CharacteristicExtent * CharacteristicExtent * PwModelBooleanCleanupRelativeArea,
        PwModelBooleanCleanupMinArea,
        PwModelBooleanCleanupMaxArea);

    GeometryOps::FWeldVerticesParams WeldParams;
    WeldParams.Tolerance = CleanupLength;
    WeldParams.bOnlyUniquePairs = true;
    const GeometryOps::FOpResult WeldResult = GeometryOps::WeldVertices(Mesh, WeldParams);
    const int32 DegenerateCleanupTrianglesBefore = Mesh->GetTriangleCount();

    GeometryOps::FRemoveDegeneratesParams DegenerateParams;
    DegenerateParams.Mode = GeometryOps::ERepairMeshMode::DeleteOnly;
    DegenerateParams.MinTriangleArea = CleanupArea;
    DegenerateParams.MinEdgeLength = CleanupLength;
    DegenerateParams.bCompactOnCompletion = true;
    const GeometryOps::FOpResult DegenerateResult =
        GeometryOps::RemoveDegenerates(Mesh, DegenerateParams);

    OpResult.Warnings.Append(WeldResult.Warnings);
    OpResult.Warnings.Append(DegenerateResult.Warnings);
    if (!WeldResult.bSuccess || !DegenerateResult.bSuccess)
    {
        const GeometryOps::FOpResult& FailedRepair = !WeldResult.bSuccess
            ? WeldResult
            : DegenerateResult;
        ReportOpResult(Op, FailedRepair);
        return false;
    }

    Result.TrianglesBefore = CleanupTrianglesBefore;
    Result.TrianglesAfter = Mesh->GetTriangleCount();
    Result.SliversRemoved = FMath::Max(
        0, DegenerateCleanupTrianglesBefore - Result.TrianglesAfter);

    // ---- what the material rule could not answer ---------------------------------------------
    //
    // Raised HERE because nothing downstream can. Inheriting allocates no slot, so the slot count
    // is unchanged, the mesh is valid, and `materialSlotList`, `unboundSlots` and
    // static_mesh.describe all report the model as correct - the only place the choice shows is
    // which section the new faces render in.
    if (!bTagsBlockGeometry)
    {
        // Collision geometry - see bTagsBlockGeometry. Nothing here has a material to be wrong
        // about, so nothing is reported.
        return true;
    }

    if (!bTagged && bFallbackUsed && TargetMaterials.Num() > 1)
    {
        TArray<int32> Candidates;
        TargetMaterials.GetKeys(Candidates);
        Candidates.Sort();

        TArray<FString> Named;
        Named.Reserve(Candidates.Num());
        for (const int32 Candidate : Candidates)
        {
            Named.Add(DescribeSlot(Candidate));
        }

        Warn(PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS, Op,
            FString::Printf(
                TEXT("'%s' produced faces onto geometry carrying %d different material slots ")
                TEXT("(%s), so there is no single slot for them to inherit. They were put on %s, ")
                TEXT("the slot most of that geometry is on. Write material=\"<Slot>\" on '%s' to ")
                TEXT("name the faces it creates, or on a generator inside its block to tag that ")
                TEXT("generator's own surface."),
                *Op.OpName, Candidates.Num(), *FString::Join(Named, TEXT(", ")),
                *DescribeSlot(NewFaceSlot), *Op.OpName));
    }
    else if (NewFaceSlot == INDEX_NONE && ResultMaterials.FindRef(0) > 0)
    {
        Warn(PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS, Op,
            FString::Printf(
                TEXT("'%s' produced %d triangle(s) with no geometry in front of it to inherit a ")
                TEXT("material slot from, so they keep material ID 0. That is not a neutral ")
                TEXT("default: slots are a MODEL-WIDE table in first-use order, so ID 0 is ")
                TEXT("whichever slot the first part in the document tagged and this geometry ")
                TEXT("ships in another part's material. Write material=\"<Slot>\" on '%s'."),
                *Op.OpName, ResultMaterials.FindRef(0), *Op.OpName));
    }

    // A `material=` - on the op or on a generator inside its block - that opened a slot no
    // triangle of the result carries. The realistic source is a tool solid the operation discards
    // entirely, most often one that misses the target while its siblings in the block do not:
    // PWMODEL_BOOLEAN_NO_EFFECT cannot see that, because the boolean as a whole did change the
    // geometry. NOT `trim` - it dispatches the same ApplyMeshBoolean as `subtract`, so its tool
    // surface becomes the cut face and a tag on it lands on real triangles.
    TArray<FString> UnusedTags;
    for (int32 SlotIndex = SlotMark; SlotIndex < SlotNames.Num(); ++SlotIndex)
    {
        if (ResultMaterials.FindRef(SlotIndex) == 0)
        {
            UnusedTags.Add(DescribeSlot(SlotIndex));
        }
    }
    if (UnusedTags.Num() > 0)
    {
        Warn(PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_UNUSED, Op,
            FString::Printf(
                TEXT("'%s' opened material slot(s) %s that no triangle of the result carries, so ")
                TEXT("the tag did nothing and the asset gains an empty section. A boolean's ")
                TEXT("material= names the faces the operation CREATES, and a tool solid the ")
                TEXT("operation discards produces none - most often one that misses the target ")
                TEXT("while the rest of the block hits it, which is why the op itself did not ")
                TEXT("report doing nothing. Check that solid's placement, drop the tag, or move ")
                TEXT("the geometry it was meant for out of the block."),
                *Op.OpName, *FString::Join(UnusedTags, TEXT(", "))));
    }

    return true;
}

// ---------------------------------------------------------------------------
// Op execution
// ---------------------------------------------------------------------------

void FCompiler::ReportOpResult(const FPwOp& Op, const GeometryOps::FOpResult& OpResult,
                               bool bModelLevel)
{
    for (const FString& Warning : OpResult.Warnings)
    {
        // TRANSLATED, not forwarded. GeometryOps labels its clamps with the RPC's parameter name
        // - that is the name a `geometry.*` caller passed and greps its response for - and this
        // used to re-emit the text verbatim with only an op prefix, so a document that wrote
        // `segments=(6, 5, 4)` was told about `widthSegments`, a name `.pwmodel` does not have.
        // One label cannot serve both surfaces, so the ops layer keeps the RPC spelling and the
        // rewrite happens here, at the one place a warning crosses into the document's
        // vocabulary. PwModelWarningNames (PwModelParser.h) holds the table and the reasoning.
        FString Message = FString::Printf(TEXT("'%s': %s"), *Op.OpName,
            *PwModelWarningNames::Translate(Op.OpName, Warning));
        if (bModelLevel)
        {
            ModelWarn(PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING, Op, MoveTemp(Message));
        }
        else
        {
            Warn(PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING, Op, MoveTemp(Message));
        }
    }

    if (!OpResult.bSuccess)
    {
        // TRANSLATED TOO. This branch forwarded ErrorMessage verbatim while the loop directly
        // above it translated, so one op could report a clamp in the document's vocabulary and
        // a refusal in the RPC's - `'pipe' failed [INVALID_PARAMS]: pipe requires
        // 0 < innerRadius < outerRadius`, naming two parameters no `.pwmodel` document has.
        // docs/pwmodel-format.md states normatively that a diagnostic names the parameter as
        // this format spells it; a failure is the diagnostic an author is most likely to read.
        //
        // TranslateMessage rather than Translate, because a failure names its parameter
        // MID-SENTENCE - the leading-token rule that is exactly right for `%s clamped from …`
        // cannot see it. PwModelParser.h carries why the two rules differ and what bounds the
        // looser one's prose risk.
        FString Message = FString::Printf(TEXT("'%s' failed [%s]: %s"), *Op.OpName,
            *OpResult.ErrorCode,
            *PwModelWarningNames::TranslateMessage(Op.OpName, OpResult.ErrorMessage));
        if (bModelLevel)
        {
            ModelError(PwModelDiagnosticCodes::PWMODEL_OP_FAILED, Op, MoveTemp(Message));
        }
        else
        {
            Error(PwModelDiagnosticCodes::PWMODEL_OP_FAILED, Op, MoveTemp(Message));
        }
    }
}

bool FCompiler::ShouldCheckSelfIntersection(const FPwOp& Op) const
{
    const FPwModelOpSpec* Spec = PwModelOpTable::Find(Op.OpName, EPwModelOpContext::Part);
    if (Spec && Spec->bBoolean)
    {
        return true;
    }

    // Only operations that can fold, reconnect, or add triangles to one connected shell need an
    // AABB-tree rebuild. Global affine transforms preserve intersections; attribute-only edits,
    // deletions, compaction, and edge subdivision cannot create one. Arrays append independent
    // shells, and the published metric deliberately excludes crossings between shells.
    static const TSet<FString> IntersectionCapableOps = {
        TEXT("torus"), TEXT("arch"), TEXT("revolve"), TEXT("append_buffers"),
        TEXT("simplify_mesh"), TEXT("subdivide"), TEXT("remesh_uniform"), TEXT("poke"),
        TEXT("extrude"), TEXT("inset"), TEXT("outset"), TEXT("offset_faces"), TEXT("shell"),
        TEXT("bend"), TEXT("twist"), TEXT("taper"), TEXT("noise_deform"),
        TEXT("harmonic_deform"), TEXT("smooth"), TEXT("relax"), TEXT("stretch"),
        TEXT("spherify"), TEXT("cylindrify"), TEXT("weld_vertices"), TEXT("fill_holes"),
        TEXT("remove_degenerates"), TEXT("merge_vertices"), TEXT("set_vertex_position"),
        TEXT("self_union"), TEXT("mirror"), TEXT("sweep"),
        TEXT("extrude_along_spline"), TEXT("bridge")
    };
    return IntersectionCapableOps.Contains(Op.OpName);
}

void FCompiler::CheckSelfIntersectionAfterOp(const FPwOp& Op, UDynamicMesh* Mesh)
{
    if (!Mesh || SelfIntersectionDiagnosticParts.Contains(CurrentPartName)
        || !ShouldCheckSelfIntersection(Op))
    {
        return;
    }

    const GeometryUtils::FMeshSelfIntersection SelfIntersection =
        GeometryUtils::MeasureMeshSelfIntersection(Mesh);
    if (!SelfIntersection.bMeasured || SelfIntersection.PairCount == 0)
    {
        return;
    }

    SelfIntersectionDiagnosticParts.Add(CurrentPartName);
    bSelfIntersectionDiagnosticAttributed = true;
    const FString Where = SelfIntersection.bHasWitness
        ? FString::Printf(TEXT(" The first crossing found is at (%g, %g, %g)."),
            SelfIntersection.Witness.X, SelfIntersection.Witness.Y,
            SelfIntersection.Witness.Z)
        : FString();
    Warn(PwModelDiagnosticCodes::PWMODEL_SELF_INTERSECTING_SURFACE, Op,
        FString::Printf(
            TEXT("'%s' is the first operation whose output in part '%s' has %d%s self-intersecting "
                 "triangle pair(s) inside %d connected shell(s). The bevel/boolean/deformer output "
                 "should be repaired or its unsafe input skipped.%s"),
            *Op.OpName, *CurrentPartName, SelfIntersection.PairCount,
            SelfIntersection.bTruncated ? TEXT("+") : TEXT(""),
            SelfIntersection.SelfIntersectingComponents, *Where));
}

void FCompiler::RememberBevelOutputGroups(UDynamicMesh* Mesh, int32 FirstNewGroupID)
{
    if (!Mesh || FirstNewGroupID < 0 || !Mesh->GetMeshRef().HasTriangleGroups())
    {
        return;
    }

    Mesh->ProcessMesh([this, FirstNewGroupID](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (const int32 TriangleID : ReadMesh.TriangleIndicesItr())
        {
            const int32 GroupID = ReadMesh.GetTriangleGroup(TriangleID);
            if (GroupID >= FirstNewGroupID)
            {
                PriorBevelGroupIDs.Add(GroupID);
            }
        }
    });
}

bool FCompiler::RunOp(const FPwOp& Op, UDynamicMesh* Mesh, bool bNested)
{
    // Classification comes from the OP TABLE, which is the authority the parser validates
    // against. FPwModelOpSpec::bBoolean is kept separate from bAcceptsBlock precisely so this
    // is not a name comparison: the previous "has a block, or is one of these four names" test
    // would have turned any future block-taking non-boolean op into a silent Union, and any
    // future boolean whose name was not in the list into a generator lookup that fails.
    //
    // A null Spec means the op is not in the table at all. That is a hand-built AST rather than
    // a parsed document - the parser rejects unknown ops - so it falls through to the dispatchers
    // and lands on RunGenerator's PWSRC_UNKNOWN_OP, which is the one diagnostic for it.
    const FPwModelOpSpec* Spec = PwModelOpTable::Find(Op.OpName, EPwModelOpContext::Part);

    if (Spec && Spec->bBoolean)
    {
        // A boolean rewrites the whole mesh, so every recorded footprint stops describing where
        // anything is. Dropping them costs coverage after the first boolean in a part and is the
        // only way the warning can never make a claim it cannot support.
        AppendedFootprints.Reset();
        // A boolean can preserve group IDs from either operand while changing their topology, so
        // IDs remembered from an earlier bevel are no longer reliable evidence of a bevel strip.
        // This applies equally to a part and to a boolean tool mesh.
        PriorBevelGroupIDs.Reset();
        return RunBoolean(Op, Mesh, bNested);
    }

    // Generators before modifiers, from the same table flag rather than from which dispatcher
    // happens to answer first: the two name sets are disjoint today, and an op added to both
    // would otherwise silently resolve to whichever chain is tried first.
    if (Spec && Spec->bGenerator)
    {
        return RunGenerator(Op, Mesh, bNested);
    }

    // A modifier the OP TABLE lets carry `material=` is one that APPENDS triangles of its own -
    // `sweep` and `extrude_along_spline` today - and the engine stamps every one of them with
    // material ID 0. The flag is the authority rather than a name list for the same reason
    // bGenerator is above: an op given the parameter and not the tagging, or the reverse, would be
    // exactly the silent half-wiring this whole path exists to close. Generators never reach here.
    //
    // Scoped like RunGenerator's own tagging, and for the same three-scope reason: at part level
    // always, inside a BOOLEAN TOOL BLOCK too (the block's geometry becomes faces of the part, so
    // it needs a slot as much as a part-level sweep does, and ApplyModifierMaterialTag falls back
    // to the boolean's slot when there is nothing local to inherit), and never inside a hull body,
    // whose geometry carries no material at all.
    //
    // bSelfTagsMaterial is excluded because it means the op ALREADY tagged its own output per
    // face - `bevel`, from the two faces either side of each edge - and retagging would flatten
    // that to one slot for the whole chamfer.
    const bool bAppendsTaggableGeometry = Spec && Spec->bAcceptsMaterial && !Spec->bSelfTagsMaterial
        && (!bNested || BooleanBlock.bActive);
    FModifierMaterialSnapshot MaterialBefore;
    if (bAppendsTaggableGeometry)
    {
        SnapshotModifierMaterial(Mesh, MaterialBefore);
    }

    GeometryOps::FOpResult OpResult;
    if (DispatchModifier(Op, Mesh, OpResult))
    {
        // Same invalidation as the boolean branch, and for a wider reason: a modifier may MOVE
        // geometry (translate, rotate, bend) or MULTIPLY it (mirror, array_*), and either way
        // the boxes recorded for the ops before it no longer say where their geometry sits.
        AppendedFootprints.Reset();
        ReportOpResult(Op, OpResult);
        if (OpResult.bSuccess && bAppendsTaggableGeometry)
        {
            ApplyModifierMaterialTag(Op, Mesh, MaterialBefore);
        }
        return OpResult.bSuccess;
    }

    return RunGenerator(Op, Mesh, bNested);
}

bool FCompiler::RunOps(const TArray<FPwOp>& Ops, UDynamicMesh* Mesh, bool bNested)
{
    // RunOps is one-to-one with a mesh - a part, a boolean tool, a hull body - so scoping the
    // append footprints here is what keeps a nested block's ops from being compared against the
    // enclosing part's. Save-and-restore rather than clear-on-exit because the nesting is
    // recursive: RunBoolean calls back into RunOps for its block while the part's list is live.
    TArray<FAppendedFootprint> OuterFootprints = MoveTemp(AppendedFootprints);
    AppendedFootprints.Reset();

    TSet<int32> OuterPriorBevelGroups;
    if (bNested)
    {
        // A boolean tool (and a collision hull) is discarded or converted to another shape. Its
        // bevel history must not leak into the render part that owns the block.
        OuterPriorBevelGroups = MoveTemp(PriorBevelGroupIDs);
        PriorBevelGroupIDs.Reset();
    }

    bool bOk = true;
    for (const FPwOp& Op : Ops)
    {
        // A nested boolean tool becomes render geometry if its owning boolean succeeds, so it
        // needs the same first-offender and prior-bevel tracking as the enclosing part. A nested
        // collision hull has no active BooleanBlock and remains deliberately excluded.
        const bool bTrackRenderGeometry = !bNested || BooleanBlock.bActive;
        const bool bCaptureBevelGroups = bTrackRenderGeometry
            && Op.OpName == TEXT("bevel") && Mesh
            && Mesh->GetMeshRef().HasTriangleGroups();
        const int32 FirstNewBevelGroupID = bCaptureBevelGroups
            ? Mesh->GetMeshRef().MaxGroupID()
            : INDEX_NONE;
        const bool bOpOk = RunOp(Op, Mesh, bNested);
        if (!bOpOk)
        {
            bOk = false;
            break;
        }

        if (bTrackRenderGeometry)
        {
            CheckSelfIntersectionAfterOp(Op, Mesh);
            if (bCaptureBevelGroups)
            {
                RememberBevelOutputGroups(Mesh, FirstNewBevelGroupID);
            }
        }
    }

    AppendedFootprints = MoveTemp(OuterFootprints);
    if (bNested)
    {
        PriorBevelGroupIDs = MoveTemp(OuterPriorBevelGroups);
    }
    return bOk;
}

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

bool FCompiler::RejectReservedConstructs(const FPwModelDocument& Document)
{
    // The payoff for reserving the keywords: an author who writes a skeleton or animation
    // block gets a format-specific signpost diagnostic instead of PWSRC_UNKNOWN_OP. A
    // `use skeleton` line is intentionally not handled here; it is the live skin reference
    // consumed by ResolveAndValidateSkin below.
    for (const FPwModelReservedBlock& Block : Document.ReservedBlocks)
    {
        const bool bAnimation = Block.Keyword == TEXT("animation");
        const TCHAR* FormatExtension = bAnimation ? TEXT(".pwanim") : TEXT(".pwskel");
        const TCHAR* DocPage = bAnimation ? TEXT("docs/pwanim-format.md") : TEXT("docs/pwskel-format.md");
        const FString Message = FString::Printf(
            TEXT("'%s' is a signpost for the %s format, not a .pwmodel construct; see %s."),
            *Block.Keyword, FormatExtension, DocPage);
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_CONSTRUCT_IN_WRONG_FORMAT,
            Block.Line, Block.Column,
            Message,
            FString());
    }
    return Document.ReservedBlocks.Num() == 0;
}

bool FCompiler::ResolveAndValidateSkin(const FPwModelDocument& Document)
{
    const FPwUse* SkeletonUse = Document.Uses.FindByPredicate(
        [](const FPwUse& Use) { return Use.Kind == TEXT("skeleton"); });
    const bool bHasSkinRule = Document.Skin.IsSet();

    bool bHasBoneBinding = false;
    for (const FPwModelPart& Part : Document.Parts)
    {
        bHasBoneBinding |= !Part.BoneBinding.IsEmpty();
    }

    if (!SkeletonUse)
    {
        if (!bHasBoneBinding && !bHasSkinRule)
        {
            return true;
        }

        for (const FPwModelPart& Part : Document.Parts)
        {
            if (Part.BoneBinding.IsEmpty())
            {
                continue;
            }
            AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_NO_SKELETON,
                Part.Line, Part.Column,
                FString::Printf(TEXT("Part '%s' binds bone '%s', but the document has no 'use skeleton from \"<asset path>\"' reference."),
                    *Part.Name, *Part.BoneBinding), Part.Name);
        }
        if (bHasSkinRule)
        {
            const FPwOp& Skin = Document.Skin.GetValue();
            AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_NO_SKELETON,
                Skin.Line, Skin.Column,
                TEXT("A 'skin' block requires a 'use skeleton from \"<asset path>\"' reference."),
                FString());
        }
        return false;
    }

    bSkeletalOutput = true;
    Result.bSkeletal = true;

    PwSkeletonRef::FDiagnosticCodes SkeletonCodes;
    SkeletonCodes.NotAnAssetPath = PwModelDiagnosticCodes::PWMODEL_SKELETON_NOT_AN_ASSET_PATH;
    SkeletonCodes.NotFound = PwModelDiagnosticCodes::PWMODEL_SKELETON_NOT_FOUND;
    SkeletonCodes.WrongKind = PwModelDiagnosticCodes::PWMODEL_SKELETON_WRONG_KIND;
    SkeletonCodes.HasNoBones = PwModelDiagnosticCodes::PWMODEL_SKELETON_HAS_NO_BONES;

    const PwSkeletonRef::FResult Resolved = PwSkeletonRef::Resolve(
        *SkeletonUse, SkeletonCodes, Result.Diagnostics);
    if (!Resolved.IsResolved())
    {
        return false;
    }

    Skeleton = Resolved.Skeleton;
    SkeletonPath = Resolved.ObjectPath;
    Result.SkeletonPath = SkeletonPath;

    bool bValid = true;
    if (Document.Collision.IsSet())
    {
        const FPwModelCollision& Collision = Document.Collision.GetValue();
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_COLLISION_ON_SKELETAL,
            Collision.Line, Collision.Column,
            TEXT("A skeletal model cannot carry a 'collision' block. Compile the collision as a separate static model or remove the block."),
            FString());
        bValid = false;
    }
    if (Document.Lightmap.IsSet())
    {
        const FPwOp& Lightmap = Document.Lightmap.GetValue();
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_LIGHTMAP_ON_SKELETAL,
            Lightmap.Line, Lightmap.Column,
            TEXT("A skeletal model cannot carry a 'lightmap' block; skeletal asset lightmap data is not authored by this format."),
            FString());
        bValid = false;
    }

    const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
    TArray<FString> BoneNames;
    BoneNames.Reserve(ReferenceSkeleton.GetRawBoneNum());
    for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetRawBoneNum(); ++BoneIndex)
    {
        BoneNames.Add(ReferenceSkeleton.GetBoneName(BoneIndex).ToString());
    }

    for (const FPwModelPart& Part : Document.Parts)
    {
        if (Part.BoneBinding.IsEmpty())
        {
            continue;
        }

        if (ReferenceSkeleton.FindBoneIndex(FName(*Part.BoneBinding)) != INDEX_NONE)
        {
            continue;
        }

        TArray<FString> Suggestions;
        const FString Closest = PwSuggest::Closest(
            Part.BoneBinding, TArrayView<const FString>(BoneNames));
        if (!Closest.IsEmpty())
        {
            Suggestions.Add(Closest);
        }
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_BONE_NOT_FOUND,
            Part.Line, Part.Column,
            FString::Printf(TEXT("Bone '%s' is not present in skeleton '%s'."),
                *Part.BoneBinding, *SkeletonPath), Part.Name, MoveTemp(Suggestions));
        bValid = false;
    }

    if (!bHasSkinRule && !bHasBoneBinding)
    {
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_UNBOUND,
            SkeletonUse->Line, SkeletonUse->Column,
            FString::Printf(TEXT("Skeleton '%s' is referenced, but no part has 'bone=' and no 'skin { smooth }' rule is present."),
                *SkeletonPath), FString());
        bValid = false;
    }
    else if (!bHasSkinRule && bHasBoneBinding)
    {
        for (const FPwModelPart& Part : Document.Parts)
        {
            if (!Part.BoneBinding.IsEmpty())
            {
                continue;
            }
            AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_INCOMPLETE,
                Part.Line, Part.Column,
                FString::Printf(TEXT("Part '%s' has no 'bone=' binding. Without 'skin { smooth }', every part must name a skeleton bone."),
                    *Part.Name), Part.Name);
            bValid = false;
        }
    }

    return bValid;
}

// PARTS whose bounding boxes interpenetrate - the half of the ununioned-overlap question that
// the per-op check structurally cannot answer.
//
// WHY A SECOND CHECK RATHER THAN A WIDER ONE. AppendedFootprints is scoped to ONE MESH by RunOps,
// which is correct for what it does: a part mesh, a boolean tool and a hull body each get their
// own list so a `subtract` block's two circles are compared like a part's two circles. The cost
// is that parts are never compared with each other AT ALL, and that gap is not an edge case - it
// is where this format's models actually live. `harmonic_deform` carries one center and one
// phase, so deforming each piece differently FORCES one part per piece, and the diagnostic went
// blind exactly where the author had most reason to want it. Measured on a host character built
// that way: 49 interpenetrating components across 20 one-piece parts, and zero warnings.
//
// ONE DIAGNOSTIC PER MODEL, NOT ONE PER PAIR, and that is the load-bearing decision. Overlapping
// parts are how an organic model is built here - a lobe per part, a limb per part - so a
// per-pair warning runs to dozens on a document that is entirely correct. A warning that fires
// dozens of times on correct work is not a quieter version of a good warning, it is a worse one:
// it trains the author to skip the code, and the one pair that mattered goes with the rest. The
// count plus the first several pairs is the most that can be said before it becomes noise.
//
// The remedy is deliberately NOT "wrap it in union { }". `union` is an op INSIDE a part, so two
// parts cannot be unioned without first being made one part - and merging them is often not
// available either, because ops inside a boolean allocate no material slot, so unioning two
// solids carrying different slots drops one of them. The message says what is actually true:
// this is normal if it was meant, and the fix if it was not is to move a part or merge it.
//
// Same geometry rules as the per-op check, for the same reasons: axis-aligned boxes with a
// positive-thickness requirement on all three axes (flush-adjacent parts are the most common
// correct arrangement there is), and both sides must enclose a volume (an open shell bounds no
// interior, so "these interpenetrate" says nothing about buried faces).
void FCompiler::WarnOnInterpenetratingParts(const FPwModelDocument& Document)
{
    // Same value and the same reasoning as WarnOnUnunionedOverlap's: the scale at which "these
    // were meant to meet" stops being float residue.
    constexpr double OverlapEpsilon = 1e-3;

    // Enough pairs to see a pattern, few enough that the message stays inside the response
    // budget a diagnostic-heavy document is already competing for.
    constexpr int32 MaxNamedPairs = 6;

    TArray<FString> NamedPairs;
    int32 PairCount = 0;
    int32 FirstOffendingPart = INDEX_NONE;

    for (int32 Second = 0; Second < Result.Parts.Num(); ++Second)
    {
        const FPwModelPartInfo& Later = Result.Parts[Second];
        if (!Later.bMeshIsClosed || Later.MeshBounds.IsValid == 0)
        {
            continue;
        }

        for (int32 First = 0; First < Second; ++First)
        {
            const FPwModelPartInfo& Earlier = Result.Parts[First];
            if (!Earlier.bMeshIsClosed || Earlier.MeshBounds.IsValid == 0)
            {
                continue;
            }

            const FVector OverlapMin = FVector::Max(Earlier.MeshBounds.Min, Later.MeshBounds.Min);
            const FVector OverlapMax = FVector::Min(Earlier.MeshBounds.Max, Later.MeshBounds.Max);
            const FVector Overlap = OverlapMax - OverlapMin;
            if (Overlap.X <= OverlapEpsilon || Overlap.Y <= OverlapEpsilon || Overlap.Z <= OverlapEpsilon)
            {
                continue;
            }

            ++PairCount;
            if (FirstOffendingPart == INDEX_NONE)
            {
                FirstOffendingPart = Second;
            }
            if (NamedPairs.Num() < MaxNamedPairs)
            {
                NamedPairs.Add(FString::Printf(TEXT("'%s'/'%s' (%.3g x %.3g x %.3g uu)"),
                    *Earlier.PartName, *Later.PartName, Overlap.X, Overlap.Y, Overlap.Z));
            }
        }
    }

    if (PairCount == 0)
    {
        return;
    }

    const FString Listed = FString::Join(NamedPairs, TEXT(", "));
    const FString Remainder = (PairCount > NamedPairs.Num())
        ? FString::Printf(TEXT(", and %d more"), PairCount - NamedPairs.Num())
        : FString();

    // Anchored on the first part that overlaps something earlier, because that is the first line
    // an author can edit to change the answer; the model-wide facts are in the text.
    const FPwModelPart* Anchor = Document.Parts.IsValidIndex(FirstOffendingPart)
        ? &Document.Parts[FirstOffendingPart] : nullptr;

    AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP_PARTS,
        Anchor ? Anchor->Line : -1, Anchor ? Anchor->Column : -1,
        FString::Printf(
            TEXT("%d pair(s) of PARTS produced geometry whose bounding boxes interpenetrate: %s%s. Parts are ")
            TEXT("appended into one mesh, never unioned, so each pair keeps both surfaces and the faces buried ")
            TEXT("inside the shared volume. This is often deliberate - a shape per lobe or per limb is how this ")
            TEXT("format builds an organic model - and there is no 'union { }' remedy across parts, because that ")
            TEXT("op lives inside one. If it was not deliberate, move a part or merge the two into one part and ")
            TEXT("union there, remembering that ops inside a boolean allocate no material slot. The test is ")
            TEXT("bounding boxes, so it also fires on parts whose boxes interpenetrate while the solids do not."),
            PairCount, *Listed, *Remainder),
        Anchor ? Anchor->Name : FString());
}

bool FCompiler::BuildParts(const FPwModelDocument& Document,
                           TArray<TStrongObjectPtr<UDynamicMesh>>& OutPartMeshes)
{
    OutPartMeshes.Reserve(Document.Parts.Num());
    AllowFloatingParts.Reset();
    AllowFloatingParts.Reserve(Document.Parts.Num());

    for (const FPwModelPart& Part : Document.Parts)
    {
        CurrentPartName = Part.Name;
        PriorBevelGroupIDs.Reset();
        AllowFloatingParts.Add(Part.bAllowFloating);

        TStrongObjectPtr<UDynamicMesh> PartMesh(NewTransientMesh());

        const bool bOk = RunOps(Part.Ops, PartMesh.Get(), /*bNested=*/false);
        if (!bOk)
        {
            PartMesh->MarkAsGarbage();
            CurrentPartName.Reset();
            return false;
        }

        // The part transform maps part-local space to mesh space and is applied ONCE, after
        // the part's ops have run - which is what makes a `transform` op inside the part
        // part-local rather than a second mesh-space transform.
        const FTransform PartTransform = ReadTransformParams(Part.Transform);
        if (!PartTransform.Equals(FTransform::Identity))
        {
            UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh(
                PartMesh.Get(), PartTransform, /*bFixOrientationForNegativeScale=*/true, nullptr);
        }

        FPwModelPartInfo Info;
        Info.PartName = Part.Name;
        Info.bAllowFloating = Part.bAllowFloating;
        Info.MeshTriangleCount = PartMesh->GetTriangleCount();
        Info.MeshVertexCount = GeometryUtils::GetMeshVertexCount(PartMesh.Get());

        // Winding, measured HERE and not only on the merged mesh, because the merged number
        // averages an inverted part away: a correct 20-cube (+8000) beside an inverted 10-cube
        // (-1000) reads as a healthy +7000, and one inverted part inside an otherwise correct
        // model is the realistic case. Taken after the part transform is baked, so a
        // negative-determinant `scale=` - which TransformMesh above compensates for by
        // reversing the winding - is reflected rather than reported against pre-transform
        // geometry.
        //
        // MeasureMeshOrientation and not MeasureMeshHealth: the bowtie sweep and the
        // connected-component pass are the expensive halves and the merge stage already runs
        // them once. This adds one edge walk and one triangle walk per part.
        const GeometryUtils::FMeshOrientation Orientation =
            GeometryUtils::MeasureMeshOrientation(PartMesh.Get());
        Info.bMeshIsClosed = Orientation.IsClosed();
        Info.bMeshOrientationConsistent = Orientation.IsOrientationConsistent();
        Info.MeshSignedVolume = Orientation.SignedVolume;

        // Taken here rather than before the part transform, for the same reason the winding is:
        // the reported box has to be the one this part contributes to the merged mesh, not the
        // one its ops built in part-local space.
        //
        // Triangle-referenced, not FDynamicMesh3::GetBounds - a cut can leave isolated vertices
        // behind and the vertex walk counts them, which reported a cut part at its UNCUT extent.
        // See GetTriangleReferencedBounds.
        const UE::Geometry::FAxisAlignedBox3d PartBox = GetTriangleReferencedBounds(PartMesh.Get());
        if (!PartBox.IsEmpty())
        {
            Info.MeshBounds = FBox(FVector(PartBox.Min), FVector(PartBox.Max));
        }

        Result.Parts.Add(MoveTemp(Info));

        OutPartMeshes.Add(MoveTemp(PartMesh));
    }

    CurrentPartName.Reset();

    // After the loop, because it is the first moment every part's final box exists - a part is
    // not comparable until its ops, its booleans and its part transform have all run.
    WarnOnInterpenetratingParts(Document);

    return true;
}

bool FCompiler::BindRigidPartWeights(const FPwModelDocument& Document,
                                      TArray<TStrongObjectPtr<UDynamicMesh>>& PartMeshes)
{
    if (!bSkeletalOutput)
    {
        return true;
    }
    if (!Skeleton)
    {
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_NO_SKELETON,
            -1, -1, TEXT("Rigid skin binding reached the compiler without a resolved skeleton."),
            FString());
        return false;
    }

    const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
    for (int32 PartIndex = 0; PartIndex < Document.Parts.Num(); ++PartIndex)
    {
        const FPwModelPart& Part = Document.Parts[PartIndex];
        if (Part.BoneBinding.IsEmpty())
        {
            continue;
        }
        if (!PartMeshes.IsValidIndex(PartIndex) || !PartMeshes[PartIndex].Get())
        {
            AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_INCOMPLETE,
                Part.Line, Part.Column,
                FString::Printf(TEXT("Part '%s' has no mesh to receive rigid skin weights."), *Part.Name),
                Part.Name);
            return false;
        }

        const int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(FName(*Part.BoneBinding));
        if (BoneIndex == INDEX_NONE)
        {
            // ResolveAndValidateSkin owns the author-facing bone diagnostic. This guard is
            // only for a future direct AST caller that bypasses that preflight.
            return false;
        }

        UDynamicMesh* PartMesh = PartMeshes[PartIndex].Get();
        GeometryUtils::CopySkeletonBonesToMesh(Skeleton, PartMesh);

        // The profile is created after all part geometry exists. This keeps the per-part
        // binding valid for the later merge while avoiding the post-bind zero-fill trap.
        bool bProfileExisted = false;
        UGeometryScriptLibrary_MeshBoneWeightFunctions::MeshCreateBoneWeights(
            PartMesh, bProfileExisted, /*bReplaceExistingProfile=*/true);
        (void)bProfileExisted;

        TArray<FGeometryScriptBoneWeight> Weights;
        Weights.Emplace(BoneIndex, 1.0f);
        UGeometryScriptLibrary_MeshBoneWeightFunctions::SetAllVertexBoneWeights(
            PartMesh, Weights);
    }
    return true;
}

bool FCompiler::FillMissingUVChannels(const FPwModelDocument& Document,
                                      TArray<TStrongObjectPtr<UDynamicMesh>>& PartMeshes)
{
    // Every channel ANY part populated has to exist on EVERY part before they merge.
    // Otherwise the merged mesh has elements in that channel - so the bake's
    // MeshHasUsableUVs guard sees a populated channel and does not fire - while the silent
    // part's triangles carry none, and the asset ships untextured with no error at all.
    //
    // Driven by element presence rather than by which parts wrote a `uv` op: a primitive
    // generator populates channel 0 without any `uv` op, so a declaration-driven rule would
    // re-project UVs that were already correct.
    TSet<int32> RequiredChannels;
    for (const TStrongObjectPtr<UDynamicMesh>& PartMesh : PartMeshes)
    {
        const int32 NumLayers = MeshNumUVLayers(PartMesh.Get());
        for (int32 Channel = 0; Channel < NumLayers; ++Channel)
        {
            if (MeshHasUVElements(PartMesh.Get(), Channel))
            {
                RequiredChannels.Add(Channel);
            }
        }
    }

    TArray<int32> SortedChannels = RequiredChannels.Array();
    SortedChannels.Sort();

    TArray<FString> Filled;
    TArray<FString> Reprojected;
    bool bFailed = false;

    for (int32 Index = 0; Index < PartMeshes.Num(); ++Index)
    {
        UDynamicMesh* PartMesh = PartMeshes[Index].Get();
        if (PartMesh->GetTriangleCount() == 0)
        {
            continue;
        }

        const FString PartName = Document.Parts.IsValidIndex(Index)
            ? Document.Parts[Index].Name : FString::Printf(TEXT("#%d"), Index);

        for (int32 Channel : SortedChannels)
        {
            // EVERY triangle, not merely SOME. The element-count question this used to ask
            // (MeshHasUVElements) treats a channel with elements as done, and that is exactly the
            // state the rescue exists to repair: a part that mixes UV-carrying geometry with
            // geometry an op appended without UVs has elements in channel 0 AND triangles left at
            // -1. The old predicate skipped it, so the compiler's own box-projection rescue never
            // fired on the one mesh it was written for - it shipped untextured triangles and, if
            // an op ever read them per-triangle, an out-of-bounds read.
            // GeometryUtils::MeshHasUVsOnEveryTriangle is the shell crash guard's predicate,
            // channel-parameterised; sharing it is what keeps the two from drifting.
            if (GeometryUtils::MeshHasUVsOnEveryTriangle(PartMesh, Channel))
            {
                continue;
            }

            // Distinguished BEFORE the repair, because the repair makes them indistinguishable
            // and they are not the same event for the reader: an absent channel loses nothing,
            // a partial one has its authored UVs replaced.
            const bool bPartial = MeshHasUVElements(PartMesh, Channel);

            if (!GeometryUtils::EnsureMeshHasUVChannel(PartMesh, Channel))
            {
                // The one path that produces exactly the mesh this stage exists to prevent: the
                // channel stays absent on THIS part while other parts carry elements in it, so
                // after the merge the bake's usable-UV guard sees a populated channel and this
                // part's triangles ship untextured. It used to `continue` with no diagnostic at
                // all - the only failure in the file that emitted nothing - which made the
                // outcome indistinguishable from a clean compile. Fails the compile rather than
                // warning: nothing downstream can repair it, and an asset is the wrong thing to
                // leave on disk.
                AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED,
                    -1, -1,
                    FString::Printf(TEXT("Part '%s' carries no UV elements in channel %d, which another part populates, and the channel could not be created (a mesh supports at most 8 UV channels, 0-7). Merging would ship that part untextured."),
                        *PartName, Channel),
                    PartName);
                bFailed = true;
                continue;
            }
            // The WHOLE channel, including the triangles that already had UVs, in the partial
            // case. Deliberate, and the more destructive of the two options: projecting only the
            // unset triangles would preserve authored UVs but leave the part carrying two
            // unrelated UV layouts stitched together at an arbitrary seam, which looks correct to
            // every guard downstream and wrong in the viewport. A partial channel is a broken
            // channel; one coherent projection is the honest repair, and the warning says the
            // authored UVs were replaced so the author can put a `uv` op back where they want it.
            BoxProjectChannel(PartMesh, Channel);

            (bPartial ? Reprojected : Filled).Add(
                FString::Printf(TEXT("%s/channel %d"), *PartName, Channel));
        }
    }

    // ONE diagnostic for the whole model, across all three repairs. One per part would give a
    // five-part model with no UVs five identical warnings, which trains the reader to skip them.
    // Three clauses rather than three codes because the reader's question is the same in each
    // case - which parts and channels did the compiler invent UVs for - and only the third
    // costs them anything they authored.
    TArray<FString> Clauses;
    if (Filled.Num() > 0)
    {
        Clauses.Add(FString::Printf(
            TEXT("filled %s, which carried no elements in a channel another part populates"),
            *FString::Join(Filled, TEXT(", "))));
    }
    if (UVPaddedOnAppend.Num() > 0)
    {
        Clauses.Add(FString::Printf(
            TEXT("padded %s, where an op appended geometry with no UVs into a channel the part was already using"),
            *FString::Join(UVPaddedOnAppend, TEXT(", "))));
    }
    if (Reprojected.Num() > 0)
    {
        Clauses.Add(FString::Printf(
            TEXT("REPLACED the existing UVs of %s: those channels carried UVs on some triangles and none on others, "
                 "which ships the rest untextured and is read out of bounds by any op that reads UVs per triangle "
                 "(shell). A partial channel cannot be patched in place, so the whole channel was reprojected - "
                 "re-author it with a 'uv' op if the replaced layout mattered"),
            *FString::Join(Reprojected, TEXT(", "))));
    }

    if (Clauses.Num() > 0)
    {
        AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED,
            -1, -1,
            FString::Printf(TEXT("Box-projected UVs so every triangle carries UVs in every channel this model populates: %s."),
                *FString::Join(Clauses, TEXT("; "))),
            FString());
    }

    return !bFailed;
}

// Slots are allocated in model-wide first-use order, and that is the format's rule: the index is
// a consequence of where the author wrote `material="X"`, so it moves only when they move the
// tag. The implicit `Default` slot is the one slot with no such cause. Nothing in the document
// names it, so its index was a consequence of where the author did NOT write a tag - and
// dropping one `material=` from a sibling generator inserted it into the MIDDLE of the table,
// pushing every slot after it up one. Referencers address sections by INDEX (a placed actor's
// OverrideMaterials, a section's material assignment, an LOD section setting), so the author's
// "slot 1" then bound the phantom instead, on every placed copy, with the geometry byte for byte
// what it was.
//
// Rotating it to the end restores the property the format promises: a slot the author named has
// the index their tags put it at, whatever the untagged geometry around it does. It costs the
// documents where `Default` currently sits in the middle - mixed-tagging documents where some
// untagged generator runs before a tagged one - and those are exactly the documents this exists
// to fix. Fully tagged models have no `Default`, and fully untagged ones have nothing else, so
// neither moves.
//
// Not applied when a part-level op wrote `material="Default"` itself: that is a name the author
// chose, and a name keeps its first-use index.
//
// The rotation is a permutation of [0, SlotNames.Num()), so it is spelled as single-pair remaps
// through a scratch ID past the end of the used range - the shifts then run in ascending order,
// each into an index the previous step has already vacated, and nothing chains.
void FCompiler::MoveImplicitDefaultSlotLast(UDynamicMesh* Merged)
{
    if (!ImplicitDefaultSlot.bAllocated || bDefaultSlotExplicitlyTagged)
    {
        return;
    }

    const int32 SlotCount = SlotNames.Num();
    const int32 DefaultIndex = SlotNames.IndexOfByKey(FString(DefaultSlotName));
    if (DefaultIndex == INDEX_NONE || DefaultIndex == SlotCount - 1)
    {
        return;
    }

    // Past both the slot list and anything a raw `material_id=` wrote, so the scratch ID cannot
    // collide with triangles the shifts below have yet to touch.
    bool bHasMaterialIDs = false;
    const int32 MaxMaterialID =
        UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(Merged, bHasMaterialIDs);
    const int32 ScratchID = FMath::Max(SlotCount, MaxMaterialID + 1);

    UGeometryScriptLibrary_MeshMaterialFunctions::RemapMaterialIDs(Merged, DefaultIndex, ScratchID);
    for (int32 Index = DefaultIndex + 1; Index < SlotCount; ++Index)
    {
        UGeometryScriptLibrary_MeshMaterialFunctions::RemapMaterialIDs(Merged, Index, Index - 1);
    }
    UGeometryScriptLibrary_MeshMaterialFunctions::RemapMaterialIDs(Merged, ScratchID, SlotCount - 1);

    SlotNames.RemoveAt(DefaultIndex);
    SlotNames.Add(FString(DefaultSlotName));
}

void FCompiler::MergeParts(TArray<TStrongObjectPtr<UDynamicMesh>>& PartMeshes, UDynamicMesh* Merged)
{
    PartIndexMappings.Reset();
    PartIndexMappings.Reserve(PartMeshes.Num());
    MergedTriangleToPart.Reset();

    for (TStrongObjectPtr<UDynamicMesh>& PartMesh : PartMeshes)
    {
        // Identity: the part transform was already baked in BuildParts, so the merge is a
        // pure append. Material IDs need no per-part offset because slots are allocated
        // from ONE model-wide list keyed by name - which is also what makes two parts
        // tagging "Shell" share a slot rather than getting one each.
        UE::Geometry::FMeshIndexMappings& IndexMappings = PartIndexMappings.AddDefaulted_GetRef();
        Merged->EditMesh(
            [&PartMesh, &IndexMappings](UE::Geometry::FDynamicMesh3& AppendToMesh)
            {
                PartMesh->ProcessMesh(
                    [&AppendToMesh, &IndexMappings](const UE::Geometry::FDynamicMesh3& OtherMesh)
                    {
                        // This is the default EnableAllMatching mode of
                        // FGeometryScriptAppendMeshOptions. Keep it explicit because the direct
                        // editor call below is replacing that wrapper only to retain IndexMaps.
                        AppendToMesh.EnableMatchingAttributes(OtherMesh, false, false);
                        UE::Geometry::FDynamicMeshEditor Editor(&AppendToMesh);
                        Editor.AppendMesh(&OtherMesh, IndexMappings);
                    });
            },
            EDynamicMeshChangeType::GeneralEdit,
            EDynamicMeshAttributeChangeFlags::Unknown,
            /*bDeferChangeEvents=*/false);

        const int32 PartIndex = PartIndexMappings.Num() - 1;
        for (const TPair<int32, int32>& Mapping :
             IndexMappings.GetTriangleMap().GetForwardMap())
        {
            MergedTriangleToPart.Add(Mapping.Value, PartIndex);
        }

        PartMesh->MarkAsGarbage();
    }
    PartMeshes.Reset();

    // Before the padding below, so a padded `MaterialN` keeps N as its index: the rotation
    // permutes only the slots the build allocated, and every padded slot sits after those.
    MoveImplicitDefaultSlotLast(Merged);

    // Only append_buffers can write a raw material ID, and it can write one past the end of
    // the slot list. Pad rather than clamp: a triangle referencing a slot the asset does not
    // have is an invalid mesh description, and silently remapping it would move geometry
    // onto someone else's material.
    bool bHasMaterialIDs = false;
    const int32 MaxMaterialID =
        UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(Merged, bHasMaterialIDs);
    if (bHasMaterialIDs && MaxMaterialID >= SlotNames.Num())
    {
        const int32 FirstPadded = SlotNames.Num();
        while (SlotNames.Num() <= MaxMaterialID)
        {
            SlotNames.Add(FString::Printf(TEXT("Material%d"), SlotNames.Num()));
        }
        AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_OUT_OF_RANGE,
            -1, -1,
            FString::Printf(TEXT("Geometry references material ID %d but only %d slot(s) were declared; padded with unbound slots %d-%d."),
                MaxMaterialID, FirstPadded, FirstPadded, MaxMaterialID),
            FString());
    }
}

bool FCompiler::ApplyModelUVLayouts(const FPwModelDocument& Document, UDynamicMesh* Merged)
{
    for (const FPwOp& Op : Document.UVLayouts)
    {
        const int32 Channel = GetInt(Op.Params, TEXT("channel"), 0);
        const int32 TextureResolution = GetInt(Op.Params, TEXT("texture_resolution"),
            GeometryOps::LayoutUVTextureResolutionDefault);

        GeometryOps::FOpResult OpResult = GeometryOps::FOpResult::Ok();
        if (Merged->GetTriangleCount() > 0 && !MeshHasUVElements(Merged, Channel))
        {
            OpResult = GeometryOps::FOpResult::Fail(ErrorCodes::ERR_NO_UV_ELEMENTS,
                FString::Printf(
                    TEXT("'uv_layout' wrote no UV elements into channel %d: it can only pack islands that part-level UV ops already created."),
                    Channel));
        }
        else if (Merged->GetTriangleCount() > 0)
        {
            bool bPacked = false;
            Merged->EditMesh([&](UE::Geometry::FDynamicMesh3& EditMesh)
            {
                UE::Geometry::FDynamicMeshUVOverlay* UVOverlay =
                    EditMesh.Attributes()->GetUVLayer(Channel);
                UVOverlay->SplitBowties();

                UE::Geometry::FDynamicMeshUVPacker Packer(UVOverlay);
                Packer.TextureResolution = TextureResolution;
                Packer.bScaleIslandsByWorldSpaceTexelRatio = true;
                bPacked = Packer.StandardPack();
            }, EDynamicMeshChangeType::AttributeEdit,
               EDynamicMeshAttributeChangeFlags::UVs,
               /*bDeferChangeEvents=*/false);

            OpResult = bPacked
                ? OkWithCounts(Merged)
                : GeometryOps::FOpResult::Fail(ErrorCodes::ERR_UV_GENERATION_FAILED,
                    FString::Printf(
                        TEXT("'uv_layout' could not pack channel %d at texture_resolution=%d."),
                        Channel, TextureResolution));
        }
        ReportOpResult(Op, OpResult, /*bModelLevel=*/true);
        if (!OpResult.bSuccess)
        {
            return false;
        }
    }

    return true;
}

bool FCompiler::ValidateCrossPartUVChannel(
    const FPwModelDocument& Document, UDynamicMesh* Merged,
    int32 Channel, bool bLightmapChannel)
{
    if (!GeometryUtils::MeshHasUVsOnEveryTriangle(Merged, Channel))
    {
        if (bLightmapChannel)
        {
            ModelError(PwModelDiagnosticCodes::PWMODEL_LIGHTMAP_UV_INVALID,
                Document.Lightmap.GetValue(),
                FString::Printf(TEXT("Lightmap UV channel %d is not assigned on every triangle."),
                    Channel));
            return false;
        }
        return true;
    }

    TStrongObjectPtr<UDynamicMesh> UVMesh(NewTransientMesh());
    UDynamicMesh* UVMeshOut = nullptr;
    bool bInvalidTopology = false;
    bool bValidUVSet = false;
    UGeometryScriptLibrary_MeshUVFunctions::CopyMeshUVLayerToMesh(
        Merged, Channel, UVMesh.Get(), UVMeshOut, bInvalidTopology, bValidUVSet, nullptr);
    if (!bValidUVSet || bInvalidTopology)
    {
        UVMesh->MarkAsGarbage();
        if (bLightmapChannel)
        {
            ModelError(PwModelDiagnosticCodes::PWMODEL_LIGHTMAP_UV_INVALID,
                Document.Lightmap.GetValue(),
                FString::Printf(TEXT("Lightmap UV channel %d has invalid overlay topology and cannot be checked for overlap."),
                    Channel));
            return false;
        }
        return true;
    }

    if (Document.Parts.Num() < 2)
    {
        UVMesh->MarkAsGarbage();
        return true;
    }

    TSet<uint64> PartPairs;
    TSet<int32> InvolvedParts;
    UVMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
    {
        const TFunction<bool(UE::Geometry::FIntrTriangle3Triangle3d&)> IntersectFn =
            [](UE::Geometry::FIntrTriangle3Triangle3d& Intr)
            {
                Intr.SetReportCoplanarIntersection(true);
                if (!UE::Geometry::FDynamicMeshAABBTree3::TriangleIntersection(Intr)
                    || Intr.Quantity < 3)
                {
                    return false;
                }

                UE::Geometry::FPolygon2d Intersection;
                for (int32 PointIndex = 0; PointIndex < Intr.Quantity; ++PointIndex)
                {
                    Intersection.AppendVertex(
                        FVector2d(Intr.Points[PointIndex].X, Intr.Points[PointIndex].Y));
                }
                return Intersection.Area() != 0.0;
            };

        UE::Geometry::FDynamicMeshAABBTree3 Tree(&Mesh, /*bAutoBuild=*/true);
        const MeshIntersection::FIntersectionsQueryResult Hits =
            Tree.FindAllSelfIntersections(
                /*bIgnoreTopoConnected=*/true,
                UE::Geometry::IMeshSpatial::FQueryOptions(), IntersectFn);

        for (const MeshIntersection::FPolygonIntersection& Hit : Hits.Polygons)
        {
            const int32* PartA = MergedTriangleToPart.Find(Hit.TriangleID[0]);
            const int32* PartB = MergedTriangleToPart.Find(Hit.TriangleID[1]);
            if (PartA == nullptr || PartB == nullptr || *PartA == *PartB)
            {
                continue;
            }

            const int32 First = FMath::Min(*PartA, *PartB);
            const int32 Second = FMath::Max(*PartA, *PartB);
            const uint64 PairKey =
                (static_cast<uint64>(static_cast<uint32>(First)) << 32)
                | static_cast<uint64>(static_cast<uint32>(Second));
            PartPairs.Add(PairKey);
            InvolvedParts.Add(First);
            InvolvedParts.Add(Second);
        }
    });
    UVMesh->MarkAsGarbage();

    if (PartPairs.Num() == 0)
    {
        return true;
    }

    TArray<FString> PartNames;
    int32 AnchorLine = -1;
    int32 AnchorColumn = -1;
    FString AnchorPartName;
    for (int32 PartIndex = 0; PartIndex < Document.Parts.Num(); ++PartIndex)
    {
        if (!InvolvedParts.Contains(PartIndex))
        {
            continue;
        }

        const FPwModelPart& Part = Document.Parts[PartIndex];
        PartNames.Add(Part.Name);
        for (const FPwOp& Op : Part.Ops)
        {
            if (Op.OpName == TEXT("uv")
                && GetInt(Op.Params, TEXT("channel"), 0) == Channel
                && (Op.Line > AnchorLine
                    || (Op.Line == AnchorLine && Op.Column > AnchorColumn)))
            {
                AnchorLine = Op.Line;
                AnchorColumn = Op.Column;
                AnchorPartName = Part.Name;
            }
        }
    }

    FString Message = FString::Printf(
        TEXT("UV channel %d has positive-area overlap across parts: %s. Pack the merged atlas with model-level 'uv_layout channel=%d'."),
        Channel, *FString::Join(PartNames, TEXT(", ")), Channel);
    if (AnchorLine < 0)
    {
        if (bLightmapChannel)
        {
            ModelError(PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS,
                Document.Lightmap.GetValue(), MoveTemp(Message));
            return false;
        }
        AddDiagnostic(EPwSeverity::Warning,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS,
            -1, -1, MoveTemp(Message), FString(), TArray<FString>(), TEXT("model"));
        return true;
    }

    AddDiagnostic(bLightmapChannel ? EPwSeverity::Error : EPwSeverity::Warning,
        PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS,
        AnchorLine, AnchorColumn, MoveTemp(Message), AnchorPartName);
    return !bLightmapChannel;
}

bool FCompiler::ValidateCrossPartUVs(
    const FPwModelDocument& Document, UDynamicMesh* Merged)
{
    const int32 LightmapChannel = Document.Lightmap.IsSet()
        ? GetInt(Document.Lightmap->Params, TEXT("channel"), INDEX_NONE)
        : INDEX_NONE;
    bool bValid = true;

    if (LightmapChannel != INDEX_NONE)
    {
        bValid = ValidateCrossPartUVChannel(
            Document, Merged, LightmapChannel, /*bLightmapChannel=*/true);
    }

    const int32 NumChannels = MeshNumUVLayers(Merged);
    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        if (Channel == LightmapChannel || !MeshHasUVElements(Merged, Channel))
        {
            continue;
        }
        ValidateCrossPartUVChannel(Document, Merged, Channel, /*bLightmapChannel=*/false);
    }
    return bValid;
}

bool FCompiler::ValidateMergedMesh(UDynamicMesh* Merged)
{
    const int32 TriangleCount = Merged->GetTriangleCount();
    Result.MeshTriangleCount = TriangleCount;
    Result.MeshVertexCount = GeometryUtils::GetMeshVertexCount(Merged);

    if (TriangleCount == 0)
    {
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_EMPTY_MESH, -1, -1,
            TEXT("Every part ran, and the merged mesh has no triangles. A boolean that removed everything is the usual cause."),
            FString());
        return false;
    }

    if (TriangleCount > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH)
    {
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_MESH_TOO_LARGE, -1, -1,
            FString::Printf(TEXT("The merged mesh has %d triangles, over the %d limit."),
                TriangleCount, GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH),
            FString());
        return false;
    }

    // A non-finite position survives the bake and turns up later as an asset with an
    // infinite bounding box, which is far harder to trace back here than one check is.
    const UE::Geometry::FDynamicMesh3& EditMesh = Merged->GetMeshRef();
    for (int32 VertexID : EditMesh.VertexIndicesItr())
    {
        const FVector3d Position = EditMesh.GetVertex(VertexID);
        if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z))
        {
            AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_INVALID_GEOMETRY, -1, -1,
                FString::Printf(TEXT("Vertex %d is at a non-finite position (%g, %g, %g)."),
                    VertexID, Position.X, Position.Y, Position.Z),
                FString());
            return false;
        }
    }

    // ---- mesh health --------------------------------------------------------------------
    //
    // The same walk geometry.check_health reports, which this stage never ran. That omission is
    // why every example model compiled clean while carrying degenerates and, at one point, 272
    // boundary edges: an open shell shipped as a successful asset with model.compile answering
    // success:true and saying nothing. Per-example degenerate counts are not repeated here; the
    // corpus reading is in docs/wiki-src/model.examples.md and moves as examples are reworked.
    //
    // WARNINGS, not errors, and the two conditions are split rather than pooled:
    //
    //  - An open mesh is the more serious of the two and gets its own code, because a static
    //    mesh with a boundary renders one-sided from behind and its complex collision is not a
    //    solid. It is still not an error: open is CORRECT for a card, a plane, or a
    //    procedural_mesh + append_buffers surface, and origami_crane is exactly that. The
    //    compiler cannot tell an intended shell from a subtract that broke through a wall - only
    //    the author can, and making it an error would need an author-declared `open` intent that
    //    the format does not have.
    //  - Degenerates and bowties are a rung below: they are common, the static-mesh build's own
    //    bRemoveDegenerates welds most of them away, and two shipped examples carry them. They
    //    matter as a TREND, which is why the count is in the message and in the result.
    //
    // Both counts also reach the caller as result fields, so a caller that DOES want to refuse an
    // open mesh can gate on MeshBoundaryEdges itself rather than parse a diagnostic string.
    const GeometryUtils::FMeshHealth Health = GeometryUtils::MeasureMeshHealth(Merged);
    Result.MeshBoundaryEdges = Health.BoundaryEdges;
    Result.MeshDegenerateTriangles = Health.DegenerateTriangles;
    Result.MeshNonManifoldVertices = Health.NonManifoldVertices;
    Result.MeshComponentCount = Health.ComponentCount;
    // Orphans. NOT paired with a diagnostic, for the same reason as winding below: at merge time
    // the compiler cannot say which op stranded which vertex, and it is not a fault in its own
    // right - the bake is driven by triangles, so an orphan reaches no asset. What it is, is the
    // only field that reconciles MeshVertexCount (the vertex buffer) with the reported bounds
    // (reduced over triangles), which otherwise disagree about the same mesh in silence.
    Result.MeshUnreferencedVertices = Health.UnreferencedVertices;
    // Winding. NOT paired with a diagnostic here, deliberately: at merge time the compiler
    // cannot say which op inverted what, and a model-level "something is inside out" with no
    // line number is a worse answer than a number the caller gates on. The AUTHORING mistake
    // that causes it is caught at its call site instead, with a line, by
    // PWMODEL_EXTRUDE_FACING_OPPOSED. What this pair adds is a signal that exists at all -
    // before it, an inverted closed shell was byte-identical to a correct one in every field
    // of this response.
    Result.MeshInconsistentEdges = Health.InconsistentEdges;
    Result.MeshSignedVolume = Health.SignedVolume;

    // EMBEDDING. The six fields above cannot see a surface that passes through itself, and the
    // two shapes of that fault both walk straight through the documented gate: a membrane's two
    // fans are oppositely wound so their volume contributions cancel EXACTLY (the number is the
    // correct annulus figure, on a ring with a lid across its bore), and a pinched sweep moves
    // the number smoothly through 62% and 25% of its analytic volume before the sign ever flips.
    // A separate call and not part of MeasureMeshHealth: this one builds an AABB tree per
    // component, which is a different cost class from that walk's O(E + T + V) and is not paid
    // per part.
    //
    // NOT paired with an error, and counted per shell rather than model-wide, for the reason on
    // PWMODEL_SELF_INTERSECTING_SURFACE: appended parts and appended sibling ops interpenetrate
    // deliberately in this format and are reported by their own two codes at bounding-box
    // granularity, so pooling them here would put a warning on a large fraction of correct
    // documents and get the code ignored wholesale.
    const GeometryUtils::FMeshSelfIntersection SelfIntersection =
        GeometryUtils::MeasureMeshSelfIntersection(Merged);
    if (SelfIntersection.bMeasured)
    {
        Result.MeshSelfIntersections = SelfIntersection.PairCount;
        Result.MeshSelfIntersectingComponents = SelfIntersection.SelfIntersectingComponents;
        Result.bMeshSelfIntersectionTruncated = SelfIntersection.bTruncated;
    }

    // Size, which none of the counts above can express. Measured on the same merged mesh and at
    // the same stage, so a validate-only run answers with the box a compile would have written.
    //
    // Triangle-referenced, not FDynamicMesh3::GetBounds - a cut can leave isolated vertices
    // behind and the vertex walk counts them, which reported a cut model at its UNCUT extent.
    // See GetTriangleReferencedBounds.
    const UE::Geometry::FAxisAlignedBox3d MergedBox = GetTriangleReferencedBounds(Merged);
    if (!MergedBox.IsEmpty())
    {
        Result.MeshBounds = FBox(FVector(MergedBox.Min), FVector(MergedBox.Max));
    }

    // Spatial isolation is deliberately a second graph over the existing edge-connected
    // components. Many unwelded/interpenetrating shells are normal in .pwmodel; only an island
    // outside the largest individual connected component by triangle count is reported, and the
    // report remains a warning.
    TArray<MeshAudit::FComponentMeasurement> SpatialComponents;
    MeshAudit::MeasureComponents(Merged, SpatialComponents);
    MeshAudit::FSpatialMeasurement Spatial;
    MeshAudit::MeasureSpatialProximity(Merged, Spatial, SpatialComponents);
    MeshAudit::ClassifyFloatingComponents(
        SpatialComponents, Spatial, MeshAudit::DefaultFloatingToleranceFraction,
        Result.FloatingGeometry);

    // `allow_floating=true` is part-specific. A floating connected component is suppressed only
    // when every merged triangle maps back to an explicitly allowed part; mixed components stay
    // visible so one allowed ornament cannot hide a real detached region beside it.
    Result.FloatingGeometry.UnsuppressedCount = 0;
    Result.FloatingGeometry.SuppressedCount = 0;
    for (MeshAudit::FFloatingComponent& Floating : Result.FloatingGeometry.Components)
    {
        TSet<int32> OwningPartSet;
        bool bMapped = false;
        bool bAllowed = true;
        if (SpatialComponents.IsValidIndex(Floating.ComponentIndex))
        {
            for (const int32 TriangleID : SpatialComponents[Floating.ComponentIndex].TriangleIDs)
            {
                const int32* PartIndex = MergedTriangleToPart.Find(TriangleID);
                if (!PartIndex)
                {
                    bAllowed = false;
                    break;
                }
                bMapped = true;
                OwningPartSet.Add(*PartIndex);
                if (!AllowFloatingParts.IsValidIndex(*PartIndex)
                    || !AllowFloatingParts[*PartIndex])
                {
                    bAllowed = false;
                    break;
                }
            }
        }
        Floating.OwningPartIndices = OwningPartSet.Array();
        Floating.OwningPartIndices.Sort();
        Floating.bSuppressed = bMapped && bAllowed;
        if (!Floating.bSuppressed)
        {
            ++Result.FloatingGeometry.UnsuppressedCount;
        }
        else
        {
            ++Result.FloatingGeometry.SuppressedCount;
        }
    }

    if (Result.FloatingGeometry.UnsuppressedCount > 0)
    {
        FString FloatingDiagnosticPart;
        bool bFloatingDiagnosticPartIsUnambiguous = true;
        for (const MeshAudit::FFloatingComponent& Floating : Result.FloatingGeometry.Components)
        {
            if (Floating.bSuppressed || Floating.OwningPartIndices.Num() != 1)
            {
                continue;
            }
            const int32 PartIndex = Floating.OwningPartIndices[0];
            if (!Result.Parts.IsValidIndex(PartIndex))
            {
                bFloatingDiagnosticPartIsUnambiguous = false;
                break;
            }
            const FString& Candidate = Result.Parts[PartIndex].PartName;
            if (FloatingDiagnosticPart.IsEmpty())
            {
                FloatingDiagnosticPart = Candidate;
            }
            else if (FloatingDiagnosticPart != Candidate)
            {
                bFloatingDiagnosticPartIsUnambiguous = false;
                break;
            }
        }
        if (!bFloatingDiagnosticPartIsUnambiguous)
        {
            FloatingDiagnosticPart.Empty();
        }
        AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_FLOATING_COMPONENT,
            -1, -1,
            FString::Printf(
                TEXT("The merged mesh has %d spatial island(s); %d component(s) are isolated "
                     "from the largest individual connected component by triangle count. "
                     "Triangle-level nearest distance "
                     "was compared with tolerance %.6g (%.6g of the bounding-sphere radius %.6g). "
                     "Use part-level allow_floating=true only for an intentional named ornament; "
                     "the measurement remains in the result."),
                Result.FloatingGeometry.IslandCount,
                Result.FloatingGeometry.UnsuppressedCount,
                Result.FloatingGeometry.Tolerance,
                Result.FloatingGeometry.ToleranceFraction,
                Result.FloatingGeometry.BoundingSphereRadius),
            FloatingDiagnosticPart);
    }

    if (Health.BoundaryEdges > 0)
    {
        AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_MESH_NOT_CLOSED, -1, -1,
            FString::Printf(
                TEXT("The merged mesh is not closed: %d boundary edge(s) across %d connected component(s). ")
                TEXT("A static mesh with an open boundary renders one-sided from behind and its complex ")
                TEXT("collision is not solid. Intended for a card, a plane or an append_buffers surface; ")
                TEXT("otherwise a cut broke through a wall, or a shell op left its opening unclosed."),
                Health.BoundaryEdges, Health.ComponentCount),
            FString());
    }

    if (Health.DegenerateTriangles > 0 || Health.NonManifoldVertices > 0)
    {
        AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_DEGENERATE_GEOMETRY, -1, -1,
            FString::Printf(
                TEXT("The merged mesh carries %d degenerate triangle(s) (area < %g) and %d non-manifold ")
                TEXT("(bowtie) vertex/vertices. Most degenerates die in the static-mesh build's own ")
                TEXT("bRemoveDegenerates pass, so this is a trend to watch rather than a defect to fix: a ")
                TEXT("jump means two boolean operands were placed to touch EXACTLY rather than to overlap. ")
                TEXT("Overlap them - three foils of radius 15 struck at 15 pass through one point and leave ")
                TEXT("degenerates; radius 15.5 struck at 15, same overall radius, leaves none."),
                Health.DegenerateTriangles, GeometryUtils::DegenerateAreaEpsilon,
                Health.NonManifoldVertices),
            FString());
    }

    if (SelfIntersection.PairCount > 0 && !bSelfIntersectionDiagnosticAttributed)
    {
        const FString Where = SelfIntersection.bHasWitness
            ? FString::Printf(TEXT(" The first crossing found is at (%g, %g, %g)."),
                SelfIntersection.Witness.X, SelfIntersection.Witness.Y,
                SelfIntersection.Witness.Z)
            : FString();
        AddDiagnostic(EPwSeverity::Warning,
            PwModelDiagnosticCodes::PWMODEL_SELF_INTERSECTING_SURFACE, -1, -1,
            FString::Printf(
                TEXT("%d%s pair(s) of triangles inside %d connected shell(s) cross or coincide: ")
                TEXT("the surface passes through itself, so it is not the boundary of the solid ")
                TEXT("it looks like. No other health field can see this - a membrane across a ")
                TEXT("bore is two oppositely wound fans whose volumes cancel exactly, so ")
                TEXT("signedVolume is the figure the correct solid would have had, and a sweep ")
                TEXT("whose walls were pushed through each other moves it smoothly with nothing ")
                TEXT("else changing at all.%s Usual causes: a `revolve` whose profile ends sit ")
                TEXT("off the axis at the same height (repeat the first point as the last so it ")
                TEXT("is read as a closed section), a swept or twisted section thinner than the ")
                TEXT("sag its own rotation puts into each ruled quad, or a displacement op whose ")
                TEXT("magnitude approached the local edge length. Counted per shell, so ")
                TEXT("interpenetrating parts and appended siblings are NOT this - those are ")
                TEXT("PWMODEL_UNUNIONED_OVERLAP_PARTS and PWMODEL_UNUNIONED_OVERLAP."),
                SelfIntersection.PairCount, SelfIntersection.bTruncated ? TEXT("+") : TEXT(""),
                SelfIntersection.SelfIntersectingComponents, *Where),
            FString());
    }

    return true;
}

bool FCompiler::ApplySkinWeights(const FPwModelDocument& Document, UDynamicMesh* Merged)
{
    if (!bSkeletalOutput)
    {
        return true;
    }
    if (!Skeleton || !Merged)
    {
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_NO_SKELETON,
            -1, -1, TEXT("Skin stages require a resolved skeleton and merged mesh."), FString());
        return false;
    }

    const FPwOp* SmoothRule = nullptr;
    if (Document.Skin.IsSet())
    {
        for (const FPwOp& Child : Document.Skin.GetValue().Children)
        {
            if (Child.OpName == TEXT("smooth"))
            {
                SmoothRule = &Child;
                break;
            }
        }
    }

    if (SmoothRule)
    {
        // The merged mesh is the final geometry. Create the profile here, immediately before
        // the solve, so later vertices cannot receive the engine's silent zero-fill values.
        GeometryUtils::CopySkeletonBonesToMesh(Skeleton, Merged);

        bool bProfileExisted = false;
        UGeometryScriptLibrary_MeshBoneWeightFunctions::MeshCreateBoneWeights(
            Merged, bProfileExisted, /*bReplaceExistingProfile=*/true);
        (void)bProfileExisted;

        FGeometryScriptSmoothBoneWeightsOptions SmoothOptions;
        SmoothOptions.MaxInfluences = GetInt(
            SmoothRule->Params, TEXT("max_influences"), SmoothOptions.MaxInfluences);
        SmoothOptions.Stiffness = static_cast<float>(GetNumber(
            SmoothRule->Params, TEXT("stiffness"), SmoothOptions.Stiffness));
        SmoothOptions.VoxelResolution = GetInt(
            SmoothRule->Params, TEXT("voxel_resolution"), SmoothOptions.VoxelResolution);
        const FString Method = GetIdentifier(
            SmoothRule->Params, TEXT("method"), TEXT("direct_distance"));
        SmoothOptions.DistanceWeighingType = Method == TEXT("geodesic_voxel")
            ? EGeometryScriptSmoothBoneWeightsType::GeodesicVoxel
            : EGeometryScriptSmoothBoneWeightsType::DirectDistance;

        UGeometryScriptLibrary_MeshBoneWeightFunctions::ComputeSmoothBoneWeights(
            Merged, Skeleton, SmoothOptions, FGeometryScriptBoneWeightProfile(), nullptr);
        SkinSolveStageName = TEXT("smooth skin solve");
    }
    else
    {
        SkinSolveStageName = TEXT("rigid part binding");
    }

    // Smooth solve first, rigid part bindings second. The smooth API has no selection input;
    // overwriting the mapped vertices is the only way to express mixed rigid/smooth skin in the
    // frozen contract, and the K2 mappings are the authoritative correspondence.
    for (int32 PartIndex = 0; PartIndex < Document.Parts.Num(); ++PartIndex)
    {
        const FPwModelPart& Part = Document.Parts[PartIndex];
        if (Part.BoneBinding.IsEmpty() || !PartIndexMappings.IsValidIndex(PartIndex))
        {
            continue;
        }

        const int32 BoneIndex = Skeleton->GetReferenceSkeleton().FindBoneIndex(
            FName(*Part.BoneBinding));
        if (BoneIndex == INDEX_NONE)
        {
            return false;
        }

        TArray<FGeometryScriptBoneWeight> Weights;
        Weights.Emplace(BoneIndex, 1.0f);
        for (const TPair<int32, int32>& VertexMap :
             PartIndexMappings[PartIndex].GetVertexMap().GetForwardMap())
        {
            bool bValidVertexID = false;
            UGeometryScriptLibrary_MeshBoneWeightFunctions::SetVertexBoneWeights(
                Merged, VertexMap.Value, Weights, bValidVertexID);
            (void)bValidVertexID;
        }
    }

    // This is the ONLY weight-coverage gate. In particular, do not replace it with the asset
    // creator's validation: UE accepts an existing attribute filled with all-zero influences.
    const GeometryUtils::FSkinWeightCoverage Coverage =
        GeometryUtils::ScanSkinWeightCoverage(Merged);
    Result.bHasSkinWeights = Coverage.bHasProfile;
    Result.bFullyWeighted = Coverage.IsFullyWeighted();
    Result.SkinVertexCount = Coverage.VertexCount;
    Result.SkinWeightedVertexCount = Coverage.WeightedCount;
    Result.SkinUnweightedVertexCount = Coverage.UnweightedCount;

    SkinVertexCountAfterSolve = GeometryUtils::GetMeshVertexCount(Merged);
    if (!Coverage.IsFullyWeighted())
    {
        TArray<FString> UnboundParts;
        if (!SmoothRule)
        {
            for (const FPwModelPart& Part : Document.Parts)
            {
                if (Part.BoneBinding.IsEmpty())
                {
                    UnboundParts.Add(Part.Name);
                }
            }
        }

        const int32 Line = Document.Skin.IsSet() ? Document.Skin.GetValue().Line : -1;
        const int32 Column = Document.Skin.IsSet() ? Document.Skin.GetValue().Column : -1;
        FString Message = FString::Printf(
            TEXT("The final skin solve left %d of %d merged vertices without bone influences."),
            Coverage.UnweightedCount, Coverage.VertexCount);
        if (UnboundParts.Num() > 0)
        {
            Message += FString::Printf(
                TEXT(" Unbound parts: %s."), *FString::Join(UnboundParts, TEXT(", ")));
        }
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_INCOMPLETE,
            Line, Column, MoveTemp(Message), FString());
        return false;
    }

    return true;
}

bool FCompiler::CheckSkinFreshness(UDynamicMesh* Merged, const TCHAR* StageName)
{
    if (!bSkeletalOutput)
    {
        return true;
    }

    const int32 CurrentVertexCount = GeometryUtils::GetMeshVertexCount(Merged);
    if (CurrentVertexCount == SkinVertexCountAfterSolve)
    {
        return true;
    }

    AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_SKIN_STALE,
        -1, -1,
        FString::Printf(
            TEXT("Skin weights are stale: stage '%s' changed the merged vertex count from %d after the solve to %d before asset creation."),
            StageName ? StageName : TEXT("unknown"), SkinVertexCountAfterSolve, CurrentVertexCount),
        FString());
    return false;
}

bool FCompiler::BuildCollisionGeometry(const FPwModelDocument& Document, UDynamicMesh* Merged,
                                       FPwModelCollisionResult& OutCollision)
{
    if (!Document.Collision.IsSet())
    {
        return true;
    }

    const FPwModelCollision& Collision = Document.Collision.GetValue();

    // Hull bodies are geometry ops, so the collision builder runs them through this
    // callback rather than depending on the ops layer itself. Diagnostics are redirected
    // into a scratch array for the duration: the builder folds the failure into its own
    // PWMODEL_DEGENERATE_HULL, and leaving them in the result too would report one problem
    // twice.
    TArray<FPwDiagnostic> HullDiagnostics;
    auto BuildOps = [this, &HullDiagnostics](const TArray<FPwOp>& Ops, UDynamicMesh* Out,
                                             TArray<FString>& OutErrors) -> bool
    {
        HullDiagnostics.Reset();

        TArray<FPwDiagnostic>* PreviousSink = Sink;
        Sink = &HullDiagnostics;
        const bool bOk = RunOps(Ops, Out, /*bNested=*/true);
        Sink = PreviousSink;

        for (const FPwDiagnostic& Diagnostic : HullDiagnostics)
        {
            if (Diagnostic.Severity == EPwSeverity::Error)
            {
                OutErrors.Add(Diagnostic.ToString());
            }
        }
        return bOk;
    };

    OutCollision = BuildCollision(Collision, Merged, BuildOps);

    for (const FString& Warning : OutCollision.Warnings)
    {
        AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING,
            Collision.Line, Collision.Column, Warning, FString());
    }

    if (!OutCollision.bSuccess)
    {
        // The builder anchors its own failures to the offending element, so prefer its
        // position; the block header is only the fallback for a failure with no element.
        const FString Code = OutCollision.ErrorCode.IsEmpty()
            ? FString(PwModelDiagnosticCodes::PWMODEL_COLLISION_FAILED) : OutCollision.ErrorCode;
        const int32 Line = OutCollision.ErrorLine >= 0 ? OutCollision.ErrorLine : Collision.Line;
        const int32 Column = OutCollision.ErrorColumn >= 0 ? OutCollision.ErrorColumn : Collision.Column;
        AddDiagnostic(EPwSeverity::Error, *Code, Line, Column, OutCollision.ErrorMessage, FString());
        return false;
    }

    Result.CollisionElements = OutCollision.ElementsWritten;
    return true;
}

void FCompiler::CreateAsset(const FPwModelDocument& Document, UDynamicMesh* Merged,
                            const FPwModelCollisionResult& Collision, FStringView Source)
{
    if (bSkeletalOutput)
    {
        FSkeletalMeshCreateSpec Spec;
        Spec.AssetPath = Options.OutputAssetPath;
        Spec.bOverwrite = Options.bOverwrite;
        Spec.Skeleton = Skeleton;
        Spec.MaterialSlots = SlotNames;
        Spec.SourcePath = Options.SourcePath;
        Spec.SourceHash = HashSource(Source);
        Spec.bSave = Options.bSave;

        for (const FPwModelMaterialBinding& Binding : Document.Materials)
        {
            if (SlotNames.Contains(Binding.Slot))
            {
                Spec.MaterialBindings.Add(Binding.Slot, Binding.AssetPath);
            }
        }

        const FSkeletalMeshCreateResult Created = CreateSkeletalMesh(Merged, Spec);
        Result.Diagnostics.Append(Created.Diagnostics);
        for (const FString& Warning : Created.Warnings)
        {
            AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING,
                -1, -1, Warning, FString());
        }

        if (!Created.bSuccess && Created.Diagnostics.Num() == 0)
        {
            AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_ASSET_CREATE_FAILED,
                -1, -1,
                FString::Printf(TEXT("SkeletalMesh asset creation failed [%s]: %s"),
                    *Created.ErrorCode, *Created.ErrorMessage), FString());
        }
        if (!Created.bSuccess)
        {
            return;
        }

        // The skeletal creator owns the UE 5.8 post-edit/build boundary. This branch only
        // records its value-out result; it deliberately does not call PostEditChange again.
        Result.AssetPath = Options.OutputAssetPath;
        Result.bSavedToDisk = Created.bSavedToDisk;
        Result.SaveState = Created.SaveState;
        Result.AssetTriangleCount = Created.TriangleCount;
        Result.AssetVertexCount = Created.VertexCount;
        Result.SkeletonPackageName = Created.SkeletonPackageName;
        Result.SkeletonSaveState = Created.SkeletonSaveState;
        Result.bSkeletonSavedToDisk = IsAssetSaveStateDurable(Created.SkeletonSaveState);
        Result.ClearedFeatures = Created.ClearedFeatures;
        // The creator's own verdict on the bindings, which nothing used to read. It is the only
        // report of a slot that ended up on the default material, and it is the ASSET's answer
        // rather than the source's - a slot bound to a path that would not load is bound in the
        // document and unbound in the mesh.
        Result.UnboundSlots = Created.UnboundSlots;
        return;
    }

    FStaticMeshCreateSpec Spec;
    Spec.AssetPath = Options.OutputAssetPath;
    Spec.bOverwrite = Options.bOverwrite;
    Spec.MaterialSlots = SlotNames;
    Spec.SourcePath = Options.SourcePath;
    Spec.SourceHash = HashSource(Source);
    Spec.bSave = Options.bSave;

    // Only bindings for slots the geometry actually uses. The parser has already warned
    // about the rest, and passing them on would have the creator load materials that end up
    // on no triangle.
    for (const FPwModelMaterialBinding& Binding : Document.Materials)
    {
        if (SlotNames.Contains(Binding.Slot))
        {
            Spec.MaterialBindings.Add(Binding.Slot, Binding.AssetPath);
        }
    }

    if (Document.Lightmap.IsSet())
    {
        Spec.LightMapChannel = GetInt(Document.Lightmap.GetValue().Params, TEXT("channel"), INDEX_NONE);

        // INDEX_NONE has to survive an omitted `resolution`: the parameter is optional and the
        // parser materialises no defaults into Params, so an absent key means "leave the asset
        // at the UStaticMesh default of 4" and an explicit one means "write this". Without this
        // line the channel was set on every model and the resolution on none, so three shipped
        // examples paid for `mode=patch_builder` + `mode=layout texture_resolution=256` lightmap
        // UVs and then baked them at 4x4.
        Spec.LightMapResolution = GetInt(Document.Lightmap.GetValue().Params, TEXT("resolution"), INDEX_NONE);
    }

    if (Document.Collision.IsSet())
    {
        Spec.CollisionTrace = Collision.Trace;
        Spec.SimpleCollision = Collision.Geom;
    }

    const FStaticMeshCreateResult Created = CreateStaticMesh(Merged, Spec);
    Result.Diagnostics.Append(Created.Diagnostics);

    for (const FString& Warning : Created.Warnings)
    {
        AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING,
            -1, -1, Warning, FString());
    }

    if (!Created.bSuccess && Created.Diagnostics.Num() == 0)
    {
        AddDiagnostic(EPwSeverity::Error, PwModelDiagnosticCodes::PWMODEL_ASSET_CREATE_FAILED, -1, -1,
            FString::Printf(TEXT("Asset creation failed [%s]: %s"), *Created.ErrorCode, *Created.ErrorMessage),
            FString());
    }
    if (!Created.bSuccess)
    {
        return;
    }

    // The MESH counts are still not re-stamped from Created: MeshTriangleCount and
    // MeshVertexCount stay the merged mesh's, set by ValidateMergedMesh, which is what keeps a
    // bValidateOnly run reporting the same numbers as a real one.
    //
    // The ASSET counts are a different question and are stamped here, because this is the only
    // point in the pipeline where a built UStaticMesh exists. CreateStaticMesh reads them off
    // LOD0's render data after the build, so they include what the build removed (degenerate
    // triangles) and what it added (vertices split at seams) - neither of which is derivable
    // from the mesh above. They stay at -1 on the validate path and on a failed create, and the
    // RPC omits them there rather than publishing a 0 that reads as an empty mesh.
    Result.AssetPath = Options.OutputAssetPath;
    Result.bSavedToDisk = Created.bSavedToDisk;
    Result.SaveState = Created.SaveState;
    Result.AssetTriangleCount = Created.TriangleCount;
    Result.AssetVertexCount = Created.VertexCount;
    Result.UnboundSlots = Created.UnboundSlots;
}

// A binding that names an asset which will not load, reported from the VALIDATE path.
//
// model.validate is the "check before you compile" surface, and it was blind to exactly one
// class of defect: a `materials { Shell = "/Game/DoesNotExist/M_NotThere" }` returned ZERO
// diagnostics from validate and then warned on compile. "Validates clean, then warns" is the
// one outcome that surface exists to prevent - an author iterating on a string has no reason
// to run a compile they have been told is unnecessary, so the binding ships resolving to the
// default material and the model renders in grey.
//
// ONLY on the validate path, which is why this is not folded into a stage both paths run.
// CreateStaticMesh loads the same bindings and emits the same sentence a few lines later
// (GeometryAssetCreate.cpp), and the compile path already carries it; running this there too
// would report one condition twice. The message text is copied deliberately rather than
// paraphrased so the two surfaces are greppable as one string - only the anchor differs, and
// this one is better: it names the binding's own line, where CreateStaticMesh's warning has no
// position at all and comes back at -1.
//
// Loading, not an asset-registry lookup, because the condition being reported is "this does not
// LOAD as a UMaterialInterface" - a registry hit for an asset of some other class would answer
// the wrong question, and the compile path this mirrors resolves it with LoadObject. LOAD_NoWarn
// | LOAD_Quiet only silences the engine's own log line for a path that is expected to be missing
// here; it does not change the verdict.
//
// Filtered to slots the geometry USES, mirroring CreateAsset. A binding for a slot nothing tags
// already has its own diagnostic (PWMODEL_UNUSED_MATERIAL, from the parser) and is dropped
// before the creator ever sees it, so resolving it would report a path that cannot matter.
void FCompiler::WarnOnUnloadableMaterialBindings(const FPwModelDocument& Document)
{
    for (const FPwModelMaterialBinding& Binding : Document.Materials)
    {
        if (Binding.AssetPath.IsEmpty() || !SlotNames.Contains(Binding.Slot))
        {
            continue;
        }

        const UMaterialInterface* Bound = LoadObject<UMaterialInterface>(
            nullptr, *Binding.AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
        if (Bound != nullptr)
        {
            continue;
        }

        AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING,
            Binding.Line, Binding.Column,
            FString::Printf(
                TEXT("Material slot '%s' is bound to '%s', which could not be loaded; using the default material"),
                *Binding.Slot, *Binding.AssetPath),
            FString());
    }
}

// An author who wrote a `materials` block has stated the slot table they intend, so geometry
// landing in a slot that block never names is a mistake by definition - that geometry ships on
// the engine default material, and the only place the fact appears otherwise is materialSlotList.
//
// It no longer reports a RENUMBERING: MoveImplicitDefaultSlotLast puts the implicit slot at the
// end of the table, so no slot the block declares can sit after it. The index is still named,
// because it is what a referencer binding by index has to avoid.
//
// Four conditions, and each one excludes a population for which this is not a mistake:
//
//   - the slot was allocated implicitly, and no op tags it by name. An explicit
//     material="Default" is a slot the author chose, wherever it lands.
//   - the document HAS a `materials` block. A document with no block is the ordinary untagged
//     case and stays silent, which is what PwModelParser.cpp ValidateMaterialSlots keeps
//     `Default` out of ReferencedSlots for.
//   - that block does not bind `Default`. Binding it IS one of the two fixes, and the binding
//     reaches the asset (PWMODEL_UNUSED_MATERIAL is deliberately not raised for it).
//   - at least one declared slot reached the table. A document whose geometry is untagged
//     THROUGHOUT allocates only `Default` and already gets one PWMODEL_UNUSED_MATERIAL per
//     binding saying the same thing with the same remedy.
void FCompiler::WarnOnImplicitDefaultSlot(const FPwModelDocument& Document)
{
    if (!ImplicitDefaultSlot.bAllocated || bDefaultSlotExplicitlyTagged
        || Document.Materials.Num() == 0)
    {
        return;
    }

    auto IsDeclared = [&Document](const FString& Slot)
    {
        return Document.Materials.ContainsByPredicate(
            [&Slot](const FPwModelMaterialBinding& Binding) { return Binding.Slot == Slot; });
    };

    const FString DefaultSlot(DefaultSlotName);
    const int32 DefaultIndex = SlotNames.IndexOfByKey(DefaultSlot);
    if (DefaultIndex == INDEX_NONE || IsDeclared(DefaultSlot))
    {
        return;
    }

    const bool bAnyDeclaredSlotAllocated = SlotNames.ContainsByPredicate(IsDeclared);
    if (!bAnyDeclaredSlotAllocated)
    {
        return;
    }

    FString Message = FString::Printf(
        TEXT("Op '%s' is untagged, so it allocated the implicit slot '%s', which `materials` does ")
        TEXT("not bind; that geometry ships on the engine default material. '%s' is placed after ")
        TEXT("every slot your tags name, at index %d, so nothing you declared was renumbered. ")
        TEXT("Tag the op with material=\"<Slot>\", or bind '%s' in `materials`."),
        *ImplicitDefaultSlot.OpName, *DefaultSlot, *DefaultSlot, DefaultIndex, *DefaultSlot);

    AddDiagnostic(EPwSeverity::Warning, PwModelDiagnosticCodes::PWMODEL_IMPLICIT_DEFAULT_SLOT,
        ImplicitDefaultSlot.Line, ImplicitDefaultSlot.Column, MoveTemp(Message),
        ImplicitDefaultSlot.PartName);
}

void FCompiler::Run(FStringView Source)
{
    // Stage 1 - parse. Nothing is built, so nothing needs unwinding on failure.
    FPwModelDocument Document;
    TArray<FPwDiagnostic> ParseDiagnostics;
    const bool bParsed = FPwModelParser::Parse(Source, Document, ParseDiagnostics);

    Result.Diagnostics.Append(MoveTemp(ParseDiagnostics));
    Result.Version = Document.Header.Version;

    // WHAT THE SOURCE DECLARES, or nothing - never a default dressed as an answer.
    //
    // One line decides the output class, and the parser recovers rather than aborting, so a
    // document that failed elsewhere may still carry it. Two cases, and they are not the same:
    // a recovered `use skeleton from` says the author asked for a skeletal mesh whatever else
    // went wrong, while a failed parse with no such line proves nothing - the parse may have
    // died before reaching one. The first is reported, the second is left unknown and the RPC
    // omits the class fields.
    //
    // This used to answer `assetClass: "UStaticMesh", skeletal: false` for both, because the
    // field defaults to false and nothing distinguished "static" from "not determined". A
    // skeletal source with a typo was told, confidently, that it was building a static mesh.
    const bool bDeclaresSkeleton = Document.Uses.ContainsByPredicate(
        [](const FPwUse& Use) { return Use.Kind == TEXT("skeleton"); });
    if (bDeclaresSkeleton)
    {
        Result.bSkeletal = true;
        Result.bAssetClassKnown = true;
    }
    else if (bParsed)
    {
        // A complete parse with no skeleton declaration IS the static answer, even if a later
        // stage fails: nothing after the parse can turn a static document into a skeletal one.
        Result.bAssetClassKnown = true;
    }

    if (!bParsed)
    {
        return;
    }

    // Stage 2 - reject the reserved constructs, before any mesh exists.
    if (!RejectReservedConstructs(Document))
    {
        return;
    }

    // Stage 2a/2b - resolve the direct skeleton asset reference and reject combinations the
    // skeletal asset seam cannot represent. No geometry has been built yet, so these failures
    // cannot leave a partially-created output asset behind.
    if (!ResolveAndValidateSkin(Document))
    {
        return;
    }

    // Stage 3 - one transient mesh per part, then merge.
    TArray<TStrongObjectPtr<UDynamicMesh>> PartMeshes;
    const bool bBuilt = BuildParts(Document, PartMeshes);
    if (!bBuilt)
    {
        for (TStrongObjectPtr<UDynamicMesh>& PartMesh : PartMeshes)
        {
            PartMesh->MarkAsGarbage();
        }
        return;
    }

    // Stage 3a - rigid part bindings are prepared while each part still has its own mesh.
    // The final merged scan below remains the only coverage gate.
    if (!BindRigidPartWeights(Document, PartMeshes))
    {
        for (TStrongObjectPtr<UDynamicMesh>& PartMesh : PartMeshes)
        {
            PartMesh->MarkAsGarbage();
        }
        return;
    }

    // Before the merge, and it can fail: a part that cannot be given a channel another part
    // populates is the one case the merge would hide, so nothing is created.
    if (!FillMissingUVChannels(Document, PartMeshes))
    {
        for (TStrongObjectPtr<UDynamicMesh>& PartMesh : PartMeshes)
        {
            PartMesh->MarkAsGarbage();
        }
        return;
    }

    TStrongObjectPtr<UDynamicMesh> Merged(NewTransientMesh());
    MergeParts(PartMeshes, Merged.Get());

    if (!ApplyModelUVLayouts(Document, Merged.Get()))
    {
        Merged->MarkAsGarbage();
        return;
    }

    if (!ValidateCrossPartUVs(Document, Merged.Get()))
    {
        Merged->MarkAsGarbage();
        return;
    }

    // The slot table, after MergeParts so any padded slot is in it. Both fields come from the
    // same array on purpose: a count that can disagree with the list it counts is a second
    // source of truth for the one number an author gates on.
    Result.MaterialSlots = SlotNames.Num();
    Result.MaterialSlotList.Reset(SlotNames.Num());
    for (const FString& SlotName : SlotNames)
    {
        FPwModelSlotReport& Slot = Result.MaterialSlotList.AddDefaulted_GetRef();
        Slot.Name = SlotName;
        for (const FPwModelMaterialBinding& Binding : Document.Materials)
        {
            if (Binding.Slot == SlotName)
            {
                Slot.BoundAssetPath = Binding.AssetPath;
                break;
            }
        }
    }

    // Here and not earlier: the diagnostic names the index the implicit slot took, and the table
    // is only final once MergeParts has padded it.
    WarnOnImplicitDefaultSlot(Document);

    // Stage 4 - validate the merged mesh. With one asset, "if anything failed create
    // nothing" is trivially true rather than something to unwind.
    if (!ValidateMergedMesh(Merged.Get()))
    {
        Merged->MarkAsGarbage();
        return;
    }

    // Stage 6a/6b/6c - solve smooth weights, overwrite rigid partitions, then perform the
    // exhaustive final coverage gate.
    if (!ApplySkinWeights(Document, Merged.Get()))
    {
        Merged->MarkAsGarbage();
        return;
    }

    // Stage 7 - collision, then exactly one asset creation seam.
    FPwModelCollisionResult Collision;
    if (!BuildCollisionGeometry(Document, Merged.Get(), Collision))
    {
        Merged->MarkAsGarbage();
        return;
    }

    // Stage 6d - this read is immediately before the write/validate outcome. Any future stage
    // inserted after the solve must either leave the count unchanged or fail with a named stale
    // diagnostic instead of relying on the caller to remember "RUN THIS LAST".
    if (!CheckSkinFreshness(Merged.Get(), TEXT("pre-create preparation")))
    {
        Merged->MarkAsGarbage();
        return;
    }

    if (Options.bValidateOnly)
    {
        // Stands in for the one CreateAsset check that is not about creating anything.
        WarnOnUnloadableMaterialBindings(Document);
    }
    else
    {
        CreateAsset(Document, Merged.Get(), Collision, Source);
    }

    Merged->MarkAsGarbage();

    Result.bSuccess = !PwDiagnosticsHaveError(Result.Diagnostics);
}

} // namespace PwModelCompilerPrivate

FPwModelCompileResult FPwModelCompiler::Compile(FStringView Source, const FPwModelCompileOptions& Options)
{
    FPwModelCompileResult Result;

    PwModelCompilerPrivate::FCompiler Compiler(Options, Result);
    Compiler.Run(Source);

    return Result;
}
