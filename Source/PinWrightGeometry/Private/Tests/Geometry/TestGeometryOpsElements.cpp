// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the extracted element-edit ops (GeometryOps_Elements.h).
//
// These run against a transient UDynamicMesh with no actor, no editor world and no
// FHandlerContext anywhere - which is the whole point of the extraction, and the property
// the .pwmodel compiler depends on. If a later change reintroduces a Ctx read or an actor
// lookup into one of these ops, this file stops compiling rather than failing at runtime.
//
// What each test guards, in the order a regression would appear:
//
//  1. Error codes and message text. The ~146 dispatcher tests observe these verbs only
//     through the response JSON, so the extraction is behaviour-preserving ONLY if each op
//     returns the exact ErrorCodes.h value and the exact message the handler emitted at that
//     failure point. Changing "Invalid vertex index: %d" to anything else is invisible in a
//     compile and breaks callers that match on it.
//  2. Before/after counts. The wrappers now echo vertexCount/triangleCount straight out of
//     FOpResult instead of re-reading the mesh, so a wrong count field silently changes a
//     published response rather than crashing.
//  3. bChanged. delete_vertex/delete_triangle report it as their `success` JSON field inside
//     an otherwise-successful response; if the op hard-wired it to true those verbs would
//     start claiming they removed elements they did not.
//  4. The color overlay. set_vertex_color is the vertex-color reference implementation for
//     the whole plugin: it must write the attribute-overlay PrimaryColors channel and never
//     the legacy FDynamicMesh3 per-vertex buffer, which has no alpha and which nothing in
//     the render or bake path reads (see TestGeometrySetVertexColorPersistsToOverlay.cpp for
//     the end-to-end form of that same regression).
//  5. The null-mesh contract. Several of these ops used to dereference Mesh with no guard at
//     all, behind a ternary in a counts helper that read as one. No RPC wrapper can reach
//     that (GeometryTarget::ResolveOrSendError never returns true with a null Mesh), but the
//     .pwmodel compiler calls the same ops, and a code back is the only alternative to a
//     crash inside an engine call.
//
// The last test in this file is deliberately NOT an element op: it pins
// GeometryUtils::EnsureMeshHasUVs against UV-channel truncation. It lives here because the
// files that would otherwise host it are owned elsewhere, and because it is the same
// grow-only-vs-exact rule set_uvs' own layer guard turns on.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Elements.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
// helper with a common name would collide with a sibling test TU when Unity merges them.

// A bare transient mesh - no actor, no component, no world.
UDynamicMesh* GeometryOpsElementsTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// Three unshared vertices and one triangle over them, built through the op under test so the
// fixture needs nothing from a sibling op family.
UDynamicMesh* GeometryOpsElementsTest_NewTriangleMesh()
{
    UDynamicMesh* Mesh = GeometryOpsElementsTest_NewMesh();
    GeometryOps::FAppendTriangleParams Params;
    GeometryOps::FAppendTriangleIndices Indices;
    GeometryOps::AppendTriangle(Mesh, Params, Indices);
    return Mesh;
}

// A box, which unlike the hand-built triangle carries a populated UV0 overlay.
UDynamicMesh* GeometryOpsElementsTest_NewBoxMesh()
{
    UDynamicMesh* Mesh = GeometryOpsElementsTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

void GeometryOpsElementsTest_ExpectFailure(
    FAutomationTestBase& Test,
    const TCHAR* Label,
    const GeometryOps::FOpResult& Op,
    const TCHAR* ExpectedCode,
    const FString& ExpectedMessage)
{
    const FString FailedLabel = FString::Printf(TEXT("%s reports failure"), Label);
    const FString CodeLabel = FString::Printf(TEXT("%s error code"), Label);
    const FString MessageLabel = FString::Printf(TEXT("%s error message"), Label);

    Test.TestFalse(*FailedLabel, Op.bSuccess);
    Test.TestEqual(*CodeLabel, Op.ErrorCode, FString(ExpectedCode));
    Test.TestEqual(*MessageLabel, Op.ErrorMessage, ExpectedMessage);
}
}

