// Copyright (c) 2026 Alexander Penkin. MIT License.

// Failure-direction guard for B-set-vertex-color-set-all-misses-appended.
//
// set_all painted the colour-overlay elements that already EXISTED, and the one call that creates
// them was guarded on an empty overlay. So the first colour write populated the layer, and every
// triangle appended after that carried no element, was unreachable by every later set_all, and
// rendered at the overlay default - while the response reported the whole mesh's vertex count as
// modified, which is what kept it invisible.
//
// The existing coverage could not see this: TestGeometrySetVertexColorPersistsToOverlay only does
// create_box -> set_all -> hasColors, on a mesh that never grows. This test grows it. The
// mid-sequence assertion is the known-bad control - after the append and BEFORE the second
// set_all, the new triangle must genuinely have no colour element, otherwise the fixture is not
// reproducing the condition and the final assertion would pass for the wrong reason.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "DynamicMeshActor.h"
#include "UDynamicMesh.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::FSinkPtr;
using GeometryTestHelpers::DestroyActorsWithLabel;
using GeometryTestHelpers::FindActorByLabel;

namespace SetVertexColorAppendTest
{
    struct FColorCoverage
    {
        bool bRead = false;
        int32 TriangleCount = 0;
        int32 TrianglesWithNoColor = 0;
        int32 ElementsOffTargetColor = 0;
    };

    // Counts triangles carrying no colour assignment, and elements whose value is not the target.
    // Read off the mesh rather than off a response, because the response is the thing that was
    // lying.
    inline FColorCoverage ReadColorCoverage(const FString& Label, const FVector4f& Target)
    {
        FColorCoverage Out;
        ADynamicMeshActor* Actor = Cast<ADynamicMeshActor>(FindActorByLabel(Label));
        if (!Actor || !Actor->GetDynamicMeshComponent() || !Actor->GetDynamicMeshComponent()->GetDynamicMesh())
        {
            return Out;
        }
        const UE::Geometry::FDynamicMesh3& Mesh =
            Actor->GetDynamicMeshComponent()->GetDynamicMesh()->GetMeshRef();
        if (!Mesh.HasAttributes() || !Mesh.Attributes()->HasPrimaryColors())
        {
            return Out;
        }
        const UE::Geometry::FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
        if (!Colors)
        {
            return Out;
        }

        Out.bRead = true;
        for (const int32 TriangleID : Mesh.TriangleIndicesItr())
        {
            ++Out.TriangleCount;
            if (!Colors->IsSetTriangle(TriangleID))
            {
                ++Out.TrianglesWithNoColor;
            }
        }
        for (const int32 ElementID : Colors->ElementIndicesItr())
        {
            const FVector4f Value = Colors->GetElement(ElementID);
            if (!Value.Equals(Target, 1e-4f))
            {
                ++Out.ElementsOffTargetColor;
            }
        }
        return Out;
    }

    inline void PaintAll(FRpcDispatcher& Dispatcher, FSinkPtr& Sink, const FString& Label,
        double R, double G, double B, bool& bOutOk, TSharedPtr<FJsonObject>& OutResult)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetBoolField(TEXT("setAll"), true);
        Params->SetNumberField(TEXT("r"), R);
        Params->SetNumberField(TEXT("g"), G);
        Params->SetNumberField(TEXT("b"), B);
        Params->SetNumberField(TEXT("a"), 1.0);
        FString ErrorCode;
        bOutOk = false;
        Dispatch(Dispatcher, Sink, TEXT("geometry.set_vertex_color"), TEXT("req-color-all"),
            Params, bOutOk, OutResult, ErrorCode);
    }
}

