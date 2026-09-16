// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Modeling.cpp - implementations for GeometryOps_Modeling.h.
//
// Extracted verbatim from MeshOpsHandler.cpp: the engine calls, their option structs, their
// guards and their version forks are unchanged, and every error code an op returns is the one
// the handler emitted at that same point. The wrappers forward those codes, which is what keeps
// the dispatcher tests blind to the split.
//
// The null guard, the before/after count snapshot and the clamp-and-warn pair are
// GeometryOps.h's, shared with the other four families: this file no longer carries a private
// copy of any of them. Adopting the shared guard also retired this family's own null-mesh
// answer (INVALID_ARGUMENT / "No dynamic mesh to operate on") in favour of the one all five now
// send (MESH_NOT_FOUND / "DynamicMesh not available"). That is invisible on the wire: every RPC
// wrapper reaches these ops through GeometryTarget::ResolveOrSendError or
// GetOrCreateDynamicMesh, neither of which can hand an op a null mesh - only the .pwmodel
// compiler can, and it wants a code back rather than a null dereference inside an engine call.
#include "Handlers/Geometry/GeometryOps_Modeling.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryScriptDebugSink.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Operations/MeshBevel.h"
#include "GroupTopology.h"
#include "Compat/EngineVersionCompat.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshModelingFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshSelectionFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshSubdivideFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"

namespace
{
    // Uniquely prefixed: this module builds with bUseUnity = true, so a plainly-named
    // anonymous-namespace helper would collide with a sibling TU once Unity merges them.
    //
    // Only the two helpers that are genuinely about MODELING live here now. The null guard,
    // the before/after count snapshot and the clamp-and-warn pair moved to GeometryOps.h,
    // where all five families share one copy.

    // What GeometryOpsModeling_BuildSelection decided the face op should run on.
    enum class EGeometryOpsModeling_FaceSelection : uint8
    {
        // No face direction was given. The engine selection is left EMPTY on purpose, which is
        // what the selection-consuming entry points read as "the whole mesh".
        WholeMesh,
        // A direction was given and matched at least one face.
        Subset,
        // A direction was given and matched NOTHING. The op must not run at all.
        MatchedNothing
    };

    // Builds the engine selection a face op runs on, and decides whether the op may run.
    //
    // WHERE THE WHOLE-MESH FALLBACK ACTUALLY LIVES. Not here: every selection-consuming entry
    // point these four ops call opens with the same inline block -
    //
    //   MeshModelingFunctions.cpp:613  ApplyMeshLinearExtrudeFaces
    //   MeshModelingFunctions.cpp:722  ApplyMeshOffsetFaces
    //   MeshModelingFunctions.cpp:815  ApplyMeshInsetOutsetFaces
    //       TArray<int32> Triangles;
    //       if (Selection.GetNumSelected() == 0) { for (int32 tid : EditMesh.TriangleIndicesItr())
    //                                                  { Triangles.Add(tid); } }
    //       else { Selection.ConvertToMeshIndexArray(EditMesh, Triangles, ...Triangle); }
    //
    // - so an empty selection IS the whole mesh, hardcoded, with no option to say otherwise.
    //
    // EGeometryScriptEmptySelectionBehavior (GeometryScriptSelectionTypes.h:57, values
    // FullMeshSelection / EmptySelection) is the engine's own name for that choice, but it is a
    // FIELD on only four option structs - FGeometryScriptPerlinNoiseOptions,
    // FGeometryScriptIterativeMeshSmoothingOptions and FGeometryScriptDisplaceFromTextureOptions
    // (MeshDeformFunctions.h:158, :175, :203) and FGeometryScriptSelectiveTessellateOptions
    // (MeshSubdivideFunctions.h:48), each defaulted to FullMeshSelection and each read as
    // `if (Selection.IsEmpty() && Options.EmptyBehavior != FullMeshSelection) return;`.
    // FGeometryScriptMeshLinearExtrudeOptions,
    // FGeometryScriptMeshOffsetFacesOptions and FGeometryScriptMeshInsetOutsetFacesOptions do
    // NOT carry it. So the honest answer to "does PinWright pass, default or ignore it" is
    // NONE OF THE THREE: for these four ops there is no parameter to pass it to, and
    // EmptySelection behaviour can only be implemented on this side of the call, by returning
    // before it. That is what MatchedNothing below is.
    //
    // WHY A FILTER THAT MATCHES NOTHING IS A NO-OP RATHER THAN THE WHOLE MESH:
    //   - The empty-selection convention answers "no filter given". It is exactly wrong for
    //     "filter given, satisfied by nothing": the author named a SUBSET, and operating on
    //     everything is the maximally destructive reading of that request. It shipped - two
    //     extrudes in an example model carried it and nobody noticed, because the result is
    //     plausible.
    //   - A no-op is the only outcome that cannot damage geometry, and it is precisely what the
    //     engine's own EGeometryScriptEmptySelectionBehavior::EmptySelection means. This adopts
    //     an engine-sanctioned behaviour rather than inventing one.
    //   - A hard error was considered and REJECTED: an FOpResult failure aborts the whole part
    //     in the .pwmodel compiler (PwModelCompiler.cpp's ReportOpResult -> PWMODEL_OP_FAILED),
    //     so a parameterised document with one variant whose filter legitimately matches nothing
    //     would fail the entire build. Warn + no-op keeps the mesh intact and still reports.
    //   - The report is machine-readable, not just prose: FacesSelected stays 0, FOpResult
    //     ::bChanged stays false (nothing ran, so FinishOp's count delta is zero) and
    //     FFaceOpOutcome::bFilterMatchedNothing separates this 0 from the whole-mesh 0.
    //   - "Operate on everything anyway" stays reachable - but only if an author asks for it
    //     explicitly, which is what an EGeometryScriptEmptySelectionBehavior parameter would be
    //     for. That parameter is NOT added here, and note for whoever adds it: for these four
    //     ops it has to be honoured entirely on this side, because the engine structs have no
    //     field to forward it to.
    EGeometryOpsModeling_FaceSelection GeometryOpsModeling_BuildSelection(
        UDynamicMesh* Mesh,
        const GeometryOps::FFaceSelectionSpec& Spec,
        FGeometryScriptMeshSelection& OutSelection,
        const TCHAR* OpName,
        GeometryOps::FOpResult& Result)
    {
        if (!Spec.bHasDirection)
        {
            return EGeometryOpsModeling_FaceSelection::WholeMesh;
        }

        FVector Normal = Spec.Direction;
        if (Normal.IsNearlyZero())
        {
            // A zero direction has no meaning as a filter, and reinterpreting it as +Z silently
            // is the same class of defect as the zero-match fallback below: the author gets a
            // filter they did not ask for and no way to see it. Substituting is still better
            // than handing the engine a vector it will Normalized() into a NaN, but it says so.
            Normal = FVector::UpVector;
            Result.Warnings.Add(FString::Printf(TEXT(
                "%s: the face direction is (0, 0, 0), which cannot describe a normal - it was "
                "read as +Z (0, 0, 1). Pass a real axis, or omit the direction to operate on the "
                "whole mesh."),
                OpName));
        }

        // The trailing 3 is MinNumTrianglePoints and is INERT for a Triangles selection, despite
        // reading like a threshold: SelectMeshElementsByNormalAngle's containment predicate tests
        // only the NORMAL (MeshSelectionFunctions.cpp:1021-1025 - it ignores the Point argument),
        // and the caller feeds all three corners the SAME per-triangle normal
        // (MeshSelectionFunctions.cpp:155-166, UseNormal = Mesh.GetTriNormal(tid)). So the count
        // is always 0 or 3 and any value in [1,3] selects identically. Left at 3 because that is
        // what shipped.
        UGeometryScriptLibrary_MeshSelectionFunctions::SelectMeshElementsByNormalAngle(
            Mesh, OutSelection, Normal, Spec.AngleTolerance,
            EGeometryScriptMeshSelectionType::Triangles, false, 3);

        if (OutSelection.GetNumSelected() > 0)
        {
            return EGeometryOpsModeling_FaceSelection::Subset;
        }

        // Names the direction ACTUALLY used (post zero-substitution), the tolerance, the count it
        // found and the mesh's triangle count - so an author can tell a wrong filter from an
        // empty mesh without a second call.
        Result.Warnings.Add(FString::Printf(TEXT(
            "%s: the face-direction filter (%.3f, %.3f, %.3f) with an angle tolerance of %.1f deg "
            "matched 0 of the mesh's %d triangles, so %s did NOTHING and the mesh is unchanged. A "
            "filter that matches nothing is NOT 'the whole mesh'. Check the sign and the axis of "
            "the direction, or widen the tolerance (faceAngleTolerance / face_angle_tolerance); "
            "omit the direction (faceDirection / face_direction) entirely to operate on the whole "
            "mesh."),
            OpName, Normal.X, Normal.Y, Normal.Z, Spec.AngleTolerance,
            Result.TrianglesBefore, OpName));

        return EGeometryOpsModeling_FaceSelection::MatchedNothing;
    }

    // The second half of the face-filter story, and the one that is NOT a defect we can repair:
    // on an OPEN mesh a filtered extrude / offset_faces returns strictly less closed geometry
    // than the same call with no filter, and the only honest answer is to say so.
    //
    // Measured on an open two-sided sheet: 24 triangles (a correct closed shell) with no face
    // direction, 14 with one that matched real faces - i.e. a matching filter produced WORSE
    // geometry than no filter at all.
    //
    // Mechanism, traced end to end:
    //   MeshModelingFunctions.cpp:603 / :712   Extruder.bOffsetFullComponentsAsSolids =
    //                                          Options.bSolidsToShells   (extrude / offset_faces;
    //                                          ApplyMeshInsetOutsetFaces never sets it, which is
    //                                          why inset/outset do not carry this warning)
    //   OffsetMeshRegion.cpp:28-30             FOffsetMeshRegion::Apply splits the selected
    //                                          triangles into connected components
    //   OffsetMeshRegion.cpp:40-45             for each component,
    //                                            Region.bIsSolid = GrowToConnectedTriangles(
    //                                                component).Num() == component.Num()
    //                                          i.e. bIsSolid is true ONLY when the selection
    //                                          covers a COMPLETE connected component of the mesh
    //   OffsetMeshRegion.h:98                  "If a sub-region of Triangles is a full connected
    //                                          component, offset into a solid instead of leaving
    //                                          a shell"
    //   OffsetMeshRegion.cpp:598-602           and bIsSolid is what gates
    //                                          Editor.DuplicateTriangles(...) - the second skin;
    //                                          :734-745 flips it outward. Without it the region
    //                                          is MOVED rather than copied, so nothing closes.
    //   OffsetMeshRegion.cpp:212 ->            the skirt is 2 triangles per region-boundary
    //   DynamicMeshEditor.cpp:280-286          loop edge, and MeshRegionBoundaryLoops.cpp:50-55
    //                                          puts OPEN mesh boundary edges and INTERIOR
    //                                          selection-boundary edges into the same loop - so
    //                                          the skirt is NOT where the two paths differ.
    //
    // So the per-region result is exactly:
    //   bIsSolid   : T_region (kept) + T_region (duplicated) + 2 * BoundaryLoopEdges
    //   otherwise  : T_region (moved, not copied) + 2 * BoundaryLoopEdges
    // plus every unselected triangle, carried through untouched.
    //
    // That reproduces the measurement on an open sheet of two UNWELDED quads (4 triangles, two
    // connected components, 4 boundary edges each):
    //   no filter    -> both components fully selected -> both bIsSolid -> (2 + 2 + 8) x 2 = 24
    //   +Z filter    -> one component selected, still whole, so still bIsSolid -> 12, and the
    //                   other wing is never touched and stays a bare 2-triangle sheet -> 14
    // A face filter therefore degrades the result in one of two ways, both of them the engine's
    // documented contract for a sub-region: it drops whole components (they are never extruded
    // and get no side walls - the "missing skin" above), or it leaves a PARTIAL component, which
    // flips bIsSolid off so that region is moved-and-skirted rather than closed.
    //
    // bSolidsToShells is ALREADY the engine default (true) on both ops, and bIsSolid is derived
    // rather than settable, so there is NO engine option that would make the filtered path match
    // the unfiltered path's quality - which is why this is a warning and not an option fix.
    // DirectionMode is a red herring: MeshModelingFunctions.cpp:580-599 shows it only chooses
    // the offset VECTOR (fixed vs area-weighted average normal) and never touches the solid/shell
    // decision.
    //
    // Gated on bSolidsToShells because with it off the unfiltered call does not solidify either,
    // so there is no quality gap to report.
    void GeometryOpsModeling_WarnOpenMeshPartialSelection(
        UDynamicMesh* Mesh,
        int32 NumSelected,
        bool bSolidsToShells,
        const TCHAR* OpName,
        GeometryOps::FOpResult& Result)
    {
        if (!bSolidsToShells || NumSelected <= 0 || NumSelected >= Result.TrianglesBefore)
        {
            return;
        }

        const int32 OpenBorderEdges =
            UGeometryScriptLibrary_MeshQueryFunctions::GetNumOpenBorderEdges(Mesh);
        if (OpenBorderEdges <= 0)
        {
            // A closed solid: extruding one face of a box is the intended use and stays closed.
            return;
        }

        Result.Warnings.Add(FString::Printf(TEXT(
            "%s: the face-direction filter selected %d of the mesh's %d triangles and the mesh is "
            "OPEN (%d boundary edges), so this returns LESS closed geometry than the same call "
            "with no face direction. The %d unselected triangles are left flat - not extruded, no "
            "side walls - and a selection that covers only PART of a connected component is not "
            "capped into a solid at all, because only a COMPLETE connected component is. Measured "
            "on an open two-sided sheet: 24 triangles unfiltered (a closed shell) vs 14 filtered. "
            "Drop the face direction to close the whole sheet, or use `shell` to thicken it."),
            OpName, NumSelected, Result.TrianglesBefore, OpenBorderEdges,
            Result.TrianglesBefore - NumSelected));
    }

    // Locally-spelled enum -> engine enum. Each is a switch with NO default: a value added to
    // the local enum without a case here fails to compile, where a static_cast or a defaulted
    // switch would silently map it onto whatever the engine's first enumerator happens to be.
    // The trailing return is unreachable and exists only because MSVC warns without it.
    EGeometryScriptRemoveMeshSimplificationType GeometryOpsModeling_ToEngine(
        GeometryOps::ESimplifyMethod Method)
    {
        switch (Method)
        {
        case GeometryOps::ESimplifyMethod::StandardQEM:
            return EGeometryScriptRemoveMeshSimplificationType::StandardQEM;
        case GeometryOps::ESimplifyMethod::VolumePreserving:
            return EGeometryScriptRemoveMeshSimplificationType::VolumePreserving;
        case GeometryOps::ESimplifyMethod::AttributeAware:
            return EGeometryScriptRemoveMeshSimplificationType::AttributeAware;
        // AttributeAwareV2 joined EGeometryScriptRemoveMeshSimplificationType in UE 5.8. On
        // older engines it has no target, and SimplifyMesh refuses the request rather than
        // reaching here - see the guard there for why it is not folded onto AttributeAware.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        case GeometryOps::ESimplifyMethod::AttributeAwareV2:
            return EGeometryScriptRemoveMeshSimplificationType::AttributeAwareV2;
#else
        case GeometryOps::ESimplifyMethod::AttributeAwareV2:
            break;
#endif
        }
        return EGeometryScriptRemoveMeshSimplificationType::AttributeAware;
    }

    // EGeometryScriptMeshSimplificationQuadricVariant is 5.8-only; on older engines the
    // simplifier has one quadric and no field to select another.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    EGeometryScriptMeshSimplificationQuadricVariant GeometryOpsModeling_ToEngine(
        GeometryOps::ESimplifyQuadricVariant Variant)
    {
        switch (Variant)
        {
        case GeometryOps::ESimplifyQuadricVariant::PlaneQuadric:
            return EGeometryScriptMeshSimplificationQuadricVariant::PlaneQuadric;
        case GeometryOps::ESimplifyQuadricVariant::TriangleQuadric:
            return EGeometryScriptMeshSimplificationQuadricVariant::TriangleQuadric;
        }
        return EGeometryScriptMeshSimplificationQuadricVariant::PlaneQuadric;
    }
#endif

    EGeometryScriptUniformRemeshTargetType GeometryOpsModeling_ToEngine(
        GeometryOps::ERemeshTargetType TargetType)
    {
        switch (TargetType)
        {
        case GeometryOps::ERemeshTargetType::TriangleCount:
            return EGeometryScriptUniformRemeshTargetType::TriangleCount;
        case GeometryOps::ERemeshTargetType::TargetEdgeLength:
            return EGeometryScriptUniformRemeshTargetType::TargetEdgeLength;
        }
        return EGeometryScriptUniformRemeshTargetType::TriangleCount;
    }

    EGeometryScriptRemeshSmoothingType GeometryOpsModeling_ToEngine(
        GeometryOps::ERemeshSmoothingType SmoothingType)
    {
        switch (SmoothingType)
        {
        case GeometryOps::ERemeshSmoothingType::Uniform:
            return EGeometryScriptRemeshSmoothingType::Uniform;
        case GeometryOps::ERemeshSmoothingType::UVPreserving:
            return EGeometryScriptRemeshSmoothingType::UVPreserving;
        case GeometryOps::ERemeshSmoothingType::Mixed:
            return EGeometryScriptRemeshSmoothingType::Mixed;
        }
        return EGeometryScriptRemeshSmoothingType::Mixed;
    }

    EGeometryScriptRemeshEdgeConstraintType GeometryOpsModeling_ToEngine(
        GeometryOps::ERemeshEdgeConstraint Constraint)
    {
        switch (Constraint)
        {
        case GeometryOps::ERemeshEdgeConstraint::Fixed:
            return EGeometryScriptRemeshEdgeConstraintType::Fixed;
        case GeometryOps::ERemeshEdgeConstraint::Refine:
            return EGeometryScriptRemeshEdgeConstraintType::Refine;
        case GeometryOps::ERemeshEdgeConstraint::Free:
            return EGeometryScriptRemeshEdgeConstraintType::Free;
        case GeometryOps::ERemeshEdgeConstraint::Ignore:
            return EGeometryScriptRemeshEdgeConstraintType::Ignore;
        }
        return EGeometryScriptRemeshEdgeConstraintType::Free;
    }

    // EGeometryScriptUVLayoutType, FGeometryScriptLayoutUVsOptions and LayoutMeshUVs all arrived
    // in UE 5.5. On 5.4 the engine publishes only the repack half of that verb (RepackMeshUVs +
    // FGeometryScriptRepackUVsOptions), so this mapping - and the LayoutUV body that uses it -
    // exists only on 5.5+.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    EGeometryScriptUVLayoutType GeometryOpsModeling_ToEngine(GeometryOps::EUVLayoutType LayoutType)
    {
        switch (LayoutType)
        {
        case GeometryOps::EUVLayoutType::Transform: return EGeometryScriptUVLayoutType::Transform;
        case GeometryOps::EUVLayoutType::Stack:     return EGeometryScriptUVLayoutType::Stack;
        case GeometryOps::EUVLayoutType::Repack:    return EGeometryScriptUVLayoutType::Repack;
        case GeometryOps::EUVLayoutType::Normalize: return EGeometryScriptUVLayoutType::Normalize;
        }
        return EGeometryScriptUVLayoutType::Repack;
    }
