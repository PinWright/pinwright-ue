// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Elements.cpp - see GeometryOps_Elements.h for the contract these all share.
//
// The null guard and the before/after count snapshot are GeometryOps.h's, shared with the
// other four families; this file no longer carries a private copy of either. What it carried
// before was worse than nothing: ElementsPrivate::ReadCounts null-checked the mesh inside a
// ternary and then every caller dereferenced that same pointer with Mesh->GetMeshRef() on the
// next line, so the ternary read as a safety check while the op still crashed on null.
// BeginOp is a real guard - it returns MESH_NOT_FOUND / "DynamicMesh not available" instead.
//
// bChanged: these ops know exactly what they changed, and most of them change nothing a count
// can see (a moved vertex, a written color element, a baked translation), so they pass
// FinishOp's bForceChanged rather than letting the count-delta rule decide. DeleteVertex is
// the one that must pin the field afterwards - see the comment there.
#include "Handlers/Geometry/GeometryOps_Elements.h"

#include "Handlers/ErrorCodes.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Misc/EngineVersionComparison.h"
#include "UDynamicMesh.h"

// The ambient-occlusion bake's whole measurement layer, from the GeometryProcessing plugin's
// DynamicMesh module (already a hard dependency of this one). FMeshVertexBaker is the per-colour-
// element baker behind the editor's own Bake Vertex Colors tool; FMeshOcclusionMapEvaluator is the
// occlusion integral it drives; FMeshBakerDynamicMeshSampler adapts a mesh + AABB tree into the
// detail sampler both expect.
#include "Sampling/MeshBakerCommon.h"
#include "Sampling/MeshOcclusionMapEvaluator.h"
#include "Sampling/MeshVertexBaker.h"

#include "GeometryScript/MeshTransformFunctions.h"

namespace GeometryOps
{

FOpResult SetVertexPosition(UDynamicMesh* Mesh, const FSetVertexPositionParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    if (!EditMesh.IsVertex(Params.VertexIndex))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_VERTEX,
            FString::Printf(TEXT("Invalid vertex index: %d"), Params.VertexIndex));
    }

    const bool bMoved = EditMesh.GetVertex(Params.VertexIndex) != Params.Position;
    EditMesh.SetVertex(Params.VertexIndex, Params.Position);

    // Moving a vertex moves neither count, so the count-delta rule would report a real edit
    // as a no-op. bMoved is the authoritative answer and goes in as bForceChanged.
    FinishOp(Mesh, Result, /*bForceChanged=*/bMoved);
    return Result;
}

FOpResult AppendVertex(UDynamicMesh* Mesh, const FAppendVertexParams& Params, int32& OutVertexIndex)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    OutVertexIndex = EditMesh.AppendVertex(UE::Geometry::FVertexInfo(Params.Position));

    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult DeleteVertex(UDynamicMesh* Mesh, const FDeleteVertexParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    if (!EditMesh.IsVertex(Params.VertexIndex))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_VERTEX,
            FString::Printf(TEXT("Invalid vertex index: %d"), Params.VertexIndex));
    }

    const UE::Geometry::EMeshResult RemoveResult = EditMesh.RemoveVertex(Params.VertexIndex);

    FinishOp(Mesh, Result);

    // Pinned rather than passed as bForceChanged, because FinishOp's count-delta rule is
    // WRONG here. FDynamicMesh3::RemoveVertex (DynamicMesh3_Edits.cpp:614) removes the
    // incident triangles one at a time and returns on the first refusal, and can still
    // return Failed_VertexStillReferenced after removing every one of them - so a NON-Ok
    // result can leave the triangle count moved. The delta rule would then say bChanged, and
    // delete_vertex echoes bChanged as its `success` field inside an otherwise-successful
    // response: the verb would start claiming it removed a vertex the mesh refused to
    // remove. It has always reported the mesh's own verdict; report exactly that.
    Result.bChanged = (RemoveResult == UE::Geometry::EMeshResult::Ok);
    return Result;
}

