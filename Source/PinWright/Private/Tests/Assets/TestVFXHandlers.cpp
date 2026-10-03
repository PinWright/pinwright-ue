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
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Components/LineBatchComponent.h"
#include "Dom/JsonObject.h"
#include "DrawDebugHelpers.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/TestUtils.h"

#include <initializer_list>
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
// effect.draw_debug_shape honours scale, plane boxSize and autoDestroy, and refuses a
// control the selected shape does not draw with (board B-effect-debug-options-ignored).
// The assertions read the editor world's persistent line batcher directly, so a
// response echo cannot stand in for the draw. Before the fix: scale and plane boxSize
// never changed the lines, autoDestroy:false still drew a 5 s timed shape, and every
// refused case below returned success.
// ============================================================================

namespace PinWrightVFXDebugShapeTests
{
    UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Timed (duration > 0) and persistent draws both land in the WorldPersistent batcher
    // (DrawDebugHelpers.cpp GetDebugLineBatcher).
    ULineBatchComponent* PersistentBatcher(UWorld* World)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        return World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent);
#else
        return World->PersistentLineBatcher;
#endif
    }

    TSharedPtr<FJsonObject> ShapePayload(const TCHAR* Shape)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("preset"), TEXT("default"));
        Payload->SetStringField(TEXT("shapeType"), Shape);
        return Payload;
    }

    TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const double V : Values)
        {
            Out.Add(MakeShared<FJsonValueNumber>(V));
        }
        return Out;
    }

    // Half-size of the axis-aligned box spanned by every batched line endpoint, relative to Center.
    FVector LineHalfExtent(const ULineBatchComponent* Batcher, const FVector& Center)
    {
        FVector Max = FVector::ZeroVector;
        for (const FBatchedLine& Line : Batcher->BatchedLines)
        {
            Max = Max.ComponentMax((Line.Start - Center).GetAbs());
            Max = Max.ComponentMax((Line.End - Center).GetAbs());
        }
        return Max;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXDrawDebugShapeScaleAndBoxSizeReachDrawnLinesTest,
    "PinWright.effect.draw_debug_shape.ScaleAndBoxSizeReachDrawnLines",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXDrawDebugShapeScaleAndBoxSizeReachDrawnLinesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightVFXDebugShapeTests;
    UWorld* World = EditorWorld();
    ULineBatchComponent* Batcher = World ? PersistentBatcher(World) : nullptr;
    if (!TestNotNull(TEXT("editor world has a persistent line batcher"), Batcher))
    {
        return false;
    }
    const FVector Center(1000.0, 2000.0, 3000.0);

    // Plane: boxSize [100,50,10] times per-axis scale [2,3,1] -> half-extent (200,150,10).
    FlushPersistentDebugLines(World);
    TSharedPtr<FJsonObject> Plane = ShapePayload(TEXT("plane"));
    Plane->SetArrayField(TEXT("location"), Numbers({Center.X, Center.Y, Center.Z}));
    Plane->SetArrayField(TEXT("boxSize"), Numbers({100.0, 50.0, 10.0}));
    Plane->SetArrayField(TEXT("scale"), Numbers({2.0, 3.0, 1.0}));
    Plane->SetNumberField(TEXT("duration"), 30.0);
    FTestResponseCapture PlaneCapture;
    InvokeHandlerWithCapture(TEXT("effect.draw_debug_shape"), Plane, PlaneCapture);
    TestTrue(FString::Printf(TEXT("plane draw succeeds (%s: %s)"), *PlaneCapture.ErrorCode, *PlaneCapture.Message),
        PlaneCapture.bSuccess);
    TestEqual(TEXT("plane draws 12 box edges"), Batcher->BatchedLines.Num(), 12);
    TestTrue(TEXT("plane lines span boxSize * scale = (200,150,10)"),
        LineHalfExtent(Batcher, Center).Equals(FVector(200.0, 150.0, 10.0), 0.01));
    const TSharedPtr<FJsonObject>* Geometry = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Extent = nullptr;
    if (TestTrue(TEXT("plane response echoes geometry.extent"),
            PlaneCapture.Result.IsValid() && PlaneCapture.Result->TryGetObjectField(TEXT("geometry"), Geometry)
            && (*Geometry)->TryGetArrayField(TEXT("extent"), Extent) && Extent->Num() == 3))
    {
        TestEqual(TEXT("echoed extent x"), (*Extent)[0]->AsNumber(), 200.0);
        TestEqual(TEXT("echoed extent y"), (*Extent)[1]->AsNumber(), 150.0);
    }

    // Box: size 10 with a uniform scalar scale 3 -> half-extent 30 on every axis.
    FlushPersistentDebugLines(World);
    TSharedPtr<FJsonObject> Box = ShapePayload(TEXT("box"));
    Box->SetArrayField(TEXT("location"), Numbers({Center.X, Center.Y, Center.Z}));
    Box->SetNumberField(TEXT("size"), 10.0);
    Box->SetNumberField(TEXT("scale"), 3.0);
    FTestResponseCapture BoxCapture;
    InvokeHandlerWithCapture(TEXT("effect.draw_debug_shape"), Box, BoxCapture);
    TestTrue(TEXT("box draw succeeds"), BoxCapture.bSuccess);
    TestTrue(TEXT("box lines span size * scale = 30"),
        LineHalfExtent(Batcher, Center).Equals(FVector(30.0), 0.01));

    // Sphere: radius 10 with scale 3 -> every endpoint within radius 30 and reaching past 27.
    FlushPersistentDebugLines(World);
    TSharedPtr<FJsonObject> Sphere = ShapePayload(TEXT("sphere"));
    Sphere->SetArrayField(TEXT("location"), Numbers({Center.X, Center.Y, Center.Z}));
    Sphere->SetNumberField(TEXT("size"), 10.0);
    Sphere->SetNumberField(TEXT("scale"), 3.0);
    FTestResponseCapture SphereCapture;
    InvokeHandlerWithCapture(TEXT("effect.draw_debug_shape"), Sphere, SphereCapture);
    TestTrue(TEXT("sphere draw succeeds"), SphereCapture.bSuccess);
    const double SphereReach = LineHalfExtent(Batcher, Center).GetMax();
    TestTrue(FString::Printf(TEXT("sphere radius is size * scale = 30 (measured %.3f)"), SphereReach),
        SphereReach > 27.0 && SphereReach < 30.01);

    FlushPersistentDebugLines(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXDrawDebugShapeAutoDestroyMapsToLineLifetimeTest,
    "PinWright.effect.draw_debug_shape.AutoDestroyMapsToLineLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXDrawDebugShapeAutoDestroyMapsToLineLifetimeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightVFXDebugShapeTests;
    UWorld* World = EditorWorld();
    ULineBatchComponent* Batcher = World ? PersistentBatcher(World) : nullptr;
    if (!TestNotNull(TEXT("editor world has a persistent line batcher"), Batcher))
    {
        return false;
    }

    // autoDestroy false: persistent lines (engine lifetime -1) that only clear_debug_shapes removes.
    FlushPersistentDebugLines(World);
    TSharedPtr<FJsonObject> Persistent = ShapePayload(TEXT("sphere"));
    Persistent->SetBoolField(TEXT("autoDestroy"), false);
    FTestResponseCapture PersistentCapture;
    InvokeHandlerWithCapture(TEXT("effect.draw_debug_shape"), Persistent, PersistentCapture);
    TestTrue(TEXT("persistent draw succeeds"), PersistentCapture.bSuccess);
    TestTrue(TEXT("persistent draw added lines"), Batcher->BatchedLines.Num() > 0);
    bool bAllPersistent = Batcher->BatchedLines.Num() > 0;
    for (const FBatchedLine& Line : Batcher->BatchedLines)
    {
        bAllPersistent &= Line.RemainingLifeTime == -1.0f;
    }
    TestTrue(TEXT("autoDestroy false draws persistent lines (lifetime -1)"), bAllPersistent);
    bool bPersistentEcho = false;
    TestTrue(TEXT("response echoes persistent true"), PersistentCapture.Result.IsValid()
        && PersistentCapture.Result->TryGetBoolField(TEXT("persistent"), bPersistentEcho) && bPersistentEcho);
    TestFalse(TEXT("persistent response carries no duration"),
        PersistentCapture.Result.IsValid() && PersistentCapture.Result->HasField(TEXT("duration")));

    // Default (autoDestroy true): the lines expire after `duration` seconds.
    FlushPersistentDebugLines(World);
    TSharedPtr<FJsonObject> Timed = ShapePayload(TEXT("sphere"));
    Timed->SetNumberField(TEXT("duration"), 7.0);
    FTestResponseCapture TimedCapture;
    InvokeHandlerWithCapture(TEXT("effect.draw_debug_shape"), Timed, TimedCapture);
    TestTrue(TEXT("timed draw succeeds"), TimedCapture.bSuccess);
    bool bAllTimed = Batcher->BatchedLines.Num() > 0;
    for (const FBatchedLine& Line : Batcher->BatchedLines)
    {
        bAllTimed &= FMath::IsNearlyEqual(Line.RemainingLifeTime, 7.0f);
    }
    TestTrue(TEXT("default draw expires after duration (lifetime 7)"), bAllTimed);
    bool bTimedPersistentEcho = true;
    TestTrue(TEXT("response echoes persistent false"), TimedCapture.Result.IsValid()
        && TimedCapture.Result->TryGetBoolField(TEXT("persistent"), bTimedPersistentEcho) && !bTimedPersistentEcho);

    FlushPersistentDebugLines(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVFXDrawDebugShapeRefusesIgnoredControlsTest,
    "PinWright.effect.draw_debug_shape.RefusesIgnoredControls",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVFXDrawDebugShapeRefusesIgnoredControlsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightVFXDebugShapeTests;
    UWorld* World = EditorWorld();
    ULineBatchComponent* Batcher = World ? PersistentBatcher(World) : nullptr;
    if (!TestNotNull(TEXT("editor world has a persistent line batcher"), Batcher))
    {
        return false;
    }

    TArray<TPair<FString, TSharedPtr<FJsonObject>>> Cases;
    auto Add = [&Cases](const TCHAR* Label, const TCHAR* Shape) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Payload = ShapePayload(Shape);
        Payload->SetNumberField(TEXT("duration"), 30.0);
        Cases.Emplace(Label, Payload);
        return Payload;
    };
    Add(TEXT("line has no dimension to scale"), TEXT("line"))->SetNumberField(TEXT("scale"), 2.0);
    Add(TEXT("sphere cannot take a non-uniform scale"), TEXT("sphere"))->SetArrayField(TEXT("scale"), Numbers({1.0, 2.0, 3.0}));
    Add(TEXT("sphere does not draw with boxSize"), TEXT("sphere"))->SetArrayField(TEXT("boxSize"), Numbers({1.0, 2.0, 3.0}));
    Add(TEXT("coordinate does not draw with color"), TEXT("coordinate"))->SetArrayField(TEXT("color"), Numbers({255.0, 0.0, 0.0}));
    Add(TEXT("point does not draw with thickness"), TEXT("point"))->SetNumberField(TEXT("thickness"), 4.0);
    Add(TEXT("plane boxSize must have three numbers"), TEXT("plane"))->SetArrayField(TEXT("boxSize"), Numbers({1.0, 2.0}));
    Add(TEXT("box scale must be non-negative"), TEXT("box"))->SetArrayField(TEXT("scale"), Numbers({1.0, -1.0, 1.0}));
    Add(TEXT("negative duration"), TEXT("sphere"))->SetNumberField(TEXT("duration"), -1.0);
    Add(TEXT("negative size"), TEXT("sphere"))->SetNumberField(TEXT("size"), -10.0);
    Add(TEXT("negative thickness"), TEXT("box"))->SetNumberField(TEXT("thickness"), -2.0);
    Add(TEXT("negative cone length"), TEXT("cone"))->SetNumberField(TEXT("length"), -100.0);
    Add(TEXT("negative cone angle"), TEXT("cone"))->SetNumberField(TEXT("angle"), -45.0);
    Add(TEXT("negative capsule halfHeight"), TEXT("capsule"))->SetNumberField(TEXT("halfHeight"), -50.0);
    {
        TSharedPtr<FJsonObject> Both = Add(TEXT("size and boxSize together"), TEXT("box"));
        Both->SetNumberField(TEXT("size"), 10.0);
        Both->SetArrayField(TEXT("boxSize"), Numbers({1.0, 2.0, 3.0}));
    }
    Add(TEXT("duration with autoDestroy false"), TEXT("sphere"))->SetBoolField(TEXT("autoDestroy"), false);

    for (const TPair<FString, TSharedPtr<FJsonObject>>& Case : Cases)
    {
        FlushPersistentDebugLines(World);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("effect.draw_debug_shape"), Case.Value, Capture);
        TestFalse(FString::Printf(TEXT("%s: refused"), *Case.Key), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s: INVALID_ARGUMENT"), *Case.Key), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestEqual(FString::Printf(TEXT("%s: nothing drawn"), *Case.Key), Batcher->BatchedLines.Num(), 0);
    }

    FlushPersistentDebugLines(World);
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
