// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Boolean.h - The boolean and mirror/array verbs as pure functions over
// UDynamicMesh. See GeometryOps.h for FOpResult and why these return a struct rather than bool.
//
// Nothing here reads FHandlerContext, resolves an actor label, reads an actor transform, marks a
// package dirty or destroys anything. The two-mesh ops take both transforms as parameters
// because ApplyMeshBoolean already works that way: the RPC wrapper passes the two actors'
// transforms, and the .pwmodel compiler passes part-local transforms with no actor in sight.
//
// What deliberately did NOT move out of BooleanHandler.cpp: the `keepTool` actor destruction and
// the actor-transform lookups. Both are level plumbing, not geometry.
#pragma once

#include "CoreMinimal.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/Geometry/GeometryOps.h"

// For EGeometryScriptBooleanOperation, which is the operation selector rather than a param.
#include "GeometryScript/MeshBooleanFunctions.h"

class UDynamicMesh;

namespace GeometryOps
{
    // "Union" / "Intersection" / "Subtract" / ... - the operation's own name, interpolated into
    // the two guard messages and the BOOLEAN_FAILED message, and echoed by the RPC wrapper as
    // its `operation` field. Derived from the enum rather than passed alongside it so the two
    // cannot disagree; geometry.difference dispatches Subtract and has always reported
    // "Subtract", which this reproduces exactly.
    const TCHAR* BooleanOperationName(EGeometryScriptBooleanOperation Operation);

    // The memory-pressure pre-flight every op in this file runs before it allocates. Answers
    // GeometryUtils::IsMemoryPressureSafe() - the reading against GEOM_MEMORY_PRESSURE_CRITICAL,
    // 90% of physical memory - unless a test has forced it false.
    //
    // The seam exists because the guards are otherwise UNTESTABLE: they refuse work only when
    // the machine is near OOM, and no automation test can put a build machine there. Without it
    // the difference between a guarded op and an unguarded one is invisible to the suite, which
    // is exactly how `trim` and `self_union` shipped without the guard their three symmetric
    // siblings have while every test stayed green.
    bool BooleanMemoryPressureSafe();

    // RAII override for the above: BooleanMemoryPressureSafe() answers false for this object's
    // lifetime. TEST-ONLY - nothing on a shipped call path constructs one. Nested instances
    // count, so an inner scope cannot lift an outer one's override.
    struct FScopedBooleanMemoryPressure
    {
        FScopedBooleanMemoryPressure();
        ~FScopedBooleanMemoryPressure();

        FScopedBooleanMemoryPressure(const FScopedBooleanMemoryPressure&) = delete;
        FScopedBooleanMemoryPressure& operator=(const FScopedBooleanMemoryPressure&) = delete;
    };

    // FGeometryScriptMeshBooleanOptions, one field for one field. Every default is now the
    // engine's, including bSimplifyOutput - see the note on that field for the flip and what it
    // changes.
    struct FBooleanParams
    {
        bool bFillHoles = true;

