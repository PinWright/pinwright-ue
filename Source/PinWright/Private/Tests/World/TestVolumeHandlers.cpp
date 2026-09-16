// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Volume domain handlers (VolumeHandler.cpp)
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/BlockingVolume.h"
#include "Engine/TriggerVolume.h"
#include "Engine/Brush.h"
#include "Misc/Guid.h"
#include "Engine/Polys.h"
#include "Model.h"
#include "Components/BrushComponent.h"
#include "Engine/Level.h"
#include "Engine/TriggerBase.h"
#include "Engine/TriggerSphere.h"
#include "GameFramework/PhysicsVolume.h"
#include "GameFramework/Volume.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "UObject/UObjectGlobals.h"

// ============================================================================
// volume.create_trigger_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateTriggerVolumeNoCrashTest,
    "PinWright.volume.create_trigger_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateTriggerVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_trigger_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_trigger_sphere — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateTriggerSphereNoCrashTest,
    "PinWright.volume.create_trigger_sphere.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateTriggerSphereNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_trigger_sphere"), Payload));
    return true;
}

// ============================================================================
// volume.create_trigger_capsule — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateTriggerCapsuleNoCrashTest,
    "PinWright.volume.create_trigger_capsule.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateTriggerCapsuleNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_trigger_capsule"), Payload));
    return true;
}

// ============================================================================
// volume.create_blocking_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateBlockingVolumeNoCrashTest,
    "PinWright.volume.create_blocking_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateBlockingVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_blocking_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_kill_z_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateKillZVolumeNoCrashTest,
    "PinWright.volume.create_kill_z_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateKillZVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_kill_z_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_pain_causing_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreatePainCausingVolumeNoCrashTest,
    "PinWright.volume.create_pain_causing_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreatePainCausingVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_pain_causing_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_physics_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreatePhysicsVolumeNoCrashTest,
    "PinWright.volume.create_physics_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreatePhysicsVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_physics_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_audio_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateAudioVolumeNoCrashTest,
    "PinWright.volume.create_audio_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateAudioVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_audio_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_reverb_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateReverbVolumeNoCrashTest,
    "PinWright.volume.create_reverb_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateReverbVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_reverb_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_post_process_volume — all params optional (UE 5.1+)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreatePostProcessVolumeNoCrashTest,
    "PinWright.volume.create_post_process_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreatePostProcessVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    // InvokeHandler returns false when the handler is absent, which is also a valid
    // outcome — we accept both.
    auto Payload = MakeShared<FJsonObject>();
    InvokeHandler(TEXT("volume.create_post_process_volume"), Payload);
    return true;
}

// ============================================================================
// volume.create_cull_distance_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateCullDistanceVolumeNoCrashTest,
    "PinWright.volume.create_cull_distance_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateCullDistanceVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_cull_distance_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_precomputed_visibility_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreatePrecomputedVisibilityVolumeNoCrashTest,
    "PinWright.volume.create_precomputed_visibility_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreatePrecomputedVisibilityVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_precomputed_visibility_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_lightmass_importance_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateLightmassImportanceVolumeNoCrashTest,
    "PinWright.volume.create_lightmass_importance_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateLightmassImportanceVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_lightmass_importance_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_nav_mesh_bounds_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateNavMeshBoundsVolumeNoCrashTest,
    "PinWright.volume.create_nav_mesh_bounds_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateNavMeshBoundsVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_nav_mesh_bounds_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_nav_modifier_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateNavModifierVolumeNoCrashTest,
    "PinWright.volume.create_nav_modifier_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateNavModifierVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_nav_modifier_volume"), Payload));
    return true;
}

// ============================================================================
// volume.create_camera_blocking_volume — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateCameraBlockingVolumeNoCrashTest,
    "PinWright.volume.create_camera_blocking_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateCameraBlockingVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.create_camera_blocking_volume"), Payload));
    return true;
}

// ============================================================================
// volume.get_volumes_info — all params optional
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeGetVolumesInfoNoCrashTest,
    "PinWright.volume.get_volumes_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeGetVolumesInfoNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.get_volumes_info"), Payload));
    return true;
}

// ============================================================================
// volume.set_volume_extent — REQ: volumeName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumeExtentNoCrashTest,
    "PinWright.volume.set_volume_extent.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumeExtentNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), TEXT("TestVolume"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.set_volume_extent"), Payload));
    return true;
}

// ============================================================================
// volume.set_volume_properties — REQ: volumeName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumePropertiesNoCrashTest,
    "PinWright.volume.set_volume_properties.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumePropertiesNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), TEXT("TestVolume"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.set_volume_properties"), Payload));
    return true;
}

