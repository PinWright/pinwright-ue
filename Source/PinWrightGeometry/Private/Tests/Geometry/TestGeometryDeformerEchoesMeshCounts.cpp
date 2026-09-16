// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-geometry-deformer-echo-mesh-counts.
//
// The geometry deformers (twist/taper/bend/noise_deform/smooth/relax/stretch/
// spherify/cylindrify/bevel/shell) and the array verbs (array_linear/
// array_radial) used to return only {actorName}(+input echo) and omit the post-op
// vertexCount/triangleCount — the single most common "did it collapse?" signal —
// even though both are trivially available on the Target.Mesh the handler already
// holds. geometry.poke was a *partial* echoer: it returned triangleCount but not
// vertexCount. That forced a separate geometry.get_mesh_info readback after every
// deform just to confirm non-collapse.
//
// The fix echoes vertexCount/triangleCount inline on each deformer's success
// response (via the in-file SnapshotMeshCounts helper / SetMeshCountFields in
// MeshOpsHandler.cpp, and a direct GetVertexCount/GetTriangleCount echo on the
// array verbs in GeometryTransformHandler.cpp), matching the keys get_mesh_info
// uses — mirroring how subdivide/simplify/extrude/inset already carry their
// post-op topology.
//
// Strategy: spawn a real DynamicMeshActor via geometry.create_box, route the
// deformer/array/poke verbs through the real dispatcher, and assert each success
// result carries a non-zero vertexCount AND triangleCount. This exercises the
// production handlers end-to-end. Counterfactual: reverting the fix (success
// result back to {actorName}(+input) only) drops vertexCount/triangleCount and
// fails this test.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // Remove any actor whose label matches Label from the editor world so the
    // test leaves no residue on the (mutated) fuzzing host.
    void DestroyDeformerProbeActors(const FString& Label)
    {
        if (!GEditor)
        {
            return;
        }
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!IsValid(World))
        {
            return;
        }
        TArray<AActor*> ToDestroy;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(Label))
            {
                ToDestroy.Add(*It);
            }
        }
        for (AActor* Actor : ToDestroy)
        {
            World->DestroyActor(Actor);
        }
    }
}