// ============================================================================
// Counts and change flags
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsElementsCountsTest,
    "PinWright.Geometry.Ops.Elements.AppendAndDeleteReportBeforeAndAfterCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsElementsCountsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsElementsTest_NewMesh());

    GeometryOps::FAppendTriangleParams TriParams;
    GeometryOps::FAppendTriangleIndices Indices;
    const GeometryOps::FOpResult AppendTri = GeometryOps::AppendTriangle(Mesh.Get(), TriParams, Indices);

    TestTrue(TEXT("append_triangle on an empty mesh succeeds"), AppendTri.bSuccess);
    TestTrue(TEXT("append_triangle reports a change"), AppendTri.bChanged);
    TestEqual(TEXT("append_triangle vertices before"), AppendTri.VerticesBefore, 0);
    TestEqual(TEXT("append_triangle triangles before"), AppendTri.TrianglesBefore, 0);
    TestEqual(TEXT("append_triangle vertices after"), AppendTri.VerticesAfter, 3);
    TestEqual(TEXT("append_triangle triangles after"), AppendTri.TrianglesAfter, 1);
    TestEqual(TEXT("append_triangle emits the new triangle id"), Indices.TriangleIndex, 0);
    TestEqual(TEXT("append_triangle emits vertex 0"), Indices.VertexIndices[0], 0);
    TestEqual(TEXT("append_triangle emits vertex 1"), Indices.VertexIndices[1], 1);
    TestEqual(TEXT("append_triangle emits vertex 2"), Indices.VertexIndices[2], 2);

    GeometryOps::FAppendVertexParams VertParams;
    VertParams.Position = FVector(10, 20, 30);
    int32 NewVertexIndex = INDEX_NONE;
    const GeometryOps::FOpResult AppendVert = GeometryOps::AppendVertex(Mesh.Get(), VertParams, NewVertexIndex);

    TestTrue(TEXT("append_vertex succeeds"), AppendVert.bSuccess);
    TestEqual(TEXT("append_vertex emits the new vertex id"), NewVertexIndex, 3);
    TestEqual(TEXT("append_vertex vertices before"), AppendVert.VerticesBefore, 3);
    // The wrapper echoes this as vertexCount, so a wrong value here is a changed response.
    TestEqual(TEXT("append_vertex vertices after"), AppendVert.VerticesAfter, 4);

    GeometryOps::FDeleteVertexParams DeleteVertParams;
    DeleteVertParams.VertexIndex = NewVertexIndex;
    const GeometryOps::FOpResult DeleteVert = GeometryOps::DeleteVertex(Mesh.Get(), DeleteVertParams);

    TestTrue(TEXT("delete_vertex succeeds"), DeleteVert.bSuccess);
    TestTrue(TEXT("delete_vertex removed the vertex"), DeleteVert.bChanged);
    TestEqual(TEXT("delete_vertex vertices after"), DeleteVert.VerticesAfter, 3);

    GeometryOps::FDeleteTriangleParams DeleteTriParams;
    DeleteTriParams.TriangleIndex = Indices.TriangleIndex;
    const GeometryOps::FOpResult DeleteTri = GeometryOps::DeleteTriangle(Mesh.Get(), DeleteTriParams);

    TestTrue(TEXT("delete_triangle succeeds"), DeleteTri.bSuccess);
    TestTrue(TEXT("delete_triangle removed the triangle"), DeleteTri.bChanged);
    TestEqual(TEXT("delete_triangle triangles before"), DeleteTri.TrianglesBefore, 1);
    TestEqual(TEXT("delete_triangle triangles after"), DeleteTri.TrianglesAfter, 0);

    return true;
}