#endif

    // EMeshAxis -> component index, with the same "anything else is Z" fallback every axis-
    // taking verb reproduces (no wrapper validates its `axis` string; see EMeshAxis).
    int32 GeometryOpsModeling_AxisIndex(GeometryOps::EMeshAxis Axis)
    {
        switch (Axis)
        {
        case GeometryOps::EMeshAxis::X: return 0;
        case GeometryOps::EMeshAxis::Y: return 1;
        default:                        return 2;
        }
    }

    // The gizmo frame bend / twist / taper hand to the engine, built from FWarpFrameSpec.
    //
    // Every FMeshSpaceDeformerOp measures its extent along the frame's Z and centres it on the
    // frame's ORIGIN (MeshSpaceDeformerOp.cpp:40-53 builds WorldToGizmo from GizmoFrame's
    // rotation and origin; the subclasses then read GizmoPos4[2]). So the frame's Z is the warp
    // axis and the frame's translation is the warp centre, and this function is only the mapping
    // from the two published parameters onto those two slots.
    //
    // The perpendicular basis is CYCLIC - frame X is (Axis + 1) % 3, frame Y is (Axis + 2) % 3 -
    // which is the same handedness harmonic_deform measures its azimuth with, and is what makes
    // taper's flareX / flareY nameable on a non-Z axis. Every cyclic permutation of the standard
    // basis has determinant +1, so the frame is right-handed on all three axes and the engine's
    // invertibility check (BendMeshOp.cpp:53) cannot trip on it.
    //
    // AT THE DEFAULTS THIS IS EXACTLY FTransform::Identity, by arithmetic: Axis=Z gives
    // frame X = world X, frame Y = world Y, frame Z = world Z, and Center=(0,0,0) gives a zero
    // translation, so the matrix is FMatrix::Identity and FFrame3d reads an identity rotation and
    // a zero origin - the same two values the hardcoded FTransform::Identity produced. That is
    // why publishing these parameters cannot move a vertex on any existing call.
    FTransform GeometryOpsModeling_WarpFrame(const GeometryOps::FWarpFrameSpec& Spec)
    {
        const int32 ZIndex = GeometryOpsModeling_AxisIndex(Spec.Axis);
        const int32 XIndex = (ZIndex + 1) % 3;
        const int32 YIndex = (ZIndex + 2) % 3;

        FVector FrameX = FVector::ZeroVector;
        FVector FrameY = FVector::ZeroVector;
        FVector FrameZ = FVector::ZeroVector;
        FrameX[XIndex] = 1.0;
        FrameY[YIndex] = 1.0;
        FrameZ[ZIndex] = 1.0;

        return FTransform(FMatrix(FrameX, FrameY, FrameZ, Spec.Center));
    }

    // HARD EDITOR CRASH GUARD for the three WARP DEFORMERS - bend, twist and taper - written once
    // because it is one precondition and one engine defect, reproduced verbatim in three engine
    // files. Each of ApplyBendWarpToMesh / ApplyTwistWarpToMesh / ApplyFlareWarpToMesh drives an
    // FMeshSpaceDeformerOp subclass, and every subclass opens with the same two lines:
    //
    //   FlareMeshOp.cpp:68     FDynamicMeshNormalOverlay* Normals = ResultMesh->Attributes()->PrimaryNormals();
    //   FlareMeshOp.cpp:69     ParallelFor(Normals->MaxElementID(), [...](int32 ElID)
    //   FlareMeshOp.cpp:71-78      if (!Normals->IsElement(ElID)) { return; }
    //                              auto VertexID = Normals->GetParentVertex(ElID);
    //                              const FVector3d& SrcPos = ResultMesh->GetVertex(VertexID);
    //   BendMeshOp.cpp:103-113 identical
    //   TwistMeshOp.cpp:48-58  identical
    //
    // Two unchecked reads, so two shapes to answer, and they get opposite answers:
    //
    // 1. PrimaryNormals() is NULL - an attribute set with zero normal layers. HasAttributes() is
    //    the only test before the dereference (DynamicMeshAttributeSet.h:250 returns nullptr), so
    //    MaxElementID() runs on `this == nullptr`. REFUSED. This is the shape the bevel guard
    //    already refuses for the same reason, and no .pwmodel op can produce it today -
    //    EnableAttributes always creates normal layer 0 and AppendBuffersToMesh only ever sets the
    //    UV layer count exactly (MeshBasicEditFunctions.cpp:890, :983) - so refusing it cannot
    //    reject reachable input.
    //
    // 2. An ALLOCATED element with no parent vertex. IsElement is the only guard and it does not
    //    imply a parent: AppendElement allocates and writes InvalidID (DynamicMeshOverlay.cpp:40-53),
    //    and only a triangle claiming the element fills the parent in (:654-668). GetVertex(-1)
    //    then indexes a TDynamicVector through a uint32 cast and reads out of bounds with no
    //    assert - the fault address is the constant 511 * sizeof(FVector3d) = 0x2FE8 that every
    //    dump in Saved/Crashes carries. REPAIRED, NOT REFUSED: the state is legal and
    //    engine-produced (the overlay's own API documents FreeUnusedElements as its cleanup -
    //    DynamicMeshOverlay.h:340,358 - and FDynamicMesh3::Copy(FMeshShapeGenerator*) orphans one
    //    element per generator normal no triangle names, DynamicMesh3.cpp:174-186), an orphan
    //    carries no geometry by definition, and a .pwmodel author has no op that could clear it.
    //    Refusing here would refuse valid modelling input, which is the worse bug.
    //
    // The failure below is therefore only for a mesh the repair could not fix: an element that IS
    // triangle-referenced yet still unparented. No engine path is known to produce that - every
    // site that invalidates a parent also drops the last reference - so this is the "the mesh is
    // genuinely corrupt" arm, kept because the alternative is an unassertable out-of-bounds read.
    //
    // Returns false with Result already failed; the caller returns it untouched.
    bool GeometryOpsModeling_GuardWarpDeformerNormals(
        UDynamicMesh* Mesh, const TCHAR* OpName, GeometryOps::FOpResult& Result)
    {
        const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
        if (!EditMesh.HasAttributes())
        {
            // The engine's whole normal pass is inside `if (ResultMesh->HasAttributes())`, so a
            // mesh with no attribute set never reaches either read. The vertex loop that follows
            // it is guarded by IsVertex and is safe on any mesh.
            return true;
        }

        if (EditMesh.Attributes()->PrimaryNormals() == nullptr)
        {
            GeometryOps::FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_NORMAL_OVERLAY,
                FString::Printf(TEXT(
                    "%s needs a normal layer on the mesh: the engine's space deformer rotates the "
                    "primary normal overlay and dereferences it without checking that one exists, "
                    "which crashes the editor outright. Add a 'recalculate_normals' op before the "
                    "deformer."), OpName));
            return false;
        }

        int32 FreedElements = 0;
        if (!GeometryUtils::EnsureMeshNormalParentsAreValid(Mesh, FreedElements))
        {
            GeometryOps::FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_NORMAL_OVERLAY,
                FString::Printf(TEXT(
                    "%s cannot run on this mesh: its primary normal overlay still carries an element "
                    "with no parent vertex after the unused elements were freed, and the engine's "
                    "space deformer reads that parent straight into GetVertex with no check, which "
                    "crashes the editor outright. Rebuild the normals with 'recalculate_normals'."),
                    OpName));
            return false;
        }

        if (FreedElements > 0)
        {
            // Said out loud rather than repaired in silence, the same rule the clamps follow: the
            // op edited an overlay the document never mentioned. Freeing an unreferenced element
            // cannot change the shaded result - no triangle was using it.
            Result.Warnings.Add(FString::Printf(TEXT(
                "Freed %d unparented normal element(s) before %s - an earlier op left them "
                "allocated but attached to no triangle, which the engine's deformer reads out of "
                "bounds"), FreedElements, OpName));
        }

        return true;
    }

    // ------------------------------------------------------------------------
    // Local enum -> engine enum.
    //
    // Every one is a switch with NO default and no trailing return outside it, so adding a
    // value to the local enum without adding it here is a compile warning-as-error rather than
    // a silent map to the first enumerator. That is the whole reason these are switches and not
    // static_cast: the two enums agree in order today, and a cast would keep compiling on the
    // day one of them stops.
    // ------------------------------------------------------------------------

    EGeometryScriptPolyOperationArea GeometryOpsModeling_ToEngine(GeometryOps::EPolyOperationArea Value)
    {
        switch (Value)
        {
        case GeometryOps::EPolyOperationArea::PerPolygroup: return EGeometryScriptPolyOperationArea::PerPolygroup;
        case GeometryOps::EPolyOperationArea::PerTriangle:  return EGeometryScriptPolyOperationArea::PerTriangle;
        default:                                            return EGeometryScriptPolyOperationArea::EntireSelection;
        }
    }

    FGeometryScriptMeshEditPolygroupOptions GeometryOpsModeling_ToEngine(const GeometryOps::FPolygroupEditSpec& Spec)
    {
        FGeometryScriptMeshEditPolygroupOptions Options;
        switch (Spec.GroupMode)
        {
        case GeometryOps::EEditPolygroupMode::AutoGenerateNew:
            Options.GroupMode = EGeometryScriptMeshEditPolygroupMode::AutoGenerateNew;
            break;
        case GeometryOps::EEditPolygroupMode::SetConstant:
            Options.GroupMode = EGeometryScriptMeshEditPolygroupMode::SetConstant;
            break;
        default:
            Options.GroupMode = EGeometryScriptMeshEditPolygroupMode::PreserveExisting;
            break;
        }
        Options.ConstantGroup = Spec.ConstantGroup;
        return Options;
    }

    EGeometryScriptOffsetFacesType GeometryOpsModeling_ToEngine(GeometryOps::EOffsetFacesType Value)
    {
        switch (Value)
        {
        case GeometryOps::EOffsetFacesType::VertexNormal: return EGeometryScriptOffsetFacesType::VertexNormal;
        case GeometryOps::EOffsetFacesType::FaceNormal:   return EGeometryScriptOffsetFacesType::FaceNormal;
        default:                                          return EGeometryScriptOffsetFacesType::ParallelFaceOffset;
        }
    }

    EGeometryScriptLinearExtrudeDirection GeometryOpsModeling_ToEngine(GeometryOps::ELinearExtrudeDirection Value)
    {
        return Value == GeometryOps::ELinearExtrudeDirection::AverageFaceNormal
            ? EGeometryScriptLinearExtrudeDirection::AverageFaceNormal
            : EGeometryScriptLinearExtrudeDirection::FixedDirection;
    }

    EGeometryScriptFillHolesMethod GeometryOpsModeling_ToEngine(GeometryOps::EFillHolesMethod Value)
    {
        switch (Value)
        {
        case GeometryOps::EFillHolesMethod::MinimalFill:          return EGeometryScriptFillHolesMethod::MinimalFill;
        case GeometryOps::EFillHolesMethod::PolygonTriangulation: return EGeometryScriptFillHolesMethod::PolygonTriangulation;
        case GeometryOps::EFillHolesMethod::TriangleFan:          return EGeometryScriptFillHolesMethod::TriangleFan;
        case GeometryOps::EFillHolesMethod::PlanarProjection:     return EGeometryScriptFillHolesMethod::PlanarProjection;
        default:                                                  return EGeometryScriptFillHolesMethod::Automatic;
        }
    }

    EGeometryScriptRepairMeshMode GeometryOpsModeling_ToEngine(GeometryOps::ERepairMeshMode Value)
    {
        switch (Value)
        {
        case GeometryOps::ERepairMeshMode::DeleteOnly:   return EGeometryScriptRepairMeshMode::DeleteOnly;
        case GeometryOps::ERepairMeshMode::RepairOrSkip: return EGeometryScriptRepairMeshMode::RepairOrSkip;
        default:                                         return EGeometryScriptRepairMeshMode::RepairOrDelete;
        }
    }

    EGeometryScriptTangentTypes GeometryOpsModeling_ToEngine(GeometryOps::ETangentType Value)
    {
        switch (Value)
        {
        case GeometryOps::ETangentType::PerTriangle:   return EGeometryScriptTangentTypes::PerTriangle;
        // StandardMikkT joined EGeometryScriptTangentTypes in UE 5.7. Its own engine comment says
        // it "will fall back to FastMikkT if unavailable", so FastMikkT on older engines is the
        // engine's own documented behaviour for the request rather than a substitution of ours.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        case GeometryOps::ETangentType::StandardMikkT: return EGeometryScriptTangentTypes::StandardMikkT;
#else
        case GeometryOps::ETangentType::StandardMikkT: return EGeometryScriptTangentTypes::FastMikkT;
#endif
        default:                                       return EGeometryScriptTangentTypes::FastMikkT;
        }
    }

    EGeometryScriptFlareType GeometryOpsModeling_ToEngine(GeometryOps::EFlareType Value)
    {
        switch (Value)
        {
        case GeometryOps::EFlareType::SinSquaredMode: return EGeometryScriptFlareType::SinSquaredMode;
        case GeometryOps::EFlareType::TriangleMode:   return EGeometryScriptFlareType::TriangleMode;
        default:                                      return EGeometryScriptFlareType::SinMode;
        }
    }

    // FGeometryScriptMeshBevelOptions exposes only a filter box. Preserve those selection
    // semantics, then validate each exact group-edge ID and ordered span before handing it to
    // FMeshBevel; a group pair is not a unique topology-edge identity.
    void GeometryOpsModeling_SelectBevelGroupEdges(
        const UE::Geometry::FDynamicMesh3& Mesh,
        const UE::Geometry::FGroupTopology& Topology,
        const GeometryOps::FBevelParams& Params,
        TArray<int32>& OutGroupEdges)
    {
        OutGroupEdges.Reset();

        if (!Params.bApplyFilterBox)
        {
            for (int32 GroupEdgeID = 0; GroupEdgeID < Topology.Edges.Num(); ++GroupEdgeID)
            {
                OutGroupEdges.Add(GroupEdgeID);
            }
            return;
        }

        const UE::Geometry::FAxisAlignedBox3d QueryBox(FBox(Params.FilterBoxMin, Params.FilterBoxMax));
        TSet<int32> FoundMeshEdges;
        UE::Geometry::FDynamicMeshAABBTree3 Spatial(&Mesh, true);
        UE::Geometry::FDynamicMeshAABBTree3::FTreeTraversal EdgeTraversal;
        EdgeTraversal.NextBoxF = [&QueryBox](const UE::Geometry::FAxisAlignedBox3d& Box, int32)
        {
            return Box.Intersects(QueryBox);
        };
        EdgeTraversal.NextTriangleF = [&QueryBox, &FoundMeshEdges, &Mesh, &Params](int32 TriangleID)
        {
            const UE::Geometry::FIndex3i TriangleEdges = Mesh.GetTriEdges(TriangleID);
            for (int32 Index = 0; Index < 3; ++Index)
            {
                const int32 MeshEdgeID = TriangleEdges[Index];
                if (FoundMeshEdges.Contains(MeshEdgeID) || !Mesh.IsGroupBoundaryEdge(MeshEdgeID))
                {
                    continue;
                }

                FVector3d A;
                FVector3d B;
                Mesh.GetEdgeV(MeshEdgeID, A, B);
                const bool bInside = Params.bFullyContained
                    ? (QueryBox.Contains(A) && QueryBox.Contains(B))
                    : (QueryBox.Contains(A) || QueryBox.Contains(B));
                if (bInside)
                {
                    FoundMeshEdges.Add(MeshEdgeID);
                }
            }
        };
        Spatial.DoTraversal(EdgeTraversal);

        TSet<int32> CandidateGroupEdges;
        for (const int32 MeshEdgeID : FoundMeshEdges)
        {
            const int32 GroupEdgeID = Topology.FindGroupEdgeID(MeshEdgeID);
            if (GroupEdgeID >= 0)
            {
                CandidateGroupEdges.Add(GroupEdgeID);
            }
        }

        if (!Params.bFullyContained)
        {
            OutGroupEdges = CandidateGroupEdges.Array();
            return;
        }

        for (const int32 GroupEdgeID : CandidateGroupEdges)
        {
            bool bFullyFound = true;
            for (const int32 MeshEdgeID : Topology.GetGroupEdgeEdges(GroupEdgeID))
            {
                if (!FoundMeshEdges.Contains(MeshEdgeID))
                {
                    bFullyFound = false;
                    break;
                }
            }
            if (bFullyFound)
            {
                OutGroupEdges.Add(GroupEdgeID);
            }
        }
    }

    void GeometryOpsModeling_PreflightBevelEdges(
        const UE::Geometry::FDynamicMesh3& Mesh,
        const UE::Geometry::FGroupTopology& Topology,
        const TArray<int32>& SelectedGroupEdges,
        const TSet<int32>& PriorBevelGroupIDs,
        TArray<int32>& OutSafeGroupEdges,
        int32& OutMeshBoundaryCount,
        int32& OutInvalidSpanCount,
        int32& OutPriorBevelCount)
    {
        OutSafeGroupEdges.Reset();
        OutMeshBoundaryCount = 0;
        OutInvalidSpanCount = 0;
        OutPriorBevelCount = 0;

        for (const int32 GroupEdgeID : SelectedGroupEdges)
        {
            bool bMeshBoundary = false;
            bool bInvalidSpan = false;
            bool bPriorBevel = false;

            if (!Topology.Edges.IsValidIndex(GroupEdgeID))
            {
                bInvalidSpan = true;
            }
            else
            {
                const UE::Geometry::FGroupTopology::FGroupEdge& GroupEdge =
                    Topology.Edges[GroupEdgeID];
                bPriorBevel = PriorBevelGroupIDs.Contains(GroupEdge.Groups.A)
                    || PriorBevelGroupIDs.Contains(GroupEdge.Groups.B);

                // A group pair is deliberately not used as identity: GroupTopology.h states that
                // multiple distinct edges may share one. Validate the exact ID and its ordered
                // mesh-edge span instead, which is what FMeshBevel consumes.
                if (GroupEdge.Span.Edges.Num() == 0
                    || GroupEdge.Span.Vertices.Num() != GroupEdge.Span.Edges.Num() + 1)
                {
                    bInvalidSpan = true;
                }
                if (!Topology.IsIsolatedLoop(GroupEdgeID)
                    && (GroupEdge.EndpointCorners.A < 0
                        || GroupEdge.EndpointCorners.B < 0
                        || !Topology.Corners.IsValidIndex(GroupEdge.EndpointCorners.A)
                        || !Topology.Corners.IsValidIndex(GroupEdge.EndpointCorners.B)))
                {
                    bInvalidSpan = true;
                }

                for (int32 SpanIndex = 0; SpanIndex < GroupEdge.Span.Edges.Num(); ++SpanIndex)
                {
                    const int32 MeshEdgeID = GroupEdge.Span.Edges[SpanIndex];
                    if (!Mesh.IsEdge(MeshEdgeID))
                    {
                        bInvalidSpan = true;
                        continue;
                    }
                    if (Mesh.IsBoundaryEdge(MeshEdgeID))
                    {
                        // MeshBevel.cpp:AddBevelGroupEdge and AddBevelEdgeLoop both return without
                        // adding a span that contains a mesh-boundary edge. This does not reject an
                        // internal open span whose endpoint vertex happens to lie on the boundary.
                        bMeshBoundary = true;
                    }

                    if (Topology.FindGroupEdgeID(MeshEdgeID) != GroupEdgeID)
                    {
                        bInvalidSpan = true;
                    }

                    if (GroupEdge.Span.Vertices.IsValidIndex(SpanIndex + 1))
                    {
                        const UE::Geometry::FIndex2i EdgeVertices = Mesh.GetEdgeV(MeshEdgeID);
                        if (!EdgeVertices.Contains(GroupEdge.Span.Vertices[SpanIndex])
                            || !EdgeVertices.Contains(GroupEdge.Span.Vertices[SpanIndex + 1]))
                        {
                            bInvalidSpan = true;
                        }
                    }
                }
            }

            if (bMeshBoundary)
            {
                ++OutMeshBoundaryCount;
            }
            if (bInvalidSpan)
            {
                ++OutInvalidSpanCount;
            }
            if (bPriorBevel)
            {
                ++OutPriorBevelCount;
            }

            if (bMeshBoundary || bInvalidSpan || bPriorBevel)
            {
                continue;
            }
            OutSafeGroupEdges.Add(GroupEdgeID);
        }
    }
}