using SetVertexColorAppendTest::ReadColorCoverage;
using SetVertexColorAppendTest::PaintAll;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySetVertexColorReachesAppendedGeometryTest,
    "PinWright.geometry.set_vertex_color.SetAllReachesGeometryAppendedAfterTheFirstWrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySetVertexColorReachesAppendedGeometryTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world available; "
                        "set_vertex_color appended-geometry assertions did not run"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_ColorAppendProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
    CreateParams->SetStringField(TEXT("name"), Label);
    bool bOk = false;
    FString Err;
    Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"), TEXT("req-color-create"),
        CreateParams, bOk, Err);
    if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bOk))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    // First colour write. This is what populates the overlay and, before the fix, froze its width.
    TSharedPtr<FJsonObject> FirstResult;
    PaintAll(Dispatcher, Sink, Label, 1.0, 0.0, 0.0, bOk, FirstResult);
    if (!TestTrue(TEXT("the first set_all succeeded"), bOk))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    // Grow the mesh. append_triangle is the least ambiguous grower available - one triangle, three
    // brand-new vertices, no colour overlay of its own - and it stands in for what
    // extrude_along_spline, sweep, bevel, shell and boolean all do to a coloured mesh.
    TSharedPtr<FJsonObject> AppendParams = MakeShared<FJsonObject>();
    AppendParams->SetStringField(TEXT("actorName"), Label);
    bOk = false;
    Dispatch(Dispatcher, Sink, TEXT("geometry.append_triangle"), TEXT("req-color-append"),
        AppendParams, bOk, Err);
    if (!TestTrue(TEXT("geometry.append_triangle grew the probe mesh"), bOk))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    // The known-bad control. The appended triangle must genuinely arrive with no colour element -
    // that is the condition this defect lives in. If the engine ever starts colouring appended
    // triangles, this fixture stops reproducing the case and must say so rather than pass.
    const SetVertexColorAppendTest::FColorCoverage Mid =
        ReadColorCoverage(Label, FVector4f(1.f, 0.f, 0.f, 1.f));
    if (!TestTrue(TEXT("the probe's colour overlay could be read after the append"), Mid.bRead) ||
        !TestTrue(TEXT("the appended triangle arrives with no colour element, which is the "
                       "condition under test"), Mid.TrianglesWithNoColor > 0))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    // Second colour write, to a different colour so a stale first-write element is distinguishable
    // from a correctly repainted one.
    TSharedPtr<FJsonObject> SecondResult;
    PaintAll(Dispatcher, Sink, Label, 0.0, 1.0, 0.0, bOk, SecondResult);

    const SetVertexColorAppendTest::FColorCoverage After =
        ReadColorCoverage(Label, FVector4f(0.f, 1.f, 0.f, 1.f));
    DestroyActorsWithLabel(Label);

    if (!TestTrue(TEXT("the second set_all succeeded"), bOk) ||
        !TestTrue(TEXT("the probe's colour overlay could be read after the second write"), After.bRead))
    {
        return false;
    }

    // The assertion the ticket is about.
    TestEqual(FString::Printf(
        TEXT("set_all must reach every triangle, including the %d appended after the first write"),
        Mid.TrianglesWithNoColor),
        After.TrianglesWithNoColor, 0);
    TestEqual(TEXT("every colour element carries the colour that was asked for"),
        After.ElementsOffTargetColor, 0);

    // The response has to show the layer was widened, because that is the only in-band signal a
    // caller has that an earlier set_all had left geometry unpainted.
    double ElementsCreated = -1.0;
    if (TestTrue(TEXT("the response reports colorElementsCreated"),
            SecondResult.IsValid() &&
            SecondResult->TryGetNumberField(TEXT("colorElementsCreated"), ElementsCreated)))
    {
        TestTrue(TEXT("growing the layer is reported, not silent"), ElementsCreated > 0.0);
    }

    // And the count must not be the whole-mesh number it used to be regardless of what was
    // reached. A box plus a loose triangle has more vertices than the box alone, so a stale
    // implementation reporting VertexCount() would still be a number - it is the FIRST write that
    // pins the lie down, where the loop reached everything and the count happened to be right.
    double VerticesModified = -1.0;
    if (SecondResult.IsValid() &&
        SecondResult->TryGetNumberField(TEXT("verticesModified"), VerticesModified))
    {
        TestTrue(TEXT("verticesModified reports painted vertices, and is positive"),
            VerticesModified > 0.0);
    }
    return true;
}
