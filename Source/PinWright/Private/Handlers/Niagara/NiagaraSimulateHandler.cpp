// Copyright (c) 2026 Alexander Penkin. MIT License.

// NiagaraSimulateHandler.cpp
// Handler: niagara.simulate
//
// The one verb that proves a Niagara system EMITS. niagara.validate is static: a system can compile
// clean, validate `valid: true` and still spawn nothing (SpawnRate 0, a burst count of 0, a spawn
// module in the wrong stage). This verb runs the system in a private preview world for a bounded
// number of fixed steps and reads each emitter's live particle count after every step, so
// `emitted` is a measurement of the simulation, never a reading of the graph.

#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Niagara/NiagaraCompileVerdict.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Render/CaptureSubjectProviders_Niagara.h"
#include "Utils/PackageDirtyUtils.h"
#include "Utils/RenderingAvailability.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
// Compat header, not the raw engine one: UE 5.3-5.5 do not define UE_VERSION_NEWER_THAN_OR_EQUAL.
#include "Compat/EngineVersionCompat.h"
#include "Misc/ScopeExit.h"
#include "NiagaraComponent.h"
#include "NiagaraComputeExecutionContext.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "PreviewScene.h"
#include "RenderingThread.h"
#include "UObject/Package.h"

// Named, not anonymous: Unity merges this TU with its Niagara siblings.
namespace PinWrightNiagaraSimulate
{
    inline constexpr double MaxSeconds = 60.0;
    inline constexpr double MinDeltaTime = 0.0001;
    // The bound on work per call. Every step reads every emitter, and a GPU system pays a render
    // flush per step, so the cap is on steps rather than on seconds: 60 s at 1/60 s.
    inline constexpr int32 MaxSteps = 3600;
    // Published samples per emitter when the caller names no sampleEvery. maxCount is read over
    // EVERY step regardless; this only bounds the response.
    inline constexpr int32 DefaultPublishedSamples = 30;

    const TCHAR* ExecutionStateToString(ENiagaraExecutionState State)
    {
        switch (State)
        {
        case ENiagaraExecutionState::Active:        return TEXT("active");
        case ENiagaraExecutionState::Inactive:      return TEXT("inactive");
        case ENiagaraExecutionState::InactiveClear: return TEXT("inactiveClear");
        case ENiagaraExecutionState::Complete:      return TEXT("complete");
        case ENiagaraExecutionState::Disabled:      return TEXT("disabled");
        default:                                    return TEXT("unknown");
        }
    }

    // FPreviewScene's teardown schedules a process-wide full-purge GC whenever
    // r.ForceGCOnPreviewSceneExit is set (PreviewScene.cpp, Uninitialize). Suppressed for the
    // destruction only, the same choice render.capture_mesh makes. Plain save/restore at
    // ECVF_SetByCode works on every engine in range (TGuardConsoleVariable only arrived in 5.6).
    struct FScopedPreviewSceneGcSuppression
    {
        IConsoleVariable* Variable =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.ForceGCOnPreviewSceneExit"));
        int32 Previous = Variable ? Variable->GetInt() : 0;

        FScopedPreviewSceneGcSuppression()
        {
            if (Variable)
            {
                Variable->Set(0, ECVF_SetByCode);
            }
        }
        ~FScopedPreviewSceneGcSuppression()
        {
            if (Variable)
            {
                Variable->Set(Previous, ECVF_SetByCode);
            }
        }
    };

    struct FEmitterTrack
    {
        FString Name;
        bool bGpu = false;
        TArray<TSharedPtr<FJsonValue>> Samples;
        int32 MaxExactCount = 0;
        int32 ExactReadings = 0;
        int32 Readings = 0;
        // Readings taken while the emitter was not Disabled. All-disabled means it never ran.
        int32 NonDisabledReadings = 0;
    };
}