namespace GeometryOps
{

// ============================================================================
// Normals and tangents
// ============================================================================

FOpResult RecalculateNormals(UDynamicMesh* Mesh, const FRecalculateNormalsParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptCalculateNormalsOptions NormalOptions;
    NormalOptions.bAreaWeighted = Params.bAreaWeighted;
    NormalOptions.bAngleWeighted = Params.bAngleWeighted;

    // RecomputeNormals raises no error a non-null mesh can reach, so there is no HasError check
    // here - see the sweep table in docs/geometry-debug-sink-sweep.md. It does raise one WARNING
    // that changes the result: on a mesh whose normal overlay is empty it falls back to
    // per-vertex normals ("Consider using 'Set Mesh To Per Vertex Normals' or 'Compute Split
    // Normals' instead") and succeeds, so the caller gets normals of a different KIND than the
    // ones requested and used to have no way to find out.
    //
    // The quoted remedy is what the ENGINE writes, not what a caller reads: both names in it are
    // GeometryScript blueprint nodes rather than verbs of this plugin, so following the sentence
    // could only fail. DrainWarningsInto replaces that one clause with `split_normals` - see its
    // comment in GeometryScriptDebugSink.h - and forwards the diagnosis ahead of it verbatim.
    FGeometryScriptDebugSink Debug;

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // The empty-overlay fallback described above is a UE 5.4 engine change
    // (MeshNormalsFunctions.cpp:137-141). On 5.3 RecomputeNormals neither creates the elements nor
    // warns: it calls RecomputeOverlayNormals/CopyToOverlay against an overlay holding nothing, so
    // a mesh built from buffers carrying no normals - the ordinary case for generated geometry and
    // for every .pwmodel part - keeps no normals at all and the caller is told nothing.
    //
    // Both halves of the 5.4 behaviour are reproduced here so the verb means the same thing on
    // every supported engine: the elements are created with the engine's own predicate and
    // initialiser, and the engine's own warning text is appended to the same sink, which is what
    // routes it through DrainWarningsInto's remedy rewrite instead of past it.
    // Detected through ProcessMesh rather than inside the edit, so a mesh that already carries
    // normals is not handed a spurious change broadcast by the check itself.
    bool bCreatedFallbackNormals = false;
    Mesh->ProcessMesh([&bCreatedFallbackNormals](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        bCreatedFallbackNormals = !ReadMesh.HasAttributes()
            || ReadMesh.Attributes()->PrimaryNormals() == nullptr
            || ReadMesh.Attributes()->PrimaryNormals()->ElementCount() == 0;
    });
    if (bCreatedFallbackNormals)
    {
        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            if (!EditMesh.HasAttributes())
            {
                EditMesh.EnableAttributes();
            }
            EditMesh.Attributes()->PrimaryNormals()->CreateFromPredicate(
                [](int, int, int) { return true; }, 0.0f);
        });
        UE::Geometry::AppendWarning(Debug.Get(), EGeometryScriptErrorType::InvalidInputs,
            FText::FromString(TEXT(
                "RecomputeNormals: TargetMesh did not have normals to recompute; falling back to "
                "per-vertex normals. Consider using 'Set Mesh To Per Vertex Normals' or "
                "'Compute Split Normals' instead.")));
    }
#endif

    UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(Mesh, NormalOptions, false, Debug.Get());
    Debug.DrainWarningsInto(Result);

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult FlipNormals(UDynamicMesh* Mesh, const FFlipNormalsParams& /*Params*/)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    UGeometryScriptLibrary_MeshNormalsFunctions::FlipNormals(Mesh, nullptr);

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult RecomputeTangents(UDynamicMesh* Mesh, const FRecomputeTangentsParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptTangentsOptions TangentOptions;
    TangentOptions.Type = GeometryOpsModeling_ToEngine(Params.Type);
    TangentOptions.UVLayer = Params.UVLayer;

    // The failure channel, and here it guards an ORDINARY input rather than an exotic one.
    // ComputeTangents refuses outright when the mesh has no UV layer at Options.UVLayer or no
    // normal layer at all - "TargetMesh is missing UV Set or Normals required to compute
    // Tangents" (MeshNormalsFunctions.cpp, first statement of its EditMesh lambda) - and a
    // UV-less mesh is not a corner case in this plugin: geometry.import_obj produces one on every
    // call, because AppendBuffersToMesh sets the target's UV layer count from buffers that carry
    // no UVs. So `import_obj` then `recompute_tangents` reported success and wrote no tangents,
    // for as long as both verbs have existed.
    //
    // The op is atomic on that path without any help: the engine checks before it enables
    // tangents or writes an overlay, and it returns from the lambda rather than continuing.
    // FinishOp is not reached, so bForceChanged never claims a change that did not happen.
    //
    // The MikkT platform fallback is a WARNING, not an error (MeshNormalsFunctions.cpp, the
    // StandardMikkT / IsSupported branch), so HasError() correctly ignores it and
    // DrainWarningsInto forwards it - the caller learns its tangents came from FastMikkT instead
    // of failing over a difference it may not care about.
    FGeometryScriptDebugSink Debug;

    UGeometryScriptLibrary_MeshNormalsFunctions::ComputeTangents(Mesh, TangentOptions, Debug.Get());

    Debug.DrainWarningsInto(Result);

    if (Debug.HasError())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_TANGENTS_FAILED,
            FString::Printf(TEXT("Tangent computation failed - %s"), *Debug.ErrorText()));
    }

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult SplitNormals(UDynamicMesh* Mesh, const FSplitNormalsParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptSplitNormalsOptions SplitOptions;
    SplitOptions.bSplitByOpeningAngle = Params.bSplitByOpeningAngle;
    SplitOptions.OpeningAngleDeg = Params.SplitAngle;
    SplitOptions.bSplitByFaceGroup = Params.bSplitByFaceGroup;
    SplitOptions.GroupLayer.bDefaultLayer = Params.bUseDefaultGroupLayer;
    SplitOptions.GroupLayer.ExtendedLayerIndex = Params.ExtendedGroupLayerIndex;

    // A NEGATIVE opening angle is the same class of surprise, and it fails in the direction
    // nobody expects. The engine compares dihedral angles through a COSINE of this value, so
    // the sign is discarded: -60 and +60 select exactly the same edges. Measured on this build,
    // `sphere subdivisions=3` (48 triangles) baked to 144 render vertices at split_angle=0 -
    // 48 x 3, fully faceted - and to 48 at BOTH -60 and +60. So an author reaching for "-1 to
    // split everything" gets LESS splitting than the default, silently, and 0 is the maximum
    // this op splits rather than a floor to go under. It is not clamped, because the absolute
    // value is a legal threshold and running it is what the engine already does; what was
    // missing is being told which threshold ran.
    if (Params.bSplitByOpeningAngle && Params.SplitAngle < 0.0)
    {
        Result.Warnings.Add(FString::Printf(TEXT(
            "split_angle %g is negative, and the engine compares dihedral angles through a cosine "
            "of it - the sign is discarded and %g is the threshold that runs. A negative value "
            "therefore splits LESS than the default, not more; split_angle=0 is the maximum this "
            "op splits and is the spelling of per-face normals"),
            Params.SplitAngle, FMath::Abs(Params.SplitAngle)));
    }

    // Both predicates off is a legal request with a surprising outcome - ComputeSplitNormals
    // still runs and re-averages every normal, so the mesh comes back fully smoothed rather
    // than unchanged. That reads as "split_normals did the opposite of its name", so it says so.
    if (!Params.bSplitByOpeningAngle && !Params.bSplitByFaceGroup)
    {
        Result.Warnings.Add(TEXT(
            "split_normals with both split_by_opening_angle and split_by_face_group off has no "
            "split predicate left, so it recomputes normals with no hard edges at all - the mesh "
            "comes back fully smoothed rather than unchanged"));
    }

    FGeometryScriptCalculateNormalsOptions CalcOptions;
    CalcOptions.bAngleWeighted = true;
    CalcOptions.bAreaWeighted = true;

    UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(
        Mesh, SplitOptions, CalcOptions, nullptr);

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

// ============================================================================
// Topology budget
// ============================================================================

FOpResult SimplifyMesh(UDynamicMesh* Mesh, const FSimplifyMeshParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // Method used to be pinned to StandardQEM here, one line, no comment. That is the engine's
    // FIRST enumerator, not its default: FGeometryScriptSimplifyMeshOptions ships
    // AttributeAware (MeshSimplifyFunctions.h), which is volume-preserving and accounts for
    // vertex normals. Nothing in the tree - no comment, no doc, no test, no board entry - named
    // a reason for the override, and its effect was to hand every caller the crudest of the four
    // metrics while the parameter that would have let them ask for anything else did not exist.
    // The default now follows the engine and the caller can still ask for StandardQEM by name.
    //
    // bAllowSeamCollapse was likewise set explicitly to true, which is also the engine's default,
    // so that line changed nothing and is now just the params default arriving.
    //
    // ENGINE VERSION. FGeometryScriptSimplifyMeshOptions grew RegularizeWeight, QuadricVariant
    // and the five attribute-weight/scale fields in UE 5.8, along with the AttributeAwareV2
    // method. On 5.3-5.7 the struct carries the seven fields written unconditionally below and
    // nothing else, so the 5.8-only writes are guarded out; a caller who set one of them is
    // warned rather than silently served a different simplification.
#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    if (Params.Method == GeometryOps::ESimplifyMethod::AttributeAwareV2)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION, FString::Printf(TEXT(
            "method 'AttributeAwareV2' needs EGeometryScriptRemoveMeshSimplificationType::"
            "AttributeAwareV2, which the engine only ships from UE 5.8; this editor is %d.%d. "
            "'AttributeAware' is the closest metric this engine has, but it weighs only normals "
            "where V2 weighs normals, tangents, colour and UVs - so it is a different "
            "simplification and is not substituted silently. Ask for it by name to use it."),
            ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
    }
#endif

    FGeometryScriptSimplifyMeshOptions SimplifyOptions;
    SimplifyOptions.Method = GeometryOpsModeling_ToEngine(Params.Method);
    SimplifyOptions.bAllowSeamCollapse = Params.bAllowSeamCollapse;
    SimplifyOptions.bAllowSeamSmoothing = Params.bAllowSeamSmoothing;
    SimplifyOptions.bAllowSeamSplits = Params.bAllowSeamSplits;
    SimplifyOptions.bPreserveVertexPositions = Params.bPreserveVertexPositions;
    SimplifyOptions.bRetainQuadricMemory = Params.bRetainQuadricMemory;
    SimplifyOptions.bAutoCompact = Params.bAutoCompact;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    SimplifyOptions.RegularizeWeight = static_cast<float>(Params.RegularizeWeight);
    SimplifyOptions.QuadricVariant = GeometryOpsModeling_ToEngine(Params.QuadricVariant);
    SimplifyOptions.NormalAttributeWeight = static_cast<float>(Params.NormalAttributeWeight);
    SimplifyOptions.TangentAttributeWeight = static_cast<float>(Params.TangentAttributeWeight);
    SimplifyOptions.ColorAttributeWeight = static_cast<float>(Params.ColorAttributeWeight);
    SimplifyOptions.TexCoordAttributeWeight = static_cast<float>(Params.TexCoordAttributeWeight);
    SimplifyOptions.ScaleCorrection = static_cast<float>(Params.ScaleCorrection);
#else
    {
        const FSimplifyMeshParams Defaults;
        TArray<FString> Ignored;
        if (Params.RegularizeWeight != Defaults.RegularizeWeight)       { Ignored.Add(TEXT("regularizeWeight")); }
        if (Params.QuadricVariant != Defaults.QuadricVariant)           { Ignored.Add(TEXT("quadricVariant")); }
        if (Params.NormalAttributeWeight != Defaults.NormalAttributeWeight)     { Ignored.Add(TEXT("normalAttributeWeight")); }
        if (Params.TangentAttributeWeight != Defaults.TangentAttributeWeight)   { Ignored.Add(TEXT("tangentAttributeWeight")); }
        if (Params.ColorAttributeWeight != Defaults.ColorAttributeWeight)       { Ignored.Add(TEXT("colorAttributeWeight")); }
        if (Params.TexCoordAttributeWeight != Defaults.TexCoordAttributeWeight) { Ignored.Add(TEXT("texCoordAttributeWeight")); }
        if (Params.ScaleCorrection != Defaults.ScaleCorrection)         { Ignored.Add(TEXT("scaleCorrection")); }
        if (Ignored.Num() > 0)
        {
            Result.Warnings.Add(FString::Printf(TEXT(
                "simplify: %s reached the op but this engine (UE %d.%d) has no such field on "
                "FGeometryScriptSimplifyMeshOptions - those arrived with UE 5.8 - so the "
                "simplification ran on the engine's own settings for them. A parameter that "
                "changed nothing is reported rather than passed over silently."),
                *FString::Join(Ignored, TEXT(", ")), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
        }
    }
#endif

    const int32 TargetTriCount = FMath::Max(
        1, FMath::RoundToInt(Result.TrianglesBefore * (Params.TargetPercentage / 100.0)));

    UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(
        Mesh, TargetTriCount, SimplifyOptions, nullptr);

    FinishOp(Mesh, Result);
    return Result;
}

FOpResult Subdivide(UDynamicMesh* Mesh, const FSubdivideParams& Params, FSubdivideOutcome* Outcome)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    const int32 Iterations = FMath::Clamp(Params.Iterations, 1, GEOM_MAX_SUBDIVIDE_ITERATIONS);
    if (Iterations != Params.Iterations)
    {
        UE_LOG(LogMcpGeometryHandlersNew, Warning,
            TEXT("Subdivide iterations clamped from %d to %d (MAX_SUBDIVIDE_ITERATIONS)"),
            Params.Iterations, Iterations);
        Result.Warnings.Add(FString::Printf(
            TEXT("Subdivide iterations clamped from %d to %d (max %d)"),
            Params.Iterations, Iterations, GEOM_MAX_SUBDIVIDE_ITERATIONS));
    }
    if (Outcome)
    {
        Outcome->EffectiveIterations = Iterations;
    }

    if (!GeometryUtils::IsMemoryPressureSafe())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_MEMORY_PRESSURE,
            FString::Printf(TEXT("Memory pressure too high (%.1f%% used). Subdivide blocked to prevent OOM."),
                GeometryUtils::GetMemoryUsagePercent()));
    }

    // PN tessellation quadruples the triangle count per iteration.
    int64 EstimatedTriangles = static_cast<int64>(Result.TrianglesBefore);
    for (int32 i = 0; i < Iterations; ++i)
    {
        EstimatedTriangles *= 4;
    }

    if (EstimatedTriangles > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH)
    {
        // FailIn, not Fail: Fail builds a FRESH struct, so returning one here threw away the
        // "iterations clamped" warning recorded above and the before-counts BeginOp captured.
        // The two conditions coincide on exactly the request that needs both reported - a
        // 99-iteration subdivide clamps to GEOM_MAX_SUBDIVIDE_ITERATIONS AND blows the triangle
        // budget - so this return was the only path the clamp could have been observed on, and
        // it silently deleted it.
        return FOpResult::FailIn(Result, ErrorCodes::ERR_POLYGON_LIMIT_EXCEEDED,
            FString::Printf(TEXT("Subdivide would exceed triangle limit. Current: %d, Estimated after: %lld, Max allowed: %d"),
                Result.TrianglesBefore, EstimatedTriangles, GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH));
    }

    // The failure channel. ApplyPNTessellation returns TargetMesh from both of its failure paths
    // - Validate() rejecting the inputs and Compute() returning false - and reports each only
    // into this argument (MeshSubdivideFunctions.cpp), so a subdivide the engine declined used to
    // come back as success with bChanged false.
    //
    // ONE sink for the whole loop, deliberately, and checked INSIDE it. A per-iteration sink
    // would report only the last iteration's verdict; a check after the loop would keep running
    // tessellations on a mesh the engine has already refused once. Breaking at the first failure
    // is also what makes the reported iteration number meaningful.
    //
    // Not atomic, and the message says which iteration so the caller can tell how far it got:
    // iteration N failing after N-1 succeeded leaves a mesh subdivided N-1 times. Rolling that
    // back would need a full mesh copy per iteration on the success path, which is a real cost
    // paid on every subdivide to tidy a partial result the caller can simply re-derive by
    // re-running with fewer iterations from the original.
    FGeometryScriptDebugSink Debug;

    for (int32 i = 0; i < Iterations; ++i)
    {
        FGeometryScriptPNTessellateOptions TessOptions;
        TessOptions.bRecomputeNormals = Params.bRecomputeNormals;
        UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyPNTessellation(Mesh, TessOptions, 1, Debug.Get());

        if (Debug.HasError())
        {
            Debug.DrainWarningsInto(Result);
            FinishOp(Mesh, Result);
            return FOpResult::FailIn(Result, ErrorCodes::ERR_TESSELLATION_FAILED,
                FString::Printf(
                    TEXT("Subdivide failed on iteration %d of %d - %s. The mesh carries the %d "
                         "iteration(s) that did succeed."),
                    i + 1, Iterations, *Debug.ErrorText(), i));
        }
    }

    Debug.DrainWarningsInto(Result);

    FinishOp(Mesh, Result);

    if (Result.TrianglesAfter > GEOM_WARNING_TRIANGLE_THRESHOLD)
    {
        UE_LOG(LogMcpGeometryHandlersNew, Warning,
            TEXT("Subdivide result has %d triangles (warning threshold: %d)"),
            Result.TrianglesAfter, GEOM_WARNING_TRIANGLE_THRESHOLD);
        Result.Warnings.Add(FString::Printf(
            TEXT("Subdivide result has %d triangles (warning threshold %d)"),
            Result.TrianglesAfter, GEOM_WARNING_TRIANGLE_THRESHOLD));
    }

    return Result;
}

