// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the geometry.convert_to_static_mesh UV-less crash guard.
//
// Building a DynamicMesh that has triangles but NO UVs (authored via
// geometry.append_buffers / append_vertex without uvs) and then baking it through
// geometry.convert_to_static_mesh used to HARD-CRASH the editor on the async
// static-mesh build worker:
//
//   Assertion failed: (Index >= 0) & (Index < ArrayNum) [Array.h]
//   FStaticMeshOperations::ComputeMikktTangents() [StaticMeshOperations.cpp]
//     <- ComputeTangentsAndNormals <- SetupRenderMeshDescription
//     <- FStaticMeshBuilder::Build <- CacheDerivedData   (Background Worker)
//
// Root cause: MikkT tangent generation REQUIRES a UV channel; with no UVs the baked
// UV array is size 0 and MikkT indexes [0] into it -> out-of-bounds -> crash. The
// StaticMesh render build runs MikkT whenever bRecomputeTangents OR bRecomputeNormals
// is set (both invoke it), so merely disabling the geometry-script tangent option
// does NOT prevent the crash — normal recompute alone still triggers MikkT. The prior
// convert tests only baked create_box primitives (which carry UVs), so this UV-less
// path was an untested latent crash.
//
// The fix ensures UVs BEFORE the bake: GeometryUtils::EnsureMeshHasUVs applies a
// deterministic box projection (SetNumUVSets + SetMeshUVsFromBoxProjection, framed on
// the mesh bounding box — a pure per-triangle projection, never XAtlas auto-unwrap
// which can hang on degenerate meshes) when the mesh has no usable UV layer. Once UVs
// exist, MikkT has data and the bake succeeds + saves a usable, texturable StaticMesh.
//
// Strategy: create an empty procedural DynamicMeshActor, append a tetrahedron with
// NO uvs, then bake it. Pre-fix this crashes the whole suite on the build worker;
// post-fix convert returns success (Capture.bSuccess) and the .uasset saves. The
// direct proof the fix engaged: the source mesh, UV-less before convert, reports
// hasUVs:true afterward (geometry.get_mesh_info) because the projection ran.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectGlobals.h"

using GeometryTestHelpers::DestroyActorsWithLabel;

// File-local JSON builders. Uniquely named (NoUV* prefix) so a Unity build merging
// this file's anonymous namespace with a sibling test's (e.g. TestBulkEditHandler's
// BulkBuf* helpers) cannot ODR-collide.
namespace
{
    TSharedPtr<FJsonValue> NoUVVec3(double X, double Y, double Z)
    {
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(X));
        A.Add(MakeShared<FJsonValueNumber>(Y));
        A.Add(MakeShared<FJsonValueNumber>(Z));
        return MakeShared<FJsonValueArray>(A);
    }

    TSharedPtr<FJsonValue> NoUVTri(int32 I0, int32 I1, int32 I2)
    {
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(I0));
        A.Add(MakeShared<FJsonValueNumber>(I1));
        A.Add(MakeShared<FJsonValueNumber>(I2));
        return MakeShared<FJsonValueArray>(A);
    }

    // Four corners of a tetrahedron (mesh-local space) — the exact UV-less repro
    // input, matching the crash reproduction: no `uvs` array is ever supplied.
    TArray<TSharedPtr<FJsonValue>> NoUVTetraVertices()
    {
        TArray<TSharedPtr<FJsonValue>> V;
        V.Add(NoUVVec3(0.0, 0.0, 0.0));
        V.Add(NoUVVec3(100.0, 0.0, 0.0));
        V.Add(NoUVVec3(50.0, 100.0, 0.0));
        V.Add(NoUVVec3(50.0, 50.0, 100.0));
        return V;
    }

    // The four outward-wound faces of the tetrahedron above (a closed manifold).
    TArray<TSharedPtr<FJsonValue>> NoUVTetraTriangles()
    {
        TArray<TSharedPtr<FJsonValue>> T;
        T.Add(NoUVTri(0, 1, 2));
        T.Add(NoUVTri(0, 3, 1));
        T.Add(NoUVTri(0, 2, 3));
        T.Add(NoUVTri(1, 3, 2));
        return T;
    }
}

