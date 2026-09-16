// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for VFX domain handlers
// Covers: EffectHandler.cpp
//   - Debug shapes: list_debug_shapes, clear_debug_shapes, draw_debug_shape
//   - Niagara runtime: set_niagara_parameter, activate_niagara, deactivate_niagara,
//                      advance_simulation, spawn_niagara
//   - World: cleanup
//   - Niagara module authoring (modules 1–30): add_spawn_rate_module through add_simulation_stage
#include "Misc/AutomationTest.h"
#include "Dispatch/SafePoint.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/TestUtils.h"

#include <limits>

namespace PinWrightVFXHandlerTests
{
    bool ReadSource(const FString& RelativePath, FString& OutSource)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return false;
        }
        return FFileHelper::LoadFileToString(
            OutSource, *FPaths::Combine(Plugin->GetBaseDir(), RelativePath));
    }
}

// ============================================================================
// effect.list_debug_shapes
// No required params — returns the list of supported shape names
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXListDebugShapesValidParamsNoCrashTest,
    "PinWright.effect.list_debug_shapes.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXListDebugShapesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.list_debug_shapes"), Payload));
    return true;
}

// ============================================================================
// effect.list_debug_shapes returns a SELF-DESCRIBING result so the method name
// (which parallels the world-scoped draw_debug_shape / clear_debug_shapes) does
// not read as "enumerate what is currently drawn" — board
// E-effect-list-debug-shapes-types-not-drawn. The static type catalog must be
// reachable under a `shapeTypes` key and the result must carry a `note` that
// disambiguates types-vs-drawn at the call site. Reverting the fix (dropping the
// `shapeTypes` key and the `note`) flips these assertions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXListDebugShapesResultIsSelfDescribingTest,
    "PinWright.effect.list_debug_shapes.ResultIsSelfDescribing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXListDebugShapesResultIsSelfDescribingTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandlerWithCapture(TEXT("effect.list_debug_shapes"), Payload, Capture));
    TestTrue(TEXT("list_debug_shapes succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // The catalog must be reachable under the self-describing `shapeTypes` key,
    // not only the ambiguous `shapes` key, and must still contain the known types.
    // The membership asserts below each do their own TryGetArrayField on
    // `shapeTypes`, so a missing or non-array field already fails them — no
    // separate presence assertion is needed.
    TestTrue(TEXT("shapeTypes includes sphere"),
        JsonStringArrayContains(Capture.Result, TEXT("shapeTypes"), TEXT("sphere")));
    TestTrue(TEXT("shapeTypes includes arrow"),
        JsonStringArrayContains(Capture.Result, TEXT("shapeTypes"), TEXT("arrow")));

    // The result must carry a `note` that disambiguates supported TYPES from the
    // shapes currently drawn, so the call-site reading is corrected without a
    // method rename. The note must mention the sibling draw/teardown verbs.
    FString Note;
    TestTrue(TEXT("result carries a note string"),
        Capture.Result->TryGetStringField(TEXT("note"), Note));
    TestTrue(TEXT("note disambiguates TYPES from drawn shapes"),
        Note.Contains(TEXT("not the shapes currently drawn")));
    TestTrue(TEXT("note points at the draw verb"),
        Note.Contains(TEXT("effect.draw_debug_shape")));
    return true;
}

// ============================================================================
// effect.clear_debug_shapes
// No required params — flushes persistent debug lines from the editor world
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXClearDebugShapesValidParamsNoCrashTest,
    "PinWright.effect.clear_debug_shapes.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXClearDebugShapesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.clear_debug_shapes"), Payload));
    return true;
}

// ============================================================================
// effect.draw_debug_shape
// REQ: preset
// OPT: shapeType, location, rotation, scale, color, duration, size, thickness
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXDrawDebugShapeValidParamsNoCrashTest,
    "PinWright.effect.draw_debug_shape.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXDrawDebugShapeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("preset"), TEXT("default"));
    Payload->SetStringField(TEXT("shapeType"), TEXT("sphere"));
    Payload->SetNumberField(TEXT("size"), 50.0);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.draw_debug_shape"), Payload));
    return true;
}

// ============================================================================
// effect.set_niagara_parameter
// REQ: parameterName
// OPT: systemName, parameterType, value
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXSetNiagaraParameterValidParamsNoCrashTest,
    "PinWright.effect.set_niagara_parameter.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXSetNiagaraParameterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemName"), TEXT("NS_Fire"));
    Payload->SetStringField(TEXT("parameterName"), TEXT("SpawnRate"));
    Payload->SetStringField(TEXT("parameterType"), TEXT("Float"));
    Payload->SetNumberField(TEXT("value"), 200.0);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.set_niagara_parameter"), Payload));
    return true;
}

