// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Geometry domain handlers.
// Covers all 8 handler files: AdvancedMeshOpsHandler, BooleanHandler, GeometryTransformHandler,
// LODCollisionHandler, MeshInfoHandler, MeshOpsHandler, PrimitiveHandler, SplineHandler.
// Strategy: MissingRequiredParam tests exercise early-exit validation paths;
//           ValidParamsNoCrash tests verify handlers are callable with realistic inputs.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Utils/JsonBuilders.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "DynamicMeshActor.h"
#include "UDynamicMesh.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SplineComponent.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include <limits>


// ============================================================================
// AdvancedMeshOpsHandler — geometry.bridge
// Required: actorName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBridgeValidParamsNoCrashTest,
    "PinWright.geometry.bridge.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBridgeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("TestMeshActor"));
    Payload->SetNumberField(TEXT("edgeGroupA"), 0);
    Payload->SetNumberField(TEXT("edgeGroupB"), 1);
    // No `subdivisions` here: bridge does not declare it, so the dispatcher would reject the
    // payload with UNKNOWN_PARAMS before the handler ran.
    TestTrue(TEXT("geometry.bridge handler found and invoked"), InvokeHandler(TEXT("geometry.bridge"), Payload));
    return true;
}

// ============================================================================
// AdvancedMeshOpsHandler — geometry.sweep
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySweepValidParamsNoCrashTest,
    "PinWright.geometry.sweep.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySweepValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("TestSweepMesh"));
    TestTrue(TEXT("geometry.sweep handler found and invoked"), InvokeHandler(TEXT("geometry.sweep"), Payload));
    return true;
}

// ============================================================================
// BooleanHandler — geometry.boolean_union
// Required: targetActor, toolActor
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanUnionValidParamsNoCrashTest,
    "PinWright.geometry.boolean_union.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBooleanUnionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("targetActor"), TEXT("TargetMesh"));
    Payload->SetStringField(TEXT("toolActor"), TEXT("ToolMesh"));
    Payload->SetBoolField(TEXT("keepTool"), true);
    TestTrue(TEXT("geometry.boolean_union handler found and invoked"), InvokeHandler(TEXT("geometry.boolean_union"), Payload));
    return true;
}

// ============================================================================
// GeometryTransformHandler — geometry.mirror
// Required: actorName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMirrorValidParamsNoCrashTest,
    "PinWright.geometry.mirror.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryMirrorValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("TestMeshActor"));
    Payload->SetStringField(TEXT("axis"), TEXT("X"));
    Payload->SetBoolField(TEXT("weld"), true);
    TestTrue(TEXT("geometry.mirror handler found and invoked"), InvokeHandler(TEXT("geometry.mirror"), Payload));
    return true;
}

// ============================================================================
// GeometryTransformHandler — geometry.array_linear
// Required: actorName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryArrayLinearValidParamsNoCrashTest,
    "PinWright.geometry.array_linear.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryArrayLinearValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("TestMeshActor"));
    Payload->SetNumberField(TEXT("count"), 3);

    TSharedPtr<FJsonObject> OffsetObj = MakeShared<FJsonObject>();
    OffsetObj->SetNumberField(TEXT("x"), 100.0);
    OffsetObj->SetNumberField(TEXT("y"), 0.0);
    OffsetObj->SetNumberField(TEXT("z"), 0.0);
    Payload->SetObjectField(TEXT("offset"), OffsetObj);

    TestTrue(TEXT("geometry.array_linear handler found and invoked"), InvokeHandler(TEXT("geometry.array_linear"), Payload));
    return true;
}

// ============================================================================
// MeshInfoHandler — geometry.get_mesh_info
// Required: actorName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryGetMeshInfoValidParamsNoCrashTest,
    "PinWright.geometry.get_mesh_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryGetMeshInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("TestMeshActor"));
    TestTrue(TEXT("geometry.get_mesh_info handler found and invoked"), InvokeHandler(TEXT("geometry.get_mesh_info"), Payload));
    return true;
}

// ============================================================================
// MeshOpsHandler — geometry.recalculate_normals
// Required: actorName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryRecalculateNormalsValidParamsNoCrashTest,
    "PinWright.geometry.recalculate_normals.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryRecalculateNormalsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("TestMeshActor"));
    Payload->SetBoolField(TEXT("areaWeighted"), true);
    // Was `splitAngle`, which this verb never read: RecomputeNormals preserves hard edges and
    // takes no angle. `angleWeighted` is the second flag the engine options struct actually has.
    Payload->SetBoolField(TEXT("angleWeighted"), true);
    TestTrue(TEXT("geometry.recalculate_normals handler found and invoked"), InvokeHandler(TEXT("geometry.recalculate_normals"), Payload));
    return true;
}

// ============================================================================
// MeshOpsHandler — geometry.auto_uv
// Required: actorName; optional uvChannel (default 0)
// ============================================================================