// ============================================================================
// Error codes and message text
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsElementsErrorCodesTest,
    "PinWright.Geometry.Ops.Elements.InvalidIndicesReturnTheHandlerErrorCodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsElementsErrorCodesTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsElementsTest_NewTriangleMesh());

    GeometryOps::FSetVertexPositionParams SetPos;
    SetPos.VertexIndex = 7;
    SetPos.Position = FVector(1, 2, 3);
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("set_vertex_position on a dead vertex"),
        GeometryOps::SetVertexPosition(Mesh.Get(), SetPos),
        ErrorCodes::ERR_INVALID_VERTEX, TEXT("Invalid vertex index: 7"));

    GeometryOps::FDeleteVertexParams DeleteVert;
    DeleteVert.VertexIndex = 7;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("delete_vertex on a dead vertex"),
        GeometryOps::DeleteVertex(Mesh.Get(), DeleteVert),
        ErrorCodes::ERR_INVALID_VERTEX, TEXT("Invalid vertex index: 7"));

    GeometryOps::FDeleteTriangleParams DeleteTri;
    DeleteTri.TriangleIndex = 4;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("delete_triangle on a dead triangle"),
        GeometryOps::DeleteTriangle(Mesh.Get(), DeleteTri),
        ErrorCodes::ERR_INVALID_TRIANGLE, TEXT("Invalid triangle index: 4"));

    // setAll off with no vertex named is the -1 default the RPC layer passes through.
    GeometryOps::FSetVertexColorParams SetColor;
    SetColor.VertexIndex = -1;
    int32 VerticesModified = -1;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("set_vertex_color with no target vertex"),
        GeometryOps::SetVertexColor(Mesh.Get(), SetColor, VerticesModified),
        ErrorCodes::ERR_INVALID_VERTEX, TEXT("Invalid vertex index: -1"));
    TestEqual(TEXT("a failed set_vertex_color reports no modified vertices"), VerticesModified, 0);

    GeometryOps::FSetUVsParams SetUVs;
    SetUVs.VertexIndex = 9;
    int32 ElementsModified = -1;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("set_uvs on a dead vertex"),
        GeometryOps::SetUVs(Mesh.Get(), SetUVs, ElementsModified),
        ErrorCodes::ERR_INVALID_VERTEX, TEXT("Invalid vertex index: 9"));

    // A negative channel is the one GetUVLayer refuses; the attribute set has no upper
    // ceiling, so this is the only path that reaches UV_LAYER_ERROR.
    GeometryOps::FSetUVsParams NegativeChannel;
    NegativeChannel.VertexIndex = 0;
    NegativeChannel.UVChannel = -1;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("set_uvs on a negative channel"),
        GeometryOps::SetUVs(Mesh.Get(), NegativeChannel, ElementsModified),
        ErrorCodes::ERR_UV_LAYER_ERROR, TEXT("Failed to access UV layer"));

    return true;
}

// ============================================================================
// set_vertex_color writes the overlay, never the legacy buffer
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsElementsVertexColorTest,
    "PinWright.Geometry.Ops.Elements.SetVertexColorWritesTheAttributeOverlay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsElementsVertexColorTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsElementsTest_NewTriangleMesh());

    GeometryOps::FSetVertexColorParams Params;
    Params.bSetAll = true;
    Params.Color = FLinearColor(0.25f, 0.5f, 0.75f, 0.5f);

    int32 VerticesModified = 0;
    const GeometryOps::FOpResult Op = GeometryOps::SetVertexColor(Mesh.Get(), Params, VerticesModified);

    TestTrue(TEXT("set_vertex_color succeeds"), Op.bSuccess);
    TestTrue(TEXT("set_vertex_color reports a change"), Op.bChanged);
    TestEqual(TEXT("set_vertex_color colored every vertex"), VerticesModified, 3);

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    if (!TestTrue(TEXT("set_vertex_color enabled the attribute set"), EditMesh.HasAttributes()))
    {
        return true;
    }
    if (!TestTrue(TEXT("set_vertex_color enabled the primary color overlay"),
            EditMesh.Attributes()->HasPrimaryColors()))
    {
        return true;
    }

    // The legacy FDynamicMesh3 per-vertex color buffer is a DIFFERENT storage location, has
    // no alpha, and is read by nothing in the render or bake path. Writing it instead of the
    // overlay is the exact regression this op must never take.
    TestFalse(TEXT("set_vertex_color left the legacy per-vertex color buffer disabled"),
        EditMesh.HasVertexColors());

    const UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay = EditMesh.Attributes()->PrimaryColors();
    TestTrue(TEXT("the color overlay carries elements"), ColorOverlay->ElementCount() > 0);

    bool bAllMatch = true;
    for (int32 ElementID : ColorOverlay->ElementIndicesItr())
    {
        const FVector4f Element = ColorOverlay->GetElement(ElementID);
        // Alpha specifically: the legacy buffer is FVector3f and would drop it.
        if (!FMath::IsNearlyEqual(Element.X, 0.25f) || !FMath::IsNearlyEqual(Element.Y, 0.5f) ||
            !FMath::IsNearlyEqual(Element.Z, 0.75f) || !FMath::IsNearlyEqual(Element.W, 0.5f))
        {
            bAllMatch = false;
            break;
        }
    }
    TestTrue(TEXT("every overlay element carries the requested RGBA"), bAllMatch);

    return true;
}

