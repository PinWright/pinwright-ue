// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps.h - The result type every extracted geometry operation returns, plus the
// scaffolding all five families share.
//
// The operations themselves live in GeometryOps_<family>.h beside this file; each family
// declares its own F<Verb>Params structs and functions. This header carries only what they all
// share, so no family header depends on another.
//
// Why a struct rather than bool: ~146 automation tests drive the real dispatcher with JSON and
// observe these verbs only through the response, several of them precisely because a past change
// silently altered an echo field or an error code. A bool discards WHY a call failed and forces
// the RPC wrapper to guess a code, which is the one thing that would make the extraction
// observable from outside.
#pragma once

#include "CoreMinimal.h"
// ENUM_CLASS_FLAGS for EColorChannels below; CoreMinimal does not pull this in.
#include "Misc/EnumClassFlags.h"

class UDynamicMesh;

namespace GeometryOps
{
    struct FOpResult
    {
        bool bSuccess = false;

        // Empty on success. Otherwise the same ErrorCodes.h ERR_* value the handler emitted at
        // this failure point before extraction - the wrapper forwards it verbatim, which is what
        // keeps the dispatcher tests green.
        FString ErrorCode;
        FString ErrorMessage;

        // Non-fatal notes the op wants to surface: a clamped dimension, an op that ran against a
        // mesh it cannot affect.
        //
        // EVERY consumer of an FOpResult must forward these. The .pwmodel compiler routes them to
        // PWMODEL_STAGE_WARNING; an RPC wrapper calls GeometryOps::AddOpWarnings
        // (Handlers/Geometry/GeometryOpWarnings.h) immediately before its SendSuccess, which
        // emits them as a `warnings` array when the array is non-empty and changes nothing when
        // it is. This used to read "wrappers may ignore these to preserve their response shape",
        // and every wrapper did - so for the whole life of the clamp layer a caller passing
        // segments=1 got a bare success and never learned the engine used 3. A warning that
        // reaches no caller is not a warning.
        TArray<FString> Warnings;

        // Zero when the op does not count that element type.
        int32 TrianglesBefore = 0;
        int32 TrianglesAfter = 0;
        int32 VerticesBefore = 0;
        int32 VerticesAfter = 0;

        // False when the op ran to completion but altered nothing.
        bool bChanged = false;

        // Skin-weight repair, filled by the ops that run
        // GeometrySkinWeights::FPreEditSnapshot::Repair. All three stay at their defaults for an
        // unskinned mesh, which is every .pwmodel boolean and every unskinned RPC call, so a
        // wrapper that echoes them behind `bSkinned` changes no existing response.
        //
        // Why they are worth publishing at all: the repair is invisible in the geometry, and a
        // caller that cannot see how many vertices had to be re-derived cannot tell a clean cut
        // from one that rebuilt half the skinning. VerticesUnresolved above zero means the op
        // left vertices the repair could NOT account for - the one case where the mesh is still
        // suspect after the op reports success.
        bool bSkinned = false;
        int32 SkinWeightsTransferred = 0;
        int32 SkinWeightsUnresolved = 0;

        static FOpResult Ok()
        {
            FOpResult Result;
            Result.bSuccess = true;
            return Result;
        }

        // Fresh-struct failure. Correct ONLY before the result has accumulated anything - i.e.
        // in a validator that takes no mesh, or on the first line of an op.
        static FOpResult Fail(const TCHAR* Code, FString Message)
        {
            FOpResult Result;
            Result.bSuccess = false;
            Result.ErrorCode = Code;
            Result.ErrorMessage = MoveTemp(Message);
            return Result;
        }

