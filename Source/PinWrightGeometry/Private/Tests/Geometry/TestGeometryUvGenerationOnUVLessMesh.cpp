// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red / regression test for B-geometry-uv-gen-silent-noop.
//
// The geometry UV-generation family writes into UVChannel WITHOUT first ensuring
// that UV layer exists:
//   * geometry.project_uv (box/planar/cylindrical) — MeshOpsHandler.cpp:1682 calls
//     SetMeshUVsFromBoxProjection with no SetNumUVSets/EnsureMeshHasUVs beforehand.
//   * geometry.unwrap_uv / auto_uv / pack_uv_islands — the shared
//     GeometryUtils::ApplyXAtlasUnwrap (GeometryUtils.cpp:159) calls
//     AutoGenerateXAtlasMeshUVs as its first statement, likewise with no layer-ensure.
//
// On a DynamicMesh whose UV overlay for that channel is absent — the normal state of a
// mesh hand-authored via geometry.append_buffers with no `uvs` array — the underlying
// GeometryScript call finds no layer at UVChannel, writes its complaint to the discarded
// nullptr debug object, and returns having done nothing. The handler never inspects the
// result and unconditionally reports success ("UV projection applied" /
// "UV unwrapping completed"). The mesh gains ZERO UV sets, so geometry.get_mesh_info
// still reports hasUVs:false (hasUVs is GetNumUVSets(Mesh) > 0) — a silent false success.
//
// The known-correct helper GeometryUtils::EnsureMeshHasUVs (GeometryUtils.cpp:238,
// SetNumUVSets) proves the maintainers already know the layer must pre-exist; neither
// UV-generation path invokes it.
//
// Strategy (mirrors the ticket's verbatim replay — one UV-less pyramid per verb so the
// two guilty call sites are exercised independently):
//   1. geometry.create_procedural_mesh {name}      -> empty DynamicMeshActor.
//   2. geometry.append_buffers {vertices,triangles} with NO `uvs` -> a UV-less mesh.
//   3. assert get_mesh_info reports hasUVs:false (the mesh genuinely starts UV-less).
//   4. run the UV-generation verb; assert it reports success (it always has).
//   5. assert get_mesh_info now reports hasUVs:true — the honest post-condition of any
//      UV-generation op. PRE-FIX this is FALSE (the write silently no-oped), so the
//      final assertion FAILS -> the test reproduces the defect.
//   6. assert the op populated real UV ELEMENTS, not just an empty layer: a follow-up
//      geometry.set_uvs {vertexIndex:0} must SUCCEED. hasUVs is only GetNumUVSets(Mesh) > 0
//      (a layer-count flag), so a bare SetNumUVSets would flip it true while leaving zero
//      elements — the ticket's true defect, whose honest downstream symptom is
//      set_uvs -> [NO_UV_ELEMENTS]. This element-level check is strictly stronger.
//
// Also asserts the fix's rejection guard: an out-of-range uvChannel (>= 8; the engine's
// SetNumUVSets caps at 8 sets / channels 0-7) must be REJECTED (success:false) at both the
// project_uv and shared ApplyXAtlasUnwrap sites — instead of the ticket's silent no-op that
// still reported success — and a rejected projectionType must leave the mesh untouched.
//
// Counterfactual / green side: once each call site ensures the target UV channel before
// writing, GetNumUVSets(Mesh) > 0, get_mesh_info's hasUVs flips true, and the real
// projection/XAtlas edit runs and populates elements, so the assertions pass. The
// differential holds by construction and covers BOTH call sites, so a fix that touches only
// project_uv (leaving ApplyXAtlasUnwrap broken) still fails.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

#include "Tests/TestUtils.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"