// ============================================================================
// volume.set_volume_properties — class-mismatch fail-loud regression
// ----------------------------------------------------------------------------
// Regression for E-volume-set-properties-class-mismatch-silent-noop: the verb
// applies each documented property only after a class-specific down-cast
// (Cast<APhysicsVolume>/APainCausingVolume/AAudioVolume). On a class-mismatched
// volume (the ATriggerVolume create_trigger_volume produces) every cast failed,
// nothing was added to propertiesSet, yet the handler still called SendSuccess
// with propertiesSet:[] — a success masking a total no-op, with no error or
// signal naming the dropped props.
//
// This test spawns a real TriggerVolume via the production create handler, then
// calls set_volume_properties with valid PhysicsVolume/PainCausingVolume props
// against it and asserts the handler now FAILS LOUD: the response is an error
// (not success), the code is CLASS_MISMATCH, and the error data's `skipped` array
// names the dropped props. If the fail-loud branch is reverted, the call returns
// isError:false again and these assertions fire.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumePropertiesClassMismatchFailsLoudTest,
    "PinWright.volume.set_volume_properties.ClassMismatchFailsLoud",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumePropertiesClassMismatchFailsLoudTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping class-mismatch fail-loud assertion"));
        return true;
    }

    // Spawn a real TriggerVolume via the production create handler (it is neither a
    // PhysicsVolume, PainCausingVolume, nor AudioVolume — so every property below is
    // class-mismatched).
    const FString VolumeLabel = FString::Printf(TEXT("McpClassMismatchTrigger_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("volumeName"), VolumeLabel);
    TSharedPtr<FJsonObject> LocationJson = MakeShared<FJsonObject>();
    LocationJson->SetNumberField(TEXT("x"), 6000.0);
    LocationJson->SetNumberField(TEXT("y"), 6000.0);
    LocationJson->SetNumberField(TEXT("z"), 100.0);
    CreatePayload->SetObjectField(TEXT("location"), LocationJson);

    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("volume.create_trigger_volume handler found"),
        InvokeHandlerWithCapture(TEXT("volume.create_trigger_volume"), CreatePayload, CreateCapture));
    if (!CreateCapture.bSuccess || !CreateCapture.Result.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("create_trigger_volume did not succeed — skipping"));
        return true;
    }

    // The handler may disambiguate the label; use the echoed volumeName for the readback.
    FString SpawnedName = VolumeLabel;
    CreateCapture.Result->TryGetStringField(TEXT("volumeName"), SpawnedName);

    // Locate the spawned TriggerVolume so we can clean it up regardless of outcome.
    ATriggerVolume* Spawned = nullptr;
    for (TActorIterator<ATriggerVolume> It(World); It; ++It)
    {
        if (It->GetActorLabel() == SpawnedName)
        {
            Spawned = *It;
            break;
        }
    }

    // Now set valid PhysicsVolume + PainCausingVolume props on the class-mismatched volume.
    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("volumeName"), SpawnedName);
    SetPayload->SetBoolField(TEXT("bWaterVolume"), true);
    SetPayload->SetNumberField(TEXT("priority"), 5.0);
    SetPayload->SetBoolField(TEXT("bPainCausing"), true);

    FTestResponseCapture SetCapture;
    TestTrue(TEXT("volume.set_volume_properties handler found"),
        InvokeHandlerWithCapture(TEXT("volume.set_volume_properties"), SetPayload, SetCapture));

    // Core regression assertions: a total class-mismatch no-op must FAIL, not succeed.
    TestFalse(TEXT("class-mismatched set_volume_properties is an error, not success"),
        SetCapture.bSuccess);
    TestEqual(TEXT("error code is CLASS_MISMATCH"),
        SetCapture.ErrorCode, FString(TEXT("CLASS_MISMATCH")));

    // The error data must name the dropped props so the omission is explicit.
    if (SetCapture.Result.IsValid())
    {
        TestTrue(TEXT("error data lists bWaterVolume as skipped"),
            JsonStringArrayContains(SetCapture.Result, TEXT("skipped"), TEXT("bWaterVolume")));
        TestTrue(TEXT("error data lists bPainCausing as skipped"),
            JsonStringArrayContains(SetCapture.Result, TEXT("skipped"), TEXT("bPainCausing")));
    }

    if (Spawned)
    {
        Spawned->Destroy();
    }
    return true;
}

// ============================================================================
// volume.set_volume_bounds — REQ: volumeName, bounds
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumeBoundsNoCrashTest,
    "PinWright.volume.set_volume_bounds.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumeBoundsNoCrashTest::RunTest(const FString& Parameters)
{
    // Provide volumeName + a 6-element bounds array [minX,minY,minZ,maxX,maxY,maxZ]
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), TEXT("TestVolume"));

    TArray<TSharedPtr<FJsonValue>> BoundsArray;
    for (double Val : {-500.0, -500.0, -200.0, 500.0, 500.0, 200.0})
    {
        BoundsArray.Add(MakeShared<FJsonValueNumber>(Val));
    }
    Payload->SetArrayField(TEXT("bounds"), BoundsArray);

    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.set_volume_bounds"), Payload));
    return true;
}

// ============================================================================
// volume.add_trigger_volume — REQ: actorPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeAddTriggerVolumeNoCrashTest,
    "PinWright.volume.add_trigger_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeAddTriggerVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), TEXT("NonExistentActor"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.add_trigger_volume"), Payload));
    return true;
}

// ============================================================================
// volume.add_blocking_volume — REQ: actorPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeAddBlockingVolumeNoCrashTest,
    "PinWright.volume.add_blocking_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeAddBlockingVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), TEXT("NonExistentActor"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.add_blocking_volume"), Payload));
    return true;
}

// ============================================================================
// volume.add_kill_z_volume — REQ: actorPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeAddKillZVolumeNoCrashTest,
    "PinWright.volume.add_kill_z_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeAddKillZVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), TEXT("NonExistentActor"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.add_kill_z_volume"), Payload));
    return true;
}

// ============================================================================
// volume.add_physics_volume — REQ: actorPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeAddPhysicsVolumeNoCrashTest,
    "PinWright.volume.add_physics_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeAddPhysicsVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), TEXT("NonExistentActor"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.add_physics_volume"), Payload));
    return true;
}

// ============================================================================
// volume.add_cull_distance_volume — REQ: actorPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeAddCullDistanceVolumeNoCrashTest,
    "PinWright.volume.add_cull_distance_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeAddCullDistanceVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), TEXT("NonExistentActor"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("volume.add_cull_distance_volume"), Payload));
    return true;
}

// ============================================================================
// volume.add_post_process_volume — REQ: actorPath (UE 5.1+, conditional registration)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeAddPostProcessVolumeNoCrashTest,
    "PinWright.volume.add_post_process_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeAddPostProcessVolumeNoCrashTest::RunTest(const FString& Parameters)
{
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), TEXT("NonExistentActor"));
    InvokeHandler(TEXT("volume.add_post_process_volume"), Payload);
    return true;
}