FOpResult AppendTriangle(UDynamicMesh* Mesh, const FAppendTriangleParams& Params, FAppendTriangleIndices& OutIndices)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    OutIndices.VertexIndices[0] = EditMesh.AppendVertex(UE::Geometry::FVertexInfo(Params.V0));
    OutIndices.VertexIndices[1] = EditMesh.AppendVertex(UE::Geometry::FVertexInfo(Params.V1));
    OutIndices.VertexIndices[2] = EditMesh.AppendVertex(UE::Geometry::FVertexInfo(Params.V2));

    OutIndices.TriangleIndex = EditMesh.AppendTriangle(
        OutIndices.VertexIndices[0], OutIndices.VertexIndices[1], OutIndices.VertexIndices[2], Params.GroupID);

    // The three vertices are appended unconditionally, so even a refused triangle leaves the
    // mesh changed - which is why this is a fixed true and not the AppendTriangle return.
    FinishOp(Mesh, Result, /*bForceChanged=*/true);
    return Result;
}

FOpResult DeleteTriangle(UDynamicMesh* Mesh, const FDeleteTriangleParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    if (!EditMesh.IsTriangle(Params.TriangleIndex))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_TRIANGLE,
            FString::Printf(TEXT("Invalid triangle index: %d"), Params.TriangleIndex));
    }

    const UE::Geometry::EMeshResult RemoveResult = EditMesh.RemoveTriangle(Params.TriangleIndex);

    // Unlike RemoveVertex, RemoveTriangle (DynamicMesh3_Edits.cpp:678) rejects before it
    // touches anything, so a non-Ok result cannot have moved a count: the delta can only ever
    // agree with the mesh's verdict here, and bForceChanged alone is exact.
    FinishOp(Mesh, Result, /*bForceChanged=*/RemoveResult == UE::Geometry::EMeshResult::Ok);
    return Result;
}

namespace ElementsPrivate
{
    // The neutral value a colour element is born with, in both the engine's CreatePerVertex seed
    // and the grow loop below. Named because the two must agree: an element created by one path
    // and painted through a channel mask by the other keeps this in the channels the mask left
    // alone, and a caller reading white there needs to know it is the overlay default rather
    // than a measurement.
    static const FVector4f ColorElementDefault(1.0f, 1.0f, 1.0f, 1.0f);

    // Vertex colors must land in the mesh's attribute-overlay PrimaryColors channel, NOT
    // the legacy FDynamicMesh3 per-vertex color buffer (EnableVertexColors/SetVertexColor).
    // The overlay is the only channel get_mesh_info's GetHasVertexColors, the
    // DynamicMeshComponent renderer, and the convert_to_static_mesh bake read — a write to
    // the legacy buffer leaves PrimaryColors() null, so hasColors stays false and the tint is
    // dropped everywhere it would matter. Enable attributes + the primary-color overlay, and
    // seed a complete per-vertex layer when the overlay is empty.
    UE::Geometry::FDynamicMeshColorOverlay* EnsureColorOverlay(UE::Geometry::FDynamicMesh3& EditMesh)
    {
        if (!EditMesh.HasAttributes())
        {
            EditMesh.EnableAttributes();
        }
        if (!EditMesh.Attributes()->HasPrimaryColors())
        {
            EditMesh.Attributes()->EnablePrimaryColors();
        }
        UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay = EditMesh.Attributes()->PrimaryColors();

        // A freshly-enabled overlay has no elements. Seed a complete per-vertex color layer
        // (one neutral-white element per vertex, every triangle wired to its vertex elements)
        // via the engine primitive rather than hand-rolling the element/triangle wiring. The
        // float InitElementValue is copied into all four channels -> (1,1,1,1). Keep the empty
        // guard so an already-populated/seamed overlay is not clobbered by CreatePerVertex's
        // internal ClearElements().
        if (ColorOverlay->ElementCount() == 0)
        {
#if UE_VERSION_OLDER_THAN(5, 7, 0)
            // CreatePerVertex was added in UE 5.7. On 5.6 and earlier, the always-share
            // predicate produces the same layer for every vertex a triangle references: one
            // element per parent vertex, seeded to 1.0.
            ColorOverlay->CreateFromPredicate([](int, int, int) { return true; }, 1.0f);
            // CreateFromPredicate walks TRIANGLES, so a live vertex no triangle references gets
            // no element - where CreatePerVertex, which walks vertices, gives it one. Append
            // those so the seeded layer is identical on every engine, including the orphan
            // element the FreeUnusedElements repair below exists to catch before it reaches the
            // engine baker.
            for (const int32 IsolatedVertexID : EditMesh.VertexIndicesItr())
            {
                if (!EditMesh.IsReferencedVertex(IsolatedVertexID))
                {
                    const int32 IsolatedElementID =
                        ColorOverlay->AppendElement(FVector4f(1.0f, 1.0f, 1.0f, 1.0f));
                    ColorOverlay->SetParentVertex(IsolatedElementID, IsolatedVertexID);
                }
            }
#else
            ColorOverlay->CreatePerVertex(1.0f);
#endif
        }
        return ColorOverlay;
    }