// convert_to_static_mesh must NOT crash the editor when the source DynamicMesh has
// triangles but no UVs — it must bake and save a usable StaticMesh with tangent
// recompute skipped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertNoUVMeshDoesNotCrashTest,
    "PinWright.geometry.convert_to_static_mesh.NoUVMeshDoesNotCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertNoUVMeshDoesNotCrashTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping convert_to_static_mesh no-UV crash test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_NoUVTetra_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/PinWrightTests/SM_NoUVTetra_%s"), *Suffix);

    // 1. Create an empty procedural DynamicMeshActor to build onto.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("geometry.create_procedural_mesh"), CreateParams, Capture);
        if (!TestTrue(TEXT("geometry.create_procedural_mesh registered"), bFound) ||
            !TestTrue(TEXT("procedural mesh seed created"), Capture.bSuccess))
        {
            DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // 2. Append a tetrahedron with NO uvs (the crash-triggering state: triangles
    //    but an empty/absent UV overlay).
    {
        TSharedPtr<FJsonObject> AppendParams = MakeShared<FJsonObject>();
        AppendParams->SetStringField(TEXT("actorName"), Label);
        AppendParams->SetArrayField(TEXT("vertices"), NoUVTetraVertices());
        AppendParams->SetArrayField(TEXT("triangles"), NoUVTetraTriangles());

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("geometry.append_buffers"), AppendParams, Capture);
        if (!TestTrue(TEXT("geometry.append_buffers registered"), bFound) ||
            !TestTrue(TEXT("UV-less tetrahedron appended"), Capture.bSuccess))
        {
            DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // 3. THE HEART OF THE TEST: bake the UV-less mesh. Pre-fix, the async static-mesh
    //    build worker's ComputeMikktTangents indexes [0] into the size-0 UV array and
    //    hard-crashes the whole suite here. Post-fix, the handler box-projects UVs onto
    //    the mesh before baking so MikkT has data — the bake succeeds and saves.
    TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
    ConvertParams->SetStringField(TEXT("actorName"), Label);
    ConvertParams->SetStringField(TEXT("assetPath"), AssetPath);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("geometry.convert_to_static_mesh"), ConvertParams, Capture);
    TestTrue(TEXT("geometry.convert_to_static_mesh registered"), bFound);
    TestTrue(TEXT("convert of a UV-less mesh succeeds instead of crashing"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // The bake produced a usable, on-disk StaticMesh asset.
        bool bCreated = false;
        TestTrue(TEXT("convert reports created:true for the UV-less bake"),
            Capture.Result->TryGetBoolField(TEXT("created"), bCreated) && bCreated);

        const UObject* Baked = StaticFindObject(
            UStaticMesh::StaticClass(), nullptr, *ToObjectPath(AssetPath));
        TestNotNull(TEXT("the baked StaticMesh object exists after convert"), Baked);
    }

    // Direct proof the fix engaged: the source mesh was UV-less before convert, and
    // EnsureMeshHasUVs box-projected UV0 onto it, so get_mesh_info now reports UVs.
    // (Pre-fix, no projection ran — the run crashed before reaching here at all.)
    //
    // The readback itself is asserted, not merely branched on: this hasUVs check is the
    // ONLY assertion in the file that the projection actually RAN (everything above only
    // proves the bake did not crash). Guarding it behind a bare `if (InfoCapture.bSuccess)`
    // made a failed/unregistered get_mesh_info silently delete the whole proof and the test
    // still pass green.
    {
        TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
        InfoParams->SetStringField(TEXT("actorName"), Label);
        FTestResponseCapture InfoCapture;
        TestTrue(TEXT("geometry.get_mesh_info registered"),
            InvokeHandlerWithCapture(TEXT("geometry.get_mesh_info"), InfoParams, InfoCapture));
        TestTrue(TEXT("geometry.get_mesh_info succeeded on the converted source mesh"),
            InfoCapture.bSuccess);
        TestTrue(TEXT("geometry.get_mesh_info returned a result"), InfoCapture.Result.IsValid());
        bool bHasUVs = false;
        TestTrue(TEXT("source mesh gained UVs from the pre-bake projection"),
            InfoCapture.bSuccess && InfoCapture.Result.IsValid()
            && InfoCapture.Result->TryGetBoolField(TEXT("hasUVs"), bHasUVs) && bHasUVs);
    }

    // Cleanup: drop the baked asset (registry + disk) and the source actor so the
    // disposable host is left clean.
    CleanupTestAsset(AssetPath);
    DestroyActorsWithLabel(Label);
    return true;
}