        // In-place failure: the failure path every op must use once it has recorded ANYTHING.
        //
        // Fail() builds a fresh struct, so a `return FOpResult::Fail(...)` silently discards the
        // warnings and before-counts already on the result. That is not hypothetical - it made
        // Subdivide's "iterations clamped" warning unreachable on its POLYGON_LIMIT_EXCEEDED
        // path, and drove two other sites to hand-save and re-apply their counts around the call.
        static FOpResult& FailIn(FOpResult& Result, const TCHAR* Code, FString Message)
        {
            Result.bSuccess = false;
            Result.ErrorCode = Code;
            Result.ErrorMessage = MoveTemp(Message);
            return Result;
        }
    };

    // ------------------------------------------------------------------------------------
    // The shared op scaffolding (GeometryOps.cpp).
    //
    // Every family used to carry its own copy of these - GeometryOpsModeling_Begin/_Finish,
    // PrimCaptureBefore/PrimFinish, ElementsPrivate::ReadCounts/Succeeded,
    // GeometryOpsBoolean_FillCounts, RecordBefore/RecordAfter - which is how the null-mesh
    // contract and the bChanged rule managed to differ per family.
    // ------------------------------------------------------------------------------------

    // The null-mesh failure, identical in all five families: MESH_NOT_FOUND / "DynamicMesh not
    // available", the same pair GeometryTarget::ResolveOrSendError sends for a resolved actor
    // carrying no mesh. Unreachable from the RPC front-ends, whose meshes come from that
    // resolver or from GetOrCreateDynamicMesh; it is the .pwmodel compiler that needs a code
    // back instead of a null dereference inside an engine call.
    FOpResult NullMeshFailure();

    // Opens an op: null guard plus the before-counts the result carries. Returns false with
    // Result already filled when there is nothing to run on.
    bool BeginOp(UDynamicMesh* Mesh, FOpResult& Result);

    // Before-counts only, for the ops that guard their mesh some other way (a two-mesh boolean
    // checks both handles at once) and so cannot use BeginOp's return.
    void CaptureBefore(UDynamicMesh* Mesh, FOpResult& Result);

    // Closes an op that ran: after-counts, bSuccess, and bChanged from the count delta.
    //
    // bForceChanged is for the ops that alter attributes without moving either count - normals,
    // tangents, UVs, vertex positions - where a count comparison would report a real edit as a
    // no-op, and for the element ops that already know exactly what they changed.
    void FinishOp(UDynamicMesh* Mesh, FOpResult& Result, bool bForceChanged = false);

    // The engine hole filler writes normals and UV0 for every new triangle whenever the mesh has
    // attributes, but it checks only HasAttributes() before dereferencing both overlays. Grow the
    // missing layers without changing an attribute-free or boundary-free mesh; on an unexpected
    // creation failure, fail Result with the existing typed overlay error. bAttributesChanged
    // keeps the operation's `changed` response honest when only overlays moved. Shared by
    // fill_holes and bridge's one-loop fallback so the two entry points cannot drift apart.
    bool PrepareHoleFillAttributes(UDynamicMesh* Mesh, const TCHAR* OpName, FOpResult& Result,
                                   bool& bAttributesChanged);

    // ------------------------------------------------------------------------------------
    // Clamp-and-warn.
    //
    // "A clamp reports itself" is a contract rule for every family, not a Primitives detail:
    // silent clamping is how a caller ends up with a 100 uu box it asked to be 0.001 uu, or with
    // six subdivide iterations it asked to be ninety-nine. Each of these appends a warning only
    // when the value actually moved.
    //
    // `Label` IS THE RPC'S PARAMETER NAME, always, and this layer knows nothing about any other
    // front-end. Every message opens `"<Label> clamped from …"`, which is the shape a caller
    // greps. `.pwmodel` publishes several of these values under other names - `height_steps` for
    // `heightSteps`, one Vector3 `segments` for `widthSegments`/`heightSegments`/`depthSegments`
    // - and its compiler translates the leading token on re-emission from the table in
    // PwModelWarningNames (Model/PwModelParser.h). One label cannot satisfy both surfaces, so the
    // ops layer does not try: it stays the RPC's, with no caller-mode flag and no thread-local,
    // and the front-end that spells things differently owns the mapping. Adding a label here that
    // `.pwmodel` spells differently means adding a row there.
    // ------------------------------------------------------------------------------------