        // The engine's default - and it did NOT used to be. This surface pinned it FALSE and
        // gave no front-end a way to reach it, on the reasoning that an unsimplified result
        // keeps a triangulation the caller can predict. Measured cost of that reasoning:
        //
        //   union { cylinder r=25.5 h=4 segments=32; array_linear count=16 } onto a 32-segment
        //   300-tall cylinder produced 135,324 triangles against 2,608 appended - 52x.
        //
        // Flipped to true. Four facts decide it, and the first two are why 'predictable
        // triangulation' was never actually what the pin protected:
        //
        //  - bSimplifyOutput reaches the engine as FMeshBoolean::bSimplifyAlongNewEdges, which
        //    is scoped to the triangles the boolean itself created ALONG THE NEW CUT EDGES
        //    ("the small planar triangles that the boolean operation tends to generate",
        //    MeshBoolean.h:77). It cannot retriangulate geometry a generator produced.
        //  - It runs at FMeshBoolean::SimplificationAngleTolerance, default 0.1 DEGREES from
        //    coplanar (MeshBoolean.h:82). Only genuinely coplanar fans collapse - and see the
        //    SimplifyPlanarTolerance note below for why the option's 0.01 never reaches it.
        //  - FMeshBoolean defaults bPreserveTriangleGroups, bPreserveVertexUVs,
        //    bPreserveOverlayUVs and bPreserveVertexNormals all TRUE (MeshBoolean.h:90-98), so
        //    the pass is structurally barred from distorting polygroups, UVs or normals - the
        //    three things a .pwmodel document authors and the bake reads.
        //  - FTrimParams below has ALWAYS run the engine's true. Trim and the three symmetric
        //    booleans disagreeing about their own simplification was not a decision; it was one
        //    struct being written from the engine and the other from a wrapper.
        //
        // It IS an output change: every existing document's boolean results are now simplified,
        // so triangle counts move. Audited before flipping - across the twelve
        // Examples/pwmodel documents (21 union, 44 subtract, 9 intersection, 0 trim) not one
        // sets simplify_output, and no automation test asserts an exact triangle count after a
        // boolean. `simplify_output=false` restores the old behaviour exactly, which is the
        // difference between this flip and the unreachable pin it replaces.
        bool bSimplifyOutput = true;

        // Planar-face tolerance the simplifier works to - and UE 5.8 IGNORES IT HERE.
        //
        // Carried and assigned anyway, but published by NEITHER front-end, because publishing a
        // knob that provably does nothing is worse than not having one. ApplyMeshBoolean reads
        // exactly one simplification field off this struct - `MeshBoolean.bSimplifyAlongNewEdges
        // = Options.bSimplifyOutput` (MeshBooleanFunctions.cpp:87) - and never assigns
        // FMeshBoolean::SimplificationAngleTolerance, so the tolerance stays at the operation's
        // own default whatever is put here. The engine's neighbouring ApplyMeshSelfUnion DOES
        // forward it (:161-162), which is what makes this an omission rather than a design.
        // Publish it in both front-ends the moment an engine version wires it up.
        double SimplifyPlanarTolerance = 0.01;

        // Off, an empty result is refused - and that refusal is now DETECTED. ApplyMeshBoolean
        // returns the TARGET MESH on that path (MeshBooleanFunctions.cpp:96-99), never null, and
        // sends its complaint to the Debug argument, which every call site here used to pass as
        // nullptr. Boolean() now passes a real UGeometryScriptDebug
        // (Handlers/Geometry/GeometryScriptDebugSink.h), so the observable difference is:
        //   false -> ERR_BOOLEAN_FAILED carrying the engine's own text, target untouched
        //   true  -> the target becomes the empty mesh, and bChanged is true
        // Before the sink was wired the first case answered success with `changed: false`, which
        // is indistinguishable from a disjoint boolean and is what let callers keep building on
        // geometry the engine had declined to produce. The .pwmodel compiler still catches the
        // second as PWMODEL_EMPTY_MESH one stage later.
        bool bAllowEmptyResult = false;

        // Which of the two operands' local spaces the result is written back into. Only ever
        // observable when the two transforms differ, so it does nothing for a caller that passes
        // identity for both (the .pwmodel compiler, whose ops bake their transform into the
        // vertices before the boolean runs). The RPC verbs pass the two ACTORS' transforms, so
        // it is real there - and a non-target space leaves the mesh data in a frame the target
        // actor's own transform does not correct for, which reads as the result teleporting.
        //
        // EGeometryScriptBooleanOutputSpace and FGeometryScriptMeshBooleanOptions::
        // OutputTransformSpace both arrived in UE 5.6. Before that ApplyMeshBoolean always
        // inverts the TARGET transform out of the result (MeshBooleanFunctions.cpp), i.e. it is
        // permanently TargetTransformSpace, so the field has nothing to select and is compiled
        // out rather than carried and dropped. BooleanHandler refuses `tool`/`shared` there.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        EGeometryScriptBooleanOutputSpace OutputTransformSpace =
            EGeometryScriptBooleanOutputSpace::TargetTransformSpace;
#endif
    };

