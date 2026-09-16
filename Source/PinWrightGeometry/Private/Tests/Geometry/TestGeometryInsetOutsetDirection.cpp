// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-geometry-inset-outset-direction-swapped.
//
// geometry.inset (wiki: "shrink inward") used to pass Options.Distance = -Distance
// and geometry.outset (wiki: "expand outward") Options.Distance = +Distance. But the
// engine's FGeometryScriptMeshInsetOutsetFacesOptions::Distance convention is the
// opposite: POSITIVE Distance insets inward. FInsetMeshRegion forces the inset
// direction toward the region centroid and moves the boundary to
// Midpoint + Distance*InsetDir (PolyEditingEdgeUtil), so a positive Distance shrinks
// the footprint and a negative one grows it. Both handlers therefore did the
// geometric OPPOSITE of their name: a face-targeted inset GREW the bounding box
// outward and a face-targeted outset left the outer footprint unchanged (pulled the
// inner face in). The op reported success with identical vert/tri deltas either way,
// so only the bounding-box extent revealed the wrong direction.
//
// The fix swaps the signs: inset now passes +Distance (inward), outset -Distance
// (outward).
//
// Strategy (exercises the production handlers end-to-end via the real dispatcher):
//   1. Spawn a real 400x300x20 box DynamicMeshActor via geometry.create_box and read
//      its baseline local-space boundingBox extent (half-extents ~200/150/10).
//   2. Face-target the +Z (top) face with faceDirection {0,0,1} and inset by 35;
//      assert a real face was selected (facesSelected >= 1) and the XY extent did NOT
//      grow (an inset must not push the boundary outward).
//   3. On a fresh identical box, outset the +Z face by 35; assert the XY extent GREW
//      by ~the distance (an outset must push the boundary outward).
//
// Counterfactual: reverting the sign swap makes inset grow the extent (~+35) and
// outset leave it unchanged, failing both the "inset did not grow" and the "outset
// grew" assertions below.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryInsetOutsetDirectionTest,
    "PinWright.geometry.inset_outset.DirectionMatchesName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryInsetOutsetDirectionTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping inset/outset direction test"));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Spawn a 400x300x20 box (depth->Z, so faceDirection {0,0,1} targets the large
    // +Z face) and return its label. Empty on failure.
    auto SpawnBox = [&Dispatcher, &Sink, this](const FString& Tag) -> FString
    {
        const FString Label = FString::Printf(TEXT("PW_InsetDirProbe_%s_%s"),
            *Tag, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Label);
        Params->SetNumberField(TEXT("width"), 400.0);
        Params->SetNumberField(TEXT("height"), 300.0);
        Params->SetNumberField(TEXT("depth"), 20.0);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-insetdir-create-") + Tag, Params, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the ") + Tag + TEXT(" probe box"), bCreated))
        {
            return FString();
        }
        return Label;
    };

    // Read the local-space boundingBox extent (half-extents) via get_mesh_info.
    auto ReadExtent = [&Dispatcher, &Sink, this]
        (const FString& Label, const FString& Tag, double& OutX, double& OutY) -> bool
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.get_mesh_info"),
            TEXT("req-insetdir-info-") + Tag, Params, bSuccess, Result, ErrorCode);
        const TSharedPtr<FJsonObject>* BBox = nullptr;
        const TSharedPtr<FJsonObject>* Extent = nullptr;
        if (!TestTrue(Tag + TEXT(" get_mesh_info succeeded"), bSuccess) ||
            !TestTrue(Tag + TEXT(" get_mesh_info carries a result"), Result.IsValid()) ||
            !TestTrue(Tag + TEXT(" get_mesh_info has a boundingBox"),
                Result.IsValid() && Result->TryGetObjectField(TEXT("boundingBox"), BBox)) ||
            !TestTrue(Tag + TEXT(" boundingBox has an extent"),
                BBox && (*BBox)->TryGetObjectField(TEXT("extent"), Extent)))
        {
            return false;
        }
        return (*Extent)->TryGetNumberField(TEXT("x"), OutX)
            && (*Extent)->TryGetNumberField(TEXT("y"), OutY);
    };

    // Run inset/outset on the +Z face and assert a real face was targeted.
    auto RunFaceOp = [&Dispatcher, &Sink, this]
        (const FString& Method, const FString& Label) -> bool
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetNumberField(TEXT("distance"), 35.0);
        TSharedPtr<FJsonObject> Dir = MakeShared<FJsonObject>();
        Dir->SetNumberField(TEXT("x"), 0.0);
        Dir->SetNumberField(TEXT("y"), 0.0);
        Dir->SetNumberField(TEXT("z"), 1.0);
        Params->SetObjectField(TEXT("faceDirection"), Dir);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, Method,
            TEXT("req-insetdir-op-") + Method, Params, bSuccess, Result, ErrorCode);
        if (!TestTrue(Method + TEXT(" succeeded"), bSuccess) ||
            !TestTrue(Method + TEXT(" carries a result"), Result.IsValid()))
        {
            return false;
        }
        // A real +Z face must have been selected — otherwise the op is a whole-mesh
        // no-op and the direction below would be untested.
        double FacesSelected = 0.0;
        Result->TryGetNumberField(TEXT("facesSelected"), FacesSelected);
        const bool bTargeted = TestTrue(Method + TEXT(" targeted a real face (facesSelected >= 1)"),
            FacesSelected >= 1.0);

        // ...and the op must have actually MOVED geometry. The inset arm below can only
        // assert that the footprint did not GROW — a correct top-face inset leaves the outer
        // bounding box exactly where it was, because the side faces still reach the original
        // extents — so an inset gutted to a no-op satisfies that assertion just as well as a
        // working one. `changed` is FinishOp's count-delta verdict, republished by
        // ReportMeshChange (MeshOpsHandler.cpp); ApplyMeshInsetOutsetFaces adds a ring of
        // vertices in BOTH directions, so a real op reports true and a no-op reports false.
        bool bChanged = false;
        Result->TryGetBoolField(TEXT("changed"), bChanged);
        const bool bMoved = TestTrue(
            Method + TEXT(" changed the mesh (an op gutted to a no-op passes the extent checks)"),
            bChanged);

        return bTargeted && bMoved;
    };

    // ---- INSET must NOT grow the footprint ----
    const FString InsetLabel = SpawnBox(TEXT("inset"));
    bool bInsetChecked = false;
    double InsetBaseX = 0.0, InsetBaseY = 0.0, InsetX = 0.0, InsetY = 0.0;
    if (!InsetLabel.IsEmpty()
        && ReadExtent(InsetLabel, TEXT("inset-baseline"), InsetBaseX, InsetBaseY)
        && RunFaceOp(TEXT("geometry.inset"), InsetLabel)
        && ReadExtent(InsetLabel, TEXT("inset-after"), InsetX, InsetY))
    {
        // With the sign bug inset passed -35 and the extent grew by ~35 (200 -> 235).
        // A correct inset moves the top-face boundary inward, leaving the outer
        // footprint at or below the baseline half-extent.
        TestTrue(TEXT("inset did NOT grow the X extent (would grow ~+35 with the sign bug)"),
            InsetX <= InsetBaseX + 1.0);
        TestTrue(TEXT("inset did NOT grow the Y extent (would grow ~+35 with the sign bug)"),
            InsetY <= InsetBaseY + 1.0);
        bInsetChecked = true;
    }
    GeometryTestHelpers::DestroyActorsWithLabel(InsetLabel);

    // ---- OUTSET must grow the footprint ----
    const FString OutsetLabel = SpawnBox(TEXT("outset"));
    double OutsetBaseX = 0.0, OutsetBaseY = 0.0, OutsetX = 0.0, OutsetY = 0.0;
    if (!OutsetLabel.IsEmpty()
        && ReadExtent(OutsetLabel, TEXT("outset-baseline"), OutsetBaseX, OutsetBaseY)
        && RunFaceOp(TEXT("geometry.outset"), OutsetLabel)
        && ReadExtent(OutsetLabel, TEXT("outset-after"), OutsetX, OutsetY))
    {
        // With the sign bug outset passed +35 and the extent stayed unchanged. A
        // correct outset moves the boundary outward, growing the half-extent by ~35.
        TestTrue(TEXT("outset grew the X extent by ~the distance (stays unchanged with the sign bug)"),
            OutsetX >= OutsetBaseX + 30.0);
        TestTrue(TEXT("outset grew the Y extent by ~the distance (stays unchanged with the sign bug)"),
            OutsetY >= OutsetBaseY + 30.0);

        // Direct swap catch: the outset footprint must end up strictly larger than the
        // inset footprint. If the verbs are swapped this inverts.
        if (bInsetChecked)
        {
            TestTrue(TEXT("outset footprint is larger than inset footprint (X)"), OutsetX > InsetX + 1.0);
        }
    }
    GeometryTestHelpers::DestroyActorsWithLabel(OutsetLabel);

    return true;
}
