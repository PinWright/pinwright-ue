// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the SECOND wave of discarded engine failures, after the boolean family.
//
// Same defect, different verbs: a GeometryScript entry point reports failure ONLY through its
// `UGeometryScriptDebug*` argument and returns its TargetMesh on every path, so a call site
// passing nullptr there cannot tell a refusal from a success. TestGeometryBooleanFailureDetection
// covers the boolean family; this file covers the sites wired after it.
//
// EVERY test here drives a failure inside the ENGINE, never one of PinWright's own guards. That
// distinction is the whole reason the defect survived: the pre-existing "failure" tests for these
// verbs asserted error codes on null handles, which the ops reject one line before the engine is
// reached, so they were green throughout and would have stayed green through any regression.
// Revert the sink wiring at any site below and its test fails, because each asserts a FAILURE the
// nullptr-Debug version reports as a success.
//
// WHAT IS COVERED, and by which engine refusal:
//
//   geometry.import_obj      AppendBuffersToMesh rejects a triangle and CONTINUES, so a malformed
//                            OBJ used to produce an actor with silently missing faces under a
//                            success response. Both engine rejections a real .obj can reach are
//                            driven here - duplicate face and non-manifold edge - end to end over
//                            the dispatcher, which is the only layer that proves the caller sees
//                            it.
//   GeometryOps::AppendBuffers   the same engine call from geometry.append_buffers, where the
//                            target is the caller's existing mesh rather than a fresh one. Pins
//                            the rollback as well as the failure.
//   GeometryOps::RecomputeTangents   ComputeTangents refuses a mesh with no UV layer. Not exotic:
//                            geometry.import_obj produces exactly that mesh on every call.
//   GeometryOps::UnwrapUVXAtlas  XAtlas refuses a NON-COMPACT mesh, which is the normal state of
//                            any mesh this plugin has deleted triangles from. This also covers the
//                            sink and the error code that AutoUVPatchBuilder shares with it.
//
// WHAT IS DELIBERATELY NOT COVERED, stated rather than left as an absence:
//
//   GeometryOps::AutoUVPatchBuilder is WIRED but has no FAILURE test - only a success control.
//     The long note above that control records what was tried and what the engine did (nothing,
//     not even a log line). Read it before assuming the site is untested by oversight.
//
//   ApplyPNTessellation (GeometryOps::Subdivide and GeometryOps::Poke) is WIRED but has NO test,
//     because on UE 5.8 no input reaching those ops can make it fail. Its two error paths are
//     `FPNTriangles::Validate()`, which fails only on `TessellationLevel < 0 || Mesh == nullptr`
//     (PinWright passes a literal 1 and a mesh BeginOp has already guarded), and
//     `FPNTriangles::Compute()`, whose failure returns are a null/negative-level check, a
//     ProgressCancel PinWright does not pass, and a missing-normals check that cannot fire
//     because Compute computes per-vertex normals itself when the mesh has none
//     (PNTriangles.cpp, the bHasNormals branch). A test here would be one that cannot fail,
//     which this codebase has a documented history of shipping; the wiring stands so that an
//     engine which starts reporting is heard.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Advanced.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::MakeDispatcher;
using GeometryTestHelpers::DestroyActorsWithLabel;
using GeometryTestHelpers::FindActorByLabel;

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// A single triangle listed twice. The second AppendTriangle finds all three edges present and
// each still a boundary edge, so it passes the non-manifold test and falls into the duplicate
// test, which finds the existing triangle already carries the third vertex
// (DynamicMesh3_Edits.cpp, AppendTriangle) - DuplicateTriangleID.
const TCHAR* const GeomFailTest_DuplicateFaceObj =
    TEXT("v 0 0 0\n")
    TEXT("v 100 0 0\n")
    TEXT("v 0 100 0\n")
    TEXT("f 1 2 3\n")
    TEXT("f 1 2 3\n");

// Three faces on the shared edge 1-2. The third finds that edge already carrying two triangles,
// so it is not a boundary edge any more - NonManifoldID.
//
// The middle face is wound 2-1-4 on purpose: it must use the SAME undirected edge as the first,
// which is what loads that edge to two triangles before the third arrives.
const TCHAR* const GeomFailTest_NonManifoldObj =
    TEXT("v 0 0 0\n")
    TEXT("v 100 0 0\n")
    TEXT("v 0 100 0\n")
    TEXT("v 0 -100 0\n")
    TEXT("v 0 0 100\n")
    TEXT("f 1 2 3\n")
    TEXT("f 2 1 4\n")
    TEXT("f 1 2 5\n");