    // Union / subtract / intersection of Tool into Target, both resolved through their own
    // transforms. Mutates Target in place; Tool is only read.
    //
    // FOpResult::bChanged is the disjoint-boolean tell. The engine returns the target UNCHANGED
    // when the two meshes do not overlap in their resolved common space, so bSuccess alone
    // cannot separate a real cut from a no-effect pass - the most confusing failure on this
    // surface, because every downstream op then runs happily on unmodified geometry.
    //
    // This is the ONE op in the family whose bChanged reaches the wire (the four boolean verbs
    // echo it as `changed`), so it keeps the triangle-delta-only definition it shipped with
    // rather than FinishOp's triangles-or-vertices one. See the note at the end of Boolean().
    //
    // Fails with MEMORY_PRESSURE or POLYGON_LIMIT_EXCEEDED before touching either mesh.
    //
    // Its BOOLEAN_FAILED arm is LIVE. It used to be dead: the arm tested ApplyMeshBoolean's
    // return for null, and ApplyMeshBoolean returns the target mesh on every path it has,
    // including the empty-result refusal (MeshBooleanFunctions.cpp:38-47, :96-99, :131-133), so
    // the return could be null only when Target was - which the guard above already rejects. The
    // refusal travels through the UGeometryScriptDebug argument instead, and this op now passes
    // one (Handlers/Geometry/GeometryScriptDebugSink.h) and fails on it with the engine's own
    // message text. The null test is retained behind it purely as a future-engine guard.
    //
    // Which failures that makes reachable, precisely: an empty result with bAllowEmptyResult
    // off. For Subtract that means the tool swallowed the target whole; for Intersection it
    // means the two do not overlap at all. Both used to answer success with bChanged == false -
    // the same signal a DISJOINT SUBTRACT sends, which is a genuine success over an unchanged
    // mesh. The two are now distinguishable: a disjoint subtract still succeeds with
    // bChanged == false, a refused boolean fails.
    FOpResult Boolean(
        UDynamicMesh* Target,
        const FTransform& TargetTransform,
        UDynamicMesh* Tool,
        const FTransform& ToolTransform,
        EGeometryScriptBooleanOperation Operation,
        const FBooleanParams& Params);

    struct FTrimParams
    {
        // True intersects Target with Tool (keep what is inside the tool); false subtracts.
        // The operation selector, not an option: it picks which engine operation runs, and it is
        // the reason trim is a separate op rather than a Boolean() call.
        bool bKeepInside = false;

        // Below: FGeometryScriptMeshBooleanOptions again, and here every default IS the
        // engine's - including bSimplifyOutput, which trim has always left on. That difference
        // from FBooleanParams above is behaviour this struct records rather than erases, and it
        // is why the two structs stay separate: one default cannot be right for both.
        bool bFillHoles = true;
        bool bSimplifyOutput = true;
        // Ignored by UE 5.8 and published by neither front-end - see FBooleanParams above.
        double SimplifyPlanarTolerance = 0.01;

        // FALSE, deliberately, and re-decided rather than inherited once the refusal became
        // detectable. An empty result IS sometimes the right answer to a trim, so the question
        // is real - but the defect was never the default, it was that the refusal was invisible:
        // the engine declined the trim, left the mesh whole, and the caller was told it
        // succeeded. That is now ERR_BOOLEAN_FAILED carrying the engine's own remedy. Flipping
        // the default to true instead would:
        //   - silently change what every existing .pwmodel document and every existing
        //     geometry.boolean_trim call PRODUCES - a trim that leaves the mesh intact today
        //     would start emptying it, with no caller having asked;
        //   - split the family, since the four boolean verbs keep false;
        //   - trade a loud failure for an empty mesh that fails later and further away, in
        //     collision generation or the StaticMesh bake.
        // The caller who wants an empty result asks for one: the parameter is published on
        // geometry.boolean_trim as `allowEmptyResult` and in .pwmodel as `allow_empty_result`.
        bool bAllowEmptyResult = false;