FOpResult RemeshUniform(UDynamicMesh* Mesh, const FRemeshUniformParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // Every assignment below is the params default arriving where a hardcoded engine default
    // used to sit: the three lines this replaced set bDiscardAttributes, bReprojectToInputMesh
    // and TargetType to exactly the values FGeometryScriptRemeshOptions and
    // FGeometryScriptUniformRemeshOptions already carry, so the call is unchanged for a caller
    // that sets nothing.
    FGeometryScriptRemeshOptions RemeshOptions;
    RemeshOptions.bDiscardAttributes = Params.bDiscardAttributes;
    RemeshOptions.bReprojectToInputMesh = Params.bReprojectToInputMesh;
    RemeshOptions.SmoothingType = GeometryOpsModeling_ToEngine(Params.SmoothingType);
    RemeshOptions.SmoothingRate = static_cast<float>(Params.SmoothingRate);
    RemeshOptions.MeshBoundaryConstraint = GeometryOpsModeling_ToEngine(Params.MeshBoundaryConstraint);
    RemeshOptions.GroupBoundaryConstraint = GeometryOpsModeling_ToEngine(Params.GroupBoundaryConstraint);
    RemeshOptions.MaterialBoundaryConstraint = GeometryOpsModeling_ToEngine(Params.MaterialBoundaryConstraint);
    RemeshOptions.bAllowFlips = Params.bAllowFlips;
    RemeshOptions.bAllowSplits = Params.bAllowSplits;
    RemeshOptions.bAllowCollapses = Params.bAllowCollapses;
    RemeshOptions.bPreventNormalFlips = Params.bPreventNormalFlips;
    RemeshOptions.bPreventTinyTriangles = Params.bPreventTinyTriangles;
    RemeshOptions.bUseFullRemeshPasses = Params.bUseFullRemeshPasses;
    RemeshOptions.RemeshIterations = Params.RemeshIterations;
    RemeshOptions.bAutoCompact = Params.bAutoCompact;

    FGeometryScriptUniformRemeshOptions UniformOptions;
    UniformOptions.TargetType = GeometryOpsModeling_ToEngine(Params.TargetType);
    UniformOptions.TargetTriangleCount = Params.TargetTriangleCount;
    UniformOptions.TargetEdgeLength = static_cast<float>(Params.TargetEdgeLength);

    UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh(
        Mesh, RemeshOptions, UniformOptions, nullptr);

    // The remesher targets an edge length derived from the budget, so it only approximates it.
    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult Poke(UDynamicMesh* Mesh, const FPokeParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    if (!GeometryUtils::IsMemoryPressureSafe())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_MEMORY_PRESSURE,
            FString::Printf(TEXT("Memory pressure too high (%.1f%% used). Poke operation blocked to prevent OOM."),
                GeometryUtils::GetMemoryUsagePercent()));
    }

    const int64 EstimatedTriangles = static_cast<int64>(Result.TrianglesBefore) * 4;
    if (EstimatedTriangles > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH)
    {
        // FailIn for the same reason as Subdivide's budget guard above: BeginOp has already
        // recorded the before-counts, and Fail would return a struct that never saw them.
        return FOpResult::FailIn(Result, ErrorCodes::ERR_POLYGON_LIMIT_EXCEEDED,
            FString::Printf(TEXT("Poke would exceed triangle limit. Current: %d, Estimated: %lld, Max: %d"),
                Result.TrianglesBefore, EstimatedTriangles, GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH));
    }

    FGeometryScriptMeshOffsetFacesOptions PokeOptions;
    PokeOptions.Distance = Params.Offset;
    PokeOptions.OffsetType = GeometryOpsModeling_ToEngine(Params.OffsetType);
    PokeOptions.AreaMode = GeometryOpsModeling_ToEngine(Params.Common.AreaMode);
    PokeOptions.GroupOptions = GeometryOpsModeling_ToEngine(Params.Common.Groups);
    PokeOptions.UVScale = Params.Common.UVScale;
    PokeOptions.bSolidsToShells = Params.bSolidsToShells;
    UGeometryScriptLibrary_MeshModelingFunctions::ApplyMeshOffsetFaces(
        Mesh, PokeOptions, FGeometryScriptMeshSelection(), nullptr);

    // Only the tessellation half is guarded. ApplyMeshOffsetFaces above raises nothing but its
    // null-mesh error on UE 5.8 (MeshModelingFunctions.cpp), which BeginOp already rejected, so a
    // sink on it would be a check that cannot fire - see the sweep note in
    // GeometryScriptDebugSink.h. ApplyPNTessellation has two real ones.
    //
    // Not atomic: the offset has already been applied when the tessellation is refused, and the
    // message says so rather than implying the mesh is untouched.
    FGeometryScriptDebugSink Debug;

    FGeometryScriptPNTessellateOptions TessOptions;
    TessOptions.bRecomputeNormals = Params.bRecomputeNormals;
    UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyPNTessellation(Mesh, TessOptions, 1, Debug.Get());

    Debug.DrainWarningsInto(Result);

    if (Debug.HasError())
    {
        FinishOp(Mesh, Result);
        return FOpResult::FailIn(Result, ErrorCodes::ERR_TESSELLATION_FAILED,
            FString::Printf(
                TEXT("Poke failed at its tessellation step - %s. The face offset that precedes it "
                     "HAS been applied to the mesh."),
                *Debug.ErrorText()));
    }

    FinishOp(Mesh, Result);

    if (Result.TrianglesAfter > GEOM_WARNING_TRIANGLE_THRESHOLD)
    {
        UE_LOG(LogMcpGeometryHandlersNew, Warning,
            TEXT("Poke result has %d triangles (warning threshold: %d)"),
            Result.TrianglesAfter, GEOM_WARNING_TRIANGLE_THRESHOLD);
        Result.Warnings.Add(FString::Printf(
            TEXT("Poke result has %d triangles (warning threshold %d)"),
            Result.TrianglesAfter, GEOM_WARNING_TRIANGLE_THRESHOLD));
    }

    return Result;
}

// ============================================================================
// Face modeling
// ============================================================================

FOpResult Extrude(UDynamicMesh* Mesh, const FExtrudeParams& Params, FFaceOpOutcome* Outcome)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptMeshLinearExtrudeOptions ExtrudeOptions;
    ExtrudeOptions.Distance = Params.Distance;
    ExtrudeOptions.Direction = Params.Direction;
    // Was hardcoded to FixedDirection. That is also the engine's default, so routing the
    // parameter through leaves the default path byte-identical and only adds the
    // AverageFaceNormal mode that was previously unreachable from either front-end.
    ExtrudeOptions.DirectionMode = GeometryOpsModeling_ToEngine(Params.DirectionMode);
    ExtrudeOptions.AreaMode = GeometryOpsModeling_ToEngine(Params.Common.AreaMode);
    ExtrudeOptions.GroupOptions = GeometryOpsModeling_ToEngine(Params.Common.Groups);
    ExtrudeOptions.UVScale = Params.Common.UVScale;
    ExtrudeOptions.bSolidsToShells = Params.bSolidsToShells;

    FGeometryScriptMeshSelection Selection;
    const EGeometryOpsModeling_FaceSelection SelectionKind =
        GeometryOpsModeling_BuildSelection(Mesh, Params.Faces, Selection, TEXT("extrude"), Result);
    if (Outcome)
    {
        Outcome->FacesSelected = Selection.GetNumSelected();
        Outcome->bFilterMatchedNothing =
            (SelectionKind == EGeometryOpsModeling_FaceSelection::MatchedNothing);
    }

    if (SelectionKind == EGeometryOpsModeling_FaceSelection::MatchedNothing)
    {
        // The author named a subset and nothing satisfied it. Do NOTHING - see the long note on
        // GeometryOpsModeling_BuildSelection for why this is a no-op rather than a whole-mesh
        // fallback and rather than a hard error. FinishOp still runs, so the result is a normal
        // success carrying after-counts equal to the before-counts and bChanged == false.
        FinishOp(Mesh, Result);
        return Result;
    }

    if (!Params.Faces.bHasDirection)
    {
        // The single most confusing silent outcome on this surface: with no face filter the
        // whole mesh is selected, and extruding every face of a CLOSED solid duplicates it
        // rather than pushing a face out.
        Result.Warnings.Add(TEXT(
            "No face direction given, so the whole mesh is extruded - on a closed solid that duplicates it"));
    }

    GeometryOpsModeling_WarnOpenMeshPartialSelection(
        Mesh, Selection.GetNumSelected(), Params.bSolidsToShells, TEXT("extrude"), Result);

    UGeometryScriptLibrary_MeshModelingFunctions::ApplyMeshLinearExtrudeFaces(
        Mesh, ExtrudeOptions, Selection, nullptr);

    FinishOp(Mesh, Result);
    return Result;
}

FOpResult Inset(UDynamicMesh* Mesh, const FInsetParams& Params, FFaceOpOutcome* Outcome)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptMeshInsetOutsetFacesOptions Options;
    // Engine convention: POSITIVE Distance insets inward. FInsetMeshRegion forces the inset
    // direction toward the region centroid (PolyEditingEdgeUtil) and moves the boundary to
    // Midpoint + Distance*InsetDir, so a positive Distance shrinks the face inward. (Reproject
    // is also only honored for the positive/inset case.)
    Options.Distance = Params.Distance;
    // Was hardcoded true, which is also the engine's default - so this is a widening, not a
    // change: an inset that omits `reproject` reprojects exactly as it always has.
    Options.bReproject = Params.bReproject;
    Options.bBoundaryOnly = Params.bBoundaryOnly;
    Options.Softness = Params.Softness;
    Options.AreaScale = Params.AreaScale;
    Options.AreaMode = GeometryOpsModeling_ToEngine(Params.Common.AreaMode);
    Options.GroupOptions = GeometryOpsModeling_ToEngine(Params.Common.Groups);
    Options.UVScale = Params.Common.UVScale;

    FGeometryScriptMeshSelection Selection;
    const EGeometryOpsModeling_FaceSelection SelectionKind =
        GeometryOpsModeling_BuildSelection(Mesh, Params.Faces, Selection, TEXT("inset"), Result);
    if (Outcome)
    {
        Outcome->FacesSelected = Selection.GetNumSelected();
        Outcome->bFilterMatchedNothing =
            (SelectionKind == EGeometryOpsModeling_FaceSelection::MatchedNothing);
    }

    if (SelectionKind == EGeometryOpsModeling_FaceSelection::MatchedNothing)
    {
        FinishOp(Mesh, Result);
        return Result;
    }

    UGeometryScriptLibrary_MeshModelingFunctions::ApplyMeshInsetOutsetFaces(
        Mesh, Options, Selection, nullptr);

    FinishOp(Mesh, Result);
    return Result;
}

FOpResult Outset(UDynamicMesh* Mesh, const FOutsetParams& Params, FFaceOpOutcome* Outcome)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptMeshInsetOutsetFacesOptions Options;
    // Positive Distance INSETS inward (see Inset above), so negate to push the face boundary
    // OUTWARD and grow the footprint. The engine forces reproject off for the negative/outset
    // case regardless of bReproject, so it is left at its struct default; it is a no-op here.
    // That is also why FOutsetParams has no bReproject field to pass - see the comment there.
    Options.Distance = -Params.Distance;
    Options.bBoundaryOnly = Params.bBoundaryOnly;
    Options.Softness = Params.Softness;
    Options.AreaScale = Params.AreaScale;
    Options.AreaMode = GeometryOpsModeling_ToEngine(Params.Common.AreaMode);
    Options.GroupOptions = GeometryOpsModeling_ToEngine(Params.Common.Groups);
    Options.UVScale = Params.Common.UVScale;

    FGeometryScriptMeshSelection Selection;
    const EGeometryOpsModeling_FaceSelection SelectionKind =
        GeometryOpsModeling_BuildSelection(Mesh, Params.Faces, Selection, TEXT("outset"), Result);
    if (Outcome)
    {
        Outcome->FacesSelected = Selection.GetNumSelected();
        Outcome->bFilterMatchedNothing =
            (SelectionKind == EGeometryOpsModeling_FaceSelection::MatchedNothing);
    }

    if (SelectionKind == EGeometryOpsModeling_FaceSelection::MatchedNothing)
    {
        FinishOp(Mesh, Result);
        return Result;
    }

    UGeometryScriptLibrary_MeshModelingFunctions::ApplyMeshInsetOutsetFaces(
        Mesh, Options, Selection, nullptr);

    FinishOp(Mesh, Result);
    return Result;
}

FOpResult OffsetFaces(UDynamicMesh* Mesh, const FOffsetFacesParams& Params, FFaceOpOutcome* Outcome)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptMeshOffsetFacesOptions Options;
    Options.Distance = Params.Distance;
    Options.OffsetType = GeometryOpsModeling_ToEngine(Params.OffsetType);
    Options.AreaMode = GeometryOpsModeling_ToEngine(Params.Common.AreaMode);
    Options.GroupOptions = GeometryOpsModeling_ToEngine(Params.Common.Groups);
    Options.UVScale = Params.Common.UVScale;
    Options.bSolidsToShells = Params.bSolidsToShells;

    FGeometryScriptMeshSelection Selection;
    const EGeometryOpsModeling_FaceSelection SelectionKind = GeometryOpsModeling_BuildSelection(
        Mesh, Params.Faces, Selection, TEXT("offset_faces"), Result);
    if (Outcome)
    {
        Outcome->FacesSelected = Selection.GetNumSelected();
        Outcome->bFilterMatchedNothing =
            (SelectionKind == EGeometryOpsModeling_FaceSelection::MatchedNothing);
    }

    if (SelectionKind == EGeometryOpsModeling_FaceSelection::MatchedNothing)
    {
        FinishOp(Mesh, Result);
        return Result;
    }

    // offset_faces is the other FOffsetMeshRegion caller (MeshModelingFunctions.cpp:712 sets
    // bOffsetFullComponentsAsSolids from bSolidsToShells exactly as extrude's :603 does), so it
    // inherits the same open-mesh solid/shell gap.
    GeometryOpsModeling_WarnOpenMeshPartialSelection(
        Mesh, Selection.GetNumSelected(), Params.bSolidsToShells, TEXT("offset_faces"), Result);

    UGeometryScriptLibrary_MeshModelingFunctions::ApplyMeshOffsetFaces(
        Mesh, Options, Selection, nullptr);

    FinishOp(Mesh, Result);
    return Result;
}