// ============================================================================
// volume.create_blocking_volume — brush geometry regression
// ----------------------------------------------------------------------------
// Regression for B-blocking-volume-no-brush-geometry: the create verb spawned a
// BlockingVolume whose Brush UModel was never initialized, so UCubeBuilder::Build
// silently wrote no geometry (engine EndBrush early-returns on a null Brush). The
// volume reported success but had a null brush, empty polys, and a zero-size
// bounding box — a collisionless phantom.
//
// This test exercises the real handler (volume.create_blocking_volume → the shared
// VolumeHelpers brush-build path) against the editor world and asserts the spawned
// volume has a non-null brush, populated polys, and a bounding box whose half-extent
// matches the requested extent. If the fix in CreateBoxBrushForVolume /
// BuildBoxBrushGeometry is reverted, the brush is null again and every assertion
// below fails (TestNotNull on Brush, TestTrue on poly count, TestTrue on bbox size).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateBlockingVolumeBrushGeometryTest,
    "PinWright.volume.create_blocking_volume.BuildsBrushGeometryWithCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateBlockingVolumeBrushGeometryTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping brush-geometry assertion"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString VolumeLabel = FString::Printf(TEXT("McpRegressionBlockingVolume_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FVector RequestedExtent(300.0f, 400.0f, 500.0f);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), VolumeLabel);

    TSharedPtr<FJsonObject> LocationJson = MakeShared<FJsonObject>();
    LocationJson->SetNumberField(TEXT("x"), 5000.0);
    LocationJson->SetNumberField(TEXT("y"), 5000.0);
    LocationJson->SetNumberField(TEXT("z"), 100.0);
    Payload->SetObjectField(TEXT("location"), LocationJson);

    TSharedPtr<FJsonObject> ExtentJson = MakeShared<FJsonObject>();
    ExtentJson->SetNumberField(TEXT("x"), RequestedExtent.X);
    ExtentJson->SetNumberField(TEXT("y"), RequestedExtent.Y);
    ExtentJson->SetNumberField(TEXT("z"), RequestedExtent.Z);
    Payload->SetObjectField(TEXT("extent"), ExtentJson);

    FTestResponseCapture Capture;
    TestTrue(TEXT("volume.create_blocking_volume handler found"),
        InvokeHandlerWithCapture(TEXT("volume.create_blocking_volume"), Payload, Capture));
    TestTrue(TEXT("volume.create_blocking_volume responded"), Capture.bWasCalled);
    TestTrue(TEXT("volume.create_blocking_volume succeeded"), Capture.bSuccess);

    FString ActorPath;
    TestTrue(TEXT("volume.create_blocking_volume returned actorPath"),
        Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("actorPath"), ActorPath));
    ABlockingVolume* Spawned = ActorPath.IsEmpty()
        ? nullptr
        : FindObject<ABlockingVolume>(nullptr, *ActorPath);

    TestNotNull(TEXT("ABlockingVolume was spawned"), Spawned);
    if (!Spawned)
    {
        return true;
    }

    // Core regression assertions: brush model and polys must actually exist.
    TestNotNull(TEXT("Volume->Brush UModel is initialized"), Spawned->Brush.Get());

    UBrushComponent* BrushComp = Spawned->GetBrushComponent();
    TestNotNull(TEXT("BrushComponent exists"), BrushComp);
    if (BrushComp)
    {
        TestNotNull(TEXT("BrushComponent->Brush is wired to the volume's UModel"), BrushComp->Brush.Get());
    }

    if (Spawned->Brush && Spawned->Brush->Polys)
    {
        TestTrue(TEXT("Brush has non-empty polys (box has faces)"),
            Spawned->Brush->Polys->Element.Num() > 0);
    }
    else
    {
        AddError(TEXT("Brush or Brush->Polys is null — geometry was never built"));
    }

    // World-space bounding box half-extent must match the requested extent (the box
    // builder receives full size = extent*2). A null-brush volume has a zero bbox.
    FVector Origin, BoxExtent;
    Spawned->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, BoxExtent);
    TestTrue(TEXT("Bounding box has non-zero size (geometry present)"),
        BoxExtent.X > 1.0f && BoxExtent.Y > 1.0f && BoxExtent.Z > 1.0f);

    const float Tolerance = 1.0f;
    TestTrue(TEXT("Bounding box half-extent X matches requested extent"),
        FMath::Abs(BoxExtent.X - RequestedExtent.X) <= Tolerance);
    TestTrue(TEXT("Bounding box half-extent Y matches requested extent"),
        FMath::Abs(BoxExtent.Y - RequestedExtent.Y) <= Tolerance);
    TestTrue(TEXT("Bounding box half-extent Z matches requested extent"),
        FMath::Abs(BoxExtent.Z - RequestedExtent.Z) <= Tolerance);

    return true;
}

// ============================================================================
// Dirty-flag contract for the volume verbs
//
// The bug guarded here: VolumeHandler.cpp carried no dirty ceremony at all.
// UWorld::SpawnActor dirties the level only under a transaction
// (`if (GUndo) ModifyLevel(...)`, LevelActor.cpp:735-739) and these handlers open
// none, and the three `set_*` verbs write actor/component UPROPERTYs and call
// SetActorScale3D / SetActorLocation / the brush builder — none of which mark any
// package. The viewport updated, `level.save` no-opped, and the edit vanished on
// close.
//
// Verb choice is deliberate, and the spawn verbs are the WEAK guards here.
// AActor::SetActorLabel compares against GetActorLabel(false) (ActorEditor.cpp:
// 1291-1320) and UWorld::SpawnActor calls ClearActorLabel() on every fresh actor
// (LevelActor.cpp:695-701), so on a spawn the stored label is empty, any non-empty
// name differs, and Modify(true) always fires. Every volume verb passes a non-empty
// volumeName, so on a classic (non-OFPA) map — where the actor package IS the level
// package — a "spawn then assert dirty" test cannot fail with the fix reverted. The
// load-bearing guards below are therefore the three `set_*` verbs, which set no
// label and whose only dirty path is the ceremony under test. The spawn guard
// (create_nav_mesh_bounds_volume) self-disables loudly on a classic map rather than
// passing vacuously.
//
// Every test restores the package dirty flags it clears. These run against the host
// project's real open level, and leaving a forced-clean flag behind would make a
// later `level.save` lose work — the exact failure this contract exists to prevent.
// ============================================================================