        // 5.6+ only - see FBooleanParams::OutputTransformSpace above.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        EGeometryScriptBooleanOutputSpace OutputTransformSpace =
            EGeometryScriptBooleanOutputSpace::TargetTransformSpace;
#endif
    };

    // Trim Target against Tool. Distinct from Boolean() in two ways that are behaviour, not
    // style: it runs the engine's default simplification, and it applies the memory guard but
    // NOT the polygon one. bChanged reports whether the trim did anything.
    //
    // The polygon guard is genuinely absent rather than missing: it bounds a union's output at
    // target+tool and multiplies by 3, and a trim's output is bounded by the target alone, so
    // importing it would refuse trims that cannot breach the budget. The MEMORY_PRESSURE guard
    // has no such asymmetry - the allocation is the same engine call over the same two meshes -
    // and trim went without it while its three symmetric siblings had it.
    //
    // It CAN fail with BOOLEAN_FAILED, and that now covers the ENGINE'S refusal rather than
    // only the guards above it. Two separate omissions had to be fixed for it to hold:
    // geometry.boolean_trim first discarded ApplyMeshBoolean's return value entirely, and
    // capturing that return was still not enough, because ApplyMeshBoolean never returns null
    // for a non-null target. The refusal is reported ONLY through the UGeometryScriptDebug
    // argument, which this op now passes (Handlers/Geometry/GeometryScriptDebugSink.h).
    //
    // What that changes for a caller: a trim the engine declines - a result that would be empty,
    // with bAllowEmptyResult off - is a failure carrying the engine's text, not a success over
    // an untrimmed mesh. A DISJOINT trim is unaffected and still succeeds with bChanged ==
    // false: with the default bKeepInside == false the operation is Subtract, whose result is
    // the whole target and therefore never empty.
    FOpResult Trim(
        UDynamicMesh* Target,
        const FTransform& TargetTransform,
        UDynamicMesh* Tool,
        const FTransform& ToolTransform,
        const FTrimParams& Params);

    struct FSelfUnionParams
    {
        bool bFillHoles = true;

        // Drop the open "flap" surfaces the resolve leaves behind - the half-faces of a
        // self-intersection that bound nothing. Engine default. Off is how you keep them, which
        // matters when the mesh is a surface rather than a solid and the flaps ARE the model.
        bool bTrimFlaps = true;

        // The engine's own defaults, NOT the four boolean verbs' bSimplifyOutput=false
        // override: self_union has always run ApplyMeshSelfUnion on the struct defaults, and
        // this widening must reproduce that exactly. FBooleanParams carries the opposite
        // default for the opposite reason - see the note there.
        bool bSimplifyOutput = true;

        // Coplanar tolerance for the simplification pass. Ignored when bSimplifyOutput is off.
        double SimplifyPlanarTolerance = 0.01;

        // Winding-number isovalue that decides inside from outside. 0.5 is "inside at least one
        // shell"; raising it toward 1.5 keeps only regions covered at least twice, which is the
        // knob for a mesh whose overlaps are meant to be intersections rather than unions.
        double WindingThreshold = 0.5;
    };

    // Boolean-union a mesh with itself to resolve self-intersections and drop floating geometry.
    //
    // Fails MEMORY_PRESSURE before touching the mesh, like the rest of the family. It is a
    // boolean resolve over a single input, so the allocation is a boolean's - and this is the
    // op a caller reaches for exactly when a mesh has grown large and self-intersecting.
    FOpResult SelfUnion(UDynamicMesh* Target, const FSelfUnionParams& Params);