// ============================================================================
// set_vertex_color addresses ONE vertex when setAll is off
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsElementsVertexColorSingleTest,
    "PinWright.Geometry.Ops.Elements.SetVertexColorTintsOnlyTheNamedVertex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsElementsVertexColorSingleTest::RunTest(const FString& Parameters)
{
    // The bSetAll=false branch - the one that walks GetVertexElements instead of every element
    // in the overlay - is otherwise exercised nowhere: SetVertexColorWritesTheAttributeOverlay
    // drives setAll, and the error-code test only reaches the invalid-index path. Replace this
    // branch's body with the setAll body and nothing in the suite notices, because every
    // element carrying the requested colour is exactly what setAll produces.
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsElementsTest_NewTriangleMesh());

    GeometryOps::FSetVertexColorParams Params;
    Params.VertexIndex = 1;
    Params.Color = FLinearColor(0.1f, 0.2f, 0.3f, 0.4f);

    int32 VerticesModified = 0;
    const GeometryOps::FOpResult Op = GeometryOps::SetVertexColor(Mesh.Get(), Params, VerticesModified);

    TestTrue(TEXT("a single-vertex set_vertex_color succeeds"), Op.bSuccess);
    TestTrue(TEXT("it reports a change"), Op.bChanged);
    TestEqual(TEXT("it reports one modified vertex, not the whole mesh"), VerticesModified, 1);

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    if (!TestTrue(TEXT("the primary color overlay exists"),
            EditMesh.HasAttributes() && EditMesh.Attributes()->HasPrimaryColors()))
    {
        return true;
    }
    const UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay = EditMesh.Attributes()->PrimaryColors();

    // Exact counts on both sides, so an empty overlay fails rather than asserting nothing: the
    // seeded layer is one element per vertex at (1,1,1,1) - CreatePerVertex copies its single
    // float into all four channels - so vertex 1's element must carry the request and the other
    // two must still carry the seed.
    int32 Tinted = 0;
    int32 StillSeeded = 0;
    for (int32 ElementID : ColorOverlay->ElementIndicesItr())
    {
        const FVector4f Element = ColorOverlay->GetElement(ElementID);
        if (ColorOverlay->GetParentVertex(ElementID) == 1)
        {
            if (Element.Equals(FVector4f(0.1f, 0.2f, 0.3f, 0.4f), 0.0001f))
            {
                ++Tinted;
            }
        }
        else if (Element.Equals(FVector4f(1.0f, 1.0f, 1.0f, 1.0f), 0.0001f))
        {
            ++StillSeeded;
        }
    }
    TestEqual(TEXT("the named vertex's element carries the requested RGBA"), Tinted, 1);
    TestEqual(TEXT("the other two vertices keep the seeded colour"), StillSeeded, 2);

    // A live vertex that owns no overlay element reports ZERO modified, not an unconditional
    // success of 1. An isolated vertex is the reachable form of that case: GetVertexElements
    // walks the vertex's incident triangles, and this one has none.
    GeometryOps::FAppendVertexParams Loose;
    Loose.Position = FVector(500.0, 500.0, 0.0);
    int32 LooseIndex = INDEX_NONE;
    TestTrue(TEXT("the loose vertex appends"),
        GeometryOps::AppendVertex(Mesh.Get(), Loose, LooseIndex).bSuccess);

    GeometryOps::FSetVertexColorParams LooseParams;
    LooseParams.VertexIndex = LooseIndex;
    LooseParams.Color = FLinearColor(0.9f, 0.8f, 0.7f, 0.6f);

    int32 LooseModified = -1;
    const GeometryOps::FOpResult LooseOp =
        GeometryOps::SetVertexColor(Mesh.Get(), LooseParams, LooseModified);

    TestTrue(TEXT("colouring a vertex with no overlay element is not an error"), LooseOp.bSuccess);
    TestEqual(TEXT("but it reports no modified vertices"), LooseModified, 0);
    TestFalse(TEXT("and reports no change"), LooseOp.bChanged);

    return true;
}