    double ClampDimensionWarn(double Value, const TCHAR* Label, FOpResult& Result);
    int32 ClampSegmentsWarn(int32 Value, int32 Default, const TCHAR* Label, FOpResult& Result);

    // ClampSegmentsWarn with the ceiling supplied rather than fixed at GEOM_MAX_SEGMENTS: same
    // two-tier rule (<= 0 reads as unset and takes Default, anything else is bounded), same
    // warning text, so a count that moves to a different ceiling keeps its published wording.
    // Exists for the stair step counts, which are not segment counts - see GEOM_MAX_STAIR_STEPS.
    int32 ClampCountWarn(int32 Value, int32 Default, int32 Max, const TCHAR* Label, FOpResult& Result);

    // Lerp weight clamped to [0,1].
    double ClampFactorWarn(double Value, const TCHAR* Label, FOpResult& Result);

    // Arbitrary integer range, for the loop-index and subdivision clamps in the advanced family.
    int32 ClampRangeWarn(int32 Value, int32 Min, int32 Max, const TCHAR* Label, FOpResult& Result);

    // ------------------------------------------------------------------------------------
    // Shared vocabulary
    // ------------------------------------------------------------------------------------

    // The axis selector for every op that takes one - stretch, cylindrify, mirror, array_radial.
    //
    // `None` exists because no RPC verb here validates its `axis` string: an unrecognized value
    // falls through the wrapper's if/else chain to the initial value. For mirror that is a unit
    // scale, so the clone is appended UNMIRRORED and the verb doubles the mesh in place; for
    // array_radial and the two deformers it is Z. Each op reproduces its own fallback, and the
    // string -> enum mapping (including the silent "anything else is Z") stays in the wrappers,
    // so an op cannot be handed an unspellable axis. A .pwmodel document cannot produce None -
    // the parser rejects an unknown axis before the op runs.
    enum class EMeshAxis : uint8
    {
        X,
        Y,
        Z,
        None
    };

    // Which of R/G/B/A a colour write applies to.
    //
    // Vertex colour is two independent signals in practice - RGB a per-part tint, A a mask - and a
    // write that always covers all four makes the second one unauthorable: setting alpha meant
    // re-sending an RGB the caller usually does not know per vertex, so whoever wrote the colour
    // first owned all four components. Every op that writes colour takes one of these, and `All`
    // is the default everywhere, so a caller that names no channels writes what it always did.
    enum class EColorChannels : uint8
    {
        None = 0,
        R    = 1 << 0,
        G    = 1 << 1,
        B    = 1 << 2,
        A    = 1 << 3,
        All  = R | G | B | A
    };
    ENUM_CLASS_FLAGS(EColorChannels);

    // Parses a channel spelling: any non-empty subset of "rgba", case-insensitive and order-free,
    // so "a", "rgb", "AR" and "rgba" are all legal. Returns false with OutError filled - and
    // OutChannels untouched - for an empty string, a repeated letter, or a character outside
    // r/g/b/a. It never falls back to All: a spelling this cannot read is a caller asking for
    // something specific, and guessing All would write the three channels they meant to keep.
    bool ParseColorChannels(const FString& Spec, EColorChannels& OutChannels, FString& OutError);

    // The canonical spelling of a mask, always in r,g,b,a order, for echoing back what was used.
    // EColorChannels::None spells "".
    FString ColorChannelsToString(EColorChannels Channels);

    // Existing with the masked components of Incoming substituted. The unmasked components of
    // Existing come back unchanged - that is the whole point of the mask.
    FVector4f ApplyColorChannels(const FVector4f& Existing, const FVector4f& Incoming, EColorChannels Channels);
}