// Regression for E-geometry-auto-uv-redundant-with-unwrap-uv: auto_uv used to
// hardcode UV channel 0 and expose only `actorName`, forcing a comparison-read of
// unwrap_uv's param list to reach a non-zero channel. The fix converged the two —
// auto_uv now exposes the same optional `uvChannel`, and its summary cross-references
// unwrap_uv so the discovery surface stops presenting two indistinguishable XAtlas
// verbs. This test introspects the production registration; reverting the fix
// (dropping `uvChannel` or the cross-reference) fails it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAutoUvExposesChannelAndCrossRefTest,
    "PinWright.geometry.auto_uv.ExposesChannelAndCrossRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAutoUvExposesChannelAndCrossRefTest::RunTest(const FString& Parameters)
{
    // auto_uv must expose a uvChannel param so it converges with unwrap_uv
    // (no need to comparison-read to find the channel-aware variant).
    TestNotNull(TEXT("geometry.auto_uv exposes a uvChannel param"),
        GetRegisteredParamSpec(TEXT("geometry.auto_uv"), TEXT("uvChannel")));

    // Its summary must point at unwrap_uv so the two stop reading as indistinguishable
    // synonyms at the discovery surface.
    const FString Summary = GetRegisteredSummary(TEXT("geometry.auto_uv"));
    TestTrue(TEXT("geometry.auto_uv summary references unwrap_uv"),
        Summary.Contains(TEXT("unwrap_uv")));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_box
// All params optional (all have defaults)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateBoxValidParamsNoCrashTest,
    "PinWright.geometry.create_box.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateBoxValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestBox"));
    Payload->SetNumberField(TEXT("width"), 100.0);
    Payload->SetNumberField(TEXT("height"), 100.0);
    Payload->SetNumberField(TEXT("depth"), 100.0);
    TestTrue(TEXT("geometry.create_box handler found and invoked"), InvokeHandler(TEXT("geometry.create_box"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_sphere
// All params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateSphereValidParamsNoCrashTest,
    "PinWright.geometry.create_sphere.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateSphereValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestSphere"));
    Payload->SetNumberField(TEXT("radius"), 50.0);
    Payload->SetNumberField(TEXT("subdivisions"), 16.0);
    TestTrue(TEXT("geometry.create_sphere handler found and invoked"), InvokeHandler(TEXT("geometry.create_sphere"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_cylinder
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateCylinderValidParamsNoCrashTest,
    "PinWright.geometry.create_cylinder.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateCylinderValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestCylinder"));
    Payload->SetNumberField(TEXT("radius"), 50.0);
    Payload->SetNumberField(TEXT("height"), 100.0);
    Payload->SetNumberField(TEXT("segments"), 16.0);
    TestTrue(TEXT("geometry.create_cylinder handler found and invoked"), InvokeHandler(TEXT("geometry.create_cylinder"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_cone
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateConeValidParamsNoCrashTest,
    "PinWright.geometry.create_cone.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateConeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestCone"));
    Payload->SetNumberField(TEXT("baseRadius"), 50.0);
    Payload->SetNumberField(TEXT("topRadius"), 0.0);
    Payload->SetNumberField(TEXT("height"), 100.0);
    TestTrue(TEXT("geometry.create_cone handler found and invoked"), InvokeHandler(TEXT("geometry.create_cone"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_capsule
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateCapsuleValidParamsNoCrashTest,
    "PinWright.geometry.create_capsule.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateCapsuleValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestCapsule"));
    Payload->SetNumberField(TEXT("radius"), 50.0);
    Payload->SetNumberField(TEXT("length"), 100.0);
    TestTrue(TEXT("geometry.create_capsule handler found and invoked"), InvokeHandler(TEXT("geometry.create_capsule"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_torus
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateTorusValidParamsNoCrashTest,
    "PinWright.geometry.create_torus.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateTorusValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestTorus"));
    Payload->SetNumberField(TEXT("majorRadius"), 50.0);
    Payload->SetNumberField(TEXT("minorRadius"), 20.0);
    TestTrue(TEXT("geometry.create_torus handler found and invoked"), InvokeHandler(TEXT("geometry.create_torus"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_plane
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreatePlaneValidParamsNoCrashTest,
    "PinWright.geometry.create_plane.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreatePlaneValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestPlane"));
    Payload->SetNumberField(TEXT("width"), 200.0);
    Payload->SetNumberField(TEXT("depth"), 200.0);
    TestTrue(TEXT("geometry.create_plane handler found and invoked"), InvokeHandler(TEXT("geometry.create_plane"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_disc
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateDiscValidParamsNoCrashTest,
    "PinWright.geometry.create_disc.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateDiscValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestDisc"));
    Payload->SetNumberField(TEXT("radius"), 50.0);
    Payload->SetNumberField(TEXT("segments"), 16.0);
    TestTrue(TEXT("geometry.create_disc handler found and invoked"), InvokeHandler(TEXT("geometry.create_disc"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_stairs
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateStairsValidParamsNoCrashTest,
    "PinWright.geometry.create_stairs.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateStairsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestStairs"));
    Payload->SetNumberField(TEXT("stepWidth"), 100.0);
    Payload->SetNumberField(TEXT("stepHeight"), 20.0);
    Payload->SetNumberField(TEXT("numSteps"), 8.0);
    TestTrue(TEXT("geometry.create_stairs handler found and invoked"), InvokeHandler(TEXT("geometry.create_stairs"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_spiral_stairs
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateSpiralStairsValidParamsNoCrashTest,
    "PinWright.geometry.create_spiral_stairs.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateSpiralStairsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestSpiralStairs"));
    Payload->SetNumberField(TEXT("innerRadius"), 150.0);
    Payload->SetNumberField(TEXT("curveAngle"), 90.0);
    Payload->SetNumberField(TEXT("numSteps"), 8.0);
    TestTrue(TEXT("geometry.create_spiral_stairs handler found and invoked"), InvokeHandler(TEXT("geometry.create_spiral_stairs"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_ring
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateRingValidParamsNoCrashTest,
    "PinWright.geometry.create_ring.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateRingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestRing"));
    Payload->SetNumberField(TEXT("outerRadius"), 50.0);
    Payload->SetNumberField(TEXT("innerRadius"), 25.0);
    TestTrue(TEXT("geometry.create_ring handler found and invoked"), InvokeHandler(TEXT("geometry.create_ring"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_arch
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateArchValidParamsNoCrashTest,
    "PinWright.geometry.create_arch.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateArchValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestArch"));
    Payload->SetNumberField(TEXT("majorRadius"), 100.0);
    Payload->SetNumberField(TEXT("minorRadius"), 25.0);
    Payload->SetNumberField(TEXT("angle"), 180.0);
    TestTrue(TEXT("geometry.create_arch handler found and invoked"), InvokeHandler(TEXT("geometry.create_arch"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_pipe
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreatePipeValidParamsNoCrashTest,
    "PinWright.geometry.create_pipe.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreatePipeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestPipe"));
    Payload->SetNumberField(TEXT("outerRadius"), 50.0);
    Payload->SetNumberField(TEXT("innerRadius"), 40.0);
    Payload->SetNumberField(TEXT("height"), 100.0);
    TestTrue(TEXT("geometry.create_pipe handler found and invoked"), InvokeHandler(TEXT("geometry.create_pipe"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_ramp
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateRampValidParamsNoCrashTest,
    "PinWright.geometry.create_ramp.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateRampValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestRamp"));
    Payload->SetNumberField(TEXT("width"), 100.0);
    Payload->SetNumberField(TEXT("length"), 200.0);
    Payload->SetNumberField(TEXT("height"), 50.0);
    TestTrue(TEXT("geometry.create_ramp handler found and invoked"), InvokeHandler(TEXT("geometry.create_ramp"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.revolve
// All params optional (profile defaults to built-in shape)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryRevolveValidParamsNoCrashTest,
    "PinWright.geometry.revolve.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryRevolveValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestRevolve"));
    Payload->SetNumberField(TEXT("angle"), 360.0);
    Payload->SetNumberField(TEXT("steps"), 16.0);
    TestTrue(TEXT("geometry.revolve handler found and invoked"), InvokeHandler(TEXT("geometry.revolve"), Payload));
    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_procedural_mesh
// All params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateProceduralMeshValidParamsNoCrashTest,
    "PinWright.geometry.create_procedural_mesh.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateProceduralMeshValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestProceduralMesh"));
    Payload->SetBoolField(TEXT("enableCollision"), false);
    TestTrue(TEXT("geometry.create_procedural_mesh handler found and invoked"), InvokeHandler(TEXT("geometry.create_procedural_mesh"), Payload));
    return true;
}

// ============================================================================
// SplineHandler — spline.create_spline_actor
// All params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSplineCreateSplineActorValidParamsNoCrashTest,
    "PinWright.spline.create_spline_actor.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSplineCreateSplineActorValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("TestSplineActor"));
    Payload->SetBoolField(TEXT("bClosedLoop"), false);
    Payload->SetStringField(TEXT("splineType"), TEXT("Curve"));
    TestTrue(TEXT("spline.create_spline_actor handler found and invoked"), InvokeHandler(TEXT("spline.create_spline_actor"), Payload));
    return true;
}

// ============================================================================
// SplineHandler — spline.get_splines_info (no required params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSplineGetSplinesInfoValidParamsNoCrashTest,
    "PinWright.spline.get_splines_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSplineGetSplinesInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("spline.get_splines_info handler found and invoked"), InvokeHandler(TEXT("spline.get_splines_info"), Payload));
    return true;
}

// ============================================================================
// SplineHandler — spline.get_splines_info sees SplineMeshComponents
// Regression for B-get-splines-info-ignores-spline-mesh:
// An actor created by spline.create_spline_mesh_actor is rooted on a
// USplineMeshComponent (which derives from UStaticMeshComponent, NOT
// USplineComponent). Before the fix, get_splines_info's named-lookup path
// returned [NO_SPLINE] for it and the no-arg list path dropped it entirely.
// This test spawns one and asserts the readback now reports it. If the fix is
// reverted, the named readback reverts to bSuccess=false/ErrorCode=NO_SPLINE and
// the list omits the actor, failing the assertions below.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSplineGetSplinesInfoSeesSplineMeshTest,
    "PinWright.spline.get_splines_info.SeesSplineMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSplineGetSplinesInfoSeesSplineMeshTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        // No editor world to spawn into — skip rather than false-fail.
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString ActorName = TEXT("SplineMeshReadbackRegressionActor");

    // Spawn a spline-mesh actor via the real create handler.
    {
        FTestResponseCapture CreateCapture;
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("actorName"), ActorName);
        CreatePayload->SetStringField(TEXT("componentName"), TEXT("RailMesh"));
        CreatePayload->SetStringField(TEXT("forwardAxis"), TEXT("Y"));
        TestTrue(TEXT("spline.create_spline_mesh_actor handler is registered"),
            InvokeHandlerWithCapture(TEXT("spline.create_spline_mesh_actor"), CreatePayload, CreateCapture));
        TestTrue(TEXT("spline.create_spline_mesh_actor succeeded"), CreateCapture.bSuccess);
        if (!CreateCapture.bSuccess)
        {
            return true; // Spawn failed (e.g. headless RHI) — nothing meaningful to assert.
        }
    }

    // Named readback must now succeed and report the spline-mesh state instead of NO_SPLINE.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorName);
        TestTrue(TEXT("spline.get_splines_info handler is registered"),
            InvokeHandlerWithCapture(TEXT("spline.get_splines_info"), Payload, Capture));

        TestTrue(TEXT("Named get_splines_info no longer returns NO_SPLINE for a spline-mesh actor"),
            Capture.bSuccess);
        TestNotEqual(TEXT("Error code is not NO_SPLINE"), Capture.ErrorCode, FString(TEXT("NO_SPLINE")));
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            FString ForwardAxis;
            TestTrue(TEXT("Readback reports a forwardAxis for the spline-mesh component"),
                Capture.Result->TryGetStringField(TEXT("forwardAxis"), ForwardAxis));
            TestEqual(TEXT("Reported forwardAxis matches the created value (Y)"),
                ForwardAxis, FString(TEXT("Y")));
        }
    }

    // No-arg list path must include the spline-mesh actor rather than dropping it.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TestTrue(TEXT("spline.get_splines_info (list) handler is registered"),
            InvokeHandlerWithCapture(TEXT("spline.get_splines_info"), Payload, Capture));
        TestTrue(TEXT("List get_splines_info succeeded"), Capture.bSuccess);

        bool bFoundActor = false;
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Splines = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("splines"), Splines) && Splines)
            {
                for (const TSharedPtr<FJsonValue>& Entry : *Splines)
                {
                    const TSharedPtr<FJsonObject>* EntryObj = nullptr;
                    if (Entry.IsValid() && Entry->TryGetObject(EntryObj) && EntryObj)
                    {
                        FString EntryActorName;
                        if ((*EntryObj)->TryGetStringField(TEXT("actorName"), EntryActorName)
                            && EntryActorName == ActorName)
                        {
                            bFoundActor = true;
                            break;
                        }
                    }
                }
            }
        }
        TestTrue(TEXT("No-arg list includes the spline-mesh actor (not silently dropped)"), bFoundActor);
    }

    return true;
}