REGISTER_RPC_HANDLER("niagara.simulate", "niagara",
    "Run a Niagara system in a private preview world for a bounded number of fixed steps and report "
    "each emitter's live particle count, so `emitted` is measured from the simulation rather than "
    "inferred from the graph. Read verb: no actor is placed and no package dirty flag changes.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Niagara System asset to simulate"),
        RPC_PARAM_REQ("seconds", "number",
            "Simulated duration in seconds, greater than 0 and at most 60. No default: a burst "
            "that finishes early and an effect that starts late need different windows."),
        RPC_PARAM_DEF("deltaTime", "number",
            "Fixed simulation step in seconds, at least 0.0001. seconds/deltaTime may not exceed 3600 steps.",
            "0.0333333"),
        RPC_PARAM_OPT("sampleEvery", "integer",
            "Publish one sample every N steps (the last step is always published). Counts are read "
            "after every step regardless, so maxCount never misses a short burst. Default: about 30 "
            "published samples.")
    ))
{
    using namespace PinWrightNiagaraSimulate;
    namespace NS = PinWrightCaptureSubjectNiagara;

    // Niagara does not instance a system at all without a renderer (UNiagaraSystem::
    // IsReadyToRunInternal returns false when !FApp::CanEverRender()), so under -NullRHI every
    // count would be a structural zero. Refuse instead of reporting one.
    if (!PinWrightRendering::RequireRenderer(Ctx)) return true;

    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;
    double Seconds = 0.0;
    if (!Ctx.RequireNumber(TEXT("seconds"), Seconds)) return true;
    const double DeltaTime = Ctx.GetNumber(TEXT("deltaTime"), NS::DefaultTickDeltaSeconds);

    if (!FMath::IsFinite(Seconds) || Seconds <= 0.0 || Seconds > MaxSeconds)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("seconds must be greater than 0 and at most %g (got %g)."), MaxSeconds, Seconds));
        return true;
    }
    if (!FMath::IsFinite(DeltaTime) || DeltaTime < MinDeltaTime)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("deltaTime must be at least %g seconds (got %g)."), MinDeltaTime, DeltaTime));
        return true;
    }
    const float Dt = static_cast<float>(DeltaTime);
    const int32 Steps = NS::TickCountForTime(Seconds, Dt);
    if (Steps < 1 || Steps > MaxSteps)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("seconds/deltaTime gives %d steps; it must be between 1 and %d. ")
                TEXT("Raise deltaTime or shorten seconds."), Steps, MaxSteps));
        return true;
    }
    int32 SampleEvery = FMath::Max(1, FMath::DivideAndRoundUp(Steps, DefaultPublishedSamples));
    if (Ctx.GetRawPayload().IsValid() && Ctx.GetRawPayload()->HasField(TEXT("sampleEvery")))
    {
        if (!Ctx.RequireInt(TEXT("sampleEvery"), SampleEvery)) return true;
        if (SampleEvery < 1)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("sampleEvery must be at least 1 (got %d)."), SampleEvery));
            return true;
        }
    }

    UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Loaded)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Could not load Niagara system '%s'."), *AssetPath));
        return true;
    }
    UNiagaraSystem* System = Cast<UNiagaraSystem>(Loaded);
    if (!System)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE,
            FString::Printf(TEXT("'%s' is a %s; niagara.simulate requires a NiagaraSystem."),
                *AssetPath, *Loaded->GetClass()->GetName()));
        return true;
    }

    // Read verb: preserve every dirty flag the compile wait or the instance could touch (§11).
    PinWright::PackageDirty::FScopedPackageDirtyRestore DirtyRestore;
    DirtyRestore.Capture(System);
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        DirtyRestore.Capture(Handle.GetInstance().Emitter);
    }

    // Gate 1: the same compile-readiness decision effect.spawn_niagara makes. Instancing is the
    // demand on-demand compilation defers to, so a merely queued request may be drained here.
    const PinWrightNiagara::FCompileWaitOutcome CompileWait =
        PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    const PinWrightNiagara::FCompileVerdict CompileVerdict =
        PinWrightNiagara::ReadCompileVerdict(NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System));
    if (!PinWrightNiagara::IsSystemReadyAfterCompileWait(*System, CompileWait,
            CompileVerdict.Check == PinWrightNiagara::EScriptCompileCheck::Failed))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("scriptCompileCheck"),
            PinWrightNiagara::ScriptCompileCheckToString(CompileVerdict.Check));
        Data->SetBoolField(TEXT("compileTimedOut"), CompileWait.bTimedOut);
        Data->SetBoolField(TEXT("outstandingCompilationRequests"), PinWrightNiagara::HasPendingCompileWork(*System));
        FString Message = FString::Printf(
            TEXT("Niagara system '%s' is not compiled and ready to run, so nothing was simulated. ")
            TEXT("Check niagara.compile_status, or run niagara.compile."), *AssetPath);
        if (CompileVerdict.FailedScripts.Num() > 0)
        {
            Message += TEXT(" ") + PinWrightNiagara::DescribeScriptCompileFailure(CompileVerdict.FailedScripts[0]);
        }
        Ctx.SendError(ErrorCodes::ERR_SYSTEM_NOT_COMPILED, Message, Data);
        return true;
    }

    // Gate 2: a compiled/resolved data-interface mismatch asserts inside the VectorVM on the first
    // tick and takes the editor down, so it is refused before anything ticks.
    TArray<PinWrightNiagara::FDataInterfaceCountMismatch> Mismatches;
    const PinWrightNiagara::EDataInterfaceConsistency DiCheck =
        PinWrightNiagara::CheckDataInterfaceCounts(*System, Mismatches);
    if (DiCheck == PinWrightNiagara::EDataInterfaceConsistency::Mismatched)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("dataInterfaceCheck"), PinWrightNiagara::DataInterfaceConsistencyToString(DiCheck));
        Ctx.SendError(ErrorCodes::ERR_NIAGARA_DATA_INTERFACE_MISMATCH,
            FString::Printf(TEXT("Niagara system '%s' was not simulated: %s. Ticking it would assert inside ")
                TEXT("the VectorVM. See niagara.list_orphan_data_interfaces, or recompile."),
                *AssetPath, *PinWrightNiagara::DescribeDataInterfaceMismatches(Mismatches)),
            Data);
        return true;
    }

    // A private world, never the editor's: nothing is placed in the open level.
    FPreviewScene::ConstructionValues SceneValues;
    SceneValues.SetCreatePhysicsScene(false).ShouldSimulatePhysics(false).SetTransactional(false)
        .SetCreateDefaultLighting(false);
    TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(SceneValues);
    UWorld* World = Scene->GetWorld();
    UNiagaraComponent* Component = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    Component->SetAutoActivate(false);
    // EffectType culling would otherwise deactivate a component nobody is looking at.
    Component->SetAllowScalability(false);
    Component->SetAsset(System);
    Scene->AddComponent(Component, FTransform::Identity);
    ON_SCOPE_EXIT
    {
        Component->DeactivateImmediate();
        Scene->RemoveComponent(Component);
        Component->DestroyComponent();
        FScopedPreviewSceneGcSuppression SuppressGc;
        Scene.Reset();
    };

    const NS::FDeterminismReport Determinism = NS::ReadDeterminism(*System, Component);
    const bool bInstanceLive = NS::EnsureSystemInstance(*Component);
    // Read before any step: an instance that is already Complete here was completed by its own
    // initialisation, not by the run - FNiagaraSystemInstance::InitDataInterfaces calls
    // Complete(true) when a data interface or its function table fails to bind.
    FNiagaraSystemInstanceControllerConstPtr InitController = Component->GetSystemInstanceController();
    const bool bCompletedAtInit = bInstanceLive && InitController.IsValid() && InitController->IsValid()
        && InitController->IsComplete();
    double StartAge = 0.0;
    const bool bStartAgeMeasured = NS::TryReadSimulatedAge(*Component, StartAge);

    // GPU counts are latent: without a flush GetNumParticles returns TotalSpawnedParticles, a
    // cumulative guess that ignores deaths. Same sequence as NiagaraSimCache's GPU capture.
    const bool bHasGpuSimulation = PinWrightNiagara::HasGpuComputeSimulation(*System);
    FNiagaraGpuComputeDispatchInterface* GpuDispatch =
        bHasGpuSimulation ? FNiagaraGpuComputeDispatchInterface::Get(World) : nullptr;
    // The compile gate above excludes GPU shaders. A GPU emitter whose shader map is still
    // compiling is skipped by the dispatcher (IsShaderMapComplete_RenderThread), yet its count
    // fence still passes, so it would read an exact-looking 0. With the CPU queue drained,
    // anything still outstanding here is GPU shader work.
    const bool bGpuShadersPending = bHasGpuSimulation && System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/true);

    const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
    TArray<FEmitterTrack> Tracks;
    for (const FNiagaraEmitterHandle& Handle : Handles)
    {
        FEmitterTrack& Track = Tracks.AddDefaulted_GetRef();
        Track.Name = Handle.GetName().ToString();
        const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
        Track.bGpu = EmitterData && EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim;
    }

    int32 StepsRun = 0;
    for (int32 Step = 1; bInstanceLive && Step <= Steps; ++Step)
    {
        Component->AdvanceSimulation(1, Dt);
        StepsRun = Step;
        if (GpuDispatch)
        {
            World->SendAllEndOfFrameUpdates();
            GpuDispatch->FlushPendingTicks_GameThread();
            FlushRenderingCommands();
        }

        FNiagaraSystemInstanceControllerConstPtr Controller = Component->GetSystemInstanceController();
        FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
        double Age = 0.0;
        if (!Instance || !NS::TryReadSimulatedAge(*Component, Age))
        {
            break;
        }
        const bool bPublish = Step % SampleEvery == 0 || Step == Steps;
        int32 EmitterIndex = 0;
        for (const auto& EmitterRef : Instance->GetEmitters())
        {
            if (!Tracks.IsValidIndex(EmitterIndex))
            {
                break;
            }
            FEmitterTrack& Track = Tracks[EmitterIndex++];
            const FNiagaraEmitterInstance& Emitter = EmitterRef.Get();
            const int32 Count = Emitter.GetNumParticles();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            const ENiagaraExecutionState State = Emitter.GetExecutionState();
#else
            // GetExecutionState is non-const through 5.3; it only reads the member.
            const ENiagaraExecutionState State = const_cast<FNiagaraEmitterInstance&>(Emitter).GetExecutionState();
#endif
            // FNiagaraEmitterInstance::GetNumParticles reads the GPU context only once this fence
            // has passed, and returns TotalSpawnedParticles otherwise.
            bool bExact = true;
            if (FNiagaraComputeExecutionContext* GpuContext = Emitter.GetGPUContext())
            {
                bExact = static_cast<uint32>(GpuContext->ParticleCountReadFence)
                    <= static_cast<uint32>(GpuContext->ParticleCountWriteFence);
            }
            ++Track.Readings;
            if (State != ENiagaraExecutionState::Disabled)
            {
                ++Track.NonDisabledReadings;
            }
            if (bExact)
            {
                ++Track.ExactReadings;
                Track.MaxExactCount = FMath::Max(Track.MaxExactCount, Count);
            }
            if (bPublish)
            {
                TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
                Sample->SetNumberField(TEXT("t"), Age);
                Sample->SetNumberField(TEXT("count"), Count);
                Sample->SetStringField(TEXT("state"), ExecutionStateToString(State));
                if (!bExact)
                {
                    Sample->SetBoolField(TEXT("countExact"), false);
                }
                Track.Samples.Add(MakeShared<FJsonValueObject>(Sample));
            }
        }
    }

    double EndAge = 0.0;
    const bool bEndAgeMeasured = NS::TryReadSimulatedAge(*Component, EndAge);
    // §17: the verdict is only allowed when the simulation measurably moved. A run that advanced
    // nothing reports why, and never reports `emitted: false`.
    const bool bAdvanced = bInstanceLive && bStartAgeMeasured && bEndAgeMeasured && EndAge > StartAge;

    TSharedPtr<FJsonObject> SystemObj = MakeShared<FJsonObject>();
    SystemObj->SetNumberField(TEXT("stepsRequested"), Steps);
    SystemObj->SetNumberField(TEXT("stepsRun"), StepsRun);
    SystemObj->SetNumberField(TEXT("deltaTime"), static_cast<double>(Dt));
    SystemObj->SetNumberField(TEXT("sampleEvery"), SampleEvery);
    if (bStartAgeMeasured)
    {
        SystemObj->SetNumberField(TEXT("startAgeSeconds"), StartAge);
    }
    if (bEndAgeMeasured)
    {
        SystemObj->SetNumberField(TEXT("achievedAgeSeconds"), EndAge);
    }
    FNiagaraSystemInstanceControllerConstPtr FinalController = Component->GetSystemInstanceController();
    if (FinalController.IsValid() && FinalController->IsValid())
    {
        SystemObj->SetStringField(TEXT("executionState"),
            ExecutionStateToString(FinalController->GetActualExecutionState()));
    }
    if (!bAdvanced)
    {
        SystemObj->SetStringField(TEXT("stallReason"), bCompletedAtInit
            ? FString(TEXT("The system instance completed during initialisation, before the first step: ")
                TEXT("FNiagaraSystemInstance::InitDataInterfaces completes an instance whose data interfaces ")
                TEXT("or their function bindings fail to initialise (LogNiagara: 'Error initializing data ")
                TEXT("interfaces. Completing system.'). A system duplicated in memory and not compiled since ")
                TEXT("has been seen to land here; run niagara.compile on it and simulate again."))
            : NS::DescribeAdvanceStall(*Component, Dt));
    }
    SystemObj->SetBoolField(TEXT("gpuCountsFlushed"), GpuDispatch != nullptr);
    // Authored flags only: all three determinism scopes on, and no GPU emitter (no knob covers one).
    SystemObj->SetBoolField(TEXT("deterministic"), Determinism.bSystemDeterminism
        && Determinism.NonDeterministicEmitters.Num() == 0 && Determinism.GpuEmitters.Num() == 0);

    bool bAnyEmitted = false;
    bool bAllMeasured = true;
    TArray<TSharedPtr<FJsonValue>> EmittersJson;
    for (const FEmitterTrack& Track : Tracks)
    {
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("name"), Track.Name);
        E->SetStringField(TEXT("simTarget"), Track.bGpu ? TEXT("gpu") : TEXT("cpu"));
        E->SetArrayField(TEXT("samples"), Track.Samples);
        E->SetBoolField(TEXT("countExact"), Track.Readings > 0 && Track.ExactReadings == Track.Readings);

        FString NotMeasured;
        if (!bAdvanced)
        {
            NotMeasured = TEXT("the simulation did not advance; see system.stallReason");
        }
        else if (Track.bGpu && bGpuShadersPending)
        {
            NotMeasured = TEXT("GPU compute shaders were still compiling when the run started, and a GPU ")
                TEXT("emitter without a complete shader map dispatches nothing while its count reads 0; ")
                TEXT("retry once niagara.compile_status reports outstandingCompilationRequests false");
        }
        else if (Track.bGpu && !GpuDispatch)
        {
            NotMeasured = TEXT("GPU emitter, but the preview world has no Niagara GPU compute dispatch ")
                TEXT("interface to flush, so its count is not readable");
        }
        else if (Track.ExactReadings == 0)
        {
            NotMeasured = TEXT("the GPU particle-count fence never passed, so every reading was ")
                TEXT("TotalSpawnedParticles (a cumulative guess), not a live count");
        }
        else if (Track.NonDisabledReadings == 0)
        {
            NotMeasured = TEXT("the emitter was Disabled at every step (it never simulated: disabled ")
                TEXT("handle, failed init, or GPU simulation unavailable), so whether it emits was not measured");
        }

        if (NotMeasured.IsEmpty())
        {
            const bool bEmitted = Track.MaxExactCount > 0;
            E->SetNumberField(TEXT("maxCount"), Track.MaxExactCount);
            E->SetBoolField(TEXT("emitted"), bEmitted);
            bAnyEmitted |= bEmitted;
        }
        else
        {
            E->SetStringField(TEXT("notMeasuredReason"), NotMeasured);
            bAllMeasured = false;
        }
        EmittersJson.Add(MakeShared<FJsonValueObject>(E));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetArrayField(TEXT("emitters"), EmittersJson);
    Result->SetObjectField(TEXT("system"), SystemObj);
    Result->SetStringField(TEXT("dataInterfaceCheck"), PinWrightNiagara::DataInterfaceConsistencyToString(DiCheck));
    // Present only when it is a measurement: true if any emitter measurably emitted, false only
    // when the run advanced and every emitter was measured at zero.
    if (bAdvanced && (bAnyEmitted || bAllMeasured))
    {
        Result->SetBoolField(TEXT("emitted"), bAnyEmitted);
    }
    Ctx.SendSuccess(Result);
    return true;
}