// ============================================================================
// set_uvs
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsElementsSetUVsTest,
    "PinWright.Geometry.Ops.Elements.SetUVsWritesTheVertexElementsOnItsChannel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsElementsSetUVsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsElementsTest_NewBoxMesh());

    GeometryOps::FSetUVsParams Params;
    Params.VertexIndex = 0;
    Params.UV = FVector2D(0.25, 0.75);
    Params.UVChannel = 0;

    int32 ElementsModified = 0;
    const GeometryOps::FOpResult Op = GeometryOps::SetUVs(Mesh.Get(), Params, ElementsModified);

    TestTrue(TEXT("set_uvs on a box's populated UV0 succeeds"), Op.bSuccess);
    TestTrue(TEXT("set_uvs reports a change"), Op.bChanged);
    TestTrue(TEXT("set_uvs modified at least one UV element"), ElementsModified > 0);

    const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    const UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = EditMesh.Attributes()->GetUVLayer(0);
    bool bAllMatch = UVOverlay != nullptr;
    if (UVOverlay)
    {
        for (int32 ElementID : UVOverlay->ElementIndicesItr())
        {
            if (UVOverlay->GetParentVertex(ElementID) != 0)
            {
                continue;
            }
            const FVector2f Element = UVOverlay->GetElement(ElementID);
            if (!FMath::IsNearlyEqual(Element.X, 0.25f) || !FMath::IsNearlyEqual(Element.Y, 0.75f))
            {
                bAllMatch = false;
                break;
            }
        }
    }
    TestTrue(TEXT("every UV element on the target vertex carries the requested UV"), bAllMatch);

    // A channel that did not exist is grown, but a grown layer starts element-less - so the
    // op reports NO_UV_ELEMENTS rather than claiming to have written a UV nothing carries.
    GeometryOps::FSetUVsParams GrownChannel;
    GrownChannel.VertexIndex = 0;
    GrownChannel.UV = FVector2D(0.1, 0.2);
    GrownChannel.UVChannel = 3;
    int32 GrownElementsModified = -1;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("set_uvs on a freshly grown channel"),
        GeometryOps::SetUVs(Mesh.Get(), GrownChannel, GrownElementsModified),
        ErrorCodes::ERR_NO_UV_ELEMENTS, TEXT("No UV elements found for vertex 0"));
    TestTrue(TEXT("the requested UV channel was created before the op gave up"),
        EditMesh.Attributes()->NumUVLayers() >= 4);

    return true;
}

// ============================================================================
// set_vertex_position / translate_mesh
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsElementsMovementTest,
    "PinWright.Geometry.Ops.Elements.PositionAndTranslateReportWhetherAnythingMoved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsElementsMovementTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsElementsTest_NewTriangleMesh());

    GeometryOps::FSetVertexPositionParams Params;
    Params.VertexIndex = 0;
    Params.Position = FVector(5, 6, 7);

    const GeometryOps::FOpResult Moved = GeometryOps::SetVertexPosition(Mesh.Get(), Params);
    TestTrue(TEXT("set_vertex_position succeeds"), Moved.bSuccess);
    TestTrue(TEXT("set_vertex_position reports a change"), Moved.bChanged);
    TestEqual(TEXT("the vertex landed where it was asked to"),
        Mesh->GetMeshRef().GetVertex(0), FVector(5, 6, 7));

    // Writing the same position again is a no-op the compiler wants to see as one; the
    // response JSON is unchanged either way.
    const GeometryOps::FOpResult Unmoved = GeometryOps::SetVertexPosition(Mesh.Get(), Params);
    TestTrue(TEXT("a repeated set_vertex_position still succeeds"), Unmoved.bSuccess);
    TestFalse(TEXT("a repeated set_vertex_position reports no change"), Unmoved.bChanged);

    GeometryOps::FTranslateMeshParams Translate;
    Translate.Translation = FVector(0, 0, 100);
    const GeometryOps::FOpResult Translated = GeometryOps::TranslateMesh(Mesh.Get(), Translate);
    TestTrue(TEXT("translate_mesh succeeds"), Translated.bSuccess);
    TestTrue(TEXT("translate_mesh reports a change"), Translated.bChanged);
    TestEqual(TEXT("translate_mesh baked the offset into the vertex"),
        Mesh->GetMeshRef().GetVertex(0), FVector(5, 6, 107));
    TestEqual(TEXT("translate_mesh did not change the vertex count"),
        Translated.VerticesAfter, Translated.VerticesBefore);

    GeometryOps::FTranslateMeshParams NoTranslate;
    const GeometryOps::FOpResult NotTranslated = GeometryOps::TranslateMesh(Mesh.Get(), NoTranslate);
    TestTrue(TEXT("a zero translate_mesh still succeeds"), NotTranslated.bSuccess);
    TestFalse(TEXT("a zero translate_mesh reports no change"), NotTranslated.bChanged);

    return true;
}