// ============================================================================
// SplineHandler — spline.create_spline_actor deduplication signal
// Regression for E-spline-create-actorname-echoes-deduped:
// On a label collision UE's Requested NameMode silently deduplicates the new
// actor's OBJECT name to <name>_0 while the display label (= actorName) stays
// the requested (now shared, non-unique) string. The response's actorName
// therefore does NOT round-trip through the namespace resolver (FindActorByName,
// which matches label OR object name and returns the FIRST iterated match — the
// pre-existing actor on a collision). The fix keeps actorName = label but adds,
// AFTER AddActorVerification, three fields: requestedName, actorObjectName
// (NewActor->GetName(), the unique key) and nameWasDeduplicated. This test drives
// the PRODUCTION create handler to (a) confirm a fresh name reports
// nameWasDeduplicated=false with actorObjectName==requestedName, and (b) confirm a
// colliding name reports nameWasDeduplicated=true, actorObjectName!=requestedName,
// and that actorObjectName resolves to a DIFFERENT live actor than the
// pre-existing one (i.e. round-trips, unlike the shared actorName). Reverting the
// fix (removing the fields) drops the asserted keys and fails the test.
// ============================================================================

namespace
{
    // Finds the live actor whose internal object name (GetName()) equals Name.
    AActor* FindActorByObjectName(UWorld* World, const FString& Name)
    {
        if (!World) return nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (IsValid(*It) && It->GetName() == Name)
            {
                return *It;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSplineCreateSplineActorDeduplicatedNameSignalledTest,
    "PinWright.spline.create_spline_actor.DeduplicatedNameSignalled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSplineCreateSplineActorDeduplicatedNameSignalledTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        // No editor world to spawn into — skip rather than false-fail.
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // A label unlikely to pre-exist in the loaded map, so the FIRST create is a
    // clean (non-colliding) control.
    const FString RequestedLabel = TEXT("DedupSignalProbeSpline");

    auto CreateSpline = [&](FTestResponseCapture& OutCapture) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), RequestedLabel);
        Payload->SetStringField(TEXT("splineType"), TEXT("Linear"));
        const bool bRegistered = InvokeHandlerWithCapture(
            TEXT("spline.create_spline_actor"), Payload, OutCapture);
        TestTrue(TEXT("spline.create_spline_actor handler is registered"), bRegistered);
        return bRegistered;
    };

    // --- Control: first creation with a fresh name (no collision) ---
    FString FirstObjectName;
    {
        FTestResponseCapture Capture;
        if (!CreateSpline(Capture)) return true;
        TestTrue(TEXT("first spline.create_spline_actor succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return true; // Spawn failed (e.g. headless RHI) — nothing meaningful to assert.
        }

        bool bDeduped = true;
        TestTrue(TEXT("response carries nameWasDeduplicated"),
            Capture.Result->TryGetBoolField(TEXT("nameWasDeduplicated"), bDeduped));
        TestFalse(TEXT("fresh name is NOT deduplicated"), bDeduped);

        FString RequestedEcho, ObjectName;
        TestTrue(TEXT("response carries requestedName"),
            Capture.Result->TryGetStringField(TEXT("requestedName"), RequestedEcho));
        TestTrue(TEXT("response carries actorObjectName"),
            Capture.Result->TryGetStringField(TEXT("actorObjectName"), ObjectName));
        TestEqual(TEXT("requestedName echoes the requested label"), RequestedEcho, RequestedLabel);
        TestEqual(TEXT("fresh-name actorObjectName equals the requested name"),
            ObjectName, RequestedLabel);
        FirstObjectName = ObjectName;
    }

    // --- Collision: second creation with the SAME requested name ---
    {
        FTestResponseCapture Capture;
        if (!CreateSpline(Capture)) return true;
        TestTrue(TEXT("second (colliding) spline.create_spline_actor succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return true;
        }

        bool bDeduped = false;
        TestTrue(TEXT("colliding response carries nameWasDeduplicated"),
            Capture.Result->TryGetBoolField(TEXT("nameWasDeduplicated"), bDeduped));
        TestTrue(TEXT("colliding name IS flagged as deduplicated"), bDeduped);

        FString RequestedEcho, ObjectName;
        TestTrue(TEXT("colliding response carries requestedName"),
            Capture.Result->TryGetStringField(TEXT("requestedName"), RequestedEcho));
        TestTrue(TEXT("colliding response carries actorObjectName"),
            Capture.Result->TryGetStringField(TEXT("actorObjectName"), ObjectName));
        TestEqual(TEXT("colliding requestedName still echoes the requested label"),
            RequestedEcho, RequestedLabel);
        TestNotEqual(TEXT("colliding actorObjectName differs from the requested (shared) label"),
            ObjectName, RequestedLabel);
        TestNotEqual(TEXT("colliding actorObjectName differs from the first actor's object name"),
            ObjectName, FirstObjectName);

        // The unique actorObjectName must round-trip: it resolves to a live actor,
        // and that actor is NOT the pre-existing first actor (proving the shared
        // actorName would have addressed the wrong target).
        AActor* SecondActor = FindActorByObjectName(World, ObjectName);
        AActor* FirstActor = FindActorByObjectName(World, FirstObjectName);
        TestNotNull(TEXT("actorObjectName resolves to a live actor"), SecondActor);
        TestNotNull(TEXT("first actor still resolvable by its object name"), FirstActor);
        TestTrue(TEXT("actorObjectName addresses the NEWLY created actor, not the pre-existing one"),
            SecondActor != nullptr && SecondActor != FirstActor);
    }

    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_cylinder / create_cone heightSteps wiring
// Regression for E-geometry-cylinder-no-height-divisions: create_cylinder and
// create_cone used to hardcode AppendCylinder/AppendCone's height-steps argument
// to the literal 1, exposing no param to subdivide along the axis — so a
// downstream twist/bend/taper had no intermediate side-wall loops to displace.
// These tests build a primitive with heightSteps > 1 through the PRODUCTION
// handler, read back the spawned DynamicMeshActor's actual triangle count, and
// assert it is strictly greater than the default (heightSteps=1) build. If the
// fix is reverted (param dropped / literal 1 restored), heightSteps is ignored
// and the two builds produce identical counts, failing the >-assertion.
// ============================================================================

namespace
{
    // Spawns a primitive via the named production handler and returns the resulting
    // DynamicMeshActor (nullptr if not found / no editor world). Shared by the
    // height-steps and spawn-transform regressions below.
    ADynamicMeshActor* SpawnPrimitiveActor(const FString& Method, const TSharedPtr<FJsonObject>& Payload, const FString& ActorLabel)
    {
        if (!InvokeHandler(Method, Payload))
        {
            return nullptr;
        }
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<ADynamicMeshActor> It(World); It; ++It)
        {
            if (IsValid(*It) && It->GetActorLabel() == ActorLabel)
            {
                return *It;
            }
        }
        return nullptr;
    }

    // Returns the DynamicMesh on a spawned actor, or nullptr.
    UDynamicMesh* GetActorDynamicMesh(ADynamicMeshActor* Actor)
    {
        if (!IsValid(Actor)) return nullptr;
        if (UDynamicMeshComponent* DMComp = Actor->GetDynamicMeshComponent())
        {
            return DMComp->GetDynamicMesh();
        }
        return nullptr;
    }

    // Spawns a primitive via the named production handler, locates the resulting
    // DynamicMeshActor by its actor label, and returns its triangle count
    // (INDEX_NONE if the actor or its mesh could not be found). Shared by the
    // cylinder and cone height-steps regressions.
    int32 SpawnPrimitiveAndCountTriangles(const FString& Method, const FString& ActorLabel, int32 HeightSteps)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ActorLabel);
        Payload->SetNumberField(TEXT("segments"), 12.0);
        Payload->SetNumberField(TEXT("heightSteps"), static_cast<double>(HeightSteps));

        ADynamicMeshActor* Actor = SpawnPrimitiveActor(Method, Payload, ActorLabel);
        UDynamicMesh* Mesh = GetActorDynamicMesh(Actor);
        return Mesh ? Mesh->GetTriangleCount() : INDEX_NONE;
    }

    // Builds the named primitive twice through the production handler (heightSteps=1
    // baseline vs heightSteps=4) and asserts the subdivided mesh has strictly more
    // triangles. Shared by the cylinder and cone regressions; PrimName supplies the
    // actor-label stem and the message text.
    void RunHeightStepsSubdividesTest(FAutomationTestBase& Test, const FString& Method, const FString& PrimName)
    {
        FScopedEditorWorldActorGuard ActorGuard;

        const int32 BaselineTris = SpawnPrimitiveAndCountTriangles(
            Method, FString::Printf(TEXT("HeightSteps%sBaseline"), *PrimName), 1);
        const int32 SubdividedTris = SpawnPrimitiveAndCountTriangles(
            Method, FString::Printf(TEXT("HeightSteps%sSubdivided"), *PrimName), 4);

        // TestTrue (not TestNotEqual) because TestNotEqual only has float/double
        // overloads — an int32-vs-INDEX_NONE comparison is an ambiguous overload.
        Test.TestTrue(*FString::Printf(TEXT("Baseline %s mesh was found and counted"), *PrimName),
            BaselineTris != INDEX_NONE);
        Test.TestTrue(*FString::Printf(TEXT("Subdivided %s mesh was found and counted"), *PrimName),
            SubdividedTris != INDEX_NONE);
        // heightSteps=4 adds three extra axial loops of side-wall triangles, so the
        // count must strictly exceed the single-ring (heightSteps=1) baseline.
        Test.TestTrue(
            *FString::Printf(TEXT("heightSteps=4 %s has more triangles than heightSteps=1 (%d > %d)"),
                *PrimName, SubdividedTris, BaselineTris),
            SubdividedTris > BaselineTris && BaselineTris > 0);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateCylinderHeightStepsSubdividesTest,
    "PinWright.geometry.create_cylinder.HeightStepsSubdivides",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateCylinderHeightStepsSubdividesTest::RunTest(const FString& Parameters)
{
    RunHeightStepsSubdividesTest(*this, TEXT("geometry.create_cylinder"), TEXT("Cylinder"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateConeHeightStepsSubdividesTest,
    "PinWright.geometry.create_cone.HeightStepsSubdivides",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateConeHeightStepsSubdividesTest::RunTest(const FString& Parameters)
{
    RunHeightStepsSubdividesTest(*this, TEXT("geometry.create_cone"), TEXT("Cone"));
    return true;
}

// ============================================================================
// PrimitiveHandler — spawn-transform single-source-of-truth
// Regression for B-geometry-create-double-applies-spawn-transform: create_*
// used to feed the SAME FTransform (built from location/rotation/scale) to BOTH
// the Append* primitive call (baking the offset into the mesh-LOCAL vertices)
// AND SpawnDynamicMeshActor (setting the same offset on the actor) — applying
// the spawn transform twice. The fix builds the mesh at FTransform::Identity so
// the offset lives only on the actor transform.
//
// These tests build an offset box through the PRODUCTION create_box handler and
// assert (1) the spawned mesh's LOCAL bounding box stays centered on the origin
// (the +80 X offset is NOT baked into the vertices) and (2) the WORLD-space
// center lands at the requested location, not 2x it. If the fix is reverted
// (Append* receives the spawn Transform again), the local box re-centers on +80
// and the world center jumps to +160, failing both assertions. The boolean test
// then proves the user-visible consequence: an offset-but-overlapping cutter now
// actually cuts (resultTriangles > targetTriangles, changed=true) instead of
// silently no-op'ing.
//
// SpawnPrimitiveActor / GetActorDynamicMesh (the spawn-and-locate-by-label
// helpers these tests use) are defined in the shared anonymous namespace above,
// next to SpawnPrimitiveAndCountTriangles, which is now expressed in terms of
// them.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateBoxOffsetNotDoubleAppliedTest,
    "PinWright.geometry.create_box.OffsetNotDoubleApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateBoxOffsetNotDoubleAppliedTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        // No editor world to spawn into — skip rather than false-fail.
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString ActorLabel = TEXT("OffsetBoxDoubleApplyRegression");
    const double OffsetX = 80.0;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), ActorLabel);
    Payload->SetNumberField(TEXT("width"), 60.0);
    Payload->SetNumberField(TEXT("height"), 60.0);
    Payload->SetNumberField(TEXT("depth"), 60.0);
    TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
    Loc->SetNumberField(TEXT("x"), OffsetX);
    Loc->SetNumberField(TEXT("y"), 0.0);
    Loc->SetNumberField(TEXT("z"), 0.0);
    Payload->SetObjectField(TEXT("location"), Loc);

    ADynamicMeshActor* Actor = SpawnPrimitiveActor(TEXT("geometry.create_box"), Payload, ActorLabel);
    TestNotNull(TEXT("create_box spawned the offset box actor"), Actor);
    if (!Actor) return true;

    UDynamicMesh* Mesh = GetActorDynamicMesh(Actor);
    TestNotNull(TEXT("Spawned box has a DynamicMesh"), Mesh);
    if (!Mesh) return true;

    // LOCAL-space bounding box: the spawn offset must NOT be baked into vertices.
    // A width=60 box centered in local space spans X [-30, 30] (center ~0). The
    // double-apply bug would re-center it on +80 (X [50, 110]).
    const FBox LocalBox = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh);
    const FVector LocalCenter = LocalBox.GetCenter();
    TestTrue(
        FString::Printf(TEXT("Mesh-local box center X stays at origin, not baked to +%.0f (got %.2f)"),
            OffsetX, LocalCenter.X),
        FMath::Abs(LocalCenter.X) < 1.0);

    // WORLD-space center = actor transform applied to the local center. It must
    // land at the requested location (+80), not at 2x (+160) as the double-apply
    // produced (mesh @ +80 AND actor @ +80).
    const FVector WorldCenter = Actor->GetActorTransform().TransformPosition(LocalCenter);
    TestTrue(
        FString::Printf(TEXT("World-space box center X equals requested +%.0f, not 2x (got %.2f)"),
            OffsetX, WorldCenter.X),
        FMath::IsNearlyEqual(WorldCenter.X, OffsetX, 1.0));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanSubtractOffsetToolCutsTest,
    "PinWright.geometry.boolean_subtract.OffsetToolCuts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBooleanSubtractOffsetToolCutsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // Target box centered at origin: world X/Y/Z [-100, 100].
    {
        TSharedPtr<FJsonObject> TargetPayload = MakeShared<FJsonObject>();
        TargetPayload->SetStringField(TEXT("name"), TEXT("SubtractRegressionTarget"));
        TargetPayload->SetNumberField(TEXT("width"), 200.0);
        TargetPayload->SetNumberField(TEXT("height"), 200.0);
        TargetPayload->SetNumberField(TEXT("depth"), 200.0);

        ADynamicMeshActor* Target = SpawnPrimitiveActor(TEXT("geometry.create_box"),
            TargetPayload, TEXT("SubtractRegressionTarget"));
        TestNotNull(TEXT("Target box spawned"), Target);
        if (!Target) return true;
    }

    // Tool rod offset +80 in X: world X [50, 110] — still overlaps the target
    // across X [50, 100] with full Y/Z overlap. Pre-fix, the mesh ALSO carried
    // the +80 so its true world X was [130, 190] (disjoint) and the boolean
    // produced no cut.
    {
        TSharedPtr<FJsonObject> ToolPayload = MakeShared<FJsonObject>();
        ToolPayload->SetStringField(TEXT("name"), TEXT("SubtractRegressionTool"));
        ToolPayload->SetNumberField(TEXT("width"), 60.0);
        ToolPayload->SetNumberField(TEXT("height"), 60.0);
        ToolPayload->SetNumberField(TEXT("depth"), 400.0);
        TSharedPtr<FJsonObject> ToolLoc = MakeShared<FJsonObject>();
        ToolLoc->SetNumberField(TEXT("x"), 80.0);
        ToolLoc->SetNumberField(TEXT("y"), 0.0);
        ToolLoc->SetNumberField(TEXT("z"), 0.0);
        ToolPayload->SetObjectField(TEXT("location"), ToolLoc);

        ADynamicMeshActor* Tool = SpawnPrimitiveActor(TEXT("geometry.create_box"),
            ToolPayload, TEXT("SubtractRegressionTool"));
        TestNotNull(TEXT("Offset tool box spawned"), Tool);
        if (!Tool) return true;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> SubPayload = MakeShared<FJsonObject>();
    SubPayload->SetStringField(TEXT("targetActor"), TEXT("SubtractRegressionTarget"));
    SubPayload->SetStringField(TEXT("toolActor"), TEXT("SubtractRegressionTool"));
    SubPayload->SetBoolField(TEXT("keepTool"), true);
    TestTrue(TEXT("geometry.boolean_subtract handler is registered"),
        InvokeHandlerWithCapture(TEXT("geometry.boolean_subtract"), SubPayload, Capture));
    TestTrue(TEXT("boolean_subtract reports success"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double TargetTris = 0.0, ResultTris = 0.0;
        Capture.Result->TryGetNumberField(TEXT("targetTriangles"), TargetTris);
        Capture.Result->TryGetNumberField(TEXT("resultTriangles"), ResultTris);

        // The overlapping offset tool must actually cut the target: a notch adds
        // geometry, so the result triangle count strictly exceeds the target's.
        // Pre-fix this was equal (no cut).
        TestTrue(
            FString::Printf(TEXT("Offset cutter actually cut the target (resultTriangles %.0f > targetTriangles %.0f)"),
                ResultTris, TargetTris),
            ResultTris > TargetTris);

        // The secondary changed flag must agree the mesh was modified.
        bool bChanged = false;
        TestTrue(TEXT("Response carries a changed flag"),
            Capture.Result->TryGetBoolField(TEXT("changed"), bChanged));
        TestTrue(TEXT("changed flag is true for a real cut"), bChanged);
    }

    return true;
}

// ============================================================================
// MeshOpsHandler — face-op face targeting + change reporting
// Regression for B-extrude-inset-empty-selection-whole-mesh: geometry.extrude /
// inset / outset / offset_faces used to pass a default-EMPTY
// FGeometryScriptMeshSelection (== whole mesh) to the engine face op and report
// unconditional success with no count/changed echo. On a CLOSED solid that is
// degenerate: extrude offsets the whole component as a solid and DUPLICATES the
// mesh (exact ×2 count), inset has no boundary loop and is a silent NO-OP. The
// fix (1) adds an optional `faceDirection` {x,y,z} that builds a real
// directional face subset via SelectMeshElementsByNormalAngle so a single face
// (e.g. the +Z top) can actually be targeted, and (2) echoes
// vertexCount/triangleCount + a `changed` flag + `facesSelected` so a no-op or
// whole-mesh-duplicate is detectable instead of bare success.
//
// These tests drive the PRODUCTION handlers on a real create_box (8v/12t closed
// solid) and assert the corrected contract. If the fix is reverted:
//   - the `changed`/`facesSelected`/count fields vanish → the TryGet*Field
//     assertions fail;
//   - `faceDirection` is ignored → the whole mesh extrudes → facesSelected==0
//     and the triangle count is an exact ×2 of the closed box (24), failing the
//     "selected a face subset, did not duplicate the whole mesh" assertions.
// ============================================================================

namespace
{
    // Spawns a fresh closed box via the production create_box handler with the
    // given label and returns its DynamicMesh (nullptr if unavailable).
    UDynamicMesh* SpawnClosedBox(const FString& Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Label);
        Payload->SetNumberField(TEXT("width"), 100.0);
        Payload->SetNumberField(TEXT("height"), 100.0);
        Payload->SetNumberField(TEXT("depth"), 100.0);
        ADynamicMeshActor* Actor = SpawnPrimitiveActor(TEXT("geometry.create_box"), Payload, Label);
        return GetActorDynamicMesh(Actor);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryExtrudeReportsChangeAndNoSilentDuplicateTest,
    "PinWright.geometry.extrude.ReportsChangeAndNoSilentDuplicate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryExtrudeReportsChangeAndNoSilentDuplicateTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        return true;  // no editor world — skip rather than false-fail
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // --- Whole-mesh extrude (no faceDirection): on a closed box this DUPLICATES
    // the mesh. The response must now disclose that via changed=true + the exact
    // doubled triangle count, with facesSelected==0 (whole mesh). Pre-fix none of
    // these fields existed (bare "Extrude applied" success). ---
    {
        UDynamicMesh* Mesh = SpawnClosedBox(TEXT("FaceOpWholeMeshExtrude"));
        TestNotNull(TEXT("closed box for whole-mesh extrude spawned"), Mesh);
        if (!Mesh) return true;
        const int32 TrisBefore = Mesh->GetTriangleCount();
        TestEqual(TEXT("closed box baseline is 12 triangles"), TrisBefore, 12);

        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), TEXT("FaceOpWholeMeshExtrude"));
        Payload->SetNumberField(TEXT("distance"), 50.0);
        TestTrue(TEXT("geometry.extrude registered"),
            InvokeHandlerWithCapture(TEXT("geometry.extrude"), Payload, Capture));
        TestTrue(TEXT("extrude reports success"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

        bool bChanged = false;
        TestTrue(TEXT("extrude response carries a changed flag"),
            Capture.Result->TryGetBoolField(TEXT("changed"), bChanged));
        TestTrue(TEXT("whole-mesh extrude on a closed box is reported changed=true"), bChanged);

        double FacesSelected = -1.0;
        TestTrue(TEXT("extrude response carries facesSelected"),
            Capture.Result->TryGetNumberField(TEXT("facesSelected"), FacesSelected));
        TestEqual(TEXT("no faceDirection => whole mesh => facesSelected==0"), FacesSelected, 0.0);

        double TriCount = 0.0;
        TestTrue(TEXT("extrude response echoes triangleCount"),
            Capture.Result->TryGetNumberField(TEXT("triangleCount"), TriCount));
        // The whole-mesh closed-solid extrude duplicates: 12 -> 24.
        TestEqual(TEXT("whole-mesh closed-box extrude doubles the triangle count (12->24)"),
            TriCount, 24.0);
    }

    // --- Targeted extrude (faceDirection={0,0,1}): only the +Z top face(s) are
    // selected, so a real face is raised rather than the whole mesh duplicated.
    // facesSelected must be a NON-EMPTY SUBSET (0 < n < total tris) and the result
    // must NOT be the whole-mesh ×2 (24). Pre-fix, faceDirection was ignored, so
    // facesSelected would be 0 and the count would double to 24. ---
    {
        UDynamicMesh* Mesh = SpawnClosedBox(TEXT("FaceOpTopFaceExtrude"));
        TestNotNull(TEXT("closed box for top-face extrude spawned"), Mesh);
        if (!Mesh) return true;
        const int32 TrisBefore = Mesh->GetTriangleCount();  // 12

        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), TEXT("FaceOpTopFaceExtrude"));
        Payload->SetNumberField(TEXT("distance"), 50.0);
        TSharedPtr<FJsonObject> Dir = MakeShared<FJsonObject>();
        Dir->SetNumberField(TEXT("x"), 0.0);
        Dir->SetNumberField(TEXT("y"), 0.0);
        Dir->SetNumberField(TEXT("z"), 1.0);
        Payload->SetObjectField(TEXT("faceDirection"), Dir);
        TestTrue(TEXT("geometry.extrude registered"),
            InvokeHandlerWithCapture(TEXT("geometry.extrude"), Payload, Capture));
        TestTrue(TEXT("targeted extrude reports success"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

        double FacesSelected = -1.0;
        Capture.Result->TryGetNumberField(TEXT("facesSelected"), FacesSelected);
        // A real subset of faces was selected (the top), not the whole mesh and
        // not nothing: 0 < facesSelected < 12.
        TestTrue(
            FString::Printf(TEXT("faceDirection selects a non-empty face SUBSET (0 < %.0f < %d)"),
                FacesSelected, TrisBefore),
            FacesSelected > 0.0 && FacesSelected < static_cast<double>(TrisBefore));

        double TriCount = 0.0;
        Capture.Result->TryGetNumberField(TEXT("triangleCount"), TriCount);
        // A targeted face extrude is NOT the whole-mesh duplicate (which would be 24).
        TestTrue(
            FString::Printf(TEXT("targeted extrude did not whole-mesh-duplicate (triangleCount %.0f != 24)"),
                TriCount),
            TriCount != 24.0);

        bool bChanged = false;
        Capture.Result->TryGetBoolField(TEXT("changed"), bChanged);
        TestTrue(TEXT("a real face extrude is reported changed=true"), bChanged);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryInsetWholeMeshNoOpReportedTest,
    "PinWright.geometry.inset.WholeMeshNoOpReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryInsetWholeMeshNoOpReportedTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    UDynamicMesh* Mesh = SpawnClosedBox(TEXT("FaceOpWholeMeshInset"));
    TestNotNull(TEXT("closed box for whole-mesh inset spawned"), Mesh);
    if (!Mesh) return true;
    const int32 TrisBefore = Mesh->GetTriangleCount();  // 12

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("FaceOpWholeMeshInset"));
    Payload->SetNumberField(TEXT("distance"), 20.0);
    TestTrue(TEXT("geometry.inset registered"),
        InvokeHandlerWithCapture(TEXT("geometry.inset"), Payload, Capture));
    TestTrue(TEXT("inset reports success"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

    // Whole-mesh inset of a closed box has no boundary loop -> genuine no-op. The
    // response must now DISCLOSE the no-op via changed=false instead of bare
    // success. Pre-fix this field did not exist.
    bool bChanged = true;
    TestTrue(TEXT("inset response carries a changed flag"),
        Capture.Result->TryGetBoolField(TEXT("changed"), bChanged));
    TestFalse(TEXT("whole-mesh inset on a closed box is a no-op (changed=false)"), bChanged);

    double TriCount = 0.0;
    TestTrue(TEXT("inset response echoes triangleCount"),
        Capture.Result->TryGetNumberField(TEXT("triangleCount"), TriCount));
    TestEqual(TEXT("no-op inset leaves the triangle count unchanged"),
        TriCount, static_cast<double>(TrisBefore));

    return true;
}

// Reads a boundingBox {x,y,z} sub-object into Out; returns false if the named
// sub-object or any of its x/y/z fields is missing. Shared by both get_mesh_info
// bounding-box tests below so the {x,y,z} extraction lives in one place.
static bool ReadBBoxVec3(const TSharedPtr<FJsonObject>& BBox, const FString& Key, FVector& Out)
{
    const TSharedPtr<FJsonObject>* Obj = nullptr;
    if (!BBox.IsValid() || !BBox->TryGetObjectField(Key, Obj) || !Obj || !(*Obj).IsValid())
    {
        return false;
    }
    double X = 0.0, Y = 0.0, Z = 0.0;
    const bool bOk = (*Obj)->TryGetNumberField(TEXT("x"), X)
        && (*Obj)->TryGetNumberField(TEXT("y"), Y)
        && (*Obj)->TryGetNumberField(TEXT("z"), Z);
    Out = FVector(X, Y, Z);
    return bOk;
}

// ============================================================================
// MeshInfoHandler — geometry.get_mesh_info exposes the local-space bounding box
// Regression for E-geometry-mesh-info-omits-bbox: get_mesh_info used to return
// only vertex/triangle counts + feature flags and NO spatial extent, forcing a
// separate actor.get_bounding_box round-trip to verify "did the op land at the
// expected size?" — and that workaround only matches the mesh-LOCAL extent at an
// identity actor transform. The fix folds the FBox from
// GetMeshBoundingBox(Target.Mesh) into the response as
// boundingBox {min,max,origin,extent}, reusing the same handle the counts read.
//
// This test spawns a box of distinctly NON-cubic dimensions through the
// production create_box handler, calls the production get_mesh_info handler, and
// asserts the response now carries a boundingBox whose four {x,y,z} sub-objects
// are present and whose half-extents match the box's half-dimensions {40,60,100}
// (compared as a multiset so the assertion is independent of AppendBox's internal
// width/height/depth -> X/Y/Z axis mapping). If the fix is reverted, the
// boundingBox field is absent and every assertion below fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryGetMeshInfoReportsBoundingBoxTest,
    "PinWright.geometry.get_mesh_info.ReportsBoundingBox",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryGetMeshInfoReportsBoundingBoxTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        // No editor world to spawn into — skip rather than false-fail.
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString ActorLabel = TEXT("MeshInfoBBoxRegressionBox");
    // Deliberately non-cubic so the extent is load-bearing: a half-extent of
    // {40,60,100} can only come from reading the actual geometry bounds.
    const double Width = 80.0, Height = 200.0, Depth = 120.0;

    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), ActorLabel);
        CreatePayload->SetNumberField(TEXT("width"), Width);
        CreatePayload->SetNumberField(TEXT("height"), Height);
        CreatePayload->SetNumberField(TEXT("depth"), Depth);
        ADynamicMeshActor* Actor = SpawnPrimitiveActor(TEXT("geometry.create_box"), CreatePayload, ActorLabel);
        TestNotNull(TEXT("create_box spawned the bbox-regression box"), Actor);
        if (!Actor) return true;  // spawn failed (e.g. headless RHI) — nothing to assert
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("actorName"), ActorLabel);
    TestTrue(TEXT("geometry.get_mesh_info handler is registered"),
        InvokeHandlerWithCapture(TEXT("geometry.get_mesh_info"), InfoPayload, Capture));
    TestTrue(TEXT("get_mesh_info reports success"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

    // The response must now carry a boundingBox object (the whole point of the fix).
    const TSharedPtr<FJsonObject>* BBox = nullptr;
    TestTrue(TEXT("get_mesh_info response carries a boundingBox object"),
        Capture.Result->TryGetObjectField(TEXT("boundingBox"), BBox));
    if (!BBox || !(*BBox).IsValid()) return true;

    FVector Min, Max, Origin, Extent;
    TestTrue(TEXT("boundingBox carries a min {x,y,z}"), ReadBBoxVec3(*BBox, TEXT("min"), Min));
    TestTrue(TEXT("boundingBox carries a max {x,y,z}"), ReadBBoxVec3(*BBox, TEXT("max"), Max));
    TestTrue(TEXT("boundingBox carries an origin {x,y,z}"), ReadBBoxVec3(*BBox, TEXT("origin"), Origin));
    TestTrue(TEXT("boundingBox carries an extent {x,y,z}"), ReadBBoxVec3(*BBox, TEXT("extent"), Extent));

    // A centered box has its local origin at ~0 on every axis.
    TestTrue(TEXT("box bbox origin sits at the local origin"),
        Origin.GetAbsMax() < 1.0);

    // min/max/extent are internally consistent: extent == (max - min)/2 and
    // origin == (max + min)/2 (the FBox contract the handler surfaced).
    TestTrue(TEXT("extent equals half the min->max span"),
        ((Max - Min) * 0.5).Equals(Extent, 0.5));
    TestTrue(TEXT("origin equals the midpoint of min and max"),
        ((Max + Min) * 0.5).Equals(Origin, 0.5));

    // The half-extents must be the box's half-dimensions {40,60,100}. Compared as
    // a sorted multiset so the check is independent of AppendBox's width/height/
    // depth -> X/Y/Z axis assignment, while still proving the extent is the REAL
    // geometry size (a degenerate/zeroed or missing bbox fails this).
    TArray<double> GotExtents = { FMath::Abs(Extent.X), FMath::Abs(Extent.Y), FMath::Abs(Extent.Z) };
    GotExtents.Sort();
    TArray<double> WantExtents = { Width * 0.5, Height * 0.5, Depth * 0.5 };
    WantExtents.Sort();
    TestEqual(TEXT("smallest half-extent matches the smallest half-dimension"),
        GotExtents[0], WantExtents[0], 0.5);
    TestEqual(TEXT("middle half-extent matches the middle half-dimension"),
        GotExtents[1], WantExtents[1], 0.5);
    TestEqual(TEXT("largest half-extent matches the largest half-dimension"),
        GotExtents[2], WantExtents[2], 0.5);

    return true;
}

// ============================================================================
// MeshInfoHandler — geometry.get_mesh_info on an EMPTY (0-vertex) mesh
// Regression for B-mesh-info-empty-nonfinite-json: an empty DynamicMesh (a fresh
// create_procedural_mesh, or a mesh emptied by a boolean/delete/simplify) yields
// the inverted "empty" FBox (Min=+DBL_MAX, Max=-DBL_MAX). GetExtent()=(Max-Min)*0.5
// overflowed to a non-finite value (-inf), which BuildVectorJson serialized raw —
// UE's number writer emits a bare `inf`/`nan` token, invalid JSON that the MCP
// client rejects wholesale (json.loads "Expecting value: line 43 column 11"),
// mis-reporting a LIVE editor as unreachable.
//
// This test builds a genuinely empty mesh through the PRODUCTION
// create_procedural_mesh handler, calls the PRODUCTION get_mesh_info handler, and
// asserts (1) vertexCount==0 (the fixture really is empty), (2) every boundingBox
// component is finite — the core regression: pre-fix `extent` read back as -inf,
// (3) the empty box is reported zeroed rather than the +/-DBL_MAX inverted
// sentinel, and (4) the full response serializes to JSON that round-trips (the
// on-the-wire symptom: pre-fix the serialized bytes were unparseable). NOTE: on
// this empty-mesh fixture the get_mesh_info handler guard (MeshInfoHandler.cpp)
// zeroes min/max/origin/extent BEFORE BuildVectorJson runs, so reverting that
// handler guard fails assertions 2-4 — but the BuildVectorJson SanitizeFinite guard
// never sees a non-finite value here and would stay green if reverted. That
// defense-in-depth guard (a VALID FBox carrying a NaN/Inf component) is pinned
// separately by FJsonBuildersVectorJsonFiniteGuardTest below.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryGetMeshInfoEmptyMeshFiniteBoundingBoxTest,
    "PinWright.geometry.get_mesh_info.EmptyMeshBoundingBoxIsFiniteJson",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryGetMeshInfoEmptyMeshFiniteBoundingBoxTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        // No editor world to spawn into — skip rather than false-fail.
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString ActorLabel = TEXT("MeshInfoEmptyMeshRegression");

    // create_procedural_mesh deliberately spawns an EMPTY (0-vertex) DynamicMeshActor.
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), ActorLabel);
        ADynamicMeshActor* Actor = SpawnPrimitiveActor(TEXT("geometry.create_procedural_mesh"), CreatePayload, ActorLabel);
        TestNotNull(TEXT("create_procedural_mesh spawned the empty mesh actor"), Actor);
        if (!Actor) return true;  // spawn failed (e.g. headless RHI) — nothing to assert
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("actorName"), ActorLabel);
    TestTrue(TEXT("geometry.get_mesh_info handler is registered"),
        InvokeHandlerWithCapture(TEXT("geometry.get_mesh_info"), InfoPayload, Capture));
    TestTrue(TEXT("get_mesh_info reports success on an empty mesh"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

    // The fixture must genuinely be empty for this regression to be meaningful.
    double VertexCount = -1.0;
    TestTrue(TEXT("get_mesh_info echoes vertexCount"),
        Capture.Result->TryGetNumberField(TEXT("vertexCount"), VertexCount));
    TestEqual(TEXT("the procedural-mesh fixture is empty (0 vertices)"), VertexCount, 0.0);

    const TSharedPtr<FJsonObject>* BBox = nullptr;
    TestTrue(TEXT("empty-mesh get_mesh_info still carries a boundingBox object"),
        Capture.Result->TryGetObjectField(TEXT("boundingBox"), BBox));
    if (!BBox || !(*BBox).IsValid()) return true;

    // Reuses the shared {x,y,z} reader, then asserts every component is finite
    // (pre-fix the `extent` sub-object came back as -inf here) AND zeroed (the empty
    // box is reported as {0,0,0}, not the +/-DBL_MAX inverted sentinel).
    auto CheckVec3FiniteAndZero = [&](const FString& Key)
    {
        FVector V;
        const bool bRead = ReadBBoxVec3(*BBox, Key, V);
        TestTrue(*FString::Printf(TEXT("boundingBox carries a %s {x,y,z}"), *Key), bRead);
        if (!bRead)
        {
            return;
        }
        TestTrue(*FString::Printf(TEXT("boundingBox.%s is finite (no inf/nan)"), *Key),
            FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z));
        TestTrue(*FString::Printf(TEXT("empty-mesh boundingBox.%s is zeroed"), *Key),
            FMath::IsNearlyZero(V.X) && FMath::IsNearlyZero(V.Y) && FMath::IsNearlyZero(V.Z));
    };
    CheckVec3FiniteAndZero(TEXT("min"));
    CheckVec3FiniteAndZero(TEXT("max"));
    CheckVec3FiniteAndZero(TEXT("origin"));
    CheckVec3FiniteAndZero(TEXT("extent"));

    // On-the-wire symptom: the whole response must serialize to valid, re-parseable
    // JSON. Pre-fix the non-finite extent made UE emit a bare inf/nan token, so the
    // serialized bytes failed to parse (the client's json.loads "Expecting value").
    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    TestTrue(TEXT("empty-mesh get_mesh_info response serializes"),
        FJsonSerializer::Serialize(Capture.Result.ToSharedRef(), Writer));
    Writer->Close();
    TSharedPtr<FJsonObject> Reparsed;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
    TestTrue(TEXT("serialized empty-mesh response is valid, re-parseable JSON"),
        FJsonSerializer::Deserialize(Reader, Reparsed) && Reparsed.IsValid());

    return true;
}