// GUID-suffixed, Mcp-prefixed volume label. Unique so SetActorLabel never appends
// its own "_N" disambiguator and the handlers' label lookup stays exact, and
// prefixed so it never collides with host-project content.
static FString PinWrightVolumeDirty_MakeLabel(const TCHAR* Prefix)
{
    return FString::Printf(TEXT("Mcp%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

// {x,y,z} object — the shape every volume verb reads for location and extent.
static TSharedPtr<FJsonObject> PinWrightVolumeDirty_Vec(double X, double Y, double Z)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("x"), X);
    Obj->SetNumberField(TEXT("y"), Y);
    Obj->SetNumberField(TEXT("z"), Z);
    return Obj;
}

// Spawns a fixture through its own create verb and returns the actor's REAL label.
// Reading it back from the response rather than assuming the requested string is
// what keeps the later handler lookup exact even if SetActorLabel rewrote it.
static FString PinWrightVolumeDirty_SpawnFixture(FAutomationTestBase& Test,
    const FString& Method, const TSharedPtr<FJsonObject>& Payload)
{
    FTestResponseCapture Capture;
    Test.TestTrue(*FString::Printf(TEXT("%s handler found"), *Method),
        InvokeHandlerWithCapture(Method, Payload, Capture));
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return FString();
    }
    FString Label;
    Capture.Result->TryGetStringField(TEXT("volumeName"), Label);
    return Label;
}

// Mirrors VolumeHelpers::FindVolumeByName (label match restricted to AVolume /
// ATriggerBase) so the test and the handler can never disagree about the target.
static AActor* PinWrightVolumeDirty_FindByLabel(UWorld* World, const FString& Label)
{
    if (!World || Label.IsEmpty())
    {
        return nullptr;
    }
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)
            && (Actor->IsA<AVolume>() || Actor->IsA<ATriggerBase>()))
        {
            return Actor;
        }
    }
    return nullptr;
}

// ---- volume.set_volume_extent — the non-brush branch dirties the package ----
// ATriggerSphere is an ATriggerBase, NOT an ABrush, so FindVolumeByName accepts it
// and set_volume_extent takes the SetActorScale3D else-branch: the brush-builder
// ceremony is not involved at all and this isolates the set_volume_extent hunk.
// SetActorScale3D writes RootComponent->RelativeScale3D and dirties nothing on its
// own, so with the hunk reverted the package stays clean.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumeExtentMarksPackageDirtyTest,
    "PinWright.volume.set_volume_extent.MarksPackageDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumeExtentMarksPackageDirtyTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping set_volume_extent dirty test"));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    // Captured BEFORE the fixture spawn, which dirties the level by design.
    const bool bLevelWasDirty = LevelPkg->IsDirty();

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("volumeName"), PinWrightVolumeDirty_MakeLabel(TEXT("ExtentSphere")));
    SpawnPayload->SetObjectField(TEXT("location"), PinWrightVolumeDirty_Vec(5000.0, 5000.0, 100.0));
    SpawnPayload->SetNumberField(TEXT("sphereRadius"), 100.0);

    const FString Label =
        PinWrightVolumeDirty_SpawnFixture(*this, TEXT("volume.create_trigger_sphere"), SpawnPayload);
    AActor* Volume = PinWrightVolumeDirty_FindByLabel(World, Label);
    TestNotNull(TEXT("ATriggerSphere fixture spawned"), Volume);
    if (!Volume)
    {
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
        return false;
    }

    UPackage* VolumePkg = Volume->GetPackage();

    // One block so the ordering is deterministic: destroy first (UWorld::DestroyActor
    // calls MarkPackageDirty unconditionally, LevelActor.cpp:1055), then restore the
    // flags we cleared. The reverse order would leave a forced-clean level behind.
    ON_SCOPE_EXIT
    {
        Volume->Destroy();
        VolumePkg->SetDirtyFlag(false);
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
    };

    VolumePkg->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), Label);
    Payload->SetObjectField(TEXT("extent"), PinWrightVolumeDirty_Vec(250.0, 250.0, 250.0));

    TestTrue(TEXT("volume.set_volume_extent handler found"),
        InvokeHandler(TEXT("volume.set_volume_extent"), Payload));

    TestTrue(TEXT("set_volume_extent dirties the volume's package"), VolumePkg->IsDirty());

    // Round-trip the value as well as the flag, so a future refactor cannot trade one
    // for the other. RelativeScale3D is asserted rather than GetActorBounds because
    // ATriggerBase carries an editor billboard (TriggerBase.cpp) whose screen-size-
    // scaled bounds join the actor bounds; the scale IS what the else-branch writes.
    const FVector Scale = Volume->GetActorScale3D();
    TestTrue(TEXT("extent/100 was applied as the actor scale"),
        FMath::IsNearlyEqual(Scale.X, 2.5, 1e-3)
        && FMath::IsNearlyEqual(Scale.Y, 2.5, 1e-3)
        && FMath::IsNearlyEqual(Scale.Z, 2.5, 1e-3));

    return true;
}