// ============================================================================
// The null-mesh contract
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsElementsNullMeshTest,
    "PinWright.Geometry.Ops.Elements.EveryOpRejectsANullMeshWithTheSharedContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsElementsNullMeshTest::RunTest(const FString& Parameters)
{
    // Every op in the family, including the two that used to reach Mesh->GetMeshRef() with no
    // guard whatsoever (set_vertex_position, append_vertex) and the one that used to report
    // SUCCESS on a null mesh because the geometry-script call swallowed it (translate_mesh).
    const FString ExpectedMessage(TEXT("DynamicMesh not available"));

    // The code/message pair is byte-identical to the one GeometryTarget::ResolveOrSendError
    // already sends for a resolved actor carrying no mesh, so even if a wrapper could reach
    // this path the published response would not move. Pinned, because that equality is the
    // whole argument for giving these ops a guard rather than leaving them to crash.
    TestEqual(TEXT("the op-level null-mesh code matches the resolver's"),
        FString(ErrorCodes::ERR_MESH_NOT_FOUND), FString(TEXT("MESH_NOT_FOUND")));

    GeometryOps::FSetVertexPositionParams SetPos;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("set_vertex_position on a null mesh"),
        GeometryOps::SetVertexPosition(nullptr, SetPos),
        ErrorCodes::ERR_MESH_NOT_FOUND, ExpectedMessage);

    GeometryOps::FAppendVertexParams AppendVert;
    int32 VertexIndex = INDEX_NONE;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("append_vertex on a null mesh"),
        GeometryOps::AppendVertex(nullptr, AppendVert, VertexIndex),
        ErrorCodes::ERR_MESH_NOT_FOUND, ExpectedMessage);
    TestEqual(TEXT("a refused append_vertex emits no vertex id"), VertexIndex, int32(INDEX_NONE));

    GeometryOps::FDeleteVertexParams DeleteVert;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("delete_vertex on a null mesh"),
        GeometryOps::DeleteVertex(nullptr, DeleteVert),
        ErrorCodes::ERR_MESH_NOT_FOUND, ExpectedMessage);

    GeometryOps::FAppendTriangleParams AppendTri;
    GeometryOps::FAppendTriangleIndices Indices;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("append_triangle on a null mesh"),
        GeometryOps::AppendTriangle(nullptr, AppendTri, Indices),
        ErrorCodes::ERR_MESH_NOT_FOUND, ExpectedMessage);
    TestEqual(TEXT("a refused append_triangle emits no triangle id"),
        Indices.TriangleIndex, int32(INDEX_NONE));

    GeometryOps::FDeleteTriangleParams DeleteTri;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("delete_triangle on a null mesh"),
        GeometryOps::DeleteTriangle(nullptr, DeleteTri),
        ErrorCodes::ERR_MESH_NOT_FOUND, ExpectedMessage);

    GeometryOps::FSetVertexColorParams SetColor;
    int32 VerticesModified = -1;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("set_vertex_color on a null mesh"),
        GeometryOps::SetVertexColor(nullptr, SetColor, VerticesModified),
        ErrorCodes::ERR_MESH_NOT_FOUND, ExpectedMessage);
    TestEqual(TEXT("a refused set_vertex_color reports no modified vertices"), VerticesModified, 0);

    GeometryOps::FSetUVsParams SetUVs;
    int32 ElementsModified = -1;
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("set_uvs on a null mesh"),
        GeometryOps::SetUVs(nullptr, SetUVs, ElementsModified),
        ErrorCodes::ERR_MESH_NOT_FOUND, ExpectedMessage);
    TestEqual(TEXT("a refused set_uvs reports no modified elements"), ElementsModified, 0);

    GeometryOps::FTranslateMeshParams Translate;
    Translate.Translation = FVector(0, 0, 100);
    GeometryOpsElementsTest_ExpectFailure(*this, TEXT("translate_mesh on a null mesh"),
        GeometryOps::TranslateMesh(nullptr, Translate),
        ErrorCodes::ERR_MESH_NOT_FOUND, ExpectedMessage);

    return true;
}