// ============================================================================
// JsonBuilders::BuildVectorJson — the SanitizeFinite finite guard, pinned directly
// The empty-mesh regression above never exercises this guard: the get_mesh_info
// handler zeroes an empty box BEFORE BuildVectorJson runs, so no non-finite value
// reaches SanitizeFinite there. But a VALID FBox can still carry a NaN/Inf component
// (e.g. a mesh with a NaN vertex), and only this guard catches that. Feed a vector
// of {+Inf, -Inf, NaN} straight through the builder and require every component
// reads back finite and zeroed — this is the one test that fails if SanitizeFinite
// is reverted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBuildersVectorJsonFiniteGuardTest,
    "PinWright.Utils.JsonBuilders.BuildVectorJsonFiniteGuard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FJsonBuildersVectorJsonFiniteGuardTest::RunTest(const FString& Parameters)
{
    const double PosInf = std::numeric_limits<double>::infinity();
    const double NegInf = -std::numeric_limits<double>::infinity();
    const double Nan    = std::numeric_limits<double>::quiet_NaN();

    const TSharedPtr<FJsonObject> Obj = JsonBuilders::BuildVectorJson(FVector(PosInf, NegInf, Nan));
    if (!TestNotNull(TEXT("BuildVectorJson returns an object"), Obj.Get()))
    {
        return true;
    }

    double X = PosInf, Y = NegInf, Z = Nan;
    TestTrue(TEXT("BuildVectorJson emits x"), Obj->TryGetNumberField(TEXT("x"), X));
    TestTrue(TEXT("BuildVectorJson emits y"), Obj->TryGetNumberField(TEXT("y"), Y));
    TestTrue(TEXT("BuildVectorJson emits z"), Obj->TryGetNumberField(TEXT("z"), Z));

    // Non-finite inputs must collapse to a finite 0 on the wire (no bare inf/nan token).
    TestTrue(TEXT("BuildVectorJson sanitizes +Inf to finite 0"), FMath::IsFinite(X) && FMath::IsNearlyZero(X));
    TestTrue(TEXT("BuildVectorJson sanitizes -Inf to finite 0"), FMath::IsFinite(Y) && FMath::IsNearlyZero(Y));
    TestTrue(TEXT("BuildVectorJson sanitizes NaN to finite 0"), FMath::IsFinite(Z) && FMath::IsNearlyZero(Z));

    // A finite vector must pass through untouched (the guard only rewrites non-finite).
    const TSharedPtr<FJsonObject> Finite = JsonBuilders::BuildVectorJson(FVector(1.5, -2.0, 3.25));
    double FX = 0.0, FY = 0.0, FZ = 0.0;
    if (Finite.IsValid())
    {
        Finite->TryGetNumberField(TEXT("x"), FX);
        Finite->TryGetNumberField(TEXT("y"), FY);
        Finite->TryGetNumberField(TEXT("z"), FZ);
    }
    TestTrue(TEXT("BuildVectorJson leaves finite components unchanged"),
        FMath::IsNearlyEqual(FX, 1.5) && FMath::IsNearlyEqual(FY, -2.0) && FMath::IsNearlyEqual(FZ, 3.25));

    return true;
}

