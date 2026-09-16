// Copyright (c) 2026 Alexander Penkin. MIT License.

// Failure-direction guard for B-remesh-size-param-ignored, carried forward onto the
// surviving verb.
//
// The two verbs the ticket named (quadrangulate, remesh_voxel) were placeholders that ran a
// fixed `max(100, TrisBefore/2)` remesh and never read their density parameter at all; both were
// deleted in the RPC audit that removed 170 dead methods, so there is nothing left to repair in
// them. geometry.remesh_uniform is the retopology verb that survived, and it DOES read its size
// control - but nothing failed if that stopped being true. The sibling test
// (.EchoMeshCounts) asserts the response carries an achieved count; a handler that ignored the
// request and remeshed to a constant would satisfy it perfectly, because a constant is still a
// non-zero achieved count.
//
// So this test does the one thing that separates "honoured" from "echoed": run the SAME fixture
// at two different requested densities and require the achieved triangle counts to differ in the
// requested direction. Hardcode the density again - to TrisBefore/2 or anything else independent
// of the request - and the two runs converge on the same number and this test fails.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

namespace
{
    // Remeshes a freshly spawned box probe at the requested budget and returns the ACHIEVED
    // triangle count off the response. Returns -1 if any step failed, so the caller can
    // distinguish a broken fixture from a converged pair.
    int32 RemeshBoxToBudget(FAutomationTestBase& Test, int32 RequestedTriangles, const FString& Label)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-remesh-density-create"), CreateParams, bCreated, CreateErr);
        if (!Test.TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            return -1;
        }

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetNumberField(TEXT("targetTriangleCount"), RequestedTriangles);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.remesh_uniform"),
            TEXT("req-remesh-density"), Params, bSuccess, Result, ErrorCode);

        if (!Test.TestTrue(TEXT("geometry.remesh_uniform succeeded"), bSuccess) || !Result.IsValid())
        {
            return -1;
        }

        double Achieved = 0.0;
        if (!Test.TestTrue(TEXT("response carries the achieved triangleCount"),
                Result->TryGetNumberField(TEXT("triangleCount"), Achieved)))
        {
            return -1;
        }
        return static_cast<int32>(Achieved);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryRemeshUniformHonoursDensityTest,
    "PinWright.geometry.remesh_uniform.DensityRequestChangesTheOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryRemeshUniformHonoursDensityTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world available; "
                        "remesh_uniform density-differential assertions did not run"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString CoarseLabel = FString::Printf(TEXT("PW_RemeshDensityCoarse_%s"), *Suffix);
    const FString FineLabel = FString::Printf(TEXT("PW_RemeshDensityFine_%s"), *Suffix);

    const int32 CoarseAchieved = RemeshBoxToBudget(*this, 200, CoarseLabel);
    const int32 FineAchieved = RemeshBoxToBudget(*this, 6000, FineLabel);

    DestroyActorsWithLabel(CoarseLabel);
    DestroyActorsWithLabel(FineLabel);

    if (CoarseAchieved < 0 || FineAchieved < 0)
    {
        return false;
    }

    // Both runs produced real geometry - a pair of zeroes would also be "equal" and must not be
    // read as the parameter being honoured.
    TestTrue(TEXT("the coarse request produced triangles"), CoarseAchieved > 0);
    TestTrue(TEXT("the fine request produced triangles"), FineAchieved > 0);

    // The assertion the ticket is about. Uniform remesh only APPROXIMATES a budget (it targets an
    // edge length derived from it), so this deliberately asserts the ORDER and not the value:
    // 6000 must land above 200. A density that ignores the request lands both runs on the same
    // number and fails here.
    TestTrue(FString::Printf(
        TEXT("a 6000-triangle request must retopologize denser than a 200-triangle one "
             "(coarse=%d, fine=%d); equal counts mean the requested density reached nothing"),
        CoarseAchieved, FineAchieved),
        FineAchieved > CoarseAchieved);
    return true;
}