// ---- volume.set_volume_bounds — location move + brush rebuild dirty the package ----
// ABlockingVolume is a brush, so this exercises the set_volume_bounds hunk plus the
// BuildBoxBrushGeometry one. SetActorLocation and the brush rebuild both dirty
// nothing on their own, so with the ceremony reverted the package stays clean.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumeBoundsMarksPackageDirtyTest,
    "PinWright.volume.set_volume_bounds.MarksPackageDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumeBoundsMarksPackageDirtyTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping set_volume_bounds dirty test"));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    const bool bLevelWasDirty = LevelPkg->IsDirty();

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("volumeName"), PinWrightVolumeDirty_MakeLabel(TEXT("BoundsBlocking")));
    SpawnPayload->SetObjectField(TEXT("location"), PinWrightVolumeDirty_Vec(5000.0, 5000.0, 100.0));
    SpawnPayload->SetObjectField(TEXT("extent"), PinWrightVolumeDirty_Vec(100.0, 100.0, 100.0));

    const FString Label =
        PinWrightVolumeDirty_SpawnFixture(*this, TEXT("volume.create_blocking_volume"), SpawnPayload);
    AActor* Volume = PinWrightVolumeDirty_FindByLabel(World, Label);
    TestNotNull(TEXT("ABlockingVolume fixture spawned"), Volume);
    if (!Volume)
    {
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
        return false;
    }

    UPackage* VolumePkg = Volume->GetPackage();
    ON_SCOPE_EXIT
    {
        Volume->Destroy();
        VolumePkg->SetDirtyFlag(false);
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
    };

    VolumePkg->SetDirtyFlag(false);

    // Centre of [4800,4800,0]..[5200,5200,400] is (5000,5000,200) — a real move away
    // from the fixture's z=100 spawn, so the location assertion is discriminating.
    const FVector ExpectedCenter(5000.0, 5000.0, 200.0);
    TArray<TSharedPtr<FJsonValue>> Bounds;
    for (double Value : { 4800.0, 4800.0, 0.0, 5200.0, 5200.0, 400.0 })
    {
        Bounds.Add(MakeShared<FJsonValueNumber>(Value));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), Label);
    Payload->SetArrayField(TEXT("bounds"), Bounds);

    TestTrue(TEXT("volume.set_volume_bounds handler found"),
        InvokeHandler(TEXT("volume.set_volume_bounds"), Payload));

    TestTrue(TEXT("set_volume_bounds dirties the volume's package"), VolumePkg->IsDirty());
    TestTrue(TEXT("the volume was moved to the requested bounds centre"),
        Volume->GetActorLocation().Equals(ExpectedCenter, 1.0));

    return true;
}

// ---- volume.set_volume_properties — the UPROPERTY writes dirty the package ----
// APhysicsVolume::FluidFriction is a raw UPROPERTY write with no setter and no
// render-state push, so nothing dirties without the ceremony.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumePropertiesMarksPackageDirtyTest,
    "PinWright.volume.set_volume_properties.MarksPackageDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumePropertiesMarksPackageDirtyTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping set_volume_properties dirty test"));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    const bool bLevelWasDirty = LevelPkg->IsDirty();

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("volumeName"), PinWrightVolumeDirty_MakeLabel(TEXT("PropsPhysics")));
    SpawnPayload->SetObjectField(TEXT("location"), PinWrightVolumeDirty_Vec(5000.0, 5000.0, 100.0));
    SpawnPayload->SetObjectField(TEXT("extent"), PinWrightVolumeDirty_Vec(100.0, 100.0, 100.0));

    const FString Label =
        PinWrightVolumeDirty_SpawnFixture(*this, TEXT("volume.create_physics_volume"), SpawnPayload);
    APhysicsVolume* PhysicsVol = Cast<APhysicsVolume>(PinWrightVolumeDirty_FindByLabel(World, Label));
    TestNotNull(TEXT("APhysicsVolume fixture spawned"), PhysicsVol);
    if (!PhysicsVol)
    {
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
        return false;
    }

    UPackage* VolumePkg = PhysicsVol->GetPackage();
    ON_SCOPE_EXIT
    {
        PhysicsVol->Destroy();
        VolumePkg->SetDirtyFlag(false);
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
    };

    VolumePkg->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), Label);
    Payload->SetNumberField(TEXT("fluidFriction"), 0.7);

    FTestResponseCapture Capture;
    TestTrue(TEXT("volume.set_volume_properties handler found"),
        InvokeHandlerWithCapture(TEXT("volume.set_volume_properties"), Payload, Capture));

    TestTrue(TEXT("set_volume_properties dirties the volume's package"), VolumePkg->IsDirty());
    TestEqual(TEXT("FluidFriction round-trips"), PhysicsVol->FluidFriction, 0.7f);
    TestTrue(TEXT("the response reports fluidFriction in propertiesSet"),
        Capture.bSuccess
        && JsonStringArrayContains(Capture.Result, TEXT("propertiesSet"), TEXT("fluidFriction")));

    return true;
}

// ---- volume.set_volume_properties with no properties must leave the level clean ----
// Guards the `Requested.Num() > 0` gate. A zero-property call is the common probe
// shape; dropping the gate would make every probe dirty the level and prompt a save
// for a call that changed nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumePropertiesNoArgsLeavesPackageCleanTest,
    "PinWright.volume.set_volume_properties.NoArgsLeavesPackageClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumePropertiesNoArgsLeavesPackageCleanTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping set_volume_properties clean test"));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    const bool bLevelWasDirty = LevelPkg->IsDirty();

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("volumeName"), PinWrightVolumeDirty_MakeLabel(TEXT("NoArgsPhysics")));
    SpawnPayload->SetObjectField(TEXT("location"), PinWrightVolumeDirty_Vec(5000.0, 5000.0, 100.0));
    SpawnPayload->SetObjectField(TEXT("extent"), PinWrightVolumeDirty_Vec(100.0, 100.0, 100.0));

    const FString Label =
        PinWrightVolumeDirty_SpawnFixture(*this, TEXT("volume.create_physics_volume"), SpawnPayload);
    AActor* Volume = PinWrightVolumeDirty_FindByLabel(World, Label);
    TestNotNull(TEXT("APhysicsVolume fixture spawned"), Volume);
    if (!Volume)
    {
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
        return false;
    }

    UPackage* VolumePkg = Volume->GetPackage();
    ON_SCOPE_EXIT
    {
        Volume->Destroy();
        VolumePkg->SetDirtyFlag(false);
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
    };

    VolumePkg->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), Label);

    TestTrue(TEXT("volume.set_volume_properties handler found"),
        InvokeHandler(TEXT("volume.set_volume_properties"), Payload));

    TestFalse(TEXT("a no-property call leaves the volume's package clean"), VolumePkg->IsDirty());

    return true;
}

