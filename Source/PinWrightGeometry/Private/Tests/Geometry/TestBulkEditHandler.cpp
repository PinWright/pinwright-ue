// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for geometry.append_buffers - bulk vertex + indexed-triangle upload into
// a DynamicMeshActor working seed. Exercises the happy path (a tetrahedron: 4
// vertices, 4 triangles) plus the typed-rejection contracts: triangle index out of
// range, an optional-array count mismatch, an unknown wire parameter, and a missing
// target actor. Pure mesh data, so no RHI is needed.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

#include "Tests/TestUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

// File-local JSON builders. Uniquely named (BulkBuf* prefix) so a Unity build
// merging this file's anonymous namespace with a sibling test's cannot ODR-collide.
namespace
{
    TSharedPtr<FJsonValue> BulkBufVec3(double X, double Y, double Z)
    {
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(X));
        A.Add(MakeShared<FJsonValueNumber>(Y));
        A.Add(MakeShared<FJsonValueNumber>(Z));
        return MakeShared<FJsonValueArray>(A);
    }

    TSharedPtr<FJsonValue> BulkBufTri(int32 I0, int32 I1, int32 I2)
    {
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(I0));
        A.Add(MakeShared<FJsonValueNumber>(I1));
        A.Add(MakeShared<FJsonValueNumber>(I2));
        return MakeShared<FJsonValueArray>(A);
    }

    // Four corners of a tetrahedron (mesh-local space).
    TArray<TSharedPtr<FJsonValue>> BulkBufTetraVertices()
    {
        TArray<TSharedPtr<FJsonValue>> V;
        V.Add(BulkBufVec3(0.0, 0.0, 0.0));
        V.Add(BulkBufVec3(100.0, 0.0, 0.0));
        V.Add(BulkBufVec3(50.0, 100.0, 0.0));
        V.Add(BulkBufVec3(50.0, 50.0, 100.0));
        return V;
    }

    // The four outward-wound faces of the tetrahedron above (a closed manifold).
    TArray<TSharedPtr<FJsonValue>> BulkBufTetraTriangles()
    {
        TArray<TSharedPtr<FJsonValue>> T;
        T.Add(BulkBufTri(0, 1, 2));
        T.Add(BulkBufTri(0, 3, 1));
        T.Add(BulkBufTri(0, 2, 3));
        T.Add(BulkBufTri(1, 3, 2));
        return T;
    }

    // Spawn an empty procedural-mesh DynamicMeshActor labeled Label. Returns true
    // on success (the seed the append_buffers cases build onto).
    bool BulkBufCreateSeed(const FString& Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Label);
        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("geometry.create_procedural_mesh"), Params, Capture))
        {
            return false;
        }
        return Capture.bSuccess;
    }
}

// ============================================================================
// Happy path: append a tetrahedron and confirm the echoed counts.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAppendBuffersAppendsTetrahedronTest,
    "PinWright.geometry.append_buffers.AppendsTetrahedron",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAppendBuffersAppendsTetrahedronTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping append_buffers tetrahedron test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_BulkTetra_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    if (!TestTrue(TEXT("seed procedural mesh created"), BulkBufCreateSeed(Label)))
    {
        DestroyActorsWithLabel(Label);
        return true;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetArrayField(TEXT("vertices"), BulkBufTetraVertices());
    Params->SetArrayField(TEXT("triangles"), BulkBufTetraTriangles());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("geometry.append_buffers"), Params, Capture);
    TestTrue(TEXT("append_buffers handler registered"), bFound);
    TestTrue(TEXT("append_buffers succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double AppendedV = 0.0, AppendedT = 0.0, TotalV = 0.0, TotalT = 0.0;
        Capture.Result->TryGetNumberField(TEXT("appendedVertices"), AppendedV);
        Capture.Result->TryGetNumberField(TEXT("appendedTriangles"), AppendedT);
        Capture.Result->TryGetNumberField(TEXT("totalVertices"), TotalV);
        Capture.Result->TryGetNumberField(TEXT("totalTriangles"), TotalT);
        TestEqual(TEXT("appendedVertices == 4"), static_cast<int32>(AppendedV), 4);
        TestEqual(TEXT("appendedTriangles == 4"), static_cast<int32>(AppendedT), 4);
        TestEqual(TEXT("totalVertices == 4"), static_cast<int32>(TotalV), 4);
        TestEqual(TEXT("totalTriangles == 4"), static_cast<int32>(TotalT), 4);
    }

    DestroyActorsWithLabel(Label);
    return true;
}