FOpResult Bevel(UDynamicMesh* Mesh, const FBevelParams& Params)
{
    FOpResult Result;

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // FGeometryScriptMeshBevelOptions::Subdivisions was added in UE 5.4; on 5.3 the field does
    // not exist, so a requested subdivision count cannot be honored. Reject rather than
    // silently succeed (a plain bevel with subdivisions=0 still works).
    // The one Fail in this file that may stay a Fail: it runs before BeginOp, so there are no
    // before-counts and no warnings on Result for a fresh struct to discard.
    if (Params.Subdivisions > 0)
    {
        return FOpResult::Fail(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION,
            TEXT("Bevel subdivisions require Unreal Engine 5.4 or newer"));
    }
#endif

    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // ApplyMeshPolygroupBevel bevels POLYGROUP EDGES. A mesh carrying no triangle groups has no
    // such edges, so the call succeeds and changes nothing - warn before that reads as a defect.
    if (!Mesh->GetMeshRef().HasTriangleGroups())
    {
        Result.Warnings.Add(TEXT(
            "Mesh has no polygroups, and bevel operates on polygroup edges only - nothing will change"));
    }
    else
    {
        // THE OPPOSITE FAILURE, and the more expensive one: a mesh where EVERY QUAD is its own
        // polygroup, so every interior quad boundary is a polygroup edge and bevel chamfers all
        // of them. The result is a grid of notches over the whole surface that reads as damage
        // rather than as a chamfer, at roughly 3x the triangle cost - one example model spent
        // 30,000 triangles producing an artifact.
        //
        // It is not a hypothetical mesh, it is what the REVOLVED primitives hand you. torus,
        // arch and revolve all go through FBaseRevolveGenerator, which never reads
        // FGeometryScriptPrimitiveOptions::PolygroupMode at all and defaults its own
        // PolygonGroupingMode to EProfileSweepPolygonGrouping::PerFace - and for a sweep "face"
        // means one QUAD: PolygonId = SweepIndex * NumProfileSegments + ProfileIndex
        // (RevolveGenerator.h, SweepGenerator.cpp). torus majorSegments=16 minorSegments=8 is
        // 128 groups over 256 triangles. Nothing upstream can turn that off, and nothing else
        // reports it.
        //
        // The test is DISTINCT groups against the triangle count, with an absolute floor.
        // Per-quad grouping puts the group count at half the triangle count - but so does a
        // default box (6 groups, 12 triangles), which is a mesh anyone should bevel. The floor
        // is what separates them: this fires on the dense revolved surfaces where the cost is
        // real and stays quiet on the coarse hand-grouped ones where it is not.
        const UE::Geometry::FDynamicMesh3& Read = Mesh->GetMeshRef();
        TSet<int32> DistinctGroups;
        for (const int32 TriangleID : Read.TriangleIndicesItr())
        {
            DistinctGroups.Add(Read.GetTriangleGroup(TriangleID));
        }
        const int32 GroupCount = DistinctGroups.Num();
        const int32 TriangleCount = Read.TriangleCount();
        if (GroupCount >= 32 && GroupCount * 3 >= TriangleCount)
        {
            Result.Warnings.Add(FString::Printf(
                TEXT("Mesh has %d polygroups over %d triangles - close to one group per quad, which is "
                     "what the revolved primitives (torus, arch, revolve) produce. bevel chamfers EVERY "
                     "polygroup edge, so on this mesh it will notch every interior quad boundary rather "
                     "than the silhouette edges, at roughly 3x the triangle cost. Bevel a box or a "
                     "boolean result instead, or regroup the mesh first."),
                GroupCount, TriangleCount));
        }
    }

    // HARD EDITOR CRASH GUARD, a second one, found by auditing the shell crash's siblings.
    // FMeshBevel::ComputeUVs early-outs on HasAttributes() == false and then takes
    // Attributes()->PrimaryUV() and dereferences it unconditionally:
    //
    //   MeshBevel.cpp:3778   if (Mesh.HasAttributes() == false) { return; }
    //   MeshBevel.cpp:3782   FDynamicMeshUVOverlay* UVOverlay = Mesh.Attributes()->PrimaryUV();
    //   MeshBevel.cpp:3788   ComputeArbitraryTrianglePatchUVs(Mesh, *UVOverlay, Triangles);
    //
    // ComputeNormals has the identical shape one function above it. An attribute set with ZERO
    // UV layers therefore passes the only check there is and hands a null reference to a
    // function that immediately calls IsSetTriangle on it. That state is not hypothetical here:
    // AppendBuffersToMesh calls SetNumUVLayers EXACTLY with the number of UV sets it was handed
    // (MeshBasicEditFunctions.cpp:983), so `append_buffers` with no `uvs=` deletes the default
    // layer while leaving the attribute set - and the triangle groups bevel needs - in place.
    //
    // Deliberately NOT MeshHasUsableUVs: a layer that EXISTS but carries no elements - and even
    // one whose triangles are all unset - is safe for bevel, because
    // ComputeArbitraryTrianglePatchUVs WRITES before it reads anything it did not write. Its one
    // read of pre-existing UVs is the neighbour-area sampling loop, and that loop IS guarded by
    // IsSetTriangle (PolyEditingUVUtil.cpp:27); when nothing is set the loop simply contributes
    // no area and UseUVScale falls out of the FMathd::Max(..., 0.0001) floor at :39. The
    // unconditional part is the write - FDynamicMeshUVEditor::SetTriangleUVsFromExpMap
    // (PolyEditingUVUtil.cpp:42-44) - and that CREATES the elements it needs on a real overlay
    // rather than reading existing ones. So the layer's contents never matter; only its
    // existence does.
    //
    // Do not reason from the shell guard here. shell fails on a partially-set channel 0 as well
    // as an absent one because CalculateAverageUVScale (JoinMeshLoops.cpp:54) reads UVs
    // per-triangle with NO IsSetTriangle check; bevel has no equivalent unguarded read, which is
    // why the two guards ask different questions of the same overlay.
    // Only the ZERO-LAYER shape crashes bevel, so only the zero-layer shape is rejected.
    const UE::Geometry::FDynamicMeshAttributeSet* Attributes = Mesh->GetMeshRef().Attributes();
    if (Attributes && (Attributes->PrimaryUV() == nullptr || Attributes->PrimaryNormals() == nullptr))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_NO_UV_ELEMENTS,
            TEXT("bevel needs a UV layer and a normal layer on the mesh: it rebuilds both over the "
                 "bevelled strips and the engine dereferences each overlay without checking it, "
                 "which crashes the editor outright. 'append_buffers' with no 'uvs=' removes the UV "
                 "layer - supply uvs=, or add a 'uv mode=box' op before the bevel."));
    }

    TArray<int32> SelectedGroupEdges;
    TArray<int32> SafeGroupEdges;
    int32 MeshBoundaryEdgeCount = 0;
    int32 InvalidSpanCount = 0;
    int32 PriorBevelEdgeCount = 0;
    bool bAppliedBevel = false;

    // Keep the input only while the shared self-intersection audit can measure it. If this bevel
    // is the first operation to make the surface cross itself, restoring this copy is safer than
    // guessing which valid group edge was "interrupted" from a non-unique pair of group IDs.
    const GeometryUtils::FMeshSelfIntersection SelfIntersectionBefore =
        GeometryUtils::MeasureMeshSelfIntersection(Mesh);
    UE::Geometry::FDynamicMesh3 MeshBeforeBevel;
    if (SelfIntersectionBefore.bMeasured)
    {
        MeshBeforeBevel = Mesh->GetMeshRef();
    }

    // The GeometryScript wrapper does not expose FMeshBevel::ResultInfo or any way to exclude
    // unsupported spans. Build the same group-edge selection here, preflight it, and invoke the
    // engine operation only with edges whose complete spans satisfy its actual preconditions.
    Mesh->EditMesh([&](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        UE::Geometry::FGroupTopology Topology(&EditMesh, true);
        GeometryOpsModeling_SelectBevelGroupEdges(
            EditMesh, Topology, Params, SelectedGroupEdges);
        GeometryOpsModeling_PreflightBevelEdges(
            EditMesh, Topology, SelectedGroupEdges, Params.PriorBevelGroupIDs,
            SafeGroupEdges, MeshBoundaryEdgeCount, InvalidSpanCount, PriorBevelEdgeCount);

        if (SafeGroupEdges.Num() == 0)
        {
            return;
        }

        UE::Geometry::FMeshBevel Bevel;
        Bevel.InsetDistance = Params.Distance;
        Bevel.MaterialIDMode = Params.bInferMaterialID
            ? UE::Geometry::FMeshBevel::EMaterialIDMode::InferMaterialID
            : UE::Geometry::FMeshBevel::EMaterialIDMode::ConstantMaterialID;
        Bevel.SetConstantMaterialID = Params.SetMaterialID;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        Bevel.NumSubdivisions = FMath::Clamp(Params.Subdivisions, 0, 9999);
        Bevel.RoundWeight = FMath::Clamp(Params.RoundWeight, -10.0, 10.0);
#endif
        Bevel.InitializeFromGroupTopologyEdges(EditMesh, Topology, SafeGroupEdges);
        bAppliedBevel = Bevel.Apply(EditMesh, nullptr);
    }, EDynamicMeshChangeType::GeneralEdit,
    EDynamicMeshAttributeChangeFlags::Unknown, /*bDeferChangeEvents=*/false);

    const int32 UnsafeEdgeCount = SelectedGroupEdges.Num() - SafeGroupEdges.Num();
    if (UnsafeEdgeCount > 0)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("Skipped %d unsafe selected polygroup edge span(s) before bevel: %d contain a "
                 "mesh-boundary edge the engine drops, %d have an invalid exact group-edge span, "
                 "and %d are already touched by an earlier bevel. Internal open spans, including "
                 "ones ending at a mesh-boundary vertex, remain eligible."),
            UnsafeEdgeCount, MeshBoundaryEdgeCount, InvalidSpanCount, PriorBevelEdgeCount));
    }

    // RoundWeight is ignored by the engine at Subdivisions == 0 - there is no interior loop to
    // round - so a caller who sets one without the other gets nothing and no explanation.
    if (Params.Subdivisions == 0 && !FMath::IsNearlyEqual(Params.RoundWeight, 1.0))
    {
        Result.Warnings.Add(TEXT(
            "bevel round_weight is ignored at segments=0: roundness describes where the interior "
            "edge loops sit across the bevel face, and at 0 there are none. Set segments >= 1."));
    }

    if (bAppliedBevel && SelfIntersectionBefore.bMeasured)
    {
        const GeometryUtils::FMeshSelfIntersection SelfIntersectionAfter =
            GeometryUtils::MeasureMeshSelfIntersection(Mesh);
        if (SelfIntersectionAfter.bMeasured
            && SelfIntersectionAfter.PairCount > SelfIntersectionBefore.PairCount)
        {
            const int32 IntroducedPairs =
                SelfIntersectionAfter.PairCount - SelfIntersectionBefore.PairCount;
            Mesh->SetMesh(MoveTemp(MeshBeforeBevel));
            Result.Warnings.Add(FString::Printf(
                TEXT("bevel over %d exact polygroup edge span(s) introduced %d%s new "
                     "self-intersecting triangle pair(s). Its output was discarded and the input "
                     "mesh restored; move the bevel before the boolean/topology edit or narrow "
                     "filter_box_min/filter_box_max."),
                SafeGroupEdges.Num(), IntroducedPairs,
                SelfIntersectionAfter.bTruncated ? TEXT("+") : TEXT("")));
        }
    }

    FinishOp(Mesh, Result);
    return Result;
}

FOpResult Shell(UDynamicMesh* Mesh, const FShellParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // EMPTY MESH FIRST, and it is not a stylistic ordering. FDynamicMesh3::IsClosed() returns
    // FALSE when TriangleCount() == 0 (DynamicMesh3_Queries.cpp:752-757 - it counts boundary
    // edges, and a mesh with no edges has none to disqualify it), and BeginOp does not reject
    // empty meshes. So `procedural_mesh` followed by `shell` reaches the UV guard below with
    // "no usable UVs" AND "not closed" both true, and an author who has simply not appended any
    // geometry yet gets told to add UVs to a mesh that has no triangles to put them on.
    //
    // Chosen behaviour: SUCCEED with bChanged=false and a warning, not a failure. ApplyMeshShell
    // on an empty mesh is a genuine no-op - there are no boundary loops, so FJoinMeshLoops is
    // never constructed and the crash this function guards cannot occur - and failing would
    // punish an ordering (`procedural_mesh` before the geometry that fills it) that no other op
    // in this family rejects. It warns rather than passing silently for the same reason bevel
    // warns on a mesh with no polygroups: a modifier that reports success and moves no counts is
    // the most confusing outcome this surface produces, so it has to say why. Returning here
    // rather than falling through to ApplyMeshShell also keeps the empty case off an engine path
    // that has no reason to be exercised with nothing in it.
    if (Mesh->GetMeshRef().TriangleCount() == 0)
    {
        Result.Warnings.Add(TEXT(
            "Mesh has no triangles - shell has no surface to offset and no boundary to stitch, "
            "so nothing will change"));
        FinishOp(Mesh, Result);
        return Result;
    }

    // HARD EDITOR CRASH GUARD. ApplyMeshShell stitches the offset surface back onto the
    // original along every BOUNDARY loop, and the stitcher reads the primary UV overlay with
    // no null check whatsoever:
    //
    //   MeshModelingFunctions.cpp:526   FJoinMeshLoops Join(&EditMesh, LoopA, LoopB); Join.Apply();
    //   JoinMeshLoops.cpp:81            FDynamicMeshUVOverlay* UVOverlay = Mesh->Attributes()->PrimaryUV();
    //
    // Attributes() returns nullptr on a mesh that carries no attribute set (DynamicMesh3.h:1039),
    // so PrimaryUV() runs on `this == nullptr` and loads UVLayers.ArrayNum off a null base ->
    // EXCEPTION_ACCESS_VIOLATION, no assert, no error return, the whole editor goes down. The
    // same read is unsafe one step later for an overlay that EXISTS but has no elements on its
    // triangles: GetTriElements (DynamicMeshOverlay.h:820) feeds ElementTriangles[3*tid], which
    // is -1 on an unset triangle, straight into GetElement (:501) as `ElementID * ElementSize`,
    // indexing a TDynamicVector out of range. The lines two below it in the SAME engine function
    // (JoinMeshLoops.cpp:113 and :120) both test Attributes() first - line 81 is simply missing
    // the guard.
    //
    // Reproduced against a live editor from a .pwmodel document, twice, with an identical
    // callstack:
    //
    //   part nouv_shell {
    //       procedural_mesh
    //       append_buffers vertices=[(0,0,0), (100,0,0), (100,100,0), (0,100,0)] triangles=[(0,1,2), (0,2,3)]
    //       shell thickness=5
    //   }
    //
    // append_buffers with no `uvs=` is what produces the state: AppendBuffersToMesh counts the
    // UV sets it was handed and then calls SetNumUVLayers(NumUVLayers) EXACTLY
    // (MeshBasicEditFunctions.cpp:983), so a buffer append without UVs DELETES the default layer
    // that EnableAttributes had just created.
    //
    // MeshHasUVsOnEveryTriangle, NOT MeshHasUsableUVs. MeshHasUsableUVs asks ElementCount() > 0,
    // which is the right question for the MikkT bake (a size-0 UV array is what that indexes out
    // of range) and NOT the question this call site has. CalculateAverageUVScale does not read
    // the element array as a whole - it reads a SPECIFIC triangle's elements, per boundary edge,
    // with no IsSetTriangle check (JoinMeshLoops.cpp:54). A channel 0 that carries elements for
    // some triangles and none for others therefore satisfies ElementCount() > 0 and still walks
    // the -1 -> Elements[-2] path described above the moment a boundary triangle is one of the
    // unset ones. The engine itself knows the idiom - ComputeArbitraryTrianglePatchUVs performs
    // exactly this IsSetTriangle test (PolyEditingUVUtil.cpp:27) before the equivalent read.
    // The per-triangle predicate closes that gap; the two crash guards stay related (it is
    // defined as MeshHasUsableUVs plus a sweep) rather than hand-rolled and free to drift.
    //
    // The partially-set shape is REACHABLE from ordinary .pwmodel text - it is not a theoretical
    // tightening. Any op that adds triangles to a part mesh WITHOUT writing the UV overlay leaves
    // them at -1 while the earlier geometry's elements keep ElementCount() positive:
    //
    //   part partial_uv {
    //       plane size=(100, 100)                                        <- fills channel 0
    //       append_buffers vertices=[(500,0,0), (600,0,0), (600,100,0)] triangles=[(0,1,2)]
    //       shell thickness=5
    //   }
    //
    // The compiler builds each generator into a FRESH scratch mesh and AppendMesh-es it into the
    // part mesh, so GeometryOps::AppendBuffers' own UV padding does not fire (the scratch has no
    // UVs to preserve). The append then copies min(target, source) UV layers
    // (DynamicMeshEditor.cpp:2002) and only for triangles the SOURCE had set
    // (DynamicMeshEditor.cpp:2256), so a UV-less scratch contributes triangles that stay unset
    // while the plane's elements remain. `append_triangle`, `bridge` and `edge_split` reach the
    // same state through raw FDynamicMesh3::AppendTriangle calls.
    //
    // That document specifically no longer reaches this guard: the compiler now reconciles UV
    // channels at the scratch boundary (FCompiler::ReconcileUVChannelsForAppend), which closes the
    // GENERATOR route because that is the route the scratch indirection created. It does not
    // close the IN-PLACE route - bridge, edge_split and anything else calling AppendTriangle on
    // the part mesh still leave the state mid-part, and only the merge stage sweeps for it - so
    // the guard remains reachable and remains load-bearing. Do not weaken it on the strength of
    // the compiler's repair: this op is also called straight from geometry.shell, where no
    // compiler stage runs at all. Regression tests:
    // PinWright.Geometry.Ops.Modeling.ShellRefusesAnOpenMeshWhoseUVsMissSomeTriangles (this guard,
    // on a hand-built mesh) and
    // PinWright.Model.Compiler.GeometryAppendedWithNoUVsIsGivenThemBeforeItReachesShell (the
    // compiler-side repair that keeps the document above off it).
    //
    // For the one shape the engine does survive (an attribute set with zero UV layers, where
    // PrimaryUV() returns nullptr and CalculateAverageUVScale early-outs), rejecting is still
    // right: that mesh cannot carry the UVs the join would interpolate, and the shell output
    // would need a box projection before it could be baked at all.
    //
    // The IsClosed() half keeps the guard to what is actually reachable and is evaluated only
    // when the UV check already failed. FMeshBoundaryLoops returns with an empty Loops array for
    // a closed mesh (MeshBoundaryLoops.cpp:101), so FJoinMeshLoops is never constructed and a
    // closed UV-less mesh shells today exactly as it always has. It is sound only because the
    // empty mesh was returned above: IsClosed() is false on an empty mesh, so without that
    // early return this term would misfire on exactly the document that has nothing to shell.
    if (!GeometryUtils::MeshHasUVsOnEveryTriangle(Mesh) && !Mesh->GetMeshRef().IsClosed())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_NO_UV_ELEMENTS,
            TEXT("shell needs UVs on EVERY triangle of an open mesh: it stitches the offset surface "
                 "to the original along the boundary, and the engine's stitcher reads the primary UV "
                 "overlay per triangle without checking either the overlay or the triangle, which "
                 "crashes the editor outright. This mesh has no UV channel 0, or has one that leaves "
                 "some triangles unset. Give the whole mesh UVs first (a 'uv mode=box' op, or 'uvs=' "
                 "on append_buffers), or close the mesh with 'fill_holes'."));
    }

    FGeometryScriptMeshOffsetOptions Options;
    Options.OffsetDistance = -Params.Thickness;  // Negative to go inward for shell
    Options.bFixedBoundary = Params.bFixedBoundary;
    Options.SolveSteps = Params.SolveSteps;
    Options.SmoothAlpha = Params.SmoothAlpha;
    Options.bReprojectDuringSmoothing = Params.bReprojectDuringSmoothing;
    Options.BoundaryAlpha = Params.BoundaryAlpha;

    UGeometryScriptLibrary_MeshModelingFunctions::ApplyMeshShell(Mesh, Options, nullptr);

    FinishOp(Mesh, Result);
    return Result;
}

// ============================================================================
// Warp deformers
// ============================================================================