// ============================================================================
// EnsureMeshHasUVs must not truncate higher UV channels
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryUtilsEnsureMeshHasUVsKeepsHigherChannelsTest,
    "PinWright.Geometry.Utils.EnsureMeshHasUVsKeepsHigherUVChannels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryUtilsEnsureMeshHasUVsKeepsHigherChannelsTest::RunTest(const FString& Parameters)
{
    // The regression: EnsureMeshHasUVs used to call the bare
    // UGeometryScriptLibrary_MeshUVFunctions::SetNumUVSets(Mesh, 1, nullptr), which sets the
    // layer count EXACTLY and therefore DELETED every channel above 0. The mesh shape that
    // reaches the guard is exactly the one that loses the most: an empty channel 0 is what
    // makes MeshHasUsableUVs return false, so a mesh carrying an authored lightmap in channel
    // 1 and nothing in channel 0 went into convert_to_static_mesh with two UV sets and came
    // out with one - and MarkGeometryActorModified then committed that loss to the live actor.
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsElementsTest_NewBoxMesh());

    if (!TestTrue(TEXT("the box fixture carries an attribute set"), Mesh->GetMeshRef().HasAttributes()))
    {
        return true;
    }
    Mesh->GetMeshRef().Attributes()->SetNumUVLayers(2);

    // Populate channel 1 the way an authored lightmap would be - real elements on a real
    // layer - then empty channel 0 so the mesh is the "no usable UVs" case the guard exists
    // for. Order matters: the projection must run before channel 0 is cleared, since it is
    // the only step here that touches a layer other than the one it is told to.
    UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
        Mesh.Get(), /*UVSetIndex=*/1, FTransform::Identity, FGeometryScriptMeshSelection(),
        /*MinIslandTriCount=*/2, nullptr);
    Mesh->GetMeshRef().Attributes()->GetUVLayer(0)->ClearElements();

    const int32 LightmapElementsBefore =
        Mesh->GetMeshRef().Attributes()->GetUVLayer(1)->ElementCount();
    TestEqual(TEXT("the fixture carries two UV layers"),
        Mesh->GetMeshRef().Attributes()->NumUVLayers(), 2);
    TestEqual(TEXT("the fixture's channel 0 is empty"),
        Mesh->GetMeshRef().Attributes()->GetUVLayer(0)->ElementCount(), 0);
    if (!TestTrue(TEXT("the fixture's channel 1 is populated"), LightmapElementsBefore > 0))
    {
        return true;
    }

    TestTrue(TEXT("EnsureMeshHasUVs reports usable UVs afterwards"),
        GeometryUtils::EnsureMeshHasUVs(Mesh.Get()));

    // The guard's own job: channel 0 is now projected, so the MikkT out-of-bounds crash the
    // helper exists to prevent cannot fire.
    TestTrue(TEXT("EnsureMeshHasUVs populated channel 0"),
        Mesh->GetMeshRef().Attributes()->GetUVLayer(0)->ElementCount() > 0);

    // The regression itself: the higher channel must still be there, still populated. Checked
    // before the element count is read, because the truncating form leaves GetUVLayer(1)
    // returning null - dereferencing it would take the whole suite down with an access
    // violation instead of reporting one red test.
    if (!TestEqual(TEXT("EnsureMeshHasUVs left the UV layer count alone"),
            Mesh->GetMeshRef().Attributes()->NumUVLayers(), 2))
    {
        return true;
    }
    TestEqual(TEXT("EnsureMeshHasUVs left channel 1's elements untouched"),
        Mesh->GetMeshRef().Attributes()->GetUVLayer(1)->ElementCount(), LightmapElementsBefore);

    return true;
}