// ============================================================================
// A triangle index outside [0, numVerts) is a typed INVALID_PARAMS rejection.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAppendBuffersIndexOutOfRangeTest,
    "PinWright.geometry.append_buffers.IndexOutOfRangeRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAppendBuffersIndexOutOfRangeTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping append_buffers index-range test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_BulkOOR_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    if (!TestTrue(TEXT("seed procedural mesh created"), BulkBufCreateSeed(Label)))
    {
        DestroyActorsWithLabel(Label);
        return true;
    }

    // 4 vertices, but a triangle references index 4 (valid range is [0, 4)).
    TArray<TSharedPtr<FJsonValue>> Tris;
    Tris.Add(BulkBufTri(0, 1, 4));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetArrayField(TEXT("vertices"), BulkBufTetraVertices());
    Params->SetArrayField(TEXT("triangles"), Tris);

    FTestResponseCapture Capture;
    TestTrue(TEXT("append_buffers handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.append_buffers"), Params, Capture));
    TestFalse(TEXT("out-of-range index is rejected, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("out-of-range index -> INVALID_PARAMS"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));

    DestroyActorsWithLabel(Label);
    return true;
}

// ============================================================================
// An optional array present with the wrong count (normals != vertex count) is a
// typed INVALID_PARAMS rejection.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAppendBuffersNormalsMismatchTest,
    "PinWright.geometry.append_buffers.NormalsCountMismatchRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAppendBuffersNormalsMismatchTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping append_buffers normals-mismatch test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_BulkNormMismatch_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    if (!TestTrue(TEXT("seed procedural mesh created"), BulkBufCreateSeed(Label)))
    {
        DestroyActorsWithLabel(Label);
        return true;
    }

    // 4 vertices but only 3 normals -> count mismatch.
    TArray<TSharedPtr<FJsonValue>> Normals;
    Normals.Add(BulkBufVec3(0.0, 0.0, 1.0));
    Normals.Add(BulkBufVec3(0.0, 0.0, 1.0));
    Normals.Add(BulkBufVec3(0.0, 0.0, 1.0));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetArrayField(TEXT("vertices"), BulkBufTetraVertices());
    Params->SetArrayField(TEXT("triangles"), BulkBufTetraTriangles());
    Params->SetArrayField(TEXT("normals"), Normals);

    FTestResponseCapture Capture;
    TestTrue(TEXT("append_buffers handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.append_buffers"), Params, Capture));
    TestFalse(TEXT("normals-count mismatch is rejected, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("normals-count mismatch -> INVALID_PARAMS"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));

    DestroyActorsWithLabel(Label);
    return true;
}

// ============================================================================
// An unknown wire parameter is rejected by the dispatcher's param validation
// (UNKNOWN_PARAMS). Routed through the real dispatcher because that check lives in
// ValidateHandlerParams, upstream of the handler body. Required params are supplied
// so validation reaches the unknown-param branch rather than the missing-param one.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAppendBuffersUnknownArgTest,
    "PinWright.geometry.append_buffers.UnknownArgRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAppendBuffersUnknownArgTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), TEXT("PW_BulkUnknownArgProbe"));
    Params->SetArrayField(TEXT("vertices"), BulkBufTetraVertices());
    Params->SetArrayField(TEXT("triangles"), BulkBufTetraTriangles());
    Params->SetBoolField(TEXT("bogusParam"), true);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("geometry.append_buffers"),
        TEXT("req-bulk-unknown-arg"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown parameter is rejected, not a fake success"), bSuccess);
    TestEqual(TEXT("unknown parameter -> UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

// ============================================================================
// A missing target actor is a typed ACTOR_NOT_FOUND rejection.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAppendBuffersTargetNotFoundTest,
    "PinWright.geometry.append_buffers.TargetNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAppendBuffersTargetNotFoundTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping append_buffers not-found test"));
        return true;
    }

    // A unique name no actor carries; valid geometry so validation passes and the
    // finder is the layer that rejects.
    const FString MissingLabel = FString::Printf(TEXT("PW_BulkNoSuchActor_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), MissingLabel);
    Params->SetArrayField(TEXT("vertices"), BulkBufTetraVertices());
    Params->SetArrayField(TEXT("triangles"), BulkBufTetraTriangles());

    FTestResponseCapture Capture;
    TestTrue(TEXT("append_buffers handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.append_buffers"), Params, Capture));
    TestFalse(TEXT("missing target is rejected, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("missing target -> ACTOR_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    return true;
}