// ============================================================================
// PrimitiveHandler — geometry.create_procedural_mesh enableCollision wiring
// Regression: the advertised enableCollision param used to reach only
// SetGenerateOverlapEvents, which toggles overlap-event dispatch and never touches
// the collision mode — so enableCollision=true spawned an actor with NO collision.
// The fix calls SetCollisionEnabled (QueryAndPhysics + complex-as-simple on true,
// NoCollision on false). Reverting it leaves the component on its spawn default and
// fails the enabled-case assertions. Uses the shared SpawnPrimitiveActor fixture.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateProceduralMeshEnableCollisionTest,
    "PinWright.geometry.create_procedural_mesh.EnableCollisionSetsCollisionEnabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateProceduralMeshEnableCollisionTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        // No editor world to spawn into — skip rather than false-fail.
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    auto SpawnProceduralMesh = [](const FString& Label, bool bEnableCollision) -> UDynamicMeshComponent*
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Label);
        Payload->SetBoolField(TEXT("enableCollision"), bEnableCollision);
        ADynamicMeshActor* Actor = SpawnPrimitiveActor(TEXT("geometry.create_procedural_mesh"), Payload, Label);
        return Actor ? Actor->GetDynamicMeshComponent() : nullptr;
    };

    UDynamicMeshComponent* CollidingComp = SpawnProceduralMesh(TEXT("ProcMeshCollisionOn"), true);
    TestNotNull(TEXT("create_procedural_mesh(enableCollision=true) spawned the actor"), CollidingComp);
    if (CollidingComp)
    {
        // The core regression: pre-fix this stayed on the spawn default because the flag
        // only reached SetGenerateOverlapEvents.
        TestTrue(TEXT("enableCollision=true leaves the component collision-enabled"),
            CollidingComp->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics);
        // The mesh is empty at spawn, so its collision can only come from the geometry
        // appended later — complex-as-simple is what makes that happen.
        TestTrue(TEXT("enableCollision=true makes the mesh its own (complex-as-simple) collision"),
            CollidingComp->bEnableComplexCollision);
    }

    UDynamicMeshComponent* NonCollidingComp = SpawnProceduralMesh(TEXT("ProcMeshCollisionOff"), false);
    TestNotNull(TEXT("create_procedural_mesh(enableCollision=false) spawned the actor"), NonCollidingComp);
    if (NonCollidingComp)
    {
        TestTrue(TEXT("enableCollision=false leaves the component with no collision"),
            NonCollidingComp->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
    }

    return true;
}