FOpResult Bend(UDynamicMesh* Mesh, const FBendParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // Shared with twist and taper: all three drive an FMeshSpaceDeformerOp subclass whose normal
    // pass reads the primary normal overlay unchecked. See the guard for the two shapes and why
    // one is repaired and the other refused.
    if (!GeometryOpsModeling_GuardWarpDeformerNormals(Mesh, TEXT("bend"), Result))
    {
        return Result;
    }

    FGeometryScriptBendWarpOptions BendOptions;
    // Both were hardcoded, and both hardcoded values are the engine's own defaults, so the
    // default path is unchanged and only the asymmetric / one-sided modes are newly reachable.
    BendOptions.bSymmetricExtents = Params.Extents.bSymmetricExtents;
    BendOptions.LowerExtent = Params.Extents.LowerExtent;
    BendOptions.bBidirectional = Params.bBidirectional;

    // The warp frame was FTransform::Identity here, which pinned the bend to part-local +Z
    // through the part-local origin. It is now Params.Frame, whose defaults ARE that identity.
    UGeometryScriptLibrary_MeshDeformFunctions::ApplyBendWarpToMesh(
        Mesh, BendOptions, GeometryOpsModeling_WarpFrame(Params.Frame),
        Params.Angle, Params.Extent, nullptr);

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult Twist(UDynamicMesh* Mesh, const FTwistParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // TwistMeshOp.cpp:51-58 is the same unguarded parent-vertex read as FlareMeshOp.cpp:71-78.
    if (!GeometryOpsModeling_GuardWarpDeformerNormals(Mesh, TEXT("twist"), Result))
    {
        return Result;
    }

    FGeometryScriptTwistWarpOptions TwistOptions;
    TwistOptions.bSymmetricExtents = Params.Extents.bSymmetricExtents;
    TwistOptions.LowerExtent = Params.Extents.LowerExtent;
    TwistOptions.bBidirectional = Params.bBidirectional;

    UGeometryScriptLibrary_MeshDeformFunctions::ApplyTwistWarpToMesh(
        Mesh, TwistOptions, GeometryOpsModeling_WarpFrame(Params.Frame),
        Params.Angle, Params.Extent, nullptr);

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult Taper(UDynamicMesh* Mesh, const FTaperParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // The op the crash was reported against. FlareMeshOp.cpp:78 is the faulting line: it takes
    // Normals->GetParentVertex(ElID) - InvalidID for an element no triangle claims - straight into
    // ResultMesh->GetVertex, whose bounds check is a checkSlow. NOT extent-dependent: ZMin/ZMax
    // reach the lambda only through T = Clamp((z - ZMin) / (ZMax - ZMin), 0, 1) and the DRx/DRy
    // slopes, so an extent far larger than the mesh half-height flattens T toward 0.5 and is
    // numerically ordinary. Six dumps, one fault address, no dependence on the numbers.
    if (!GeometryOpsModeling_GuardWarpDeformerNormals(Mesh, TEXT("taper"), Result))
    {
        return Result;
    }

    FGeometryScriptFlareWarpOptions FlareOptions;
    FlareOptions.bSymmetricExtents = Params.Extents.bSymmetricExtents;
    FlareOptions.LowerExtent = Params.Extents.LowerExtent;
    FlareOptions.FlareType = GeometryOpsModeling_ToEngine(Params.FlareType);

    // FlareX / FlareY are per PERPENDICULAR axis of this frame, cyclically from Frame.Axis - so
    // at the default Axis=Z they stay world X and world Y, exactly as before.
    UGeometryScriptLibrary_MeshDeformFunctions::ApplyFlareWarpToMesh(
        Mesh, FlareOptions, GeometryOpsModeling_WarpFrame(Params.Frame),
        Params.FlareX, Params.FlareY, Params.Extent, nullptr);

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult NoiseDeform(UDynamicMesh* Mesh, const FNoiseDeformParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // BaseLayer is the ONLY layer the engine struct carries in 5.8 - there is no array behind
    // that name - so multi-octave noise is two noise_deform ops at different frequencies and
    // different seeds, not a layer list this op could grow.
    //
    // Per-version (grepped across all six installed trees; 5.3 keeps GeometryScripting under
    // Plugins/Experimental/ rather than Plugins/Runtime/):
    //   - BaseLayer.RandomSeed / .FrequencyShift  MeshDeformFunctions.h:97,100 on 5.3-5.5,
    //     :99,102 on 5.6-5.8. Present on all six; `seed` and `frequency_shift` are portable.
    //   - NormalSource + EGeometryScriptPerVertexNormalSource  :133,154, **5.8 only**. Absent
    //     from 5.3-5.7, which behave as `Computed` unconditionally - the 5.8 default and this
    //     op's default. A backport drops the assignment and rejects only `average_from_overlay`.
    //   - ApplyPerlinNoiseToMesh was deprecated at 5.7 (:330) because it *squared* the frequency
    //     parameter, so the #else branch below does not merely call an older name: one
    //     `Frequency` value means two different fields either side of 5.7, silently.
    FGeometryScriptPerlinNoiseOptions NoiseOptions;
    NoiseOptions.BaseLayer.Magnitude = Params.Magnitude;
    NoiseOptions.BaseLayer.Frequency = Params.Frequency;
    NoiseOptions.BaseLayer.FrequencyShift = Params.FrequencyShift;
    NoiseOptions.BaseLayer.RandomSeed = Params.Seed;
    NoiseOptions.bApplyAlongNormal = Params.bApplyAlongNormal;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    NoiseOptions.NormalSource = (Params.NormalSource == ENoiseNormalSource::AverageFromOverlay)
        ? EGeometryScriptPerVertexNormalSource::AverageFromOverlay
        : EGeometryScriptPerVertexNormalSource::Computed;
#else
    // The backport the comment above describes: 5.3-5.7 carry no NormalSource field and behave
    // as `Computed` unconditionally, which is this op's default, so `computed` needs no
    // assignment - and `averageFromOverlay` is refused rather than quietly served as `computed`.
    if (Params.NormalSource == ENoiseNormalSource::AverageFromOverlay)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION, FString::Printf(TEXT(
            "normalSource 'averageFromOverlay' needs FGeometryScriptPerlinNoiseOptions::NormalSource, "
            "which the engine only ships from UE 5.8; this editor is %d.%d and always derives the "
            "displacement normals from the geometry. Use normalSource 'computed', which is what "
            "this engine would do anyway."),
            ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
    }
#endif

    if (Params.MagnitudeMode == ENoiseMagnitudeMode::Absolute)
    {
        // THE ENGINE CALL, UNTOUCHED. Relative mode below is a local loop, and the reason this
        // branch still exists rather than being folded into it is that every .pwmodel and every
        // geometry.noise_deform written so far took this path: a local re-implementation that
        // agreed to within a float would still have moved every vertex of every existing model.
        FGeometryScriptMeshSelection Selection;

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        UGeometryScriptLibrary_MeshDeformFunctions::ApplyPerlinNoiseToMesh2(
            Mesh, Selection, NoiseOptions, nullptr);
#else
        UGeometryScriptLibrary_MeshDeformFunctions::ApplyPerlinNoiseToMesh(
            Mesh, Selection, NoiseOptions, nullptr);
#endif

        FinishOp(Mesh, Result, /*bForceChanged=*/true);
        return Result;
    }

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    // Relative mode is built on UGeometryScriptLibrary_MeshDeformFunctions::ComputePerlinNoise,
    // the engine's public entry to the SAME noise field the mesh path uses. That function is
    // 5.8-only (MeshDeformFunctions.h:344), and re-deriving the field here would mean copying
    // the engine's private seed -> offsets chain - a pattern that could silently diverge from the
    // absolute mode's on any engine version, which is the one property this mode exists to keep.
    return FOpResult::FailIn(Result, ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION, FString::Printf(TEXT(
        "magnitudeMode 'relative' needs UGeometryScriptLibrary_MeshDeformFunctions::"
        "ComputePerlinNoise, which the engine only ships from UE 5.8; this editor is %d.%d. Use "
        "magnitudeMode 'absolute', which drives the engine's mesh noise call directly and is "
        "available on every supported engine."),
        ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
#else
    // ---- relative: magnitude as a fraction of the vertex's mean one-ring edge length --------
    //
    // The engine has no per-vertex magnitude - FGeometryScriptPerlinNoiseOptions carries one
    // scalar for the whole mesh - so this is a local loop. What it deliberately does NOT do is
    // re-derive the noise: the field comes from the engine's own public ComputePerlinNoise with
    // Magnitude pinned to 1, which runs the identical seed -> offsets -> Frequency * (Pos +
    // Offset) -> PerlinNoise3D chain the mesh path runs. Only the per-vertex scale differs, so a
    // relative pass and an absolute pass at the same seed sample the same field at the same
    // places and a mesh can be switched between the modes without the pattern moving.
    if (!Params.bApplyAlongNormal)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT, TEXT(
            "magnitudeMode 'relative' needs applyAlongNormal true. Relative scales a single "
            "displacement by the vertex's own mean one-ring edge length; the vector form "
            "displaces by three DECORRELATED noise fields at once, and the engine derives those "
            "three fields' offsets in a helper with no public entry point. Reproducing them here "
            "would mean copying private engine code that can change under us without a compile "
            "error - a silently different pattern on the next engine version. Use "
            "magnitudeMode 'absolute' for the vector form, or leave applyAlongNormal true."));
    }

    // Snapshot first, displace second. The mean one-ring edge length of a vertex is measured on
    // the ORIGINAL positions, so a vertex must not be able to read a neighbour that has already
    // moved - which is what an in-place loop would do, and would make the result depend on
    // vertex iteration order.
    TArray<int32> VertexIDs;
    FGeometryScriptVectorList Positions;
    TArray<FVector3d> OriginalPositionsByVertexID;
    Positions.Reset();

    UE::Geometry::FMeshNormals VertexNormals(&Mesh->GetMeshRef());
    bool bNormalsAvailable = true;
    {
        const UE::Geometry::FDynamicMesh3& ReadMesh = Mesh->GetMeshRef();
        VertexIDs.Reserve(ReadMesh.VertexCount());
        Positions.List->Reserve(ReadMesh.VertexCount());
        OriginalPositionsByVertexID.SetNum(ReadMesh.MaxVertexID());
        for (int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            VertexIDs.Add(VertexID);
            const FVector3d Position = ReadMesh.GetVertex(VertexID);
            Positions.List->Add((FVector)Position);
            OriginalPositionsByVertexID[VertexID] = Position;
        }

        if (Params.NormalSource == ENoiseNormalSource::AverageFromOverlay)
        {
            // The engine reports this into its Debug object and then returns having moved
            // nothing; this op has no Debug object, so it would have been a silent no-op.
            if (!ReadMesh.HasAttributes() || !ReadMesh.Attributes()->PrimaryNormals())
            {
                bNormalsAvailable = false;
            }
            else
            {
                VertexNormals.GetVertexNormalsFromOverlayNormals();
            }
        }
        else
        {
            VertexNormals.ComputeVertexNormals();
        }
    }

    if (!bNormalsAvailable)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT, TEXT(
            "normalSource 'averageFromOverlay' needs a primary normal overlay, and this mesh has "
            "none. Run a normals op first (geometry.recompute_normals, or a 'normals' op in a "
            ".pwmodel part), or use normalSource 'computed', which derives the normals from the "
            "geometry and needs no overlay."));
    }

    // Magnitude 1 makes this the UNIT noise field; the per-vertex magnitude is applied below.
    FGeometryScriptPerlinNoiseOptions UnitNoiseOptions = NoiseOptions;
    UnitNoiseOptions.BaseLayer.Magnitude = 1.0f;

    const FGeometryScriptScalarList NoiseValues =
        UGeometryScriptLibrary_MeshDeformFunctions::ComputePerlinNoise(Positions, UnitNoiseOptions);
    if (!NoiseValues.List.IsValid() || NoiseValues.List->Num() != VertexIDs.Num())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_OPERATION_FAILED, FString::Printf(TEXT(
            "the noise field returned %d values for %d vertices. Nothing was displaced; this is "
            "an engine-side failure of ComputePerlinNoise rather than a bad parameter."),
            NoiseValues.List.IsValid() ? NoiseValues.List->Num() : 0, VertexIDs.Num()));
    }

    int32 VerticesModified = 0;
    int32 IsolatedVertices = 0;

    Mesh->EditMesh([&](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        for (int32 Index = 0; Index < VertexIDs.Num(); ++Index)
        {
            const int32 VertexID = VertexIDs[Index];
            const FVector3d OriginalPos = (FVector3d)(*Positions.List)[Index];

            // Mean length of the edges INCIDENT TO THIS VERTEX, measured on the snapshot. A
            // boundary vertex simply has fewer of them - its open fan is a smaller sample of the
            // same quantity, not a different one - and two coincident vertices with disjoint
            // one-rings each get their own mean, which is what can open a hard seam. Both are
            // stated on ENoiseMagnitudeMode::Relative rather than special-cased here.
            double EdgeLengthSum = 0.0;
            int32 EdgeCount = 0;
            for (int32 EdgeID : EditMesh.VtxEdgesItr(VertexID))
            {
                const UE::Geometry::FIndex2i EdgeVertices = EditMesh.GetEdgeV(EdgeID);
                const int32 FarVertex = (EdgeVertices.A == VertexID) ? EdgeVertices.B : EdgeVertices.A;
                EdgeLengthSum += FVector3d::Distance(
                    OriginalPos, OriginalPositionsByVertexID[FarVertex]);
                ++EdgeCount;
            }

            // No one-ring, so no local feature size. Displacing it by an invented number is the
            // one thing that cannot be right, so it stays put and gets counted.
            if (EdgeCount == 0)
            {
                ++IsolatedVertices;
                continue;
            }

            const double MeanEdgeLength = EdgeLengthSum / static_cast<double>(EdgeCount);
            const double Displacement =
                Params.Magnitude * MeanEdgeLength * (*NoiseValues.List)[Index];

            const FVector3d NewPos = OriginalPos + Displacement * VertexNormals[VertexID];
            if (NewPos.Equals(OriginalPos, UE_DOUBLE_SMALL_NUMBER))
            {
                continue;
            }

            EditMesh.SetVertex(VertexID, NewPos);
            ++VerticesModified;
        }
    });

    if (IsolatedVertices > 0)
    {
        Result.Warnings.Add(FString::Printf(TEXT(
            "magnitude: %d vertices have no incident edges, so they have no mean one-ring edge "
            "length to be a fraction of and were left where they are. A vertex with no edges "
            "carries no surface; merge_vertices or a repair op removes them if they are not "
            "wanted."), IsolatedVertices));
    }

    if (VerticesModified == 0)
    {
        Result.Warnings.Add(TEXT(
            "magnitude: relative mode displaced nothing. Magnitude 0, or a mesh with no vertices "
            "that have edges. An op that runs and changes nothing is reported rather than passed "
            "over silently."));
    }

    // Normals are deliberately NOT recomputed, because the engine's noise call does not recompute
    // them either: doing it on one mode only would make switching magnitudeMode change the
    // shading as well as the displacement. Follow either mode with a normals op.
    FinishOp(Mesh, Result, /*bForceChanged=*/VerticesModified > 0);
    return Result;
#endif
}

FOpResult HarmonicDeform(UDynamicMesh* Mesh, const FHarmonicDeformParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // ---- refuse, before touching a vertex --------------------------------------------
    //
    // Every check below describes a request that would INVERT or TANGLE the surface, and this
    // op exists to make two inverted models rebuildable. Clamping an inverting request into a
    // legal one would hand back a shape the author did not ask for and did not get told about,
    // so all three refuse.
    double AmplitudeSum = 0.0;
    for (int32 Index = 0; Index < Params.Terms.Num(); ++Index)
    {
        const FHarmonicTerm& Term = Params.Terms[Index];
        if (Term.Order < 1)
        {
            return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT(
                    "harmonic term %d has order %d: an order is cycles per revolution and must be "
                    "1 or more. Order 0 is a constant offset dressed as a harmonic, and a negative "
                    "one is the same wave as its absolute value with the phase turned around."),
                    Index, Term.Order));
        }
        AmplitudeSum += FMath::Abs(Term.Amplitude);
    }

    if (Params.Target == EHarmonicTarget::Radial && AmplitudeSum >= 1.0)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT(
                "the radial amplitudes total %.4g. Radial displacement scales each vertex's "
                "perpendicular radius by (1 + sum of the terms), so a total of 1 or more lets that "
                "scale reach zero or turn negative - which folds the surface through its own axis "
                "and inverts the triangles at those angles. Keep the total below 1, or use "
                "target=axial, whose amplitudes are absolute and have no such bound."),
                AmplitudeSum));
    }

    if (Params.Terms.Num() == 0)
    {
        Result.Warnings.Add(TEXT(
            "terms is empty, so harmonic_deform moved nothing. An op that runs and changes "
            "nothing is reported rather than passed over silently."));
        FinishOp(Mesh, Result, /*bForceChanged=*/false);
        return Result;
    }

    const int32 AxisIndex = GeometryOpsModeling_AxisIndex(Params.Axis);

    // The two perpendicular axes, taken CYCLICALLY, so azimuth is measured the same handed way
    // about all three: about Z it is atan2(y, x), which is the convention both source shapes
    // this op was built for are written in.
    const int32 UAxis = (AxisIndex + 1) % 3;
    const int32 VAxis = (AxisIndex + 2) % 3;

    const FVector3d Center(Params.Center);

    // Distinct azimuths actually present, quantized, for the Nyquist warning below. A revolve
    // of N steps has exactly N of them, so this measures the mesh's angular SAMPLING rather
    // than guessing it from a vertex count.
    TSet<int32> AzimuthBuckets;
    int32 VerticesModified = 0;

    Mesh->EditMesh([&](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        for (int32 VertexID : EditMesh.VertexIndicesItr())
        {
            const FVector3d OriginalPos = EditMesh.GetVertex(VertexID);
            const FVector3d FromCenter = OriginalPos - Center;

            const double U = FromCenter[UAxis];
            const double V = FromCenter[VAxis];
            const double Radius = FMath::Sqrt(U * U + V * V);

            // On the axis, azimuth is undefined. Displacing an apex by the arbitrary value at
            // theta = 0 is what tangles the triangle fan around it, so it stays put.
            if (Radius <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const double Theta = FMath::Atan2(V, U);
            AzimuthBuckets.Add(FMath::RoundToInt32(Theta * 100000.0));

            double Sum = 0.0;
            for (const FHarmonicTerm& Term : Params.Terms)
            {
                Sum += Term.Amplitude
                    * FMath::Sin(Term.Order * Theta + FMath::DegreesToRadians(Term.PhaseDegrees));
            }

            FVector3d NewPos = OriginalPos;
            if (Params.Target == EHarmonicTarget::Radial)
            {
                // A scale of the perpendicular radius, leaving the azimuth and the axis
                // coordinate alone. Strictly positive because AmplitudeSum < 1 was enforced
                // above, so the map is radially monotone and cannot reverse a triangle.
                const double Scale = 1.0 + Sum;
                NewPos[UAxis] = Center[UAxis] + U * Scale;
                NewPos[VAxis] = Center[VAxis] + V * Scale;
            }
            else
            {
                NewPos[AxisIndex] = OriginalPos[AxisIndex] + Sum;
            }

            // Amplitude 0 must be a no-op, and it is one here by arithmetic rather than by a
            // special case: Sum is exactly 0, so NewPos equals OriginalPos and nothing is
            // written. The comparison is what keeps bChanged honest.
            if (NewPos.Equals(OriginalPos, UE_DOUBLE_SMALL_NUMBER))
            {
                continue;
            }

            EditMesh.SetVertex(VertexID, NewPos);
            ++VerticesModified;
        }
    });

    // Nyquist, against the sampling just measured. A term of order n needs more than 2n samples
    // per revolution to be a wave at all; below that it ALIASES - the displacement reads as a
    // different, lower-order wave, and the triangles between two badly separated samples can
    // cross. This is the one way a within-bounds radial request still damages the surface, so it
    // is measured rather than assumed away.
    const int32 SampleCount = AzimuthBuckets.Num();
    if (SampleCount > 0)
    {
        for (const FHarmonicTerm& Term : Params.Terms)
        {
            if (Term.Order * 2 >= SampleCount)
            {
                Result.Warnings.Add(FString::Printf(TEXT(
                    "terms: order %d against %d distinct azimuths in the mesh - at or past the "
                    "Nyquist limit of %d, so this term aliases into a lower-order wave rather "
                    "than the one requested, and the surface between two samples may cross. "
                    "Raise the base primitive's angular segment count above %d."),
                    Term.Order, SampleCount, SampleCount / 2, Term.Order * 2));
            }
        }
    }

    // The displacement invalidates the normals it did not move, exactly as spherify's does; the
    // same sink is drained for the same empty-overlay warning.
    if (VerticesModified > 0)
    {
        FGeometryScriptDebugSink RecomputeNormalsSink;
        UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(
            Mesh, FGeometryScriptCalculateNormalsOptions(), false, RecomputeNormalsSink.Get());
        RecomputeNormalsSink.DrainWarningsInto(Result);
    }

    FinishOp(Mesh, Result, /*bForceChanged=*/VerticesModified > 0);
    return Result;
}