    struct FMirrorParams
    {
        // EMeshAxis::None is the unrecognized-axis fallback GeometryOps.h describes: for this
        // verb it is a unit scale, so the clone is appended UNMIRRORED and the mesh is doubled
        // where it stands rather than reflected.
        EMeshAxis Axis = EMeshAxis::X;

        // Welds coincident boundary edges after the append. Off leaves the two halves as
        // separate shells sharing a seam.
        bool bWeld = true;

        // The seam weld's tolerance, and the reason this field now exists: the value was
        // hardcoded to 0.001 here while FWeldVerticesParams defaults to 0.0001 and the ENGINE
        // struct defaults to 1e-06, so the same "weld coincident edges" operation ran at three
        // different tolerances depending on which door you came through, and none of the three
        // was reachable from the other two.
        //
        // KEPT AT 0.001 rather than moved to either of the other two values. This is a
        // deliberate override, not an oversight to correct:
        //
        //  - It is what mirror does today, on every existing document and every existing RPC
        //    call. Tightening it by 10x (weld_vertices' default) or 1000x (the engine's) would
        //    silently stop welding seams that weld now, and the failure is invisible in the
        //    triangle count - you get a coincident-but-unwelded seam that only shows up as a
        //    lighting split or a crack after a later shell / offset.
        //  - The tolerance has a different job here than in weld_vertices. weld_vertices is a
        //    general repair over a mesh of unknown provenance, where a loose tolerance welds
        //    detail that was meant to stay separate. mirror welds ONE plane whose two sides it
        //    just generated by negating a coordinate, so the only vertices in range are the
        //    ones that were meant to meet - and they miss exactly, by whatever floating-point
        //    residue the generator left, on any ring the primitive did not align to the plane.
        //    A tolerance an order of magnitude looser is the right answer to that and the wrong
        //    answer to weld_vertices' problem.
        //
        // Both front-ends publish it, so a caller who needs the engine's 1e-06 can now ask.
        double WeldTolerance = 0.001;

        // FGeometryScriptWeldEdgesOptions::bOnlyUniquePairs. Engine default. Off lets the weld
        // merge ambiguous matches, which is what a seam with several coincident edges per
        // vertex needs and what leaves a non-manifold edge when it is wrong.
        bool bOnlyUniquePairs = true;

        // NO CombineMode field, and that is the third of mirror's three unreachable engine
        // options accounted for rather than exposed. FGeometryScriptAppendMeshOptions::
        // CombineMode decides how the SOURCE mesh's attribute set is reconciled with the
        // TARGET's, and mirror's source is `MirroredMesh->SetMesh(Target->GetMeshRef())` - a
        // byte copy of the target. Its three values (EnableAllMatching, UseTarget, UseSource)
        // are defined entirely by which attributes each side has, so on two meshes with the
        // identical attribute set all three produce the identical result. Publishing it would
        // add a knob that provably cannot change the output, which is the defect this pass
        // exists to remove.
    };

    // Mirror-AND-MERGE, not a reflection: the mesh is cloned, the clone is negated on Axis, and
    // the clone is appended back onto the original. The output is both halves. Nothing in the
    // op removes the source half, so calling it twice quadruples the geometry.
    //
    // Fails with OPERATION_FAILED when the engine refuses the scale, the append or the weld -
    // and that is now a real detection rather than a shape. All three of those calls return
    // their TargetMesh on every path they have, error paths included, so the three `if (!X)`
    // tests this op was built on were dead code. It passes a UGeometryScriptDebug through all
    // three instead (Handlers/Geometry/GeometryScriptDebugSink.h) and reports whatever the
    // engine appended, with the message naming which step failed.
    //
    // None of the three can actually fail on UE 5.8, and that is stated rather than implied: the
    // scale and the append raise only null-mesh errors, and the weld's own flag is never raised
    // because FMergeCoincidentMeshEdges::Apply() has a single `return true`
    // (GeometryCore/Private/DynamicMesh/Operations/MergeCoincidentMeshEdges.cpp:233). So mirror
    // has no reachable engine failure today. The sink is wired through all three anyway: the
    // boolean family shipped for months with its detection channel disconnected, and the cost of
    // connecting one that nothing currently uses is a few lines, while the cost of leaving it
    // disconnected is a failure that arrives silently on the next engine upgrade.
    //
    // The RPC wrapper honours the failure (geometry.mirror checks bSuccess); it still does not
    // publish the counts, which is a separate and deliberate omission.
    FOpResult Mirror(UDynamicMesh* Target, const FMirrorParams& Params);

