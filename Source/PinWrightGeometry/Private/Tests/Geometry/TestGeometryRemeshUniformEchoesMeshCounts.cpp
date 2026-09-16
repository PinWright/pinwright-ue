// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-remesh-uniform-echoes-target-not-achieved.
//
// geometry.remesh_uniform's entire purpose is to retopologize a mesh toward a
// triangle BUDGET, yet its success response used to carry only the REQUESTED
// targetTriangleCount (an echo of the input) plus a static message — it read NO
// post-op count off the live Target.Mesh it had just remeshed. Because uniform
// remesh only APPROXIMATES the target (ApplyUniformRemesh targets an edge length,
// so the achieved count can diverge substantially from the request), the one
// count-shaped field the response carried was the input, mistakable for the
// achieved result on the exact "did it hit my budget?" question the verb exists to
// answer — forcing a separate geometry.get_mesh_info to read the real count.
//
// The fix additionally echoes the ACHIEVED vertexCount/triangleCount inline off
// Target.Mesh via the shared GeometryUtils::SetMeshCountFields helper (the same
// helper simplify/subdivide's deformer siblings use), matching the keys
// get_mesh_info returns. The achieved triangleCount is a DISTINCT key from
// targetTriangleCount, so it cannot be confused with the input echo.
//
// Strategy: spawn a real DynamicMeshActor via geometry.create_box (an in-code
// fixture — no external asset load), route geometry.remesh_uniform through the
// real dispatcher, and assert the success result carries BOTH the echoed input
// targetTriangleCount AND a non-zero achieved vertexCount + triangleCount. This
// exercises the production handler end-to-end. Counterfactual: reverting the fix
// (success result back to {actorName, targetTriangleCount} only) drops
// vertexCount/triangleCount and fails this test.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

// remesh_uniform's success response carries the achieved post-op vertexCount and
// triangleCount inline (distinct from the echoed input targetTriangleCount) so the
// caller can confirm how close the retopo landed to the requested budget in one
// call — no follow-up get_mesh_info round-trip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryRemeshUniformEchoesMeshCountsTest,
    "PinWright.geometry.remesh_uniform.EchoMeshCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryRemeshUniformEchoesMeshCountsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping remesh_uniform count-echo test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_RemeshEchoProbe_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Spawn a real DynamicMeshActor (a box has a known non-zero tri/vert count)
    // whose label is the actorName remesh_uniform will resolve. In-code fixture:
    // no external content asset is loaded.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-remesh-echo-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            DestroyActorsWithLabel(Label);
            return false;
        }
    }

    // Request a specific triangle budget so the echoed input is unambiguous and
    // distinguishable from the achieved count.
    const int32 RequestedTarget = 2000;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetNumberField(TEXT("targetTriangleCount"), RequestedTarget);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.remesh_uniform"),
        TEXT("req-remesh-echo"), Params, bSuccess, Result, ErrorCode);

    if (!TestTrue(TEXT("geometry.remesh_uniform succeeded"), bSuccess) ||
        !TestTrue(TEXT("geometry.remesh_uniform carries a result object"), Result.IsValid()))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    // The requested-budget echo is still present (backward-compat).
    double EchoedTarget = 0.0;
    TestTrue(TEXT("response echoes the requested targetTriangleCount"),
        Result->TryGetNumberField(TEXT("targetTriangleCount"), EchoedTarget));
    TestEqual(TEXT("echoed targetTriangleCount equals the requested value"),
        (int32)EchoedTarget, RequestedTarget);

    // The fix: the ACHIEVED post-op counts are echoed inline off the remeshed mesh.
    double VertexCount = 0.0;
    TestTrue(TEXT("response echoes the achieved vertexCount"),
        Result->TryGetNumberField(TEXT("vertexCount"), VertexCount));
    TestTrue(TEXT("achieved vertexCount is non-zero"), VertexCount > 0.0);

    double TriangleCount = 0.0;
    TestTrue(TEXT("response echoes the achieved triangleCount (distinct from targetTriangleCount)"),
        Result->TryGetNumberField(TEXT("triangleCount"), TriangleCount));
    TestTrue(TEXT("achieved triangleCount is non-zero"), TriangleCount > 0.0);

    // The counts must be MEASURED off the just-remeshed mesh, not carried from a pre-op
    // snapshot or a constant. `> 0` cannot tell those apart: the source box's counts are
    // non-zero too, so an echo that reverted to the BEFORE counts (or to a hardcoded value)
    // would satisfy every assertion above while answering "did the retopo hit my budget?"
    // with the input state — the exact confusion this ticket exists to remove.
    //
    // geometry.get_mesh_info is an independent reader: MeshInfoHandler.cpp queries the live
    // Target.Mesh directly rather than reusing the FOpResult the remesh handler echoes, so a
    // regression that moved the count capture ahead of GeometryOps::RemeshUniform diverges
    // here even though both numbers stay positive.
    {
        TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
        InfoParams->SetStringField(TEXT("actorName"), Label);
        bool bInfoSuccess = false;
        FString InfoErrorCode;
        TSharedPtr<FJsonObject> InfoResult;
        Dispatch(Dispatcher, Sink, TEXT("geometry.get_mesh_info"),
            TEXT("req-remesh-echo-info"), InfoParams, bInfoSuccess, InfoResult, InfoErrorCode);

        if (TestTrue(TEXT("geometry.get_mesh_info succeeded on the remeshed actor"), bInfoSuccess) &&
            TestTrue(TEXT("geometry.get_mesh_info carries a result object"), InfoResult.IsValid()))
        {
            double LiveVertexCount = 0.0, LiveTriangleCount = 0.0;
            InfoResult->TryGetNumberField(TEXT("vertexCount"), LiveVertexCount);
            InfoResult->TryGetNumberField(TEXT("triangleCount"), LiveTriangleCount);
            TestEqual(TEXT("echoed vertexCount equals the live post-remesh mesh's vertex count"),
                (int32)VertexCount, (int32)LiveVertexCount);
            TestEqual(TEXT("echoed triangleCount equals the live post-remesh mesh's triangle count"),
                (int32)TriangleCount, (int32)LiveTriangleCount);
        }
    }

    DestroyActorsWithLabel(Label);
    return true;
}