// ---- volume.get_volumes_info — read-only verb must leave the level clean ----
// Guards against an over-broad "dirty everything on every call" fix, mirroring the
// lighting.list_light_types guard in Tests/Environment/TestEnvironmentDirtyFlags.cpp.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeGetVolumesInfoLeavesPackageCleanTest,
    "PinWright.volume.get_volumes_info.LeavesPackageClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeGetVolumesInfoLeavesPackageCleanTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping get_volumes_info clean test"));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    const bool bWasDirty = LevelPkg->IsDirty();
    LevelPkg->SetDirtyFlag(false);
    ON_SCOPE_EXIT
    {
        LevelPkg->SetDirtyFlag(bWasDirty);
    };

    TestTrue(TEXT("volume.get_volumes_info handler found"),
        InvokeHandler(TEXT("volume.get_volumes_info"), MakeShared<FJsonObject>()));

    TestFalse(TEXT("a read-only verb leaves the level package clean"), LevelPkg->IsDirty());

    return true;
}

// ---- volume.create_nav_mesh_bounds_volume — the spawn dirties the LEVEL package ----
// create_nav_mesh_bounds_volume is chosen because it writes no post-spawn properties,
// so the SpawnVolumeActor ceremony is the only thing in play. This test is
// DELIBERATELY self-disabling rather than falsely green: see the header comment above
// — SetActorLabel dirties the ACTOR's package and every volume verb passes a non-empty
// volumeName, so on a classic map (actor package == level package) the assertion
// cannot fail with the ceremony reverted. Only under One-File-Per-Actor / World
// Partition are the two packages distinct and the assertion discriminating.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeCreateNavMeshBoundsVolumeMarksLevelPackageDirtyTest,
    "PinWright.volume.create_nav_mesh_bounds_volume.MarksLevelPackageDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeCreateNavMeshBoundsVolumeMarksLevelPackageDirtyTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping create_nav_mesh_bounds_volume dirty test"));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    const bool bLevelWasDirty = LevelPkg->IsDirty();
    LevelPkg->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("volumeName"), PinWrightVolumeDirty_MakeLabel(TEXT("NavBounds")));
    SpawnPayload->SetObjectField(TEXT("location"), PinWrightVolumeDirty_Vec(5000.0, 5000.0, 100.0));
    SpawnPayload->SetObjectField(TEXT("extent"), PinWrightVolumeDirty_Vec(200.0, 200.0, 200.0));

    const FString Label =
        PinWrightVolumeDirty_SpawnFixture(*this, TEXT("volume.create_nav_mesh_bounds_volume"), SpawnPayload);
    AActor* Volume = PinWrightVolumeDirty_FindByLabel(World, Label);
    TestNotNull(TEXT("ANavMeshBoundsVolume fixture spawned"), Volume);
    if (!Volume)
    {
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
        return false;
    }

    UPackage* VolumePkg = Volume->GetPackage();
    ON_SCOPE_EXIT
    {
        Volume->Destroy();
        VolumePkg->SetDirtyFlag(false);
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
    };

    ULevel* Level = Volume->GetLevel();
    TestNotNull(TEXT("the spawned volume has a level"), Level);
    if (!Level)
    {
        return false;
    }

    // Skip loudly rather than pass vacuously.
    if (VolumePkg == Level->GetPackage())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("actor-shares-level-package"),
            TEXT("Actor and level share a package on this map; the spawn-dirty assertion is "
                 "not discriminating here (SetActorLabel already dirties that one package). "
                 "Skipping — this guard only bites under One-File-Per-Actor / World Partition."));
        return true;
    }

    TestTrue(TEXT("Level package dirtied by the spawn"), Level->GetPackage()->IsDirty());

    return true;
}

// ============================================================================
// Regression for B-set-volume-extent-ignores-existing-actor-scale
//
// The brush branch of volume.set_volume_extent / volume.set_volume_bounds built
// correct LOCAL geometry and never normalized the actor's existing scale, so a
// volume that arrived carrying one ended up at extent x scale in WORLD space
// while the response echoed the requested extent. Measured live against a
// running editor: extent {8000,12000,3000} on an AProceduralFoliageVolume
// already at scale (80,120,30) — written by the verb that spawned it — produced
// a world half-extent of (640000,1440000,90000), and the call reported
// newExtent {8000,12000,3000} and success. AddActorVerification publishes
// neither bounds nor scale, so no field in the response contradicted it.
//
// The contract asserted here: `extent` (and set_volume_bounds' corner pair) is a
// WORLD measurement. The volume's measured world half-extent must equal the
// request whatever scale the actor was carrying, the scale must be unit
// afterwards, and the response must publish the MEASURED extent with the request
// named separately.
//
// Both tests fail on every assertion with the fix reverted: the bounds read
// (2x,3x,4x) the request, the actor is still at scale (2,3,4), newExtent /
// bounds echo the request while the actor measures something else, and
// requestedExtent / requestedBounds / clearedScale are absent.
// ============================================================================