    // GROW the layer to cover every triangle, rather than leaving whatever elements happen to
    // exist. EnsureColorOverlay's seed only runs on an EMPTY overlay, so once anything has
    // coloured the mesh the layer is never widened again - and triangles appended afterwards by
    // extrude_along_spline, sweep, bevel, shell or a boolean carry no colour element at all.
    // Those triangles were unreachable by every later write, and they render at the overlay
    // default, white. Growing here is what makes a whole-mesh write mean the whole mesh.
    //
    // Seam-preserving by construction: only triangles with NO element assignment are touched,
    // and a corner whose vertex already carries exactly one element reuses it rather than
    // splitting the vertex. A vertex already split by a real seam has more than one element,
    // and there is no basis for guessing which side a new triangle belongs to, so it gets a
    // fresh element - a new corner on an already-seamed vertex, not a lost seam. Nothing
    // clears or rebuilds existing assignments, which is what CreatePerVertex would have done.
    //
    // Returns how many elements had to be created.
    int32 GrowColorOverlayToCoverAllTriangles(
        UE::Geometry::FDynamicMesh3& EditMesh, UE::Geometry::FDynamicMeshColorOverlay& ColorOverlay)
    {
        int32 ElementsCreated = 0;
        for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
        {
            if (ColorOverlay.IsSetTriangle(TriangleID))
            {
                continue;
            }
            const UE::Geometry::FIndex3i Tri = EditMesh.GetTriangle(TriangleID);
            UE::Geometry::FIndex3i NewTri(-1, -1, -1);
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                TArray<int32> Existing;
                ColorOverlay.GetVertexElements(Tri[Corner], Existing);
                if (Existing.Num() == 1)
                {
                    NewTri[Corner] = Existing[0];
                }
                else
                {
                    NewTri[Corner] = ColorOverlay.AppendElement(ColorElementDefault);
                    // AppendElement parents the new element to InvalidID and SetTriangle below
                    // does NOT fix that up - only CreatePerVertex/CreateFromPredicate set parents
                    // themselves. Without this the element is unattributable: GetParentVertex
                    // returns InvalidID, so the vertex count over these elements is wrong, and
                    // the engine's vertex baker feeds that InvalidID to IndexUtil::FindTriIndex
                    // and then indexes a barycentric triple with the -1 it gets back.
                    ColorOverlay.SetParentVertex(NewTri[Corner], Tri[Corner]);
                    ++ElementsCreated;
                }
            }
            ColorOverlay.SetTriangle(TriangleID, NewTri);
        }
        return ElementsCreated;
    }

    // How many DISTINCT VERTICES the given elements cover. The honest count for a whole-mesh
    // write: it used to be EditMesh.VertexCount() unconditionally - the whole mesh, whatever the
    // loop had actually reached - which is the half of that defect that made it invisible, since
    // a caller reading `verticesModified` could not tell a full repaint from one that missed
    // every appended triangle. Counted from the elements written, so a vertex no triangle
    // references (and which therefore cannot hold a colour element) is correctly not claimed.
    int32 CountParentVertices(
        const UE::Geometry::FDynamicMeshColorOverlay& ColorOverlay, const TArray<int32>& ElementIDs)
    {
        TSet<int32> Vertices;
        Vertices.Reserve(ElementIDs.Num());
        for (const int32 ElementID : ElementIDs)
        {
            Vertices.Add(ColorOverlay.GetParentVertex(ElementID));
        }
        return Vertices.Num();
    }
}