    // The 1..100 range both array verbs enforce. Exposed because the RPC wrappers must run it
    // BEFORE they resolve the actor - a bad count and a missing actor together have always
    // reported INVALID_ARGUMENT, and reporting ACTOR_NOT_FOUND instead would be an observable
    // change. ArrayLinear and ArrayRadial call it themselves, so a compiler front-end that never
    // resolves an actor is still covered.
    FOpResult ValidateArrayCount(int32 Count);

    struct FArrayLinearParams
    {
        // Total copies INCLUDING the original, which counts as 1; Count-1 copies are appended.
        int32 Count = 3;

        // Spacing between consecutive copies, applied before the first append so the row runs
        // Offset..(Count-1)*Offset with the original at zero.
        FVector Offset = FVector(100, 0, 0);
    };

    // Merge Count-1 offset copies of the mesh into the mesh itself. No actors are spawned; this
    // is an in-place geometry multiply.
    FOpResult ArrayLinear(UDynamicMesh* Target, const FArrayLinearParams& Params);

    struct FArrayRadialParams
    {
        // Total copies INCLUDING the original, which counts as 1.
        int32 Count = 6;

        FVector Center = FVector::ZeroVector;

        // EMeshAxis::None is the unrecognized-axis fallback GeometryOps.h describes; for this
        // verb it resolves to the same Z the if/else chain already leaves in place.
        EMeshAxis Axis = EMeshAxis::Z;

        // Swept across ALL Count copies, so the step is TotalAngleDegrees/Count and a full 360
        // leaves no duplicate at the start and end.
        double TotalAngleDegrees = 360.0;
    };

    // Merge Count-1 rotated copies of the mesh into the mesh itself, around Center on Axis.
    FOpResult ArrayRadial(UDynamicMesh* Target, const FArrayRadialParams& Params);

    struct FArrayAlongPathParams
    {
        // Absolute placements, not deltas: one copy of the mesh per frame, at that frame. The
        // frame list IS the count, so ValidateArrayCount is run against Frames.Num() and the
        // same 1..100 bound applies.
        //
        // Every OTHER array verb derives its placements from a rule (an offset, a swept angle),
        // which is why they can leave the original where it stands and append Count-1 copies.
        // A frame list has no such relationship to the identity transform, so the original is
        // NOT left in place - see ArrayAlongPath.
        TArray<FTransform> Frames;
    };

    // Replaces the mesh with one copy of itself per frame. N frames produce exactly N copies, so
    // a leading identity frame reproduces the original where it stood - which makes this the same
    // counting rule as ArrayLinear (Count copies, the original occupying the first) rather than a
    // different one. Keeping the original AND appending a copy at every frame was the alternative,
    // and it doubles the geometry at the origin for the ordinary path that starts there.
    //
    // No engine verb backs this: geometry.duplicate_along_spline duplicates ACTORS through
    // UEditorActorSubsystem and never touches mesh data, so there is nothing to extract and this
    // is a new op rather than a port.
    FOpResult ArrayAlongPath(UDynamicMesh* Target, const FArrayAlongPathParams& Params);
}
