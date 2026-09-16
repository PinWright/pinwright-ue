// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-set-vertex-color-wrong-channel-not-persisted.
//
// geometry.set_vertex_color used to enable and write the LEGACY FDynamicMesh3 per-vertex
// color buffer (EnableVertexColors/SetVertexColor) — a storage location distinct from the
// mesh attribute-overlay PrimaryColors channel that get_mesh_info's GetHasVertexColors, the
// DynamicMeshComponent renderer, and the convert_to_static_mesh bake all read. So the verb
// returned verticesModified success while get_mesh_info's hasColors stayed false and the
// tint was dropped everywhere it mattered — a silent false-success.
//
// The fix writes the attribute-overlay PrimaryColors channel instead (EnableAttributes +
// EnablePrimaryColors, then seed/set overlay elements). After it, get_mesh_info reports
// hasColors:true on the same actor the write targeted.
//
// Strategy (exercises the production handlers end-to-end via the real dispatcher):
//   1. Spawn a real DynamicMeshActor via geometry.create_box — the fixture is built
//      in-code by the procedural PrimitiveHandler, so it loads no external content asset.
//   2. get_mesh_info -> assert baseline hasColors == false (a fresh box has no colors).
//   3. set_vertex_color {setAll:true, r/g/b/a} -> assert success + verticesModified >= 1.
//   4. get_mesh_info -> assert hasColors == true (the overlay now carries the color).
//
// Counterfactual: reverting the fix (back to the legacy EnableVertexColors/SetVertexColor
// buffer) leaves Attributes()->PrimaryColors() null, so GetHasVertexColors stays false and
// step 4's hasColors==true assertion fails while set_vertex_color still reports success.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySetVertexColorPersistsToOverlayTest,
    "PinWright.geometry.set_vertex_color.PersistsToAttributeOverlay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySetVertexColorPersistsToOverlayTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping set_vertex_color overlay test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_VertexColorProbe_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // 1. Spawn a real DynamicMeshActor (a box has a known non-zero vertex/triangle count
    // and no vertex colors by default).
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-vcolor-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            GeometryTestHelpers::DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // Read get_mesh_info and return its hasColors flag (via bOutFound whether the call/field
    // were present) so we can compare baseline vs post-write on the same reader path.
    auto ReadHasColors = [&Dispatcher, &Sink, &Label](const FString& RequestId, bool& bOutFound) -> bool
    {
        bOutFound = false;
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.get_mesh_info"), RequestId, Params,
            bSuccess, Result, ErrorCode);
        if (!bSuccess || !Result.IsValid())
        {
            return false;
        }
        bool bHasColors = false;
        bOutFound = Result->TryGetBoolField(TEXT("hasColors"), bHasColors);
        return bHasColors;
    };

    // 2. Baseline: a fresh box carries no vertex colors — get_mesh_info reports false.
    bool bBaselineFound = false;
    const bool bBaselineHasColors = ReadHasColors(TEXT("req-vcolor-info-before"), bBaselineFound);
    if (!TestTrue(TEXT("baseline get_mesh_info reports a hasColors field"), bBaselineFound))
    {
        GeometryTestHelpers::DestroyActorsWithLabel(Label);
        return true;
    }
    TestFalse(TEXT("fresh box has no vertex colors before set_vertex_color"), bBaselineHasColors);

    // 3. Color every vertex. The verb has always reported success here; the bug was that the
    // color went to the legacy buffer, not the overlay the readers consume.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetBoolField(TEXT("setAll"), true);
        Params->SetNumberField(TEXT("r"), 0.82);
        Params->SetNumberField(TEXT("g"), 0.45);
        Params->SetNumberField(TEXT("b"), 0.18);
        Params->SetNumberField(TEXT("a"), 1.0);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.set_vertex_color"),
            TEXT("req-vcolor-setall"), Params, bSuccess, Result, ErrorCode);
        if (!TestTrue(TEXT("geometry.set_vertex_color succeeded"), bSuccess) ||
            !TestTrue(TEXT("geometry.set_vertex_color carries a result object"), Result.IsValid()))
        {
            GeometryTestHelpers::DestroyActorsWithLabel(Label);
            return true;
        }
        double VerticesModified = 0.0;
        TestTrue(TEXT("set_vertex_color echoes a verticesModified count"),
            Result->TryGetNumberField(TEXT("verticesModified"), VerticesModified));
        TestTrue(TEXT("set_vertex_color modified at least one vertex"), VerticesModified >= 1.0);
    }

    // 4. The overlay must now be populated: get_mesh_info's GetHasVertexColors (which checks
    // Attributes()->PrimaryColors()) reports true. This is the assertion the wrong-channel
    // write fails — it left PrimaryColors null and hasColors false.
    bool bAfterFound = false;
    const bool bAfterHasColors = ReadHasColors(TEXT("req-vcolor-info-after"), bAfterFound);
    TestTrue(TEXT("get_mesh_info reports a hasColors field after set_vertex_color"), bAfterFound);
    TestTrue(TEXT("set_vertex_color persisted color to the attribute overlay (hasColors flips true)"),
        bAfterHasColors);

    GeometryTestHelpers::DestroyActorsWithLabel(Label);
    return true;
}
