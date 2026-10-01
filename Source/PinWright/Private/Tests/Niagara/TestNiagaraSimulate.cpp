// Copyright (c) 2026 Alexander Penkin. MIT License.

// Coverage for niagara.simulate (F-niagara-simulate-emission-counts): the verb that proves a
// Niagara system emits by reading live particle counts, rather than inferring it from the graph.
//
// The pair that makes `emitted` mean something is SpawningEmitterReportsParticles and
// ZeroSpawnCountReportsNotEmitted: the same stock burst system, once as authored and once with
// every SpawnBurst_Instantaneous "Spawn Count" constant at 0. A verb that published a constant,
// or read the graph instead of the simulation, cannot pass both. Every test needs a renderer:
// Niagara does not instance a system under -NullRHI, and the verb refuses there.

#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

// The resolved data-interface set the mismatch fixture edits; absent on engines that have none.
#if __has_include("NiagaraScriptRuntimeCompiledData.h")
#include "NiagaraScriptRuntimeCompiledData.h"
#define PINWRIGHT_SIMULATE_TEST_HAS_RESOLVED_DI 1
#else
#define PINWRIGHT_SIMULATE_TEST_HAS_RESOLVED_DI 0
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightNiagaraSimulateTest
{
    const TCHAR* const BurstSystemPath = TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion");
    // A stock template with a GPUComputeSim emitter; the premise is re-checked at runtime.
    const TCHAR* const GpuSystemPath = TEXT("/Niagara/DefaultAssets/Templates/Systems/AttributeReaderTrails.AttributeReaderTrails");

    TSharedPtr<FJsonObject> MakePayload(const FString& AssetPath, double Seconds)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetNumberField(TEXT("seconds"), Seconds);
        return Payload;
    }

    int32 EditorLevelActorCount()
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        return World && World->PersistentLevel ? World->PersistentLevel->Actors.Num() : INDEX_NONE;
    }

    const TArray<TSharedPtr<FJsonValue>>* Emitters(const FTestResponseCapture& Capture)
    {
        const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
        return Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("emitters"), Out) ? Out : nullptr;
    }

    // Zero every "SpawnBurst_Instantaneous.Spawn Count" rapid-iteration constant the system owns,
    // in the system scripts and the emitter scripts alike, so no compile can restore one from the
    // other. Returns how many were zeroed.
    int32 ZeroBurstSpawnCounts(UNiagaraSystem& System)
    {
        int32 Zeroed = 0;
        // IsIn walks the whole outer chain, so emitter-owned scripts are included. (The
        // GetObjectsWithOuter bool overload is deprecated on 5.8 and its replacement is not on 5.3.)
        for (TObjectIterator<UNiagaraScript> It; It; ++It)
        {
            UNiagaraScript* Script = *It;
            if (!Script->IsIn(&System))
            {
                continue;
            }
            TArray<FNiagaraVariable> Variables;
            Script->RapidIterationParameters.GetParameters(Variables);
            for (const FNiagaraVariable& Variable : Variables)
            {
                if (Variable.GetType() == FNiagaraTypeDefinition::GetIntDef()
                    && Variable.GetName().ToString().EndsWith(TEXT("SpawnBurst_Instantaneous.Spawn Count")))
                {
                    const int32 Zero = 0;
                    Script->RapidIterationParameters.SetParameterData(
                        reinterpret_cast<const uint8*>(&Zero), Variable, /*bAdd=*/false);
                    ++Zeroed;
                }
            }
        }
        return Zeroed;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSimulateSpawningEmitterReportsParticlesTest,
    "PinWright.niagara.simulate.SpawningEmitterReportsParticles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSimulateSpawningEmitterReportsParticlesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraSimulateTest;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }
    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, BurstSystemPath);
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("could not load '%s'"), BurstSystemPath));
        return true;
    }

    // The stock asset is used as-is on purpose: the verb is a read, so its package must come out
    // exactly as dirty as it went in, and no actor or live instance may outlive the call.
    UPackage* Package = System->GetOutermost();
    const bool bDirtyBefore = Package->IsDirty();
    const int32 ActorsBefore = EditorLevelActorCount();
    const int32 LiveBefore = PinWrightNiagara::CountLiveSystemInstances(*System);

    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.simulate is registered"),
        InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(BurstSystemPath, 1.0), Capture));
    if (!TestTrue(FString::Printf(TEXT("simulate succeeds (code='%s', message='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return true;
    }

    bool bEmitted = false;
    TestTrue(TEXT("top-level emitted is published"), Capture.Result->TryGetBoolField(TEXT("emitted"), bEmitted));
    TestTrue(TEXT("the burst system emitted"), bEmitted);

    int32 MaxOverEmitters = 0;
    if (const TArray<TSharedPtr<FJsonValue>>* List = Emitters(Capture))
    {
        TestTrue(TEXT("one entry per emitter handle"), List->Num() == System->GetEmitterHandles().Num());
        for (const TSharedPtr<FJsonValue>& Value : *List)
        {
            double MaxCount = 0.0;
            if (Value->AsObject()->TryGetNumberField(TEXT("maxCount"), MaxCount))
            {
                MaxOverEmitters = FMath::Max(MaxOverEmitters, static_cast<int32>(MaxCount));
            }
        }
    }
    TestTrue(FString::Printf(TEXT("a live particle count above zero was read (max %d)"), MaxOverEmitters),
        MaxOverEmitters > 0);

    const TSharedPtr<FJsonObject>* SystemObj = nullptr;
    if (TestTrue(TEXT("system block present"), Capture.Result->TryGetObjectField(TEXT("system"), SystemObj)))
    {
        TestFalse(TEXT("no stallReason on a run that advanced"), (*SystemObj)->HasField(TEXT("stallReason")));
        TestTrue(TEXT("the simulation age advanced"),
            (*SystemObj)->GetNumberField(TEXT("achievedAgeSeconds")) > (*SystemObj)->GetNumberField(TEXT("startAgeSeconds")));
    }

    TestEqual(TEXT("the package dirty flag is unchanged"), Package->IsDirty(), bDirtyBefore);
    TestEqual(TEXT("no actor was left in the editor level"), EditorLevelActorCount(), ActorsBefore);
    TestEqual(TEXT("no live instance of the system outlived the call"),
        PinWrightNiagara::CountLiveSystemInstances(*System), LiveBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSimulateZeroSpawnCountReportsNotEmittedTest,
    "PinWright.niagara.simulate.ZeroSpawnCountReportsNotEmitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSimulateZeroSpawnCountReportsNotEmittedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraSimulateTest;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }

    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> Owner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_SimulateZeroSpawn"), SystemPath));
    UNiagaraSystem* System = Owner.Get();
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    // The constants only reach the simulation when the system runs with rapid-iteration
    // parameters live; settle any compile first so none can rebuild the stores after the write.
    if (!System->ShouldUseRapidIterationParameters())
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        System->SetCompileForEdit(true);
#else
        // 5.3 has no SetCompileForEdit: the flag is a public member, and the setter's
        // bNeedsRequestCompile is protected, so request the recompile directly.
        System->bCompileForEdit = true;
        System->RequestCompile(/*bForce=*/false);
#endif
    }
    PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    if (!System->ShouldUseRapidIterationParameters())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-rapid-iteration-baked"),
            TEXT("the fixture bakes out rapid-iteration parameters, so a zeroed Spawn Count cannot reach the simulation"));
        return true;
    }
    const int32 Zeroed = ZeroBurstSpawnCounts(*System);
    if (Zeroed == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-burst-constant-absent"),
            TEXT("the fixture carries no SpawnBurst_Instantaneous.Spawn Count constant to zero"));
        return true;
    }

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(SystemPath, 1.0), Capture);
    if (!TestTrue(FString::Printf(TEXT("simulate succeeds (code='%s', message='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return true;
    }

    // Premise for the verdict: the run advanced, so `false` is a measurement and not a stall.
    const TSharedPtr<FJsonObject>* SystemObj = nullptr;
    if (TestTrue(TEXT("system block present"), Capture.Result->TryGetObjectField(TEXT("system"), SystemObj)))
    {
        TestFalse(TEXT("no stallReason"), (*SystemObj)->HasField(TEXT("stallReason")));
        TestTrue(TEXT("the simulation age advanced"),
            (*SystemObj)->GetNumberField(TEXT("achievedAgeSeconds")) > (*SystemObj)->GetNumberField(TEXT("startAgeSeconds")));
    }

    bool bEmitted = true;
    TestTrue(TEXT("top-level emitted is published"), Capture.Result->TryGetBoolField(TEXT("emitted"), bEmitted));
    TestFalse(FString::Printf(TEXT("a system whose %d burst counts are 0 did not emit"), Zeroed), bEmitted);
    if (const TArray<TSharedPtr<FJsonValue>>* List = Emitters(Capture))
    {
        for (const TSharedPtr<FJsonValue>& Value : *List)
        {
            const TSharedPtr<FJsonObject> Emitter = Value->AsObject();
            double MaxCount = -1.0;
            TestTrue(TEXT("each emitter was measured"), Emitter->TryGetNumberField(TEXT("maxCount"), MaxCount));
            TestEqual(TEXT("each emitter's maxCount is 0"), static_cast<int32>(MaxCount), 0);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSimulateGpuCountIsReadOrExplainedTest,
    "PinWright.niagara.simulate.GpuEmitterCountIsReadOrExplained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSimulateGpuCountIsReadOrExplainedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraSimulateTest;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }
    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, GpuSystemPath);
    if (!System || !PinWrightNiagara::HasGpuComputeSimulation(*System))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-gpu-fixture-absent"),
            FString::Printf(TEXT("'%s' is not loadable or has no GPU compute emitter"), GpuSystemPath));
        return true;
    }

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(GpuSystemPath, 1.0), Capture);
    if (!TestTrue(FString::Printf(TEXT("simulate succeeds (code='%s', message='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return true;
    }

    int32 GpuEmitters = 0;
    int32 GpuMeasured = 0;
    if (const TArray<TSharedPtr<FJsonValue>>* List = Emitters(Capture))
    {
        for (const TSharedPtr<FJsonValue>& Value : *List)
        {
            const TSharedPtr<FJsonObject> Emitter = Value->AsObject();
            if (Emitter->GetStringField(TEXT("simTarget")) != TEXT("gpu"))
            {
                continue;
            }
            ++GpuEmitters;
            const bool bHasVerdict = Emitter->HasField(TEXT("emitted"));
            const bool bHasReason = Emitter->HasField(TEXT("notMeasuredReason"));
            // The property under test: a GPU count is either read or explained, never both, never
            // neither - and an unread count never surfaces as `emitted: false`.
            TestTrue(FString::Printf(TEXT("GPU emitter '%s' carries exactly one of emitted / notMeasuredReason"),
                *Emitter->GetStringField(TEXT("name"))), bHasVerdict != bHasReason);
            if (bHasVerdict)
            {
                ++GpuMeasured;
                // This template's GPU emitter spawns, so a measured verdict must be a positive one.
                TestTrue(TEXT("a measured GPU emitter reports emitted:true"), Emitter->GetBoolField(TEXT("emitted")));
                TestTrue(TEXT("a measured GPU emitter has maxCount > 0"), Emitter->GetNumberField(TEXT("maxCount")) > 0.0);
            }
        }
    }
    TestTrue(TEXT("the response lists the GPU emitter"), GpuEmitters > 0);
    if (GpuEmitters > 0 && GpuMeasured == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("gpu-count-not-readable"),
            TEXT("no GPU emitter count was readable on this host; only the honest-reason branch was asserted"));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSimulateRefusesUnboundedRunsTest,
    "PinWright.niagara.simulate.RefusesUnboundedRuns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSimulateRefusesUnboundedRunsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraSimulateTest;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(BurstSystemPath, 61.0), Capture);
    TestEqual(TEXT("seconds above 60 is refused"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    TSharedPtr<FJsonObject> TooManySteps = MakePayload(BurstSystemPath, 60.0);
    TooManySteps->SetNumberField(TEXT("deltaTime"), 0.001);
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), TooManySteps, Capture);
    TestEqual(TEXT("60000 steps is refused"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    return true;
}

// ---- Acceptance gaps: stall, dirty-stays-dirty, data-interface refusal, GPU vs CPU twin ----------

namespace PinWrightNiagaraSimulateAcceptanceTest
{
    TSharedPtr<FJsonObject> FindEmitter(const FTestResponseCapture& Capture, const FString& Name)
    {
        const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetArrayField(TEXT("emitters"), List))
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *List)
        {
            const TSharedPtr<FJsonObject> Emitter = Value->AsObject();
            if (Emitter.IsValid() && Emitter->GetStringField(TEXT("name")) == Name)
            {
                return Emitter;
            }
        }
        return nullptr;
    }

    // Reads one rapid-iteration constant by exact name from any script the system owns. The system
    // and emitter stores carry the same value after a compile, so the first match is the value.
    bool ReadConstant(UNiagaraSystem& System, const FString& Name, const FNiagaraTypeDefinition& Type, void* Out, int32 Size)
    {
        for (TObjectIterator<UNiagaraScript> It; It; ++It)
        {
            if (!It->IsIn(&System))
            {
                continue;
            }
            const FNiagaraVariable Variable(Type, FName(*Name));
            if (const uint8* Data = It->RapidIterationParameters.GetParameterData(Variable))
            {
                FMemory::Memcpy(Out, Data, Size);
                return true;
            }
        }
        return false;
    }
}

// A run whose ticks never move the age must say why and carry no verdict anywhere (§17). The
// fixture is the engine's own debug throttle: with fx.Niagara.SystemSimulation.SkipTickDeltaSeconds
// at or above the step, FNiagaraSystemSimulation::Tick_GameThread_Internal returns before simulating
// anything, so the instance is live, unpaused and not complete, and its age stays at 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSimulateStalledRunHasNoVerdictTest,
    "PinWright.niagara.simulate.StalledRunReportsReasonAndNoVerdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSimulateStalledRunHasNoVerdictTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraSimulateTest;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }
    IConsoleVariable* SkipTick = IConsoleManager::Get().FindConsoleVariable(
        TEXT("fx.Niagara.SystemSimulation.SkipTickDeltaSeconds"));
    if (!SkipTick || !LoadObject<UNiagaraSystem>(nullptr, BurstSystemPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-stall-fixture-unavailable"),
            TEXT("the skip-tick cvar or the stock burst system is unavailable"));
        return true;
    }
    const float Previous = SkipTick->GetFloat();
    SkipTick->Set(1.0f, ECVF_SetByCode);
    ON_SCOPE_EXIT { SkipTick->Set(Previous, ECVF_SetByCode); };

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(BurstSystemPath, 0.5), Capture);
    if (!TestTrue(FString::Printf(TEXT("a stalled run still answers (code='%s', message='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* SystemObj = nullptr;
    FString StallReason;
    if (TestTrue(TEXT("system block present"), Capture.Result->TryGetObjectField(TEXT("system"), SystemObj)))
    {
        TestTrue(TEXT("stallReason is published"), (*SystemObj)->TryGetStringField(TEXT("stallReason"), StallReason));
        TestTrue(TEXT("stallReason names the engine path that swallowed the ticks"),
            StallReason.Contains(TEXT("SkipTickDeltaSeconds")));
    }
    TestFalse(TEXT("no top-level emitted verdict on a run that did not advance"), Capture.Result->HasField(TEXT("emitted")));
    if (const TArray<TSharedPtr<FJsonValue>>* List = Emitters(Capture))
    {
        TestTrue(TEXT("emitters are still listed"), List->Num() > 0);
        for (const TSharedPtr<FJsonValue>& Value : *List)
        {
            const TSharedPtr<FJsonObject> Emitter = Value->AsObject();
            TestFalse(TEXT("no per-emitter emitted verdict"), Emitter->HasField(TEXT("emitted")));
            TestFalse(TEXT("no per-emitter maxCount"), Emitter->HasField(TEXT("maxCount")));
            TestTrue(TEXT("each emitter says why it was not measured"), Emitter->HasField(TEXT("notMeasuredReason")));
        }
    }
    return true;
}

// The other half of §11's preserve-not-clear contract: SpawningEmitterReportsParticles proves a
// clean package stays clean, which a verb that unconditionally CLEARS the flag also passes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSimulateDirtyPackageStaysDirtyTest,
    "PinWright.niagara.simulate.DirtyPackageStaysDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSimulateDirtyPackageStaysDirtyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraSimulateTest;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }
    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> Owner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_SimulateDirty"), SystemPath));
    if (!Owner.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }
    UPackage* Package = Owner->GetOutermost();
    Package->SetDirtyFlag(true);
    ON_SCOPE_EXIT { Package->SetDirtyFlag(false); };
    if (!TestTrue(TEXT("premise: the fixture package starts dirty"), Package->IsDirty()))
    {
        return true;
    }

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(SystemPath, 0.5), Capture);
    TestTrue(FString::Printf(TEXT("simulate succeeds (code='%s', message='%s')"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    TestTrue(TEXT("a package that was dirty before the read is still dirty after it"), Package->IsDirty());
    return true;
}

// A real mismatched system, not a hook. The fixture is a compiled duplicate whose resolved
// data-interface set for one script gets one extra (default) entry - the count-level shape of the
// orphan the engine leaves when compiled results are adopted from elsewhere. That is exactly what
// asserts inside the VectorVM on the first tick, so the ONLY thing standing between this test and a
// dead suite host is the verb's refusal: if the gate regresses, the run crashes rather than going
// red. That is deliberate and loud. The extra entry is popped again before teardown, and the
// fixture is held by no component, so nothing else can tick it in between.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSimulateRefusesDataInterfaceMismatchTest,
    "PinWright.niagara.simulate.RefusesDataInterfaceMismatchBeforeAnyTick",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSimulateRefusesDataInterfaceMismatchTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraSimulateTest;
    using namespace PinWrightNiagara;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }
#if PINWRIGHT_SIMULATE_TEST_HAS_RESOLVED_DI
    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> Owner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_SimulateDiMismatch"), SystemPath));
    UNiagaraSystem* System = Owner.Get();
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("could not duplicate '%s'"), NiagaraEditTestUtils::FixtureSystemAssetPath));
        return true;
    }

    // A compile in this session is what builds the resolved set the check compares against.
    System->RequestCompile(/*bForce=*/true);
    const FCompileWaitOutcome Wait = WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    TArray<FDataInterfaceCountMismatch> Mismatches;
    if (Wait.bTimedOut || CheckDataInterfaceCounts(*System, Mismatches) != EDataInterfaceConsistency::Consistent)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-unavailable"),
            TEXT("the compiled fixture did not reach a Consistent data-interface verdict to corrupt"));
        return true;
    }
    TestEqual(TEXT("premise: nothing is running the fixture"), CountLiveSystemInstances(*System), 0);

    // Same reflective route as NiagaraDataInterfaceConsistency.cpp: the field is private.
    const FObjectPropertyBase* RefProperty = CastField<FObjectPropertyBase>(
        System->GetClass()->FindPropertyByName(TEXT("ScriptRuntimeCompiledDataForEditor")));
    UNiagaraScriptRuntimeCompiledDataEditorReference* Ref = RefProperty
        ? Cast<UNiagaraScriptRuntimeCompiledDataEditorReference>(RefProperty->GetObjectPropertyValue_InContainer(System))
        : nullptr;
    if (!Ref)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-unavailable"),
            TEXT("UNiagaraSystem::ScriptRuntimeCompiledDataForEditor is not reachable on this engine"));
        return true;
    }

    // Arm one script the check actually visits: add, re-measure, keep the first that flips it.
    TOptional<FNiagaraScriptDataKey> ArmedKey;
    int32 ArmedCount = 0;
    for (TPair<FNiagaraScriptDataKey, FNiagaraScriptRuntimeCompiledData>& Pair : Ref->ScriptRuntimeCompiledDataMap)
    {
        Pair.Value.ResolvedDataInterfaces.AddDefaulted();
        if (CheckDataInterfaceCounts(*System, Mismatches) == EDataInterfaceConsistency::Mismatched)
        {
            ArmedKey = Pair.Key;
            ArmedCount = Pair.Value.ResolvedDataInterfaces.Num();
            break;
        }
        Pair.Value.ResolvedDataInterfaces.Pop();
    }
    ON_SCOPE_EXIT
    {
        if (ArmedKey.IsSet())
        {
            FNiagaraScriptRuntimeCompiledData* Data = Ref->ScriptRuntimeCompiledDataMap.Find(ArmedKey.GetValue());
            if (Data && Data->ResolvedDataInterfaces.Num() == ArmedCount)
            {
                Data->ResolvedDataInterfaces.Pop();
            }
        }
    };
    if (!TestTrue(TEXT("premise: the fixture now measures Mismatched"), ArmedKey.IsSet()))
    {
        return true;
    }

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(SystemPath, 0.5), Capture);
    TestFalse(TEXT("a mismatched system is not simulated"), Capture.bSuccess);
    TestEqual(TEXT("refused with NIAGARA_DATA_INTERFACE_MISMATCH"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_NIAGARA_DATA_INTERFACE_MISMATCH));
    // Before any tick: no instance was ever created, so nothing reached the VectorVM.
    TestEqual(TEXT("no system instance was created"), CountLiveSystemInstances(*System), 0);
    TestTrue(TEXT("the refusal left the corruption in place (nothing recompiled it away)"),
        CheckDataInterfaceCounts(*System, Mismatches) == EDataInterfaceConsistency::Mismatched);
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-resolved-di-unavailable"),
        TEXT("this engine has no NiagaraScriptRuntimeCompiledData.h, so no resolved set exists to corrupt"));
