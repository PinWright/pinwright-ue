// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-merge-vertices-welds-edges-not-vertices.
//
// geometry.merge_vertices is documented as "Merge nearby vertices on a dynamic
// mesh" with a "Merge distance tolerance" param, but the original implementation
// called UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges — the
// boundary-EDGE weld (FMergeCoincidentMeshEdges), which only sews shut open
// boundary edges. On a closed/watertight mesh there are no open boundary edges,
// so the verb silently no-op'd (merged:0) on proximity-coincident vertices while
// reporting success. That is the SAME engine call geometry.weld_vertices already
// wraps, so the two verbs were duplicate edge welds.
//
// The fix retargets merge_vertices at a real coincident-VERTEX weld over the
// underlying FDynamicMesh3: UDynamicMesh::EditMesh -> FDynamicMesh3::MergeVertices
// on each tolerance-matched pair, counting only EMeshResult::Ok merges. Merging two
// vertices that are connected by an existing edge resolves as an edge collapse
// (always manifold-valid, even on a closed mesh), so collapsing edge-adjacent
// vertices onto a single coordinate now actually welds them.
//
// Strategy (exercises the production handler end-to-end via the real dispatcher):
//   1. Spawn a real DynamicMeshActor (a subdivided box-sphere) via
//      geometry.create_sphere, then read its starting vertexCount.
//   2. Read vertex 0's position, then move a contiguous block of low-index
//      vertices onto that exact coordinate via geometry.set_vertex_position. The
//      box-sphere's first face is a vertex grid, so consecutive low indices are
//      edge-adjacent; collapsing a run of them onto one point guarantees multiple
//      edge-connected coincident pairs that MergeVertices welds via edge collapse.
//   3. Call geometry.merge_vertices with a generous tolerance.
//   4. Assert it welded at least one vertex (merged >= 1, verticesAfter <
//      verticesBefore) and echoed the post-op vertexCount.
//
// Counterfactual: reverting the fix (back to the WeldMeshEdges boundary-edge weld)
// makes merge_vertices a no-op on this closed sphere — merged:0, verticesAfter ==
// verticesBefore — which fails the merged>=1 / vertex-drop assertions below.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMergeVerticesWeldsVerticesTest,
    "PinWright.geometry.merge_vertices.WeldsCoincidentVertices",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryMergeVerticesWeldsVerticesTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping merge_vertices weld test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_MergeVertsProbe_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // 1. Spawn a real subdivided box-sphere DynamicMeshActor.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        CreateParams->SetNumberField(TEXT("radius"), 60.0);
        CreateParams->SetNumberField(TEXT("subdivisions"), 16);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_sphere"),
            TEXT("req-merge-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_sphere spawned the probe DynamicMeshActor"), bCreated))
        {
            GeometryTestHelpers::DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // 2a. Read vertex 0's position — the coordinate we collapse a block onto.
    double KeepX = 0.0, KeepY = 0.0, KeepZ = 0.0;
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetNumberField(TEXT("vertexIndex"), 0);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.get_vertex_position"),
            TEXT("req-merge-getvert0"), Params, bSuccess, Result, ErrorCode);
        const TSharedPtr<FJsonObject>* PosObj = nullptr;
        if (!TestTrue(TEXT("get_vertex_position(0) succeeded"), bSuccess) ||
            !TestTrue(TEXT("get_vertex_position(0) carries a result"), Result.IsValid()) ||
            !TestTrue(TEXT("get_vertex_position(0) has a position object"),
                Result.IsValid() && Result->TryGetObjectField(TEXT("position"), PosObj)))
        {
            GeometryTestHelpers::DestroyActorsWithLabel(Label);
            return true;
        }
        (*PosObj)->TryGetNumberField(TEXT("x"), KeepX);
        (*PosObj)->TryGetNumberField(TEXT("y"), KeepY);
        (*PosObj)->TryGetNumberField(TEXT("z"), KeepZ);
    }

    // 2b. Collapse a contiguous block of low-index vertices onto vertex 0's coord.
    // A 40-vertex run on a 16-subdivision box-sphere spans an edge-connected grid
    // patch, so several of these become edge-adjacent coincident pairs — exactly
    // the manifold-valid edge-collapse case MergeVertices welds on a closed mesh.
    const int32 BlockEnd = 40;
    for (int32 VID = 1; VID < BlockEnd; ++VID)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetNumberField(TEXT("vertexIndex"), VID);
        TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
        Pos->SetNumberField(TEXT("x"), KeepX);
        Pos->SetNumberField(TEXT("y"), KeepY);
        Pos->SetNumberField(TEXT("z"), KeepZ);
        Params->SetObjectField(TEXT("position"), Pos);
        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.set_vertex_position"),
            FString::Printf(TEXT("req-merge-setvert-%d"), VID), Params, bSuccess, ErrorCode);
        // A vertex index may not exist if the mesh is smaller than expected; tolerate
        // that rather than fail (the weld assertion below is the real check).
        if (!bSuccess)
        {
            break;
        }
    }

    // 3. Merge with a tolerance far exceeding the 0.0 separation of the collapsed
    // block. compact=true so verticesAfter reflects the welded-away vertices.
    bool bMergeSuccess = false;
    FString MergeErr;
    TSharedPtr<FJsonObject> MergeResult;
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetNumberField(TEXT("tolerance"), 0.5);
        Params->SetBoolField(TEXT("compact"), true);
        Dispatch(Dispatcher, Sink, TEXT("geometry.merge_vertices"),
            TEXT("req-merge-vertices"), Params, bMergeSuccess, MergeResult, MergeErr);
    }

    if (!TestTrue(TEXT("geometry.merge_vertices succeeded"), bMergeSuccess) ||
        !TestTrue(TEXT("geometry.merge_vertices carries a result object"), MergeResult.IsValid()))
    {
        GeometryTestHelpers::DestroyActorsWithLabel(Label);
        return true;
    }

    // 4. It must have actually welded coincident vertices (the bug: this was 0 on a
    // closed mesh because WeldMeshEdges only welds open boundary edges).
    double Merged = 0.0;
    TestTrue(TEXT("merge_vertices echoes a merged count"),
        MergeResult->TryGetNumberField(TEXT("merged"), Merged));
    TestTrue(TEXT("merge_vertices welded at least one coincident vertex (was 0 with the edge-weld bug)"),
        Merged >= 1.0);

    double VertsBefore = 0.0, VertsAfter = 0.0;
    MergeResult->TryGetNumberField(TEXT("verticesBefore"), VertsBefore);
    MergeResult->TryGetNumberField(TEXT("verticesAfter"), VertsAfter);
    TestTrue(TEXT("merge_vertices reduced the vertex count"), VertsAfter < VertsBefore);

    // The fix also echoes the post-op vertexCount/triangleCount inline.
    double VertexCount = 0.0;
    TestTrue(TEXT("merge_vertices echoes a post-op vertexCount"),
        MergeResult->TryGetNumberField(TEXT("vertexCount"), VertexCount));
    TestTrue(TEXT("post-op vertexCount is non-zero"), VertexCount > 0.0);

    GeometryTestHelpers::DestroyActorsWithLabel(Label);
    return true;
}