// Reads an {x,y,z} object field. Returns false when the field is absent, which
// the callers assert on rather than silently comparing against a zero vector.
static bool PinWrightVolumeScale_ReadVector(const TSharedPtr<FJsonObject>& Source,
    const TCHAR* FieldName, FVector& OutVector)
{
    const TSharedPtr<FJsonObject>* Obj = nullptr;
    if (!Source.IsValid() || !Source->TryGetObjectField(FieldName, Obj) || !Obj || !Obj->IsValid())
    {
        return false;
    }
    OutVector = FVector((*Obj)->GetNumberField(TEXT("x")),
                        (*Obj)->GetNumberField(TEXT("y")),
                        (*Obj)->GetNumberField(TEXT("z")));
    return true;
}

// Reads a {min:[x,y,z], max:[x,y,z]} object field. Same absent-is-false rule.
static bool PinWrightVolumeScale_ReadCorners(const TSharedPtr<FJsonObject>& Source,
    const TCHAR* FieldName, FVector& OutMin, FVector& OutMax)
{
    const TSharedPtr<FJsonObject>* Obj = nullptr;
    if (!Source.IsValid() || !Source->TryGetObjectField(FieldName, Obj) || !Obj || !Obj->IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* MinArray = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* MaxArray = nullptr;
    if (!(*Obj)->TryGetArrayField(TEXT("min"), MinArray) || MinArray->Num() != 3
        || !(*Obj)->TryGetArrayField(TEXT("max"), MaxArray) || MaxArray->Num() != 3)
    {
        return false;
    }

    OutMin = FVector((*MinArray)[0]->AsNumber(), (*MinArray)[1]->AsNumber(), (*MinArray)[2]->AsNumber());
    OutMax = FVector((*MaxArray)[0]->AsNumber(), (*MaxArray)[1]->AsNumber(), (*MaxArray)[2]->AsNumber());
    return true;
}

// The pre-existing scale both tests plant on the fixture. foliage.create_procedural
// writes Size/200 on the volume it spawns; the exact shape does not matter, only
// that it is non-unit and PER-AXIS, so a uniform defect and a per-axis one cannot
// be confused for one another.
static const FVector PinWrightVolumeScale_PriorScale(2.0f, 3.0f, 4.0f);

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumeExtentNormalizesActorScaleTest,
    "PinWright.volume.set_volume_extent.NormalizesExistingActorScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumeExtentNormalizesActorScaleTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-unavailable"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so no brush volume could "
                 "be spawned and the world-extent measurement did not run."));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    const bool bLevelWasDirty = LevelPkg->IsDirty();

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("volumeName"), PinWrightVolumeDirty_MakeLabel(TEXT("ScaledExtent")));
    SpawnPayload->SetObjectField(TEXT("location"), PinWrightVolumeDirty_Vec(5000.0, 5000.0, 100.0));
    SpawnPayload->SetObjectField(TEXT("extent"), PinWrightVolumeDirty_Vec(100.0, 100.0, 100.0));

    // ABlockingVolume is an AVolume, so it is a genuine ABrush and takes the branch under test.
    const FString Label =
        PinWrightVolumeDirty_SpawnFixture(*this, TEXT("volume.create_blocking_volume"), SpawnPayload);
    AActor* Volume = PinWrightVolumeDirty_FindByLabel(World, Label);
    TestNotNull(TEXT("ABlockingVolume fixture spawned"), Volume);
    if (!Volume)
    {
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
        return false;
    }

    UPackage* VolumePkg = Volume->GetPackage();
    ON_SCOPE_EXIT
    {
        Volume->Destroy();
        VolumePkg->SetDirtyFlag(false);
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
    };

    Volume->SetActorScale3D(PinWrightVolumeScale_PriorScale);

    const FVector RequestedExtent(500.0f, 500.0f, 500.0f);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), Label);
    Payload->SetObjectField(TEXT("extent"),
        PinWrightVolumeDirty_Vec(RequestedExtent.X, RequestedExtent.Y, RequestedExtent.Z));

    FTestResponseCapture Capture;
    TestTrue(TEXT("volume.set_volume_extent handler found"),
        InvokeHandlerWithCapture(TEXT("volume.set_volume_extent"), Payload, Capture));
    TestTrue(TEXT("volume.set_volume_extent reported success"), Capture.bSuccess);

    // Ground truth. With the residual scale left in place this reads (1000,1500,2000).
    FVector MeasuredOrigin, MeasuredExtent;
    Volume->GetActorBounds(/*bOnlyCollidingComponents=*/false, MeasuredOrigin, MeasuredExtent);

    const float Tolerance = 1.0f;
    TestTrue(TEXT("measured world half-extent X equals the requested extent"),
        FMath::Abs(MeasuredExtent.X - RequestedExtent.X) <= Tolerance);
    TestTrue(TEXT("measured world half-extent Y equals the requested extent"),
        FMath::Abs(MeasuredExtent.Y - RequestedExtent.Y) <= Tolerance);
    TestTrue(TEXT("measured world half-extent Z equals the requested extent"),
        FMath::Abs(MeasuredExtent.Z - RequestedExtent.Z) <= Tolerance);

    TestTrue(TEXT("the pre-existing actor scale was normalized to unit"),
        Volume->GetActorScale3D().Equals(FVector::OneVector));

    FVector ReportedExtent;
    TestTrue(TEXT("response carries newExtent"),
        PinWrightVolumeScale_ReadVector(Capture.Result, TEXT("newExtent"), ReportedExtent));
    TestTrue(TEXT("newExtent is the measured world half-extent, not the request"),
        ReportedExtent.Equals(MeasuredExtent, Tolerance));

    FVector ReportedRequest;
    TestTrue(TEXT("response names the requested extent separately"),
        PinWrightVolumeScale_ReadVector(Capture.Result, TEXT("requestedExtent"), ReportedRequest));
    TestTrue(TEXT("requestedExtent carries what the call asked for"),
        ReportedRequest.Equals(RequestedExtent, Tolerance));

    FVector ReportedClearedScale;
    TestTrue(TEXT("response names the scale it cleared"),
        PinWrightVolumeScale_ReadVector(Capture.Result, TEXT("clearedScale"), ReportedClearedScale));
    TestTrue(TEXT("clearedScale is the scale the actor arrived carrying"),
        ReportedClearedScale.Equals(PinWrightVolumeScale_PriorScale));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVolumeSetVolumeBoundsNormalizesActorScaleTest,
    "PinWright.volume.set_volume_bounds.NormalizesExistingActorScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVolumeSetVolumeBoundsNormalizesActorScaleTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-unavailable"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null, so no brush volume could "
                 "be spawned and the world-bounds measurement did not run."));
        return true;
    }

    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    const bool bLevelWasDirty = LevelPkg->IsDirty();

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("volumeName"), PinWrightVolumeDirty_MakeLabel(TEXT("ScaledBounds")));
    SpawnPayload->SetObjectField(TEXT("location"), PinWrightVolumeDirty_Vec(5000.0, 5000.0, 100.0));
    SpawnPayload->SetObjectField(TEXT("extent"), PinWrightVolumeDirty_Vec(100.0, 100.0, 100.0));

    const FString Label =
        PinWrightVolumeDirty_SpawnFixture(*this, TEXT("volume.create_blocking_volume"), SpawnPayload);
    AActor* Volume = PinWrightVolumeDirty_FindByLabel(World, Label);
    TestNotNull(TEXT("ABlockingVolume fixture spawned"), Volume);
    if (!Volume)
    {
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
        return false;
    }

    UPackage* VolumePkg = Volume->GetPackage();
    ON_SCOPE_EXIT
    {
        Volume->Destroy();
        VolumePkg->SetDirtyFlag(false);
        LevelPkg->SetDirtyFlag(bLevelWasDirty);
    };

    Volume->SetActorScale3D(PinWrightVolumeScale_PriorScale);

    // Half-extent (300,400,500) about (5000,5000,100) — per-axis, and away from the origin so
    // the centre write is verifiable independently of the extent.
    const FVector RequestedMin(4700.0f, 4600.0f, -400.0f);
    const FVector RequestedMax(5300.0f, 5400.0f, 600.0f);
    const FVector RequestedExtent = (RequestedMax - RequestedMin) * 0.5f;
    const FVector RequestedCenter = (RequestedMax + RequestedMin) * 0.5f;

    TArray<TSharedPtr<FJsonValue>> BoundsArray;
    for (double Val : {RequestedMin.X, RequestedMin.Y, RequestedMin.Z,
                       RequestedMax.X, RequestedMax.Y, RequestedMax.Z})
    {
        BoundsArray.Add(MakeShared<FJsonValueNumber>(Val));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), Label);
    Payload->SetArrayField(TEXT("bounds"), BoundsArray);

    FTestResponseCapture Capture;
    TestTrue(TEXT("volume.set_volume_bounds handler found"),
        InvokeHandlerWithCapture(TEXT("volume.set_volume_bounds"), Payload, Capture));
    TestTrue(TEXT("volume.set_volume_bounds reported success"), Capture.bSuccess);

    FVector MeasuredOrigin, MeasuredExtent;
    Volume->GetActorBounds(/*bOnlyCollidingComponents=*/false, MeasuredOrigin, MeasuredExtent);

    const float Tolerance = 1.0f;
    TestTrue(TEXT("measured world half-extent X spans the requested corners"),
        FMath::Abs(MeasuredExtent.X - RequestedExtent.X) <= Tolerance);
    TestTrue(TEXT("measured world half-extent Y spans the requested corners"),
        FMath::Abs(MeasuredExtent.Y - RequestedExtent.Y) <= Tolerance);
    TestTrue(TEXT("measured world half-extent Z spans the requested corners"),
        FMath::Abs(MeasuredExtent.Z - RequestedExtent.Z) <= Tolerance);
    TestTrue(TEXT("measured centre is the requested centre"),
        MeasuredOrigin.Equals(RequestedCenter, Tolerance));

    TestTrue(TEXT("the pre-existing actor scale was normalized to unit"),
        Volume->GetActorScale3D().Equals(FVector::OneVector));

    FVector ReportedMin, ReportedMax;
    TestTrue(TEXT("response carries bounds"),
        PinWrightVolumeScale_ReadCorners(Capture.Result, TEXT("bounds"), ReportedMin, ReportedMax));
    TestTrue(TEXT("bounds min is measured, not the request echoed"),
        ReportedMin.Equals(MeasuredOrigin - MeasuredExtent, Tolerance));
    TestTrue(TEXT("bounds max is measured, not the request echoed"),
        ReportedMax.Equals(MeasuredOrigin + MeasuredExtent, Tolerance));

    FVector RequestedBoundsMin, RequestedBoundsMax;
    TestTrue(TEXT("response names the requested bounds separately"),
        PinWrightVolumeScale_ReadCorners(Capture.Result, TEXT("requestedBounds"),
            RequestedBoundsMin, RequestedBoundsMax));
    TestTrue(TEXT("requestedBounds min carries what the call asked for"),
        RequestedBoundsMin.Equals(RequestedMin, Tolerance));
    TestTrue(TEXT("requestedBounds max carries what the call asked for"),
        RequestedBoundsMax.Equals(RequestedMax, Tolerance));

    FVector ReportedClearedScale;
    TestTrue(TEXT("response names the scale it cleared"),
        PinWrightVolumeScale_ReadVector(Capture.Result, TEXT("clearedScale"), ReportedClearedScale));
    TestTrue(TEXT("clearedScale is the scale the actor arrived carrying"),
        ReportedClearedScale.Equals(PinWrightVolumeScale_PriorScale));

    return true;
}