FOpResult Smooth(UDynamicMesh* Mesh, const FSmoothParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptIterativeMeshSmoothingOptions SmoothOptions;
    SmoothOptions.NumIterations = Params.Iterations;
    SmoothOptions.Alpha = Params.Alpha;

    FGeometryScriptMeshSelection Selection;

    UGeometryScriptLibrary_MeshDeformFunctions::ApplyIterativeSmoothingToMesh(
        Mesh, Selection, SmoothOptions, nullptr);

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult Relax(UDynamicMesh* Mesh, const FRelaxParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptIterativeMeshSmoothingOptions SmoothOptions;
    SmoothOptions.NumIterations = Params.Iterations;
    SmoothOptions.Alpha = Params.Strength;

    UGeometryScriptLibrary_MeshDeformFunctions::ApplyIterativeSmoothingToMesh(
        Mesh, FGeometryScriptMeshSelection(), SmoothOptions, nullptr);

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult Stretch(UDynamicMesh* Mesh, const FStretchParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // One axis mapping, not two: the open-coded switch that used to sit here was a second copy
    // of GeometryOpsModeling_AxisIndex's "anything else is Z" fallback, so a change to one
    // would have silently left stretch and cylindrify disagreeing about the same `axis` string.
    FVector ScaleVec = FVector::OneVector;
    ScaleVec[GeometryOpsModeling_AxisIndex(Params.Axis)] = Params.Factor;

    // The upper bound this guard used to carry (< 5.5) pinned the engine call to 5.4 ALONE, so
    // on 5.8 - the only version this plugin builds - the hand-rolled `#else` was the branch that
    // ran, and it is wrong in two independent ways. This is the same dead-guard defect that made
    // `mirror` emit inside-out geometry (GeometryOps_Boolean.cpp); ScaleMesh still takes
    // bFixOrientationForNegativeScale in 5.8 (MeshTransformFunctions.h:68-75).
    //
    //  1. A NEGATIVE factor is a reflection, determinant < 0, so it turns every triangle inside
    //     out. Nothing clamps Factor, so `stretch axis=z factor=-1` is reachable from both
    //     front-ends and produced a black, backface-culled mesh.
    //  2. A non-uniform scale needs the INVERSE scale applied to normals and tangents or the
    //     shading no longer matches the surface. MeshTransforms::Scale does that
    //     (MeshTransforms.cpp:148-177); the loop below moved positions only, so every stretch -
    //     positive ones included - left stale normals behind.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(
        Mesh, ScaleVec, FVector::ZeroVector, /*bFixOrientationForNegativeScale=*/ true, nullptr);
#else
    // Fallback: scale the mesh through the low-level API, then undo the inversion by hand.
    // Positions only - see (2) above; this branch does not reach 5.8 and is not the shipped path.
    {
        UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
        for (int32 VID : EditMesh.VertexIndicesItr())
        {
            FVector3d Pos = EditMesh.GetVertex(VID);
            Pos.X *= ScaleVec.X;
            Pos.Y *= ScaleVec.Y;
            Pos.Z *= ScaleVec.Z;
            EditMesh.SetVertex(VID, Pos);
        }
        if (ScaleVec.X * ScaleVec.Y * ScaleVec.Z < 0.0)
        {
            EditMesh.ReverseOrientation(/*bFlipNormals=*/ true);
        }
    }
#endif

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

namespace
{
    // ------------------------------------------------------------------------------------
    // The anisotropy diagnostic shared by spherify and cylindrify.
    //
    // WHAT THESE TWO OPS DO NOT DO: fold the surface. Both are the same radial map - a point
    // at distance d from the centre (spherify) or from the axis (cylindrify) moves to
    // f(d) = (1-t)*d + t*R. For every t in [0,1] that is strictly increasing in d and it
    // preserves direction, so it is a homeomorphism of space minus the centre and cannot push
    // a convex surface through itself. A box is convex at any segment count.
    //
    // Measured rather than argued, on a faithful port of FGridBoxMeshGenerator (the generator
    // AppendBox drives; `segments` is VERTICES per edge, so (5,5,4) is 82 vertices / 160
    // triangles) with the spherify map applied exactly as below and a triangle-triangle SAT
    // test that is valid for coplanar pairs:
    //
    //     box 150x118x92 segments (5,5,4), factor 0.4 / 0.5 / 0.72 / 0.82 / 1.0
    //     box 132x118x104 factor 0.70;  cube 100^3 factor 1.0
    //     box 400x100x60 factor 1.0;    box 400x40x40 (10:1) factor 1.0
    //     box 150x118x92 segments (9,9,9) factor 1.0
    //   -> 0 inverted triangles, 0 degenerate triangles, 0 self-intersecting triangle pairs,
    //      in EVERY one of them. (The undeformed boxes measure 0 as well, so the test is not
    //      simply blind.)
    //
    // So the black cavity a .pwmodel author sees after `box -> spherify -> noise_deform` is not
    // spherify folding. It is the NEXT op folding, on a mesh whose triangle sizes spherify has
    // skewed: the same 150x118x92 box goes in with an edge-length ratio of 1.64:1 and comes out
    // at 2.81:1 (factor 0.82), shortest edge 21.3 uu, against a first noise octave of magnitude
    // 12 - i.e. up to 24 uu of relative displacement across a 21 uu edge. Displacement along the
    // normal self-intersects wherever its magnitude approaches the local edge length.
    //
    // WHAT THE METRIC IS. Write s(d) = (1-t) + t*R/d for the radial scale at distance d, and
    // evaluate it at the bounding box's own half-extents. The warning fires on
    //
    //     Anisotropy = s(E_small) / s(E_large) - 1
    //
    // where E_large / E_small are the largest and smallest half-extents of the axes the op
    // reshapes (all three for spherify; the two perpendicular to the axis for cylindrify).
    // For spherify R IS E_large, so s(E_large) = 1 and the expression collapses to the closed
    // form t * (E_large/E_small - 1). At t = 1 the R cancels for both ops and the metric is
    // exactly the aspect ratio minus one, which is why one threshold can serve both.
    //
    // WHY NOT THE RAW SCALE SPREAD max(s)/min(s), which is the obvious candidate: a PERFECT
    // CUBE at factor 1.0 scores sqrt(3) = 1.732 on it - higher than the 1.585 of the oblong
    // box the shipped crystal_cluster.pwmodel calls clean. The corner-vs-face-centre spread is
    // inherent to turning any box into a sphere and is not a defect; it must not drive a
    // warning. The metric above is 0 for a cube at every factor and 0 for every mesh at
    // factor 0, which is the property that makes it about the mesh's shape rather than about
    // the op existing.
    //
    // WHY THE BOUNDING BOX AND NOT THE VERTICES ACTUALLY VISITED. The largest scale falls at
    // the centre of the flattest face, and whether a VERTEX lands there depends on the parity
    // of the segment count: 5 vertices per edge puts one exactly at the centre (measured
    // max scale 1.5170, matching the closed form), 6 does not (1.4149). A vertex-sampled
    // threshold would therefore flip on an even/odd segment change that alters no shape at
    // all. The closed form is parity-stable. The measured range is still reported in the
    // warning text, because that is what the op did to this mesh.
    //
    // THE THRESHOLD, 1/3. Measured Anisotropy on the three configurations the author reported:
    //
    //     150x118x92 @ 0.82  FAILING  0.517     (aspect 1.630:1)
    //     150x118x92 @ 0.72  FAILING  0.454
    //     132x118x104 @ 0.70 CLEAN    0.189     (aspect 1.269:1)
    //     any cube, any factor         0.000
    //
    // 1/3 sits between them with 1.36x clearance under the lowest failing case and 1.77x over
    // the clean one, and it states something legible: the flattest face is pushed out by more
    // than a third of its own half-extent while the longest face does not move. Note the
    // "clean" configuration is NOT a controlled comparison - it differs from the failing one in
    // aspect, in factor AND in carrying a remesh_uniform that re-uniformises exactly the edge
    // lengths spherify skewed - so the threshold is calibrated against a boundary, not a
    // controlled experiment. Move it if a controlled repro says otherwise, and move these
    // numbers with it.
    constexpr double GeometryOpsModeling_AnisotropyWarnThreshold = 1.0 / 3.0;

    void GeometryOpsModeling_WarnRadialAnisotropy(
        const TCHAR* OpName,
        const TCHAR* AspectLabel,
        const FVector& BoxSize,
        double LargeExtent,
        double SmallExtent,
        double TargetRadius,
        double Factor,
        double MeasuredMinScale,
        double MeasuredMaxScale,
        int32 VerticesMeasured,
        FOpResult& Result)
    {
        if (VerticesMeasured <= 0)
        {
            return;
        }

        // A bounding box that is flat on one axis has no finite aspect ratio. Flooring the
        // divisor rather than bailing out keeps the loudest input from being the silent one:
        // the printed aspect comes out enormous, which is the honest description of a plane
        // being projected onto a sphere.
        const double SafeSmall = FMath::Max(SmallExtent, static_cast<double>(KINDA_SMALL_NUMBER));
        const double SafeLarge = FMath::Max(LargeExtent, static_cast<double>(KINDA_SMALL_NUMBER));

        const double ScaleAtSmall = (1.0 - Factor) + Factor * TargetRadius / SafeSmall;
        const double ScaleAtLarge = (1.0 - Factor) + Factor * TargetRadius / SafeLarge;
        if (ScaleAtLarge <= KINDA_SMALL_NUMBER)
        {
            return;
        }

        const double Anisotropy = ScaleAtSmall / ScaleAtLarge - 1.0;
        if (Anisotropy <= GeometryOpsModeling_AnisotropyWarnThreshold)
        {
            return;
        }

        Result.Warnings.Add(FString::Printf(TEXT(
            "%s: the mesh's bounding box is %.4g x %.4g x %.4g, a %s aspect ratio of %.2f:1, and "
            "at factor %g that makes the radial map strongly anisotropic - it scales the flattest "
            "direction by %.3f while scaling the longest by %.3f (anisotropy %.2f, warns above "
            "%.2f). The measured radial scale over the %d vertices it moved ran %.3f to %.3f. The "
            "map is radially monotone, so %s did NOT fold the surface - but it leaves the "
            "triangles that size-uneven, and a displacement op run afterwards (noise_deform, "
            "displace, the warp deformers) self-intersects wherever its magnitude approaches the "
            "local edge length, which is what shows up as a black cavity. Start from near-cubic "
            "source geometry, lower the factor, or run remesh_uniform between %s and any "
            "displacement op."),
            OpName,
            BoxSize.X, BoxSize.Y, BoxSize.Z,
            AspectLabel,
            SafeLarge / SafeSmall,
            Factor,
            ScaleAtSmall, ScaleAtLarge,
            Anisotropy, GeometryOpsModeling_AnisotropyWarnThreshold,
            VerticesMeasured, MeasuredMinScale, MeasuredMaxScale,
            OpName, OpName));
    }
}

FOpResult Spherify(UDynamicMesh* Mesh, const FSpherifyParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    const double Factor = ClampFactorWarn(Params.Factor, TEXT("factor"), Result);

    // NOT the bounding SPHERE's radius, whatever the parameter doc used to say. This is the
    // largest HALF-EXTENT - 75 for a 150x118x92 box, where the bounding sphere is 105.93 - so
    // the target is the sphere that just touches the two most distant FACES, and the corners
    // sit outside it. Corners are therefore pulled INWARD (scale 0.76 at factor 0.82) while the
    // flat centres of the short faces are pushed OUTWARD (1.52). That is the whole source of
    // the size unevenness the warning below reports, and it is deliberate: changing the target
    // radius would move the geometry of every .pwmodel document already on disk.
    const FBox BBox = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh);
    const FVector Center = BBox.GetCenter();
    const FVector Extent = BBox.GetExtent();
    const double TargetRadius = Extent.GetMax();

    int32 VerticesModified = 0;
    double MinScale = TNumericLimits<double>::Max();
    double MaxScale = 0.0;

    // ONE mesh lock, not two Geometry Script library calls per vertex. GetVertexPosition and
    // SetVertexPosition each take the UDynamicMesh through ProcessMesh/EditMesh and hand back a
    // bool& validity out-param, so the old loop paid 2N lock round-trips on meshes the .pwmodel
    // compiler runs unattended at thousands of vertices. VertexIndicesItr visits only allocated
    // IDs, which is what makes this gap-safe without the GetAllVertexIDs bHasGaps dance.
    Mesh->EditMesh([&](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        for (int32 VertexID : EditMesh.VertexIndicesItr())
        {
            const FVector3d OriginalPos = EditMesh.GetVertex(VertexID);
            FVector3d Direction = OriginalPos - FVector3d(Center);
            const double CurrentDistance = Direction.Size();

            if (CurrentDistance <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            Direction.Normalize();
            const FVector3d SpherePos = FVector3d(Center) + Direction * TargetRadius;
            const FVector3d NewPos = FMath::Lerp(OriginalPos, SpherePos, Factor);
            EditMesh.SetVertex(VertexID, NewPos);
            ++VerticesModified;

            const double Scale = (NewPos - FVector3d(Center)).Size() / CurrentDistance;
            MinScale = FMath::Min(MinScale, Scale);
            MaxScale = FMath::Max(MaxScale, Scale);
        }
    });

    GeometryOpsModeling_WarnRadialAnisotropy(
        TEXT("spherify"), TEXT("box"), BBox.GetSize(),
        Extent.GetMax(), Extent.GetMin(), TargetRadius, Factor,
        MinScale, MaxScale, VerticesModified, Result);

    // RecomputeNormals raises no error a non-null mesh can reach, so there is no HasError check
    // here - see the sweep table in docs/geometry-debug-sink-sweep.md. It does raise one WARNING
    // that changes the result: on a mesh whose normal overlay is empty it falls back to
    // per-vertex normals ("Consider using 'Set Mesh To Per Vertex Normals' or 'Compute Split
    // Normals' instead") and succeeds, so the caller gets normals of a different KIND than the
    // ones requested and used to have no way to find out.
    FGeometryScriptDebugSink RecomputeNormalsSink;
    UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(
        Mesh, FGeometryScriptCalculateNormalsOptions(), false, RecomputeNormalsSink.Get());
    RecomputeNormalsSink.DrainWarningsInto(Result);

    FinishOp(Mesh, Result, /*bForceChanged=*/VerticesModified > 0);
    return Result;
}

FOpResult Cylindrify(UDynamicMesh* Mesh, const FCylindrifyParams& Params, FCylindrifyOutcome* Outcome)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    const double Factor = ClampFactorWarn(Params.Factor, TEXT("factor"), Result);
    const int32 AxisIndex = GeometryOpsModeling_AxisIndex(Params.Axis);

    const FBox BBox = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh);
    const FVector Center = BBox.GetCenter();
    const FVector Extent = BBox.GetExtent();

    // The fitted radius is the MEAN perpendicular radius over the vertices, not a bounding
    // quantity - spherify's max half-extent and this average are two different conventions in
    // two sibling ops, which is why the anisotropy helper takes the radius as a parameter
    // instead of deriving it.
    double AvgRadius = 1.0;
    int32 VerticesModified = 0;
    double MinScale = TNumericLimits<double>::Max();
    double MaxScale = 0.0;

    Mesh->EditMesh([&](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        // First pass: read only, and it must FINISH before the second one writes. The average is
        // over the mesh as it arrived; fusing the two loops would fold already-displaced
        // positions back into the mean and make the result depend on vertex-ID order.
        double TotalRadius = 0.0;
        int32 ValidVertexCount = 0;

        for (int32 VertexID : EditMesh.VertexIndicesItr())
        {
            FVector3d Perpendicular = EditMesh.GetVertex(VertexID) - FVector3d(Center);
            Perpendicular[AxisIndex] = 0.0;
            TotalRadius += Perpendicular.Size();
            ++ValidVertexCount;
        }

        AvgRadius = ValidVertexCount > 0 ? TotalRadius / ValidVertexCount : 1.0;
        if (AvgRadius < KINDA_SMALL_NUMBER)
        {
            AvgRadius = 1.0;
        }

        // Second pass: project each vertex onto that cylinder.
        for (int32 VertexID : EditMesh.VertexIndicesItr())
        {
            const FVector3d OriginalPos = EditMesh.GetVertex(VertexID);
            const FVector3d FromCenter = OriginalPos - FVector3d(Center);

            FVector3d Perpendicular = FromCenter;
            const double AxisCoord = FromCenter[AxisIndex];
            Perpendicular[AxisIndex] = 0.0;

            const double PerpDist = Perpendicular.Size();
            if (PerpDist <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            Perpendicular.Normalize();
            FVector3d CylinderPos = FVector3d(Center) + Perpendicular * AvgRadius;
            CylinderPos[AxisIndex] = Center[AxisIndex] + AxisCoord;

            const FVector3d NewPos = FMath::Lerp(OriginalPos, CylinderPos, Factor);
            EditMesh.SetVertex(VertexID, NewPos);
            ++VerticesModified;

            FVector3d NewPerp = NewPos - FVector3d(Center);
            NewPerp[AxisIndex] = 0.0;
            const double Scale = NewPerp.Size() / PerpDist;
            MinScale = FMath::Min(MinScale, Scale);
            MaxScale = FMath::Max(MaxScale, Scale);
        }
    });

    // Perpendicular to the axis, so a tall thin column cylindrified about Z is isotropic no
    // matter how tall it is - only the CROSS-SECTION's aspect can skew this map.
    const double PerpExtentA = Extent[(AxisIndex + 1) % 3];
    const double PerpExtentB = Extent[(AxisIndex + 2) % 3];
    GeometryOpsModeling_WarnRadialAnisotropy(
        TEXT("cylindrify"), TEXT("cross-section"), BBox.GetSize(),
        FMath::Max(PerpExtentA, PerpExtentB), FMath::Min(PerpExtentA, PerpExtentB),
        AvgRadius, Factor, MinScale, MaxScale, VerticesModified, Result);

    // RecomputeNormals raises no error a non-null mesh can reach, so there is no HasError check
    // here - see the sweep table in docs/geometry-debug-sink-sweep.md. It does raise one WARNING
    // that changes the result: on a mesh whose normal overlay is empty it falls back to
    // per-vertex normals ("Consider using 'Set Mesh To Per Vertex Normals' or 'Compute Split
    // Normals' instead") and succeeds, so the caller gets normals of a different KIND than the
    // ones requested and used to have no way to find out.
    FGeometryScriptDebugSink RecomputeNormalsSink;
    UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(
        Mesh, FGeometryScriptCalculateNormalsOptions(), false, RecomputeNormalsSink.Get());
    RecomputeNormalsSink.DrainWarningsInto(Result);

    if (Outcome)
    {
        Outcome->AverageRadius = AvgRadius;
        Outcome->VerticesModified = VerticesModified;
    }

    FinishOp(Mesh, Result, /*bForceChanged=*/VerticesModified > 0);
    return Result;
}

// ============================================================================
// Repair
// ============================================================================

FOpResult WeldVertices(UDynamicMesh* Mesh, const FWeldVerticesParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptWeldEdgesOptions WeldOptions;
    WeldOptions.Tolerance = Params.Tolerance;
    WeldOptions.bOnlyUniquePairs = Params.bOnlyUniquePairs;

    UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(Mesh, WeldOptions, nullptr);

    FinishOp(Mesh, Result);
    return Result;
}

FOpResult FillHoles(UDynamicMesh* Mesh, const FFillHolesParams& Params, FFillHolesOutcome* Outcome)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }
    bool bPreparedAttributes = false;
    if (!PrepareHoleFillAttributes(Mesh, TEXT("fill_holes"), Result, bPreparedAttributes))
    {
        return Result;
    }

    FGeometryScriptFillHolesOptions FillOptions;
    // Automatic was hardcoded and is the engine default, so the default path is unchanged.
    FillOptions.FillMethod = GeometryOpsModeling_ToEngine(Params.FillMethod);
    FillOptions.bDeleteIsolatedTriangles = Params.bDeleteIsolatedTriangles;

    int32 NumFilledHoles = 0;
    int32 NumFailedHoleFills = 0;

    UGeometryScriptLibrary_MeshRepairFunctions::FillAllMeshHoles(
        Mesh, FillOptions, NumFilledHoles, NumFailedHoleFills, nullptr);

    if (Outcome)
    {
        Outcome->FilledHoles = NumFilledHoles;
        Outcome->FailedHoles = NumFailedHoleFills;
    }
    if (NumFailedHoleFills > 0)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("%d hole(s) could not be filled"), NumFailedHoleFills));
    }

    FinishOp(Mesh, Result, bPreparedAttributes);
    return Result;
}

FOpResult RemoveDegenerates(UDynamicMesh* Mesh, const FRemoveDegeneratesParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptDegenerateTriangleOptions Options;
    // RepairOrDelete was hardcoded and is the engine default, so the default path is unchanged.
    Options.Mode = GeometryOpsModeling_ToEngine(Params.Mode);
    Options.MinTriangleArea = Params.MinTriangleArea;
    Options.MinEdgeLength = Params.MinEdgeLength;
    Options.bCompactOnCompletion = Params.bCompactOnCompletion;

    UGeometryScriptLibrary_MeshRepairFunctions::RepairMeshDegenerateGeometry(Mesh, Options, nullptr);

    FinishOp(Mesh, Result);
    return Result;
}