// File-local JSON builders. Uniquely named (UvGen* prefix) so a Unity build merging this
// file's anonymous namespace with a sibling geometry test's (e.g. TestGeometryConvertNoUVMesh's
// NoUV* helpers) cannot ODR-collide.
namespace
{
    TSharedPtr<FJsonValue> UvGenVec3(double X, double Y, double Z)
    {
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(X));
        A.Add(MakeShared<FJsonValueNumber>(Y));
        A.Add(MakeShared<FJsonValueNumber>(Z));
        return MakeShared<FJsonValueArray>(A);
    }

    TSharedPtr<FJsonValue> UvGenTri(int32 I0, int32 I1, int32 I2)
    {
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(I0));
        A.Add(MakeShared<FJsonValueNumber>(I1));
        A.Add(MakeShared<FJsonValueNumber>(I2));
        return MakeShared<FJsonValueArray>(A);
    }

    // The ticket's exact repro pyramid: a square base + apex, 5 vertices / 6 triangles,
    // supplied with NO `uvs` array so the mesh's UV overlay stays absent.
    TArray<TSharedPtr<FJsonValue>> UvGenPyramidVertices()
    {
        TArray<TSharedPtr<FJsonValue>> V;
        V.Add(UvGenVec3(-50.0, -50.0, 0.0));
        V.Add(UvGenVec3(50.0, -50.0, 0.0));
        V.Add(UvGenVec3(50.0, 50.0, 0.0));
        V.Add(UvGenVec3(-50.0, 50.0, 0.0));
        V.Add(UvGenVec3(0.0, 0.0, 100.0));
        return V;
    }

    TArray<TSharedPtr<FJsonValue>> UvGenPyramidTriangles()
    {
        TArray<TSharedPtr<FJsonValue>> T;
        T.Add(UvGenTri(0, 2, 1));
        T.Add(UvGenTri(0, 3, 2));
        T.Add(UvGenTri(0, 1, 4));
        T.Add(UvGenTri(1, 2, 4));
        T.Add(UvGenTri(2, 3, 4));
        T.Add(UvGenTri(3, 0, 4));
        return T;
    }
}