FOpResult SetVertexColor(UDynamicMesh* Mesh, const FSetVertexColorParams& Params,
    int32& OutVerticesModified, int32* OutElementsCreated)
{
    OutVerticesModified = 0;
    if (OutElementsCreated)
    {
        *OutElementsCreated = 0;
    }

    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    if (Params.Channels == EColorChannels::None)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("no colour channel named, so the write has nowhere to land. Pass channels as "
                 "any combination of r, g, b and a, or leave it off to write all four."));
    }

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay = ElementsPrivate::EnsureColorOverlay(EditMesh);

    const FVector4f Color(
        static_cast<float>(Params.Color.R),
        static_cast<float>(Params.Color.G),
        static_cast<float>(Params.Color.B),
        static_cast<float>(Params.Color.A));

    // Substitute only the masked components, leaving the rest of each element as it was. With
    // the default All mask this writes Color whole, which is what every existing caller gets.
    auto PaintElement = [ColorOverlay, &Color, &Params](int32 ElementID)
    {
        const FVector4f Existing = ColorOverlay->GetElement(ElementID);
        ColorOverlay->SetElement(ElementID, ApplyColorChannels(Existing, Color, Params.Channels));
    };

    if (Params.bSetAll)
    {
        const int32 ElementsCreated =
            ElementsPrivate::GrowColorOverlayToCoverAllTriangles(EditMesh, *ColorOverlay);
        if (OutElementsCreated)
        {
            *OutElementsCreated = ElementsCreated;
        }

        TArray<int32> PaintedElements;
        PaintedElements.Reserve(ColorOverlay->ElementCount());
        for (int32 ElementID : ColorOverlay->ElementIndicesItr())
        {
            PaintElement(ElementID);
            PaintedElements.Add(ElementID);
        }

        OutVerticesModified = ElementsPrivate::CountParentVertices(*ColorOverlay, PaintedElements);
    }
    else if (Params.VertexIndex >= 0 && EditMesh.IsVertex(Params.VertexIndex))
    {
        // Set only the overlay element(s) belonging to the target vertex. GetVertexElements
        // walks that vertex's incident triangles (O(valence)) instead of scanning every
        // overlay element (O(element count)) with a parent-vertex filter.
        TArray<int32> VertexElements;
        ColorOverlay->GetVertexElements(Params.VertexIndex, VertexElements);
        for (int32 ElementID : VertexElements)
        {
            PaintElement(ElementID);
        }
        // Report the true count: 0 when the vertex had no assigned overlay element (e.g. a
        // pre-existing sparse/split overlay) rather than an unconditional false-success 1.
        OutVerticesModified = VertexElements.Num() > 0 ? 1 : 0;
    }
    else
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_VERTEX,
            FString::Printf(TEXT("Invalid vertex index: %d"), Params.VertexIndex));
    }

    // Enabling attributes and seeding the overlay move neither count, so only the write
    // itself can report the change.
    FinishOp(Mesh, Result, /*bForceChanged=*/OutVerticesModified > 0);
    return Result;
}

namespace ElementsPrivate
{
    // Running min/mean/max. Mean is accumulated rather than derived at the end from a stored
    // array, because the caller only ever reads the three numbers.
    struct FStatAccumulator
    {
        double Min = TNumericLimits<double>::Max();
        double Max = -TNumericLimits<double>::Max();
        double Sum = 0.0;
        int32 Count = 0;

        void Add(double Value)
        {
            Min = FMath::Min(Min, Value);
            Max = FMath::Max(Max, Value);
            Sum += Value;
            ++Count;
        }

        FValueStatistics Finish() const
        {
            FValueStatistics Out;
            Out.Count = Count;
            if (Count > 0)
            {
                Out.Min = Min;
                Out.Max = Max;
                Out.Mean = Sum / static_cast<double>(Count);
            }
            return Out;
        }
    };
}