// ============================================================================
// SplineHandler — spline.set_spline_point_tangents applies BOTH tangents
// Regression: the handler read arriveTangent and leaveTangent but applied only the
// singular SetTangentAtSplinePoint (which forces leave == arrive), logging a warning
// that "UE splines use a single tangent per point" — false: a spline point carries
// independent ArriveTangent/LeaveTangent, settable via SetTangentsAtSplinePoint. The
// fix applies both and sets the point type to CurveCustomTangent (auto types make
// UpdateSpline recompute the tangents away). Reverting it collapses the leave tangent
// onto the arrive tangent and fails the leave assertion.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSplineSetSplinePointTangentsAppliesBothTest,
    "PinWright.spline.set_spline_point_tangents.AppliesArriveAndLeaveIndependently",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSplineSetSplinePointTangentsAppliesBothTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        // No editor world to spawn into — skip rather than false-fail.
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString ActorLabel = TEXT("PW_SplineTangentProbe");
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("actorName"), ActorLabel);
        TestTrue(TEXT("spline.create_spline_actor handler is registered"),
            InvokeHandler(TEXT("spline.create_spline_actor"), CreatePayload));
    }

    // Deliberately asymmetric (a cusp): pre-fix the leave tangent was discarded and the
    // point ended up with leave == arrive.
    const FVector Arrive(100.0, 0.0, 0.0);
    const FVector Leave(0.0, 250.0, 0.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ActorLabel);
    Payload->SetNumberField(TEXT("pointIndex"), 0);
    Payload->SetObjectField(TEXT("arriveTangent"), JsonBuilders::BuildVectorJson(Arrive));
    Payload->SetObjectField(TEXT("leaveTangent"), JsonBuilders::BuildVectorJson(Leave));

    FTestResponseCapture Capture;
    TestTrue(TEXT("spline.set_spline_point_tangents handler is registered"),
        InvokeHandlerWithCapture(TEXT("spline.set_spline_point_tangents"), Payload, Capture));
    TestTrue(TEXT("set_spline_point_tangents reports success"), Capture.bSuccess);

    // Read the tangents back off the live component, not just the response echo.
    USplineComponent* SplineComp = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (IsValid(*It) && It->GetActorLabel() == ActorLabel)
        {
            SplineComp = It->FindComponentByClass<USplineComponent>();
            break;
        }
    }
    TestNotNull(TEXT("the spline probe actor carries a USplineComponent"), SplineComp);
    if (!SplineComp || SplineComp->GetNumberOfSplinePoints() < 1) return true;

    const FVector GotArrive = SplineComp->GetArriveTangentAtSplinePoint(0, ESplineCoordinateSpace::Local);
    const FVector GotLeave = SplineComp->GetLeaveTangentAtSplinePoint(0, ESplineCoordinateSpace::Local);

    TestTrue(TEXT("arriveTangent is applied"), GotArrive.Equals(Arrive, 0.1));
    // The core regression: pre-fix this read back as Arrive, not Leave.
    TestTrue(TEXT("leaveTangent is applied independently (not collapsed onto arriveTangent)"),
        GotLeave.Equals(Leave, 0.1));
    // Without CurveCustomTangent the next UpdateSpline would recompute both tangents.
    TestTrue(TEXT("the point is switched to CurveCustomTangent so the tangents persist"),
        SplineComp->GetSplinePointType(0) == ESplinePointType::CurveCustomTangent);

    return true;
}