// A UV-generation verb run on a mesh with no UV layer must actually create that layer —
// get_mesh_info's hasUVs must flip true — instead of silently no-oping while reporting success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryUVGenerationCreatesUVLayerTest,
    "PinWright.geometry.uv_generation.CreatesUVLayerOnUVLessMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryUVGenerationCreatesUVLayerTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        AddError(TEXT("No editor world available; cannot exercise geometry UV-generation handlers"));
        return false;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    // Build a UV-less pyramid on a fresh DynamicMeshActor, run one UV-generation verb, and
    // return whether get_mesh_info reports hasUVs afterward. bOutRan reports whether the
    // pipeline reached the readback so a setup failure (already recorded via TestTrue below)
    // is distinguishable from a genuine hasUVs:false observation. bOutHasElements reports the
    // strictly-stronger element-level post-condition — whether a follow-up set_uvs on vertex
    // 0 succeeded (i.e. the op produced real UV elements, not just an empty layer).
    auto RunUVGenReadHasUVs =
        [this](const FString& Label, const FString& Method,
               const TSharedPtr<FJsonObject>& VerbParams, bool& bOutRan, bool& bOutHasElements) -> bool
    {
        bOutRan = false;
        bOutHasElements = false;
        VerbParams->SetStringField(TEXT("actorName"), Label);

        // 1. Empty DynamicMeshActor.
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("name"), Label);
            FTestResponseCapture Cap;
            const bool bFound = InvokeHandlerWithCapture(TEXT("geometry.create_procedural_mesh"), P, Cap);
            if (!TestTrue(TEXT("geometry.create_procedural_mesh registered"), bFound) ||
                !TestTrue(TEXT("procedural mesh seed created"), Cap.bSuccess))
            {
                return false;
            }
        }

        // 2. Append the UV-less pyramid (no `uvs` array) — the hand-authoring path that
        //    leaves the UV overlay absent.
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("actorName"), Label);
            P->SetArrayField(TEXT("vertices"), UvGenPyramidVertices());
            P->SetArrayField(TEXT("triangles"), UvGenPyramidTriangles());
            FTestResponseCapture Cap;
            const bool bFound = InvokeHandlerWithCapture(TEXT("geometry.append_buffers"), P, Cap);
            if (!TestTrue(TEXT("geometry.append_buffers registered"), bFound) ||
                !TestTrue(TEXT("UV-less pyramid appended"), Cap.bSuccess))
            {
                return false;
            }
        }

        // 3. Precondition: the mesh genuinely starts UV-less (else the post-op check would
        //    pass vacuously). A UV-less append_buffers mesh reports hasUVs:false.
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("actorName"), Label);
            FTestResponseCapture Cap;
            InvokeHandlerWithCapture(TEXT("geometry.get_mesh_info"), P, Cap);
            bool bPreHasUVs = true;
            if (Cap.bSuccess && Cap.Result.IsValid())
            {
                Cap.Result->TryGetBoolField(TEXT("hasUVs"), bPreHasUVs);
            }
            if (!TestFalse(
                    *FString::Printf(TEXT("%s: mesh has no UVs before the UV-generation verb"), *Method),
                    bPreHasUVs))
            {
                return false;
            }
        }

        // 4. Run the UV-generation verb. It has always reported success — the defect is
        //    that on a mesh with no UV layer it silently does nothing.
        {
            FTestResponseCapture Cap;
            const bool bFound = InvokeHandlerWithCapture(Method, VerbParams, Cap);
            if (!TestTrue(*FString::Printf(TEXT("%s registered"), *Method), bFound) ||
                !TestTrue(*FString::Printf(TEXT("%s reports success"), *Method), Cap.bSuccess))
            {
                return false;
            }
        }

        // 5. Read back hasUVs — the honest post-condition of any UV-generation op.
        TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
        InfoParams->SetStringField(TEXT("actorName"), Label);
        FTestResponseCapture InfoCap;
        InvokeHandlerWithCapture(TEXT("geometry.get_mesh_info"), InfoParams, InfoCap);
        if (!TestTrue(*FString::Printf(TEXT("%s: get_mesh_info succeeded"), *Method), InfoCap.bSuccess) ||
            !TestTrue(*FString::Printf(TEXT("%s: get_mesh_info returned a result"), *Method), InfoCap.Result.IsValid()))
        {
            return false;
        }
        bool bHasUVs = false;
        InfoCap.Result->TryGetBoolField(TEXT("hasUVs"), bHasUVs);

        // 6. Element-level post-condition: hasUVs is only GetNumUVSets(Mesh) > 0 (a layer
        //    presence flag), so it flips true even for an element-less layer. The ticket's
        //    real defect is ZERO UV elements, whose honest downstream symptom is
        //    set_uvs -> [NO_UV_ELEMENTS]. Probe a real element write on vertex 0: it succeeds
        //    only when the projection/XAtlas edit actually produced UV elements.
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("actorName"), Label);
            P->SetNumberField(TEXT("vertexIndex"), 0);
            P->SetNumberField(TEXT("u"), 0.5);
            P->SetNumberField(TEXT("v"), 0.5);
            FTestResponseCapture Cap;
            InvokeHandlerWithCapture(TEXT("geometry.set_uvs"), P, Cap);
            bOutHasElements = Cap.bSuccess;
        }

        bOutRan = true;
        return bHasUVs;
    };

    // --- project_uv (box): the MeshOpsHandler.cpp call site ---
    {
        const FString Label = FString::Printf(TEXT("PW_UvGenProjBox_%s"), *Suffix);
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("projectionType"), TEXT("box"));
        P->SetNumberField(TEXT("scale"), 1.0);
        bool bRan = false;
        bool bHasElements = false;
        const bool bHasUVs = RunUVGenReadHasUVs(Label, TEXT("geometry.project_uv"), P, bRan, bHasElements);
        if (bRan)
        {
            TestTrue(TEXT("geometry.project_uv must create a UV layer (hasUVs true) on a UV-less mesh, not silently no-op while reporting success"),
                bHasUVs);
            TestTrue(TEXT("geometry.project_uv must populate REAL UV elements (a follow-up set_uvs on vertex 0 must find elements, not [NO_UV_ELEMENTS]) — not merely create an empty UV layer"),
                bHasElements);
        }
        GeometryTestHelpers::DestroyActorsWithLabel(Label);
    }

    // --- unwrap_uv (XAtlas): the shared GeometryUtils::ApplyXAtlasUnwrap call site
    //     (auto_uv and pack_uv_islands route through the same routine, so this covers them too) ---
    {
        const FString Label = FString::Printf(TEXT("PW_UvGenUnwrap_%s"), *Suffix);
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetNumberField(TEXT("uvChannel"), 0);
        bool bRan = false;
        bool bHasElements = false;
        const bool bHasUVs = RunUVGenReadHasUVs(Label, TEXT("geometry.unwrap_uv"), P, bRan, bHasElements);
        if (bRan)
        {
            TestTrue(TEXT("geometry.unwrap_uv must create a UV layer (hasUVs true) on a UV-less mesh, not silently no-op while reporting success"),
                bHasUVs);
            TestTrue(TEXT("geometry.unwrap_uv must populate REAL UV elements (a follow-up set_uvs on vertex 0 must find elements, not [NO_UV_ELEMENTS]) — not merely create an empty UV layer"),
                bHasElements);
        }
        GeometryTestHelpers::DestroyActorsWithLabel(Label);
    }

    // --- Rejection guards: the fix surfaces the previously-silent no-op as an error. An
    //     out-of-range uvChannel (>= 8; the engine's SetNumUVSets caps at 8 sets, channels
    //     0-7) cannot create a layer, so both the project_uv and shared ApplyXAtlasUnwrap
    //     sites must REJECT (success:false) instead of the ticket's silent success. A
    //     rejected projectionType must also leave the mesh untouched (no empty UV layer). ---
    {
        const FString Label = FString::Printf(TEXT("PW_UvGenReject_%s"), *Suffix);

        // Seed a fresh UV-less pyramid.
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("name"), Label);
            FTestResponseCapture Cap;
            InvokeHandlerWithCapture(TEXT("geometry.create_procedural_mesh"), P, Cap);
        }
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("actorName"), Label);
            P->SetArrayField(TEXT("vertices"), UvGenPyramidVertices());
            P->SetArrayField(TEXT("triangles"), UvGenPyramidTriangles());
            FTestResponseCapture Cap;
            InvokeHandlerWithCapture(TEXT("geometry.append_buffers"), P, Cap);
        }

        // project_uv with an out-of-range channel must be rejected, not silently succeed.
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("actorName"), Label);
            P->SetStringField(TEXT("projectionType"), TEXT("box"));
            P->SetNumberField(TEXT("uvChannel"), 8);
            FTestResponseCapture Cap;
            InvokeHandlerWithCapture(TEXT("geometry.project_uv"), P, Cap);
            TestFalse(TEXT("geometry.project_uv must REJECT an out-of-range uvChannel (8), not report the silent no-op as success"),
                Cap.bSuccess);
        }

        // unwrap_uv (shared ApplyXAtlasUnwrap) with an out-of-range channel must be rejected too.
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("actorName"), Label);
            P->SetNumberField(TEXT("uvChannel"), 8);
            FTestResponseCapture Cap;
            InvokeHandlerWithCapture(TEXT("geometry.unwrap_uv"), P, Cap);
            TestFalse(TEXT("geometry.unwrap_uv must REJECT an out-of-range uvChannel (8), not report the silent no-op as success"),
                Cap.bSuccess);
        }

        // A rejected projectionType must not mutate the mesh: it must error AND leave the
        // mesh UV-less (no stray empty UV layer added before the validation).
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("actorName"), Label);
            P->SetStringField(TEXT("projectionType"), TEXT("banana"));
            FTestResponseCapture Cap;
            InvokeHandlerWithCapture(TEXT("geometry.project_uv"), P, Cap);
            TestFalse(TEXT("geometry.project_uv must reject an unknown projectionType"), Cap.bSuccess);

            TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
            InfoParams->SetStringField(TEXT("actorName"), Label);
            FTestResponseCapture InfoCap;
            InvokeHandlerWithCapture(TEXT("geometry.get_mesh_info"), InfoParams, InfoCap);
            bool bHasUVs = true;
            if (InfoCap.bSuccess && InfoCap.Result.IsValid())
            {
                InfoCap.Result->TryGetBoolField(TEXT("hasUVs"), bHasUVs);
            }
            TestFalse(TEXT("a rejected projectionType must leave the mesh untouched (no stray empty UV layer, so hasUVs stays false)"),
                bHasUVs);
        }

        GeometryTestHelpers::DestroyActorsWithLabel(Label);
    }

    return true;
}
