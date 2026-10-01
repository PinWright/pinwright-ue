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
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

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
        System->SetCompileForEdit(true);
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

#endif