// ============================================================================
// effect.activate_niagara
// REQ: systemName
// OPT: reset
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXActivateNiagaraValidParamsNoCrashTest,
    "PinWright.effect.activate_niagara.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXActivateNiagaraValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemName"), TEXT("NS_FireActor"));
    Payload->SetBoolField(TEXT("reset"), true);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.activate_niagara"), Payload));
    return true;
}

// ============================================================================
// effect.deactivate_niagara
// REQ: systemName (also accepts actorName)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXDeactivateNiagaraValidParamsNoCrashTest,
    "PinWright.effect.deactivate_niagara.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXDeactivateNiagaraValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemName"), TEXT("NS_FireActor"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.deactivate_niagara"), Payload));
    return true;
}

// ============================================================================
// effect.advance_simulation
// REQ: systemName (also accepts actorName)
// OPT: deltaTime, steps
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXAdvanceSimulationValidParamsNoCrashTest,
    "PinWright.effect.advance_simulation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXAdvanceSimulationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemName"), TEXT("NS_FireActor"));
    Payload->SetNumberField(TEXT("deltaTime"), 0.033);
    Payload->SetNumberField(TEXT("steps"), 3);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.advance_simulation"), Payload));
    return true;
}

// The validation path must reject before resolving a Niagara actor, so this test never needs
// an editor-world fixture or advances a real system. InvokeHandlerWithCapture intentionally
// exercises the handler body directly; the safe-point table assertion below covers routing,
// while dispatcher behavior remains covered by the shared safe-point tests. Every refusal names
// the shared numeric limits, which keeps the contract actionable for callers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXAdvanceSimulationRejectsOutOfRangeParamsTest,
    "PinWright.effect.advance_simulation.RejectsOutOfRangeParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXAdvanceSimulationRejectsOutOfRangeParamsTest::RunTest(const FString& Parameters)
{
    struct FInvalidProbe
    {
        const TCHAR* Description;
        double Steps;
        double DeltaTime;
    };

    const FInvalidProbe Probes[] = {
        {TEXT("negative steps"), -1.0, 0.1},
        {TEXT("non-finite steps"), std::numeric_limits<double>::infinity(), 0.1},
        {TEXT("near-fractional steps"), 3.000000005, 0.1},
        {TEXT("non-finite deltaTime"), 1.0, std::numeric_limits<double>::infinity()},
        {TEXT("zero steps"), 0.0, 0.1},
        {TEXT("fractional steps"), 1.5, 0.1},
        {TEXT("steps above the 10000 cap"), 10001.0, 0.1},
        {TEXT("zero deltaTime"), 1.0, 0.0},
        {TEXT("negative deltaTime"), 1.0, -0.1},
        {TEXT("deltaTime below the 0.0001 minimum"), 1.0, 0.00001},
        {TEXT("total duration above the 60 second cap"), 10000.0, 0.0061},
    };

    TestTrue(TEXT("advance_simulation is safe-point gated"),
        PinWrightSafePoint::IsTickUnsafeMethod(TEXT("effect.advance_simulation")));

    for (const FInvalidProbe& Probe : Probes)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemName"), TEXT("not-resolved"));
        Payload->SetNumberField(TEXT("steps"), Probe.Steps);
        Payload->SetNumberField(TEXT("deltaTime"), Probe.DeltaTime);

        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("%s reaches the registered handler"), Probe.Description),
            InvokeHandlerWithCapture(TEXT("effect.advance_simulation"), Payload, Capture));
        TestFalse(*FString::Printf(TEXT("%s is rejected before target resolution"), Probe.Description),
            Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s returns INVALID_PARAMS"), Probe.Description),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestTrue(*FString::Printf(TEXT("%s names the per-step minimum"), Probe.Description),
            Capture.Message.Contains(TEXT("0.0001")));
        TestTrue(*FString::Printf(TEXT("%s names the step-count cap"), Probe.Description),
            Capture.Message.Contains(TEXT("10000")));
        TestTrue(*FString::Printf(TEXT("%s names the total-duration cap"), Probe.Description),
            Capture.Message.Contains(TEXT("60 seconds")));
    }

    FString EffectSource;
    if (TestTrue(TEXT("EffectHandler.cpp is readable"), PinWrightVFXHandlerTests::ReadSource(
            TEXT("Source/PinWright/Private/Handlers/VFX/EffectHandler.cpp"), EffectSource)))
    {
        const int32 AdvanceStart = EffectSource.Find(
            TEXT("REGISTER_RPC_HANDLER(\"effect.advance_simulation\""));
        const int32 StepAndCaptureStart = EffectSource.Find(
            TEXT("REGISTER_RPC_HANDLER(\"effect.step_and_capture\""),
            ESearchCase::CaseSensitive, ESearchDir::FromStart, AdvanceStart);
        if (TestTrue(TEXT("advance_simulation registration source found"),
                AdvanceStart != INDEX_NONE && StepAndCaptureStart > AdvanceStart))
        {
            const FString AdvanceSource = EffectSource.Mid(
                AdvanceStart, StepAndCaptureStart - AdvanceStart);
            TestTrue(TEXT("advance_simulation success reports requestedSeconds"),
                AdvanceSource.Contains(
                    TEXT("Resp->SetNumberField(TEXT(\"requestedSeconds\")")));
            TestTrue(TEXT("advance_simulation success reports simulatedSeconds"),
                AdvanceSource.Contains(
                    TEXT("Resp->SetNumberField(TEXT(\"simulatedSeconds\")")));
            TestTrue(TEXT("advance_simulation reports early-stop details"),
                AdvanceSource.Contains(TEXT("AdvanceResult.bStoppedEarly")) &&
                    AdvanceSource.Contains(TEXT("AdvanceResult.StopReason")));
        }
    }

    FString RuntimeSource;
    if (TestTrue(TEXT("EffectRuntimeUtils.cpp is readable"), PinWrightVFXHandlerTests::ReadSource(
            TEXT("Source/PinWright/Private/Handlers/VFX/EffectRuntimeUtils.cpp"), RuntimeSource)))
    {
        TestTrue(TEXT("advance measures simulation age before and after Niagara ticks"),
            RuntimeSource.Contains(TEXT("Controller->GetAge()")));
    }

    return true;
}