FOpResult MergeVertices(UDynamicMesh* Mesh, const FMergeVerticesParams& Params, FMergeVerticesOutcome* Outcome)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // Real proximity coincident-VERTEX weld over the underlying FDynamicMesh3. This must "merge
    // nearby vertices" as documented, NOT weld open boundary edges (that operation ships
    // separately as WeldVertices, which wraps WeldMeshEdges). There is no GeometryScript library
    // one-liner for a whole-mesh tolerance vertex weld: the only vertex-merge function,
    // MergeMeshVerticesInSelections, needs two explicit keep/discard selections and defaults
    // bOnlyBoundary=true (skipping exactly the interior verts here). So we run the same
    // primitive it uses internally - FDynamicMesh3::MergeVertices - over a tolerance-matched
    // pairing of all vertices, collapsing each match onto its keep vertex. NumMerged counts only
    // EMeshResult::Ok merges; the engine refuses merges that would create non-manifold edges, so
    // a fully closed/watertight mesh with strictly-interior duplicates can legitimately report
    // merged:0 - but the count is honest rather than an unconditional "success".
    int32 NumMerged = 0;
    const double ThreshSq = Params.Tolerance * Params.Tolerance;
    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // FDynamicMesh3::MergeVertices arrived in UE 5.5. Of the two shapes it resolves, the
    // edge-adjacent one is an edge collapse (below) and the other is a pair of coincident OPEN
    // BOUNDARY vertices, which the older primitive FDynamicMesh3::MergeEdges resolves - that is
    // what joins two separately appended surfaces along a shared seam, and without it a merge of
    // non-edge-adjacent duplicates silently does nothing on 5.3/5.4. The edge pair is accepted
    // only when BOTH endpoint pairs are within the caller's tolerance, so this stays a
    // vertex-proximity merge rather than the boundary-edge weld that WeldVertices owns.
    auto TryMergeCoincidentBoundaryPair =
        [&EditMesh, ThreshSq](int32 KeepVID, int32 DiscardVID) -> bool
    {
        if (!EditMesh.IsBoundaryVertex(KeepVID) || !EditMesh.IsBoundaryVertex(DiscardVID))
        {
            return false;
        }

        // Collected before any merge: MergeEdges invalidates the one-ring enumerations.
        TArray<int32> KeepBoundaryEdges;
        for (const int32 EdgeID : EditMesh.VtxEdgesItr(KeepVID))
        {
            if (EditMesh.IsBoundaryEdge(EdgeID))
            {
                KeepBoundaryEdges.Add(EdgeID);
            }
        }
        TArray<int32> DiscardBoundaryEdges;
        for (const int32 EdgeID : EditMesh.VtxEdgesItr(DiscardVID))
        {
            if (EditMesh.IsBoundaryEdge(EdgeID))
            {
                DiscardBoundaryEdges.Add(EdgeID);
            }
        }

        for (const int32 KeepEdgeID : KeepBoundaryEdges)
        {
            const UE::Geometry::FIndex2i KeepEdgeV = EditMesh.GetEdgeV(KeepEdgeID);
            const int32 KeepOtherVID = (KeepEdgeV.A == KeepVID) ? KeepEdgeV.B : KeepEdgeV.A;
            for (const int32 DiscardEdgeID : DiscardBoundaryEdges)
            {
                const UE::Geometry::FIndex2i DiscardEdgeV = EditMesh.GetEdgeV(DiscardEdgeID);
                const int32 DiscardOtherVID =
                    (DiscardEdgeV.A == DiscardVID) ? DiscardEdgeV.B : DiscardEdgeV.A;
                if (DiscardOtherVID == KeepOtherVID
                    || FVector3d::DistSquared(EditMesh.GetVertex(KeepOtherVID),
                        EditMesh.GetVertex(DiscardOtherVID)) > ThreshSq)
                {
                    continue;
                }

                // Keeps the kept edge's vertices where they are, matching InterpolationT = 0.
                UE::Geometry::FDynamicMesh3::FMergeEdgesInfo MergeInfo;
                if (EditMesh.MergeEdges(KeepEdgeID, DiscardEdgeID, MergeInfo)
                    == UE::Geometry::EMeshResult::Ok)
                {
                    return true;
                }
            }
        }
        return false;
    };
#endif
    {
        // Greedy single-pass clustering: for each still-live vertex, fold every other still-live
        // vertex within tolerance onto it. Merged (discarded) vertices are removed from the mesh
        // by MergeVertices, so IsVertex() guards re-processing them. O(n^2) on vertex count,
        // acceptable for an interactive editor cleanup verb on a single dynamic mesh.
        const int32 MaxVID = EditMesh.MaxVertexID();
        for (int32 KeepVID = 0; KeepVID < MaxVID; ++KeepVID)
        {
            if (!EditMesh.IsVertex(KeepVID))
            {
                continue;
            }
            const FVector3d KeepPos = EditMesh.GetVertex(KeepVID);
            for (int32 DiscardVID = KeepVID + 1; DiscardVID < MaxVID; ++DiscardVID)
            {
                if (!EditMesh.IsVertex(DiscardVID))
                {
                    continue;
                }
                if (FVector3d::DistSquared(KeepPos, EditMesh.GetVertex(DiscardVID)) > ThreshSq)
                {
                    continue;
                }
                // Keep the kept vertex's position (InterpolationT = 0): a weld of duplicates
                // should not drift the surviving vertex.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
                UE::Geometry::FDynamicMesh3::FMergeVerticesInfo MergeInfo;
                const UE::Geometry::EMeshResult MergeResult =
                    EditMesh.MergeVertices(KeepVID, DiscardVID, 0.0, MergeInfo);
                if (MergeResult == UE::Geometry::EMeshResult::Ok)
                {
                    ++NumMerged;
                }
#else
                // FDynamicMesh3::MergeVertices does not exist before UE 5.5. The 5.5+ primitive
                // resolves an edge-adjacent vertex merge as an edge collapse, so do that
                // directly: if the two vertices share an edge, collapse it (always
                // manifold-valid on a closed mesh, which is the case this verb must handle).
                // Otherwise fall back to the coincident open-boundary edge merge above; anything
                // neither shape reaches is left for a later pass / a different op, the same as a
                // non-manifold-blocked merge on 5.5+.
                const int32 SharedEdgeID = EditMesh.FindEdge(KeepVID, DiscardVID);
                if (SharedEdgeID != UE::Geometry::FDynamicMesh3::InvalidID)
                {
                    UE::Geometry::FDynamicMesh3::FEdgeCollapseInfo CollapseInfo;
                    const UE::Geometry::EMeshResult MergeResult =
                        EditMesh.CollapseEdge(KeepVID, DiscardVID, 0.0, CollapseInfo);
                    if (MergeResult == UE::Geometry::EMeshResult::Ok)
                    {
                        ++NumMerged;
                    }
                }
                else if (TryMergeCoincidentBoundaryPair(KeepVID, DiscardVID))
                {
                    ++NumMerged;
                }
#endif
                // On a non-manifold-blocked merge the discard vertex survives; leave it for a
                // later keep vertex / a different op. Do not count it.
            }
        }
    }

    if (Params.bCompact)
    {
        UGeometryScriptLibrary_MeshRepairFunctions::CompactMesh(Mesh, nullptr);
    }

    if (Outcome)
    {
        Outcome->Merged = NumMerged;
    }

    FinishOp(Mesh, Result, /*bForceChanged=*/NumMerged > 0);
    return Result;
}

// ============================================================================
// UV
// ============================================================================

FOpResult UnwrapUVXAtlas(UDynamicMesh* Mesh, int32 UVChannel)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        // EnsureMeshHasUVChannel is null-safe and would report the same out-of-range message on a
        // null mesh; the shared Begin guard reaches it first with a clearer one.
        return Result;
    }

    // Ensure the target UV layer exists first - AutoGenerateXAtlasMeshUVs silently no-ops (its
    // guarded edit never runs, complaint discarded to the nullptr Debug) when the layer is
    // absent, so without this the shared unwrap_uv / auto_uv / pack_uv_islands path reports
    // success while creating zero UV elements. When the channel is out of range
    // (EnsureMeshHasUVChannel returns false - the engine's SetNumUVSets caps at 8 sets, or a
    // negative channel), the layer can't be created, so reject instead of reporting that silent
    // no-op as success.
    if (!GeometryUtils::EnsureMeshHasUVChannel(Mesh, UVChannel))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("uvChannel %d is out of range: the mesh supports at most 8 UV channels (0-7), so the UV layer could not be created"), UVChannel));
    }

    // The failure channel, and the guard above is no longer the only thing standing between a
    // refused unwrap and a success response. XAtlas has three refusals beyond the missing-layer
    // one, all reported ONLY here (MeshUVFunctions.cpp, AutoGenerateXAtlasMeshUVs):
    //
    //   "TargetMesh is non-Compact, XAtlas cannot be run. Try calling CompactMesh to update
    //    TargetMesh." - and non-compact is the NORMAL state of a mesh this plugin has edited:
    //    every delete-triangles, boolean and simplify leaves gaps in the id space. The remedy is
    //    in the engine's own sentence, which is why it is forwarded verbatim.
    //   "UV Generation Failed"          - XAtlasWrapper::ComputeUVs returned false.
    //   "UVSetIndex does not exist on TargetMesh".
    //
    // Atomic on every one of them: XAtlas writes its result in one pass at the end, and each
    // refusal returns from the edit lambda before that, so the mesh keeps the UVs it had.
    // bForceChanged is therefore not reached on a failure, which matters - it would otherwise
    // report a change over a mesh nothing was written to.
    FGeometryScriptDebugSink Debug;

    UGeometryScriptLibrary_MeshUVFunctions::AutoGenerateXAtlasMeshUVs(
        Mesh, UVChannel, FGeometryScriptXAtlasOptions(), Debug.Get());

    Debug.DrainWarningsInto(Result);

    if (Debug.HasError())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_UV_GENERATION_FAILED,
            FString::Printf(TEXT("XAtlas UV unwrap failed - %s"), *Debug.ErrorText()));
    }

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult ProjectUV(UDynamicMesh* Mesh, const FProjectUVParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // Ensure the target UV layer exists before projecting into it. SetMeshUVsFrom*Projection
    // silently no-op when UVChannel has no layer (the "UVSetIndex does not exist" complaint goes
    // only to the nullptr GeometryScriptDebug), so without this the op reports success while
    // creating zero UV elements on a hand-authored (append_buffers, no uvs) mesh.
    if (!GeometryUtils::EnsureMeshHasUVChannel(Mesh, Params.UVChannel))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("uvChannel %d is out of range: the mesh supports at most 8 UV channels (0-7), so the UV layer could not be created"), Params.UVChannel));
    }

    // The projection frame is 3D but the published scale is 2D, so the third axis follows U.
    // For X == Y - every value the RPC wrapper builds - this is the identical uniform frame the
    // scalar Scale produced, which is what keeps project_uv's output unchanged.
    const FTransform ProjectionTransform(FQuat::Identity, FVector::ZeroVector,
        FVector(Params.Scale.X, Params.Scale.Y, Params.Scale.X));

    switch (Params.Projection)
    {
    case EUVProjectionMode::Planar:
        UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromPlanarProjection(
            Mesh, Params.UVChannel, ProjectionTransform, FGeometryScriptMeshSelection(), nullptr);
        break;
    case EUVProjectionMode::Cylindrical:
        UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromCylinderProjection(
            Mesh, Params.UVChannel, ProjectionTransform, FGeometryScriptMeshSelection(),
            static_cast<float>(Params.SplitAngle), nullptr);
        break;
    default:
        UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
            Mesh, Params.UVChannel, ProjectionTransform, FGeometryScriptMeshSelection(), 2, nullptr);
        break;
    }

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult LayoutUV(UDynamicMesh* Mesh, const FLayoutUVParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // Same guard and the SAME MESSAGE TEXT as ProjectUV and UnwrapUVXAtlas. LayoutMeshUVs sends
    // its "UVSetIndex does not exist" complaint only to the GeometryScriptDebug argument, which
    // is null here as everywhere, so without this the op reports success on a channel that does
    // not exist. The text is copied rather than shared because these three are the whole set and
    // a helper would hide which call each guard protects; if a fourth appears, extract it.
    if (!GeometryUtils::EnsureMeshHasUVChannel(Mesh, Params.UVChannel))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("uvChannel %d is out of range: the mesh supports at most 8 UV channels (0-7), so the UV layer could not be created"), Params.UVChannel));
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    FGeometryScriptLayoutUVsOptions LayoutOptions;
    LayoutOptions.LayoutType = GeometryOpsModeling_ToEngine(Params.LayoutType);
    LayoutOptions.TextureResolution = Params.TextureResolution;
    LayoutOptions.Scale = static_cast<float>(Params.Scale);
    LayoutOptions.Translation = Params.Translation;
    LayoutOptions.bPreserveScale = Params.bPreserveScale;
    LayoutOptions.bPreserveRotation = Params.bPreserveRotation;
    LayoutOptions.bAllowFlips = Params.bAllowFlips;
    LayoutOptions.bEnableUDIMLayout = Params.bEnableUDIMLayout;
    // UDIMResolutions is deliberately left default-empty - see FLayoutUVParams.

    UGeometryScriptLibrary_MeshUVFunctions::LayoutMeshUVs(
        Mesh, Params.UVChannel, LayoutOptions, FGeometryScriptMeshSelection(), nullptr);
#else
    // UE 5.4 publishes only the REPACK half of this verb: RepackMeshUVs plus a two-field
    // FGeometryScriptRepackUVsOptions. LayoutMeshUVs, EGeometryScriptUVLayoutType and the
    // post-pack scale/translation/UDIM knobs all arrived in 5.5. Repack at the defaults is
    // bit-for-bit the call pack_uv_islands made before the widening, so the reachable half is
    // unchanged here; everything else is refused or reported rather than silently dropped.
    if (Params.LayoutType != EUVLayoutType::Repack)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION,
            TEXT("layoutType other than 'repack' needs UGeometryScriptLibrary_MeshUVFunctions::"
                 "LayoutMeshUVs, added in UE 5.5. This engine can only repack."));
    }

    FGeometryScriptRepackUVsOptions RepackOptions;
    RepackOptions.TargetImageWidth = Params.TextureResolution;
    // The same switch, read the other way round: RepackMeshUVs auto-orients each island before
    // packing when bOptimizeIslandRotation is set, which is exactly what NOT preserving the
    // authored rotation means. The engine defaults (bOptimizeIslandRotation=true,
    // bPreserveRotation=false) agree, so an untouched call is the engine's own default repack.
    RepackOptions.bOptimizeIslandRotation = !Params.bPreserveRotation;

    // The four knobs FGeometryScriptRepackUVsOptions has no field for. Reported per call so a
    // caller who set one learns it did not reach the engine.
    const FLayoutUVParams Defaults;
    if (Params.Scale != Defaults.Scale || Params.Translation != Defaults.Translation)
    {
        Result.Warnings.Add(TEXT(
            "layoutScale/translation are applied by LayoutMeshUVs, added in UE 5.5; this engine's "
            "RepackMeshUVs packs into the unit square and ignores both."));
    }
    if (Params.bPreserveScale != Defaults.bPreserveScale || Params.bAllowFlips != Defaults.bAllowFlips
        || Params.bEnableUDIMLayout != Defaults.bEnableUDIMLayout)
    {
        Result.Warnings.Add(TEXT(
            "preserveScale/allowFlips/enableUdimLayout are LayoutMeshUVs options, added in UE 5.5; "
            "this engine's RepackMeshUVs has no field for them and packed without them."));
    }

    UGeometryScriptLibrary_MeshUVFunctions::RepackMeshUVs(
        Mesh, Params.UVChannel, RepackOptions, nullptr);
#endif

    // Force-changed for the reason every UV op is: a repack moves UV values and moves no counts,
    // so a count-derived bChanged would report that the layout did nothing.
    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult AutoUVPatchBuilder(UDynamicMesh* Mesh, const FPatchBuilderUVParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    // See LayoutUV above for why this guard is duplicated verbatim.
    if (!GeometryUtils::EnsureMeshHasUVChannel(Mesh, Params.UVChannel))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("uvChannel %d is out of range: the mesh supports at most 8 UV channels (0-7), so the UV layer could not be created"), Params.UVChannel));
    }

    FGeometryScriptPatchBuilderOptions PatchOptions;
    PatchOptions.InitialPatchCount = Params.InitialPatchCount;
    PatchOptions.MinPatchSize = Params.MinPatchSize;
    PatchOptions.PatchCurvatureAlignmentWeight = static_cast<float>(Params.PatchCurvatureAlignmentWeight);
    PatchOptions.PatchMergingMetricThresh = static_cast<float>(Params.PatchMergingMetricThresh);
    PatchOptions.PatchMergingAngleThresh = static_cast<float>(Params.PatchMergingAngleThresh);
    PatchOptions.ExpMapOptions.NormalSmoothingRounds = Params.ExpMapNormalSmoothingRounds;
    PatchOptions.ExpMapOptions.NormalSmoothingAlpha = static_cast<float>(Params.ExpMapNormalSmoothingAlpha);
    PatchOptions.bRespectInputGroups = Params.bRespectInputGroups;
    PatchOptions.bAutoPack = Params.bAutoPack;
    PatchOptions.PackingOptions.TargetImageWidth = Params.PackingTargetImageWidth;
    PatchOptions.PackingOptions.bOptimizeIslandRotation = Params.bPackingOptimizeIslandRotation;
    // GroupLayer is deliberately left default - see FPatchBuilderUVParams.

    // The failure channel. AutoGeneratePatchBuilderMeshUVs reports three things here and nowhere
    // else (MeshUVFunctions.cpp): "UV Generation Failed" when the generator cannot partition the
    // mesh, "UVSetIndex does not exist on TargetMesh", and - the one that behaves unlike every
    // other error in this file - "Requested Polygroup Layer does not exist", which the engine
    // raises and then IGNORES, carrying on to generate UVs with no group constraint at all.
    //
    // That third one is still a failure from the caller's side and is deliberately reported as
    // one: bRespectInputGroups is an explicit request, and silently producing UVs that disregard
    // it is the same class of lie this whole change exists to remove. It is also the one path
    // where the mesh IS modified before the error can be read, so the message says so instead of
    // implying the previous UVs survived.
    FGeometryScriptDebugSink Debug;

    UGeometryScriptLibrary_MeshUVFunctions::AutoGeneratePatchBuilderMeshUVs(
        Mesh, Params.UVChannel, PatchOptions, Debug.Get());

    Debug.DrainWarningsInto(Result);

    if (Debug.HasError())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_UV_GENERATION_FAILED,
            FString::Printf(
                TEXT("PatchBuilder UV generation failed - %s. Note that the engine may still have "
                     "written UVs into channel %d before reporting this."),
                *Debug.ErrorText(), Params.UVChannel));
    }

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult TransformUVs(UDynamicMesh* Mesh, const FTransformUVsParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result))
    {
        return Result;
    }

    FGeometryScriptMeshSelection Selection;
    bool bApplied = false;

    if (Params.Translate.X != 0.0 || Params.Translate.Y != 0.0)
    {
        UGeometryScriptLibrary_MeshUVFunctions::TranslateMeshUVs(
            Mesh, Params.UVChannel, Params.Translate, Selection, nullptr);
        bApplied = true;
    }

    if (Params.Scale.X != 1.0 || Params.Scale.Y != 1.0)
    {
        UGeometryScriptLibrary_MeshUVFunctions::ScaleMeshUVs(
            Mesh, Params.UVChannel, Params.Scale, FVector2D(0.5, 0.5), Selection, nullptr);
        bApplied = true;
    }

    if (Params.Rotation != 0.0)
    {
        UGeometryScriptLibrary_MeshUVFunctions::RotateMeshUVs(
            Mesh, Params.UVChannel, Params.Rotation, FVector2D(0.5, 0.5), Selection, nullptr);
        bApplied = true;
    }

    FinishOp(Mesh, Result, /*bForceChanged=*/bApplied);
    return Result;
}

} // namespace GeometryOps