// The control: two faces sharing one edge, which is ordinary manifold geometry.
const TCHAR* const GeomFailTest_CleanObj =
    TEXT("v 0 0 0\n")
    TEXT("v 100 0 0\n")
    TEXT("v 0 100 0\n")
    TEXT("v 0 -100 0\n")
    TEXT("f 1 2 3\n")
    TEXT("f 2 1 4\n");

UDynamicMesh* GeomFailTest_NewBox()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// A single triangle, appended through the engine so the mesh ends up in exactly the state
// geometry.import_obj leaves one in: attributes enabled, ZERO UV layers.
UDynamicMesh* GeomFailTest_NewUVLessTriangle()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());

    GeometryOps::FAppendBuffersParams Params;
    Params.Vertices = { FVector(0, 0, 0), FVector(100, 0, 0), FVector(0, 100, 0) };
    Params.Triangles = { FIntVector(0, 1, 2) };

    GeometryOps::FAppendBuffersOutputs Out;
    GeometryOps::AppendBuffers(Mesh, MoveTemp(Params), Out);
    return Mesh;
}

int32 GeomFailTest_UVLayerCount(UDynamicMesh* Mesh)
{
    int32 Count = 0;
    Mesh->ProcessMesh([&Count](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        Count = ReadMesh.HasAttributes() ? ReadMesh.Attributes()->NumUVLayers() : 0;
    });
    return Count;
}

bool GeomFailTest_IsCompact(UDynamicMesh* Mesh)
{
    bool bCompact = false;
    Mesh->ProcessMesh([&bCompact](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        bCompact = ReadMesh.IsCompact();
    });
    return bCompact;
}
}