// Each deformer/array/poke success response carries the post-op vertexCount and
// triangleCount inline so the caller can confirm non-collapse in one call (no
// follow-up get_mesh_info round-trip).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryDeformerEchoesMeshCountsTest,
    "PinWright.geometry.deformers.EchoMeshCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryDeformerEchoesMeshCountsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping deformer count-echo test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_DeformEchoProbe_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Spawn a real DynamicMeshActor (a box has a known non-zero tri/vert count)
    // whose label is the actorName every deform below will resolve. The box has
    // 2 polygroup faces per side, so polygroup-bevel has edges to work on.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-deform-echo-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            DestroyDeformerProbeActors(Label);
            return true;
        }
    }

    // Dispatch one verb and assert its success result carries non-zero
    // vertexCount AND triangleCount. ExtraParams lets a verb pass its own args.
    auto AssertVerbEchoesCounts =
        [this, &Dispatcher, &Sink, &Label]
        (const FString& Method, const TFunction<void(TSharedPtr<FJsonObject>&)>& FillParams)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        if (FillParams)
        {
            FillParams(Params);
        }

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, Method,
            FString::Printf(TEXT("req-deform-echo-%s"), *Method), Params,
            bSuccess, Result, ErrorCode);

        const FString Ctx = FString::Printf(TEXT("%s "), *Method);
        if (!TestTrue(Ctx + TEXT("succeeded"), bSuccess) ||
            !TestTrue(Ctx + TEXT("carries a result object"), Result.IsValid()))
        {
            return;
        }

        double VertexCount = 0.0;
        TestTrue(Ctx + TEXT("echoes a vertexCount field"),
            Result->TryGetNumberField(TEXT("vertexCount"), VertexCount));
        TestTrue(Ctx + TEXT("vertexCount is non-zero"), VertexCount > 0.0);

        double TriangleCount = 0.0;
        TestTrue(Ctx + TEXT("echoes a triangleCount field"),
            Result->TryGetNumberField(TEXT("triangleCount"), TriangleCount));
        TestTrue(Ctx + TEXT("triangleCount is non-zero"), TriangleCount > 0.0);

        // The echo has to be MEASURED off the post-op mesh, not carried from the before-counts
        // the same FOpResult already holds. Non-zero alone cannot tell those apart, and the
        // difference is the whole point of the field: the caller reads it to answer "did this
        // deform collapse the mesh?", and a stale pre-op echo answers "no" on a mesh that
        // collapsed to zero. `> 0` would still pass in exactly that case.
        //
        // geometry.get_mesh_info is an independent reader — MeshInfoHandler.cpp queries the
        // live Target.Mesh rather than reusing the op result these verbs echo — so an echo
        // sourced from Op.VerticesBefore/TrianglesBefore, or from a constant, diverges here.
        {
            TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
            InfoParams->SetStringField(TEXT("actorName"), Label);
            bool bInfoSuccess = false;
            FString InfoErrorCode;
            TSharedPtr<FJsonObject> InfoResult;
            Dispatch(Dispatcher, Sink, TEXT("geometry.get_mesh_info"),
                FString::Printf(TEXT("req-deform-echo-info-%s"), *Method), InfoParams,
                bInfoSuccess, InfoResult, InfoErrorCode);

            if (TestTrue(Ctx + TEXT("get_mesh_info readback succeeded"), bInfoSuccess) &&
                TestTrue(Ctx + TEXT("get_mesh_info readback carries a result"), InfoResult.IsValid()))
            {
                double LiveVertexCount = 0.0, LiveTriangleCount = 0.0;
                InfoResult->TryGetNumberField(TEXT("vertexCount"), LiveVertexCount);
                InfoResult->TryGetNumberField(TEXT("triangleCount"), LiveTriangleCount);
                TestEqual(Ctx + TEXT("echoed vertexCount is the POST-op count on the live mesh"),
                    (int32)VertexCount, (int32)LiveVertexCount);
                TestEqual(Ctx + TEXT("echoed triangleCount is the POST-op count on the live mesh"),
                    (int32)TriangleCount, (int32)LiveTriangleCount);
            }
        }
    };

    // A representative spread across the affected handlers:
    //  - MeshOpsHandler pure deformers (no topology change in the common case)
    AssertVerbEchoesCounts(TEXT("geometry.twist"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("angle"), 30.0); });
    AssertVerbEchoesCounts(TEXT("geometry.taper"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("flareX"), 60.0); });
    AssertVerbEchoesCounts(TEXT("geometry.bend"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("angle"), 15.0); });
    AssertVerbEchoesCounts(TEXT("geometry.smooth"), nullptr);
    AssertVerbEchoesCounts(TEXT("geometry.relax"), nullptr);
    AssertVerbEchoesCounts(TEXT("geometry.stretch"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("factor"), 1.25); });
    AssertVerbEchoesCounts(TEXT("geometry.noise_deform"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("magnitude"), 2.0); });
    AssertVerbEchoesCounts(TEXT("geometry.spherify"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("factor"), 0.5); });
    //  - topology-changing MeshOps modeling verbs
    AssertVerbEchoesCounts(TEXT("geometry.bevel"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("distance"), 2.0); });
    AssertVerbEchoesCounts(TEXT("geometry.shell"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("thickness"), 2.0); });
    //  - poke: previously echoed triangleCount but NOT vertexCount (the #4 nuance)
    AssertVerbEchoesCounts(TEXT("geometry.poke"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("offset"), 1.0); });
    //  - GeometryTransformHandler array verbs (count multiplies geometry in place)
    AssertVerbEchoesCounts(TEXT("geometry.array_linear"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("count"), 3); });
    AssertVerbEchoesCounts(TEXT("geometry.array_radial"),
        [](TSharedPtr<FJsonObject>& P){ P->SetNumberField(TEXT("count"), 4); });

    DestroyDeformerProbeActors(Label);
    return true;
}