// ============================================================================
// effect.cleanup
// OPT: filter — when omitted the handler returns removed=0 immediately
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXCleanupNoFilterValidParamsNoCrashTest,
    "PinWright.effect.cleanup.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXCleanupNoFilterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Empty payload — handler short-circuits and returns {removed:0}
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.cleanup"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXCleanupWithFilterValidParamsNoCrashTest,
    "PinWright.effect.cleanup.WithFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXCleanupWithFilterValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("filter"), TEXT("NS_"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.cleanup"), Payload));
    return true;
}

// ============================================================================
// effect.spawn_niagara
// REQ: systemPath
// OPT: location, rotation, scale, autoDestroy, attachToActor, name
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXSpawnNiagaraValidParamsNoCrashTest,
    "PinWright.effect.spawn_niagara.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXSpawnNiagaraValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemPath"), TEXT("/Game/Effects/NS_Fire"));
    Payload->SetStringField(TEXT("name"), TEXT("FireEffect"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("effect.spawn_niagara"), Payload));
    return true;
}

// ============================================================================
// Removed effect.* Niagara asset-authoring handlers
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXRemovedNiagaraAssetAuthoringHandlersAbsentTest,
    "PinWright.effect.RemovedNiagaraAssetAuthoringHandlersAbsent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXRemovedNiagaraAssetAuthoringHandlersAbsentTest::RunTest(const FString& Parameters)
{
    const TCHAR* RemovedHandlers[] = {
        TEXT("effect.add_spawn_rate_module"),
        TEXT("effect.add_spawn_burst_module"),
        TEXT("effect.add_spawn_per_unit_module"),
        TEXT("effect.add_initialize_particle_module"),
        TEXT("effect.add_particle_state_module"),
        TEXT("effect.add_force_module"),
        TEXT("effect.add_velocity_module"),
        TEXT("effect.add_acceleration_module"),
        TEXT("effect.add_size_module"),
        TEXT("effect.add_color_module"),
        TEXT("effect.add_collision_module"),
        TEXT("effect.add_kill_particles_module"),
        TEXT("effect.add_camera_offset_module"),
        TEXT("effect.add_sprite_renderer_module"),
        TEXT("effect.add_mesh_renderer_module"),
        TEXT("effect.add_ribbon_renderer_module"),
        TEXT("effect.add_light_renderer_module"),
        TEXT("effect.add_skeletal_mesh_data_interface"),
        TEXT("effect.add_static_mesh_data_interface"),
        TEXT("effect.add_spline_data_interface"),
        TEXT("effect.add_audio_spectrum_data_interface"),
        TEXT("effect.add_collision_query_data_interface"),
        TEXT("effect.add_event_generator"),
        TEXT("effect.add_event_receiver"),
        TEXT("effect.configure_event_payload"),
        TEXT("effect.add_user_parameter"),
        TEXT("effect.set_parameter_value"),
        TEXT("effect.bind_parameter_to_source"),
        TEXT("effect.enable_gpu_simulation"),
        TEXT("effect.add_simulation_stage")
    };

    for (const TCHAR* HandlerName : RemovedHandlers)
    {
        TestFalse(FString::Printf(TEXT("%s is not registered"), HandlerName), IsRegistered(HandlerName));
    }

    return true;
}