FOpResult BakeAmbientOcclusion(UDynamicMesh* Mesh, const FBakeAmbientOcclusionParams& Params,
    FBakeAmbientOcclusionOutputs& Outputs)
{
    Outputs = FBakeAmbientOcclusionOutputs();

    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    if (Params.Channels == EColorChannels::None)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("no colour channel named, so the bake has nowhere to land. Pass channels as any "
                 "combination of r, g, b and a - 'a' alone is the cheap one, since it leaves an "
                 "RGB tint intact."));
    }
    if (!(Params.OcclusionRadius > 0.0))
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("occlusionRadius must be greater than 0; got %g. It is the maximum ray length in "
                 "the mesh's own local units, and it is what makes a part junction occlude while "
                 "the far side of the same mesh does not."), Params.OcclusionRadius));
    }
    if (Params.Samples < 1)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("samples must be at least 1; got %d. Nothing is clamped here because a sample "
                 "count is what the measurement is made of - a substituted one would report a "
                 "quality the caller did not ask for."), Params.Samples));
    }
    if (Params.Strength < 0.0 || Params.Strength > 1.0)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("strength must be within 0-1; got %g."), Params.Strength));
    }

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    if (EditMesh.TriangleCount() == 0)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_MESH_EMPTY,
            TEXT("the mesh has no triangles, so there is nothing to cast rays against and nothing "
                 "to write them to. A bake on an empty mesh would report a flat, fully-exposed "
                 "result that measured nothing."));
    }

    // THE NORMAL OVERLAY MUST COVER EVERY TRIANGLE, and an empty one is not good enough.
    //
    // It reads like a robustness nicety and it is not. Two engine call sites make an uncovered
    // triangle bake from UNINITIALIZED STACK MEMORY rather than fail:
    //   - FMeshBakerDynamicMeshSampler::TriBaryInterpolateNormal returns false and leaves its
    //     NormalOut out-parameter UNTOUCHED when the overlay does not cover the triangle
    //     (MeshBakerCommon.h, the `IsSetTriangle(TriId)` guard).
    //   - FMeshOcclusionMapEvaluator::SampleFunction declares `FVector3f DetailTriNormal;`
    //     uninitialized and IGNORES that bool return before normalizing it and building the
    //     hemisphere frame from it (MeshOcclusionMapEvaluator.cpp, SampleFunction's first lines).
    // FMeshVertexBaker::SampleSurface does fall back to the triangle normal, which is what made
    // an earlier version of this comment wrong: that fallback covers the baker's own frame, not
    // the evaluator's, and the evaluator is where the rays are cast.
    //
    // Only the UNCOVERED triangles are initialized, so authored split normals elsewhere on the
    // mesh survive untouched.
    if (!EditMesh.HasAttributes())
    {
        EditMesh.EnableAttributes();
    }
    if (EditMesh.Attributes()->NumNormalLayers() < 1)
    {
        EditMesh.Attributes()->SetNumNormalLayers(1);
    }
    {
        UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = EditMesh.Attributes()->PrimaryNormals();
        TArray<int32> TrianglesMissingNormals;
        for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
        {
            if (!NormalOverlay->IsSetTriangle(TriangleID))
            {
                TrianglesMissingNormals.Add(TriangleID);
            }
        }
        if (TrianglesMissingNormals.Num() > 0)
        {
            UE::Geometry::FMeshNormals::InitializeOverlayRegionToPerVertexNormals(
                NormalOverlay, TrianglesMissingNormals);
            Outputs.TrianglesGivenNormals = TrianglesMissingNormals.Num();
        }
    }

    // The bake writes one value per colour ELEMENT, so the overlay has to exist and cover every
    // triangle before the baker sizes its result against it - a triangle with no element is a
    // triangle whose corners the bake cannot reach, exactly as for a whole-mesh colour write.
    UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay = ElementsPrivate::EnsureColorOverlay(EditMesh);

    // A colour element no triangle references CRASHES the bake, it does not merely waste a pixel:
    // FMeshVertexBaker::SampleSurface reads that element's triangle list at [0] with no empty
    // guard. Two reachable sources - CreatePerVertex seeds one element per VERTEX, so an isolated
    // vertex gets an orphan, and delete_triangle / a boolean orphans elements of an overlay that
    // already existed. Freed BEFORE the grow below so the appends reuse the freed IDs and the
    // layer usually stays compact; whatever gaps survive are caught by the ResultWidth skip.
    {
        const int32 ElementsBeforeFree = ColorOverlay->ElementCount();
        ColorOverlay->FreeUnusedElements();
        Outputs.OrphanElementsFreed = ElementsBeforeFree - ColorOverlay->ElementCount();
    }

    Outputs.ElementsCreated =
        ElementsPrivate::GrowColorOverlayToCoverAllTriangles(EditMesh, *ColorOverlay);

    // The occlusion integral is the engine's, not this file's: FMeshOcclusionMapEvaluator casts
    // NumOcclusionRays cosine-weighted rays per corner against a FDynamicMeshAABBTree3 of the
    // same mesh and writes the VISIBLE fraction (1 = fully exposed). Correspondence is Identity
    // because the mesh occludes itself - there is no separate high-poly detail mesh here.
    const UE::Geometry::FDynamicMeshAABBTree3 Spatial(&EditMesh);
    UE::Geometry::FMeshBakerDynamicMeshSampler DetailSampler(&EditMesh, &Spatial);

    TSharedPtr<UE::Geometry::FMeshOcclusionMapEvaluator, ESPMode::ThreadSafe> Evaluator =
        MakeShared<UE::Geometry::FMeshOcclusionMapEvaluator, ESPMode::ThreadSafe>();
    Evaluator->OcclusionType = UE::Geometry::EMeshOcclusionMapType::AmbientOcclusion;
    Evaluator->NumOcclusionRays = Params.Samples;
    Evaluator->MaxDistance = Params.OcclusionRadius;
    Evaluator->SpreadAngle = 180.0;
    Evaluator->BiasAngleDeg = Params.BiasAngleDegrees;

    UE::Geometry::FMeshVertexBaker Baker;
    Baker.BakeMode = UE::Geometry::FMeshVertexBaker::EBakeMode::RGBA;
    Baker.SetTargetMesh(&EditMesh);
    Baker.SetDetailSampler(&DetailSampler);
    Baker.SetCorrespondenceStrategy(UE::Geometry::FMeshBaseBaker::ECorrespondenceStrategy::Identity);
    Baker.ColorEvaluator = Evaluator;
    Baker.Bake();

    const UE::Geometry::TImageBuilder<FVector4f>* BakeResult = Baker.GetBakeResult();
    if (BakeResult == nullptr)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_BAKE_FAILED,
            TEXT("the engine vertex baker produced no result image. Nothing was written."));
    }

    // The baker SIZES its result by the overlay's live element COUNT but INDEXES it by element
    // ID, so on an overlay carrying ID gaps the elements above that count have no pixel. Skip
    // them and say how many rather than reading past the image or writing an unmeasured value.
    const int32 ResultWidth = BakeResult->GetDimensions().GetWidth();

    ElementsPrivate::FStatAccumulator OcclusionStats;
    ElementsPrivate::FStatAccumulator ChannelStats[4];
    const EColorChannels ChannelBits[4] =
        { EColorChannels::R, EColorChannels::G, EColorChannels::B, EColorChannels::A };

    const float StrengthF = static_cast<float>(Params.Strength);
    TArray<int32> WrittenElements;
    WrittenElements.Reserve(ColorOverlay->ElementCount());

    for (int32 ElementID : ColorOverlay->ElementIndicesItr())
    {
        if (ElementID >= ResultWidth)
        {
            ++Outputs.ElementsUnreachable;
            continue;
        }

        // The AO evaluator fills R, G and B with the same scalar and pins A to 1, so any one of
        // the three carries the measurement.
        const float Occlusion = FMath::Clamp(BakeResult->GetPixel(ElementID).X, 0.0f, 1.0f);
        OcclusionStats.Add(Occlusion);

        // Strength lerps toward fully exposed, so 0 writes white and darkens nothing.
        const float Scaled = 1.0f - StrengthF * (1.0f - Occlusion);

        const FVector4f Existing = ColorOverlay->GetElement(ElementID);
        const FVector4f Incoming = Params.bMultiply
            ? FVector4f(Existing.X * Scaled, Existing.Y * Scaled, Existing.Z * Scaled, Existing.W * Scaled)
            : FVector4f(Scaled, Scaled, Scaled, Scaled);

        const FVector4f Final = ApplyColorChannels(Existing, Incoming, Params.Channels);
        ColorOverlay->SetElement(ElementID, Final);
        WrittenElements.Add(ElementID);

        for (int32 Channel = 0; Channel < 4; ++Channel)
        {
            if (EnumHasAnyFlags(Params.Channels, ChannelBits[Channel]))
            {
                ChannelStats[Channel].Add(Final[Channel]);
            }
        }
    }

    Outputs.ElementsWritten = WrittenElements.Num();
    Outputs.VerticesModified = ElementsPrivate::CountParentVertices(*ColorOverlay, WrittenElements);
    Outputs.Occlusion = OcclusionStats.Finish();
    for (int32 Channel = 0; Channel < 4; ++Channel)
    {
        Outputs.Written[Channel] = ChannelStats[Channel].Finish();
    }

    if (Outputs.ElementsUnreachable > 0)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("%d colour elements were left unbaked because the overlay carries element-ID gaps "
                 "and the engine vertex baker sizes its result by live element count; they keep "
                 "the colour they had"),
            Outputs.ElementsUnreachable));
    }
    if (Outputs.OrphanElementsFreed > 0)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("%d colour elements referenced by no triangle were freed before baking; the "
                 "engine vertex baker indexes an element's triangle list without an empty check, "
                 "so leaving them would have terminated the editor rather than skipped them"),
            Outputs.OrphanElementsFreed));
    }
    if (Outputs.TrianglesGivenNormals > 0)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("%d triangles the primary normal overlay did not cover were given per-vertex "
                 "normals before baking; the occlusion evaluator reads that overlay through an "
                 "out-parameter it leaves untouched on a miss, so an uncovered triangle would "
                 "have been shaded from uninitialized memory"),
            Outputs.TrianglesGivenNormals));
    }

    // A bake writes colours, which moves neither the vertex nor the triangle count, so only the
    // write itself can report the change.
    FinishOp(Mesh, Result, /*bForceChanged=*/Outputs.ElementsWritten > 0);
    return Result;
}