// ============================================================================
// geometry.import_obj - the engine refuses triangles and the import used to keep them quiet
//
// Driven over the real dispatcher rather than against a helper, because the loss this fixes was
// invisible EXACTLY at the response boundary: the op-level counts were always "correct" for the
// mesh that got built, and only a comparison against what the FILE described exposes the gap.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryImportDuplicateFaceFailsTest,
    "PinWright.geometry.import_obj.DuplicateFaceFailsInsteadOfSilentlyDroppingIt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryImportDuplicateFaceFailsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping import failure-detection test"));
        return true;
    }

    // The engine LOGS every message it appends to a UGeometryScriptDebug, at Error verbosity, and
    // it does so unconditionally - before it has even looked at whether a Debug object was passed
    // (GeometryScriptTypes.cpp, MakeScriptError). A test that deliberately drives an engine
    // refusal therefore emits a genuine LogGeometry Error, and UE's automation framework fails a
    // test on any uncaptured Error line. Declaring it expected is what separates "the engine
    // complained, as this test intended" from "something went wrong".
    //
    // Occurrences 0 means "any number, including none", which is deliberate: whether the
    // framework captures the line at all has proved environment-dependent (the boolean family's
    // two dispatcher tests passed in one suite run and failed in the next on identical code), and
    // a fixed count would then fail in the other direction.
    AddExpectedErrorPlain(TEXT("AppendBuffersToMesh: Triangle cannot be added because it is a duplicate of an existing Triangle"),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    const FString Label = FString::Printf(TEXT("PW_ImportDup_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetStringField(TEXT("text"), GeomFailTest_DuplicateFaceObj);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("geometry.import_obj"), TEXT("req-import-dup"),
        Params, bSuccess, ErrorCode);

    // The assertion the change exists for. On the nullptr-Debug version this is success:true with
    // triangleCount 1 over a file that described 2 faces, and nothing anywhere says so.
    TestFalse(TEXT("an OBJ the engine partly refused is reported as a failure"), bSuccess);
    TestEqual(TEXT("the failure carries IMPORT_FAILED"),
        ErrorCode, FString(ErrorCodes::ERR_IMPORT_FAILED));
    TestTrue(TEXT("the message carries the engine's own diagnosis"),
        Sink->Message.Contains(TEXT("duplicate of an existing Triangle")));
    TestTrue(TEXT("the message counts what was refused"), Sink->Message.Contains(TEXT("1 of 2")));
    TestTrue(TEXT("the message names the escape hatch"),
        Sink->Message.Contains(TEXT("allowPartial")));

    // The side-effect audit, pinned: spawning the actor is this verb's commit and it is sequenced
    // AFTER the failure check, so a refused import leaves nothing behind in the level. Without
    // that ordering the caller would be told the import failed while holding an actor carrying
    // the truncated mesh.
    TestNull(TEXT("no actor is spawned for a failed import"), FindActorByLabel(Label));

    DestroyActorsWithLabel(Label);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryImportNonManifoldFailsTest,
    "PinWright.geometry.import_obj.NonManifoldEdgeFailsInsteadOfSilentlyDroppingTheFace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryImportNonManifoldFailsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping import failure-detection test"));
        return true;
    }

    AddExpectedErrorPlain(TEXT("AppendBuffersToMesh: Triangle cannot be added because it would create invalid Non-Manifold Mesh Topology"),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    const FString Label = FString::Printf(TEXT("PW_ImportNM_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetStringField(TEXT("text"), GeomFailTest_NonManifoldObj);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("geometry.import_obj"), TEXT("req-import-nm"),
        Params, bSuccess, ErrorCode);

    TestFalse(TEXT("a non-manifold OBJ is reported as a failure"), bSuccess);
    TestEqual(TEXT("the failure carries IMPORT_FAILED"),
        ErrorCode, FString(ErrorCodes::ERR_IMPORT_FAILED));
    TestTrue(TEXT("the message carries the engine's own diagnosis"),
        Sink->Message.Contains(TEXT("Non-Manifold")));
    TestTrue(TEXT("the message counts what was refused"), Sink->Message.Contains(TEXT("1 of 3")));
    TestNull(TEXT("no actor is spawned for a failed import"), FindActorByLabel(Label));

    DestroyActorsWithLabel(Label);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryImportAllowPartialTest,
    "PinWright.geometry.import_obj.AllowPartialImportsWhatTheEngineTookAndSaysWhatItDropped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryImportAllowPartialTest::RunTest(const FString& Parameters)
{
    // The control for the two tests above, in the same shape as the boolean family's
    // AllowEmptyResult control: same file, same verb, one option flipped. If those fail and this
    // succeeds, the failure is provably the engine's triangle refusal and not a broken fixture.
    //
    // It also pins the second half of the contract - that the permissive path is not a return to
    // silence. The import succeeds, and the response still states that geometry was dropped.
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping import allowPartial test"));
        return true;
    }

    AddExpectedErrorPlain(TEXT("AppendBuffersToMesh: Triangle cannot be added because it would create invalid Non-Manifold Mesh Topology"),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    const FString Label = FString::Printf(TEXT("PW_ImportPartial_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetStringField(TEXT("text"), GeomFailTest_NonManifoldObj);
    Params->SetBoolField(TEXT("allowPartial"), true);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.import_obj"), TEXT("req-import-partial"),
        Params, bSuccess, Result, ErrorCode);

    if (TestTrue(TEXT("with allowPartial the same file imports"), bSuccess)
        && TestTrue(TEXT("a result object came back"), Result.IsValid()))
    {
        TestEqual(TEXT("the file described 3 triangles"),
            Result->GetIntegerField(TEXT("requestedTriangles")), 3);
        TestEqual(TEXT("the engine accepted 2 of them"),
            Result->GetIntegerField(TEXT("triangleCount")), 2);
        TestEqual(TEXT("and the response says the third was dropped"),
            Result->GetIntegerField(TEXT("droppedTriangles")), 1);

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (TestTrue(TEXT("a warnings array is present"),
                Result->TryGetArrayField(TEXT("warnings"), Warnings)) && Warnings)
        {
            TestEqual(TEXT("carrying one warning"), Warnings->Num(), 1);
            TestTrue(TEXT("which quotes the engine"),
                (*Warnings)[0]->AsString().Contains(TEXT("Non-Manifold")));
        }

        // The vertices of the refused triangle are still in the mesh as isolated points - the
        // engine appends every vertex before it validates a single triangle and never rolls one
        // back. Pinned because it is the reason the echoed vertexCount could not have been used
        // to detect the loss.
        TestEqual(TEXT("all 5 vertices are present despite the dropped face"),
            Result->GetIntegerField(TEXT("vertexCount")), 5);

        TestNotNull(TEXT("and the actor really was spawned"), FindActorByLabel(Label));
    }

    DestroyActorsWithLabel(Label);

    // The snake_case spelling, driven rather than assumed. It is not enough that the handler body
    // reads both keys: the dispatcher rejects any top-level field that is neither a declared Name
    // nor a declared Alias, so an undeclared alias is UNKNOWN_PARAMS before the handler runs -
    // which would look like the flag being ignored rather than refused. This asserts the declared
    // surface and the read surface agree.
    {
        const FString SnakeLabel = FString::Printf(TEXT("PW_ImportSnake_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        TSharedPtr<FJsonObject> SnakeParams = MakeShared<FJsonObject>();
        SnakeParams->SetStringField(TEXT("actorName"), SnakeLabel);
        SnakeParams->SetStringField(TEXT("text"), GeomFailTest_NonManifoldObj);
        SnakeParams->SetBoolField(TEXT("allow_partial"), true);

        bool bSnakeSuccess = false;
        FString SnakeErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.import_obj"), TEXT("req-import-snake"),
            SnakeParams, bSnakeSuccess, SnakeErrorCode);

        TestTrue(TEXT("allow_partial is honoured, not rejected as an unknown parameter"),
            bSnakeSuccess);
        TestNotEqual(TEXT("and specifically is not UNKNOWN_PARAMS"),
            SnakeErrorCode, FString(TEXT("UNKNOWN_PARAMS")));

        DestroyActorsWithLabel(SnakeLabel);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryImportCleanObjTest,
    "PinWright.geometry.import_obj.CleanObjImportsWithZeroDroppedTriangles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryImportCleanObjTest::RunTest(const FString& Parameters)
{
    // The other direction of the control pair: the new check must not have made ordinary imports
    // fail. Also pins that droppedTriangles is published on the CLEAN path too, so a caller can
    // assert "nothing was lost" without knowing which flag it passed.
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping clean import test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_ImportClean_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetStringField(TEXT("text"), GeomFailTest_CleanObj);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.import_obj"), TEXT("req-import-clean"),
        Params, bSuccess, Result, ErrorCode);

    if (TestTrue(TEXT("a well-formed OBJ still imports"), bSuccess)
        && TestTrue(TEXT("a result object came back"), Result.IsValid()))
    {
        TestEqual(TEXT("both faces landed"), Result->GetIntegerField(TEXT("triangleCount")), 2);
        TestEqual(TEXT("requestedTriangles agrees"),
            Result->GetIntegerField(TEXT("requestedTriangles")), 2);
        TestEqual(TEXT("nothing was dropped"),
            Result->GetIntegerField(TEXT("droppedTriangles")), 0);
        TestFalse(TEXT("and no warnings array is emitted"),
            Result->HasField(TEXT("warnings")));
    }

    DestroyActorsWithLabel(Label);
    return true;
}

// ============================================================================
// GeometryOps::AppendBuffers - the same engine call against the caller's own mesh
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAppendBuffersRefusalTest,
    "PinWright.Geometry.Ops.Advanced.RefusedAppendFailsAndLeavesAnEmptyMeshEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryAppendBuffersRefusalTest::RunTest(const FString& Parameters)
{
    AddExpectedErrorPlain(TEXT("AppendBuffersToMesh: Triangle cannot be added because it is a duplicate of an existing Triangle"),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());

    GeometryOps::FAppendBuffersParams Params;
    Params.Vertices = { FVector(0, 0, 0), FVector(100, 0, 0), FVector(0, 100, 0) };
    // The same triangle twice. The engine takes the first and refuses the second as a duplicate.
    Params.Triangles = { FIntVector(0, 1, 2), FIntVector(0, 1, 2) };

    GeometryOps::FAppendBuffersOutputs Out;
    const GeometryOps::FOpResult Op = GeometryOps::AppendBuffers(Mesh, MoveTemp(Params), Out);

    // False on the nullptr-Debug version, which returns success with AppendedTriangles == 1 for a
    // two-triangle request and no indication that anything was refused.
    TestFalse(TEXT("an append the engine partly refused is a failure"), Op.bSuccess);
    TestEqual(TEXT("carrying MESH_APPEND_FAILED"),
        Op.ErrorCode, FString(ErrorCodes::ERR_MESH_APPEND_FAILED));
    TestTrue(TEXT("with the engine's own diagnosis"),
        Op.ErrorMessage.Contains(TEXT("duplicate of an existing Triangle")));
    TestTrue(TEXT("and the refused count"), Op.ErrorMessage.Contains(TEXT("1 of 2")));

    // The rollback. This is the site's side-effect audit: the engine mutates inside the same call
    // in which it decides to refuse, so the only honest failure is one that puts the mesh back.
    TestEqual(TEXT("the empty target is left empty, not holding the accepted triangle"),
        Mesh->GetTriangleCount(), 0);
    TestEqual(TEXT("and not holding the appended vertices either"),
        GeometryUtils::GetMeshVertexCount(Mesh), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAppendBuffersRollbackTest,
    "PinWright.Geometry.Ops.Advanced.RefusedAppendRestoresAMeshThatAlreadyHadGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryAppendBuffersRollbackTest::RunTest(const FString& Parameters)
{
    // The case the empty-target test cannot reach: rolling back to a real prior mesh rather than
    // to nothing. Without the snapshot this leaves the box plus three orphan vertices plus one
    // triangle, under an error response saying the append failed.
    AddExpectedErrorPlain(TEXT("AppendBuffersToMesh: Triangle cannot be added because it is a duplicate of an existing Triangle"),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    UDynamicMesh* Mesh = GeomFailTest_NewBox();
    const int32 TrisBefore = Mesh->GetTriangleCount();
    const int32 VertsBefore = GeometryUtils::GetMeshVertexCount(Mesh);
    TestEqual(TEXT("fixture is a 12-triangle box"), TrisBefore, 12);

    GeometryOps::FAppendBuffersParams Params;
    Params.Vertices = { FVector(500, 0, 0), FVector(600, 0, 0), FVector(500, 100, 0) };
    Params.Triangles = { FIntVector(0, 1, 2), FIntVector(0, 1, 2) };

    GeometryOps::FAppendBuffersOutputs Out;
    const GeometryOps::FOpResult Op = GeometryOps::AppendBuffers(Mesh, MoveTemp(Params), Out);

    TestFalse(TEXT("the append failed"), Op.bSuccess);
    TestEqual(TEXT("with MESH_APPEND_FAILED"),
        Op.ErrorCode, FString(ErrorCodes::ERR_MESH_APPEND_FAILED));
    TestTrue(TEXT("the message states the mesh was restored"),
        Op.ErrorMessage.Contains(TEXT("restored")));

    TestEqual(TEXT("the box is intact"), Mesh->GetTriangleCount(), TrisBefore);
    TestEqual(TEXT("with no orphan vertices left behind"),
        GeometryUtils::GetMeshVertexCount(Mesh), VertsBefore);

    // FailIn, not Fail: the before-counts must survive, and after the rollback they are also the
    // mesh's CURRENT counts - which is what makes them worth reporting on a failure at all.
    TestEqual(TEXT("the before-counts survive the failure"), Op.TrianglesBefore, TrisBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAppendBuffersAcceptedTest,
    "PinWright.Geometry.Ops.Advanced.AnAcceptableAppendStillSucceeds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryAppendBuffersAcceptedTest::RunTest(const FString& Parameters)
{
    // The control. Same op, same mesh shape, triangles the engine accepts: if this succeeds and
    // the two above fail, the failure is the engine's refusal and not the new snapshot/rollback
    // path breaking ordinary appends.
    UDynamicMesh* Mesh = GeomFailTest_NewBox();
    const int32 TrisBefore = Mesh->GetTriangleCount();

    GeometryOps::FAppendBuffersParams Params;
    Params.Vertices = { FVector(500, 0, 0), FVector(600, 0, 0), FVector(500, 100, 0) };
    Params.Triangles = { FIntVector(0, 1, 2) };

    GeometryOps::FAppendBuffersOutputs Out;
    const GeometryOps::FOpResult Op = GeometryOps::AppendBuffers(Mesh, MoveTemp(Params), Out);

    TestTrue(TEXT("a clean append succeeds"), Op.bSuccess);
    TestEqual(TEXT("with no error code"), Op.ErrorCode, FString());
    TestEqual(TEXT("one triangle was appended"), Out.AppendedTriangles, 1);
    TestEqual(TEXT("and the mesh really grew"), Mesh->GetTriangleCount(), TrisBefore + 1);

    return true;
}

// ============================================================================
// GeometryOps::RecomputeTangents - the refusal an imported mesh walks straight into
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryTangentsOnUVLessMeshTest,
    "PinWright.Geometry.Ops.Modeling.TangentsOnAUVLessMeshFailInsteadOfReportingSuccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryTangentsOnUVLessMeshTest::RunTest(const FString& Parameters)
{
    AddExpectedErrorPlain(TEXT("ComputeTangents: TargetMesh is missing UV Set or Normals required to compute Tangents"),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    UDynamicMesh* Mesh = GeomFailTest_NewUVLessTriangle();

    // The premise of the test, asserted rather than assumed: this is the mesh shape
    // geometry.import_obj produces, and it is the reason the failure is ordinary rather than
    // contrived. AppendBuffersToMesh sets the target's UV layer count from the incoming buffers,
    // which carry none.
    TestEqual(TEXT("the fixture mesh has zero UV layers"), GeomFailTest_UVLayerCount(Mesh), 0);
    TestEqual(TEXT("and one triangle"), Mesh->GetTriangleCount(), 1);

    const GeometryOps::FOpResult Op = GeometryOps::RecomputeTangents(
        Mesh, GeometryOps::FRecomputeTangentsParams());

    // The engine refuses at the first statement of its edit lambda and reports only into Debug,
    // so on the nullptr version this was success with bChanged forced true - a claim that
    // tangents had been written to a mesh that has none.
    TestFalse(TEXT("tangents on a UV-less mesh are reported as a failure"), Op.bSuccess);
    TestEqual(TEXT("carrying TANGENTS_FAILED"),
        Op.ErrorCode, FString(ErrorCodes::ERR_TANGENTS_FAILED));
    TestTrue(TEXT("with the engine's own diagnosis"),
        Op.ErrorMessage.Contains(TEXT("missing UV Set or Normals")));
    TestFalse(TEXT("and it does not claim the mesh changed"), Op.bChanged);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryTangentsWithUVsTest,
    "PinWright.Geometry.Ops.Modeling.TangentsSucceedOnceTheMeshHasUVs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryTangentsWithUVsTest::RunTest(const FString& Parameters)
{
    // The control: one call added, same mesh, and the same op turns green. Proves the failure
    // above is specifically the missing UV layer.
    UDynamicMesh* Mesh = GeomFailTest_NewUVLessTriangle();
    TestTrue(TEXT("box-projecting UVs onto the fixture works"),
        GeometryUtils::EnsureMeshHasUVs(Mesh));
    TestTrue(TEXT("the mesh now has a UV layer"), GeomFailTest_UVLayerCount(Mesh) > 0);

    const GeometryOps::FOpResult Op = GeometryOps::RecomputeTangents(
        Mesh, GeometryOps::FRecomputeTangentsParams());

    TestTrue(TEXT("tangents now succeed"), Op.bSuccess);
    TestEqual(TEXT("with no error code"), Op.ErrorCode, FString());

    return true;
}

// ============================================================================
// GeometryOps::UnwrapUVXAtlas - the refusal every edited mesh walks into
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryXAtlasNonCompactTest,
    "PinWright.Geometry.Ops.Modeling.XAtlasRefusesANonCompactMeshInsteadOfReportingSuccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryXAtlasNonCompactTest::RunTest(const FString& Parameters)
{
    AddExpectedErrorPlain(TEXT("AutoGenerateXAtlasMeshUVs: TargetMesh is non-Compact"),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    UDynamicMesh* Mesh = GeomFailTest_NewBox();

    // Deleting one triangle leaves a hole in the id space, which is all "non-compact" means. Any
    // delete, boolean or simplify this plugin runs leaves the mesh in this state, which is why
    // the refusal is an everyday one rather than an edge case.
    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        EditMesh.RemoveTriangle(0);
    });
    TestFalse(TEXT("the fixture mesh is non-compact"), GeomFailTest_IsCompact(Mesh));

    const GeometryOps::FOpResult Op = GeometryOps::UnwrapUVXAtlas(Mesh, 0);

    TestFalse(TEXT("XAtlas refusing the mesh is reported as a failure"), Op.bSuccess);
    TestEqual(TEXT("carrying UV_GENERATION_FAILED"),
        Op.ErrorCode, FString(ErrorCodes::ERR_UV_GENERATION_FAILED));
    TestTrue(TEXT("with the engine's own diagnosis"),
        Op.ErrorMessage.Contains(TEXT("non-Compact")));
    // The remedy is in the engine's sentence and nowhere else, which is why the text is forwarded
    // verbatim rather than paraphrased.
    TestTrue(TEXT("and the engine's own remedy"),
        Op.ErrorMessage.Contains(TEXT("CompactMesh")));
    TestFalse(TEXT("and it does not claim the mesh changed"), Op.bChanged);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryXAtlasCompactTest,
    "PinWright.Geometry.Ops.Modeling.XAtlasSucceedsOnACompactMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryXAtlasCompactTest::RunTest(const FString& Parameters)
{
    // The control: an untouched box is compact, and the same call succeeds. Without this the
    // failing test cannot distinguish "the sink works" from "XAtlas is broken in this build".
    UDynamicMesh* Mesh = GeomFailTest_NewBox();
    TestTrue(TEXT("an untouched box is compact"), GeomFailTest_IsCompact(Mesh));

    const GeometryOps::FOpResult Op = GeometryOps::UnwrapUVXAtlas(Mesh, 0);

    TestTrue(TEXT("XAtlas succeeds on it"), Op.bSuccess);
    TestEqual(TEXT("with no error code"), Op.ErrorCode, FString());

    return true;
}

// ============================================================================
// GeometryOps::AutoUVPatchBuilder - an error the engine raises and then ignores
// ============================================================================

// NOT COVERED, and measured rather than assumed: AutoGeneratePatchBuilderMeshUVs' one
// caller-reachable refusal - "Requested Polygroup Layer does not exist", raised when
// Options.bRespectInputGroups is set and FPolygroupLayer::CheckExists fails - could not be
// provoked through GeometryOps::AutoUVPatchBuilder. CheckExists on the default layer is
// Mesh->HasTriangleGroups() (PolygroupSet.cpp), so the fixture stripped the box's groups with
// FDynamicMesh3::DiscardTriangleGroups() and asked the op to respect them anyway; the engine
// raised nothing, and it did not raise it into the LOG either - which is decisive, because
// MakeScriptError logs unconditionally whether or not a Debug object is passed. The mesh still
// had triangle groups by the time the engine looked, and nothing on the path between
// (BeginOp, EnsureMeshHasUVChannel, ApplyMeshUVEditorOperation, the FDynamicMeshUVEditor
// constructor) re-enables them; the mechanism is unexplained.
//
// The other two refusals are structurally unreachable here: "UVSetIndex does not exist" is
// pre-empted by EnsureMeshHasUVChannel one statement earlier, and "UV Generation Failed" needs a
// mesh FPatchBasedMeshUVGenerator cannot partition, which no fixture produced. The wiring stands
// and is exercised by the XAtlas tests above, which share its error code and its sink.
//
// The control below is kept: it pins that the op still succeeds on the path the failing test
// used, so the wiring did not turn an ordinary PatchBuilder call into an error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryPatchBuilderWithoutGroupConstraintTest,
    "PinWright.Geometry.Ops.Modeling.PatchBuilderSucceedsWhenItIsNotAskedToRespectGroups",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryPatchBuilderWithoutGroupConstraintTest::RunTest(const FString& Parameters)
{
    // The control: identical mesh, one option flipped off, and the same call succeeds.
    UDynamicMesh* Mesh = GeomFailTest_NewBox();
    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        EditMesh.DiscardTriangleGroups();
    });

    GeometryOps::FPatchBuilderUVParams Params;
    Params.bRespectInputGroups = false;

    const GeometryOps::FOpResult Op = GeometryOps::AutoUVPatchBuilder(Mesh, Params);

    TestTrue(TEXT("PatchBuilder succeeds without the group constraint"), Op.bSuccess);
    TestEqual(TEXT("with no error code"), Op.ErrorCode, FString());

    return true;
}