#endif
    return true;
}

// GPU counts against a CPU twin. One fixture, two runs: the stock SimpleSpriteBurst emitter is
// simulated as authored (CPU), then flipped to GPUComputeSim the way the editor does it
// (SimTarget + PostEditChangeVersionedProperty, which requests the recompile) and simulated again
// with the same seconds and deltaTime.
//
// THE TOLERANCE. The emitter spawns from one SpawnBurst_Instantaneous at t=0, so its population
// peak does not depend on the random seed: when the burst's Spawn Probability is 1, exactly
// "Spawn Count" particles are born in the first step and the peak is that number on both targets.
// The GPU count can only over-read (deaths reach the CPU through an asynchronous readback, a frame
// late) and can never exceed what was spawned, so the peak is exact. The test therefore asserts
// CPU maxCount == Spawn Count (the premise: the reasoning holds on this fixture) and GPU maxCount ==
// CPU maxCount. If the authored probability is below 1 the peak is a random draw per target, and
// the only honest bound left is 1 <= GPU maxCount <= Spawn Count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSimulateGpuMatchesCpuTwinTest,
    "PinWright.niagara.simulate.GpuCountMatchesCpuTwin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSimulateGpuMatchesCpuTwinTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraSimulateTest;
    using namespace PinWrightNiagaraSimulateAcceptanceTest;
    using namespace PinWrightNiagara;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }
    const FString EmitterName = TEXT("SimpleSpriteBurst");
    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> Owner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_SimulateGpuTwin"), SystemPath));
    UNiagaraSystem* System = Owner.Get();
    FNiagaraEmitterHandle* Handle = nullptr;
    if (System)
    {
        for (FNiagaraEmitterHandle& Candidate : System->GetEmitterHandles())
        {
            if (Candidate.GetName().ToString() == EmitterName)
            {
                Handle = &Candidate;
            }
        }
    }
    int32 SpawnCount = 0;
    if (!Handle || !Handle->GetEmitterData() || !Handle->GetInstance().Emitter
        || !ReadConstant(*System, FString::Printf(TEXT("Constants.%s.SpawnBurst_Instantaneous.Spawn Count"), *EmitterName),
               FNiagaraTypeDefinition::GetIntDef(), &SpawnCount, sizeof(SpawnCount)))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("the fixture has no '%s' burst emitter with a Spawn Count constant"), *EmitterName));
        return true;
    }
    float SpawnProbability = 1.0f;
    ReadConstant(*System, FString::Printf(TEXT("Constants.%s.SpawnBurst_Instantaneous.Spawn Probability"), *EmitterName),
        FNiagaraTypeDefinition::GetFloatDef(), &SpawnProbability, sizeof(SpawnProbability));
    const bool bPeakIsExact = SpawnProbability >= 1.0f;

    WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    FTestResponseCapture Cpu;
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(SystemPath, 0.5), Cpu);
    const TSharedPtr<FJsonObject> CpuEmitter = FindEmitter(Cpu, EmitterName);
    if (!TestTrue(FString::Printf(TEXT("CPU run succeeds (code='%s', message='%s')"), *Cpu.ErrorCode, *Cpu.Message),
            Cpu.bSuccess && CpuEmitter.IsValid() && CpuEmitter->HasField(TEXT("maxCount"))))
    {
        return true;
    }
    TestEqual(TEXT("CPU twin is a CPU emitter"), CpuEmitter->GetStringField(TEXT("simTarget")), FString(TEXT("cpu")));
    const int32 CpuMax = static_cast<int32>(CpuEmitter->GetNumberField(TEXT("maxCount")));

    // Flip to GPU exactly as the details panel does, then compile: VM scripts through the bounded
    // wait, GPU shaders through the engine's own completion (FNiagaraShaderScript::FinishCompilation
    // is not exported, and nothing else finishes a Niagara shader map from a test stack).
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    EmitterData->SimTarget = ENiagaraSimTarget::GPUComputeSim;
    FPropertyChangedEvent SimTargetChanged(FindFProperty<FProperty>(
        FVersionedNiagaraEmitterData::StaticStruct(), GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, SimTarget)));
    Handle->GetInstance().Emitter->PostEditChangeVersionedProperty(SimTargetChanged, Handle->GetInstance().Version);
    System->RequestCompile(/*bForce=*/false);
    const FCompileWaitOutcome Wait = WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
    if (!TestFalse(TEXT("the GPU recompile landed within the wait budget"), Wait.bTimedOut))
    {
        return true;
    }

    FTestResponseCapture Gpu;
    InvokeHandlerWithCapture(TEXT("niagara.simulate"), MakePayload(SystemPath, 0.5), Gpu);
    const TSharedPtr<FJsonObject> GpuEmitter = FindEmitter(Gpu, EmitterName);
    if (!TestTrue(FString::Printf(TEXT("GPU run succeeds (code='%s', message='%s')"), *Gpu.ErrorCode, *Gpu.Message),
            Gpu.bSuccess && GpuEmitter.IsValid()))
    {
        return true;
    }
    TestEqual(TEXT("the flipped emitter runs on the GPU"), GpuEmitter->GetStringField(TEXT("simTarget")), FString(TEXT("gpu")));
    if (!GpuEmitter->HasField(TEXT("maxCount")))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("gpu-count-not-readable"),
            FString::Printf(TEXT("the GPU twin's count was not readable on this host: %s"),
                *GpuEmitter->GetStringField(TEXT("notMeasuredReason"))));
        return true;
    }
    TestTrue(TEXT("the GPU count is exact after the flush"), GpuEmitter->GetBoolField(TEXT("countExact")));
    const int32 GpuMax = static_cast<int32>(GpuEmitter->GetNumberField(TEXT("maxCount")));
    if (bPeakIsExact)
    {
        TestEqual(TEXT("premise: the CPU peak is the authored burst size"), CpuMax, SpawnCount);
        TestEqual(TEXT("the GPU peak equals the CPU twin's peak"), GpuMax, CpuMax);
    }
    else
    {
        TestTrue(FString::Printf(TEXT("the GPU peak %d is within [1, %d]"), GpuMax, SpawnCount),
            GpuMax >= 1 && GpuMax <= SpawnCount);
    }
    return true;
}

#endif