FOpResult SetUVs(UDynamicMesh* Mesh, const FSetUVsParams& Params, int32& OutElementsModified)
{
    OutElementsModified = 0;

    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    // Ensure the mesh has UV overlay for the specified channel
    UE::Geometry::FDynamicMeshAttributeSet* Attributes = EditMesh.Attributes();
    if (!Attributes)
    {
        EditMesh.EnableAttributes();
        Attributes = EditMesh.Attributes();
    }

    // Grow-only, and the `>=` guard is the load-bearing half: SetNumUVLayers sets the layer
    // count EXACTLY, so calling it unguarded would TRUNCATE a mesh that already carries
    // higher layers (an authored lightmap in channel 1 behind a request for channel 0).
    // One call is enough — SetNumUVLayers loops over the missing layers internally, which is
    // what made the former per-layer for-loop around it N redundant calls for a single widen.
    if (Params.UVChannel >= Attributes->NumUVLayers())
    {
        Attributes->SetNumUVLayers(Params.UVChannel + 1);
    }

    UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = Attributes->GetUVLayer(Params.UVChannel);
    if (!UVOverlay)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_UV_LAYER_ERROR, TEXT("Failed to access UV layer"));
    }

    const FVector2f UVValue(static_cast<float>(Params.UV.X), static_cast<float>(Params.UV.Y));

    if (Params.VertexIndex >= 0 && EditMesh.IsVertex(Params.VertexIndex))
    {
        for (int32 ElementID : UVOverlay->ElementIndicesItr())
        {
            if (UVOverlay->GetParentVertex(ElementID) == Params.VertexIndex)
            {
                UVOverlay->SetElement(ElementID, UVValue);
                OutElementsModified++;
            }
        }

        if (OutElementsModified == 0)
        {
            return FOpResult::FailIn(Result, ErrorCodes::ERR_NO_UV_ELEMENTS,
                FString::Printf(TEXT("No UV elements found for vertex %d"), Params.VertexIndex));
        }
    }
    else
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_VERTEX,
            FString::Printf(TEXT("Invalid vertex index: %d"), Params.VertexIndex));
    }

    // Writing a UV element moves no count; only the write itself reports the change.
    FinishOp(Mesh, Result, /*bForceChanged=*/OutElementsModified > 0);
    return Result;
}

FOpResult TranslateMesh(UDynamicMesh* Mesh, const FTranslateMeshParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    UGeometryScriptLibrary_MeshTransformFunctions::TranslateMesh(Mesh, Params.Translation, nullptr);

    // Baking an offset into every vertex moves no count either, and a zero translation is a
    // real no-op the .pwmodel compiler wants reported as one.
    FinishOp(Mesh, Result, /*bForceChanged=*/!Params.Translation.IsZero());
    return Result;
}

}
