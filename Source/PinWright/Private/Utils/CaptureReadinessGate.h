// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FJsonObject;

// A FRAME-QUALITY precondition for a pixel readback, and the report that goes with it: what the
// compile queues were doing when the frame was taken.
//
// WHAT IT IS FOR. A capture issued while the materials in view are still compiling photographs the
// default material -- grey, or WorldGridMaterial -- and returns a plausible PNG with every other
// honesty field clean. `warmup` cannot catch it: the frame settles perfectly, because a stand-in
// material is stable. So the gate drains the shader queue first, reports what it drained, and
// refuses rather than shipping a frame of stand-in materials when the drain runs out of budget.
//
// WHAT IT IS NOT. It is NOT a crash fix and must not be described as one. The GPU page fault that
// prompted this work is a resource-lifetime / descriptor-residency fault at the readback, and the
// D3D12 `late shader associations` lines quoted alongside it are the Aftermath crash-dump
// DECODER's output AFTER the fault (`RHICoreNvidiaAftermath.cpp` calls `CreateShaderAssociations`
// from inside the crash-dump handler, "Allow association after a fault"), not a pre-fault state
// this gate could have avoided. The mitigation aimed at that fault is the readback flush --
// `PinWrightScreenshotUtils::FlushBeforeReadback` -- and it is plausible, not proven.
//
// WAITING IS SHADER-ONLY, ON PURPOSE. Only shader work can put a stand-in material in the frame,
// and only shader work is worth spending the caller's wall clock on. Asset compilation (textures,
// static meshes, sound waves) is a normal steady state after any map load, so waiting on it burned
// the entire budget on the game thread on every screenshot and then shot the frame anyway. Its
// counts are REPORTED, never waited on.
namespace PinWrightCaptureReadiness
{
    // Shorter than the material verb's 90 s ceiling (Handlers/Material/MaterialCompileErrorCollector.h)
    // on purpose: this wait sits inside a capture RPC that a caller expects to answer in seconds,
    // and its job is to skip a compile window rather than to finish somebody's compile.
    inline constexpr double DefaultDrainBudgetSeconds = 20.0;
    inline constexpr float DrainPollIntervalSeconds = 0.01f;

    struct FPendingWork
    {
        // False on a host with no shader compiling manager (a NullRHI or commandlet run). The
        // shader count is then not a measurement of zero, and the block says so.
        bool bShaderCompilerAvailable = false;
        int32 ShaderJobs = 0;
        // Reported, never waited on. See the header comment.
        int32 AssetCompilations = 0;

        bool HasShaderWork() const { return ShaderJobs > 0; }
    };

    struct FReadinessResult
    {
        // False means no gate ran on this path, so the block is omitted rather than published as
        // a clean reading nobody took.
        bool bMeasured = false;
        FPendingWork AtEntry;
        FPendingWork AtExit;
        // The shader queue drained inside the budget.
        bool bReady = false;
        bool bTimedOut = false;
        int32 PumpRounds = 0;
        double DrainMs = 0.0;
        double BudgetMs = 0.0;
        // PinWrightScreenshotUtils::FlushBeforeReadback ran for this capture. Carried here only so
        // one call publishes the whole readback preamble; the flush itself lives in ScreenshotUtils.
        bool bReadbackFlushed = false;

        // Refuse rather than photograph stand-in materials.
        bool ShouldRefuseReadback() const { return bMeasured && bTimedOut && AtExit.HasShaderWork(); }
    };

    // Reads both compile queues. Game thread.
    FPendingWork SurveyPendingWork();

    // Bounded, PUMPING wait on the SHADER queue only. The pump is Utils/AssetCompilePump.h's
    // AdvanceOnGameThread: a wait that only slept would starve the game-thread pass that consumes
    // finished compile results and would therefore report "still compiling" until the budget
    // expired. Game thread only. Call it immediately before the readback preamble, after the pose
    // has been applied and the frame has settled -- the compiles queued by the new pose are the
    // ones that decide whether this frame shows real materials.
    FReadinessResult DrainBeforeReadback(double BudgetSeconds = DefaultDrainBudgetSeconds);

    // The `shadersCompiling` block. Returns null when nothing was measured.
    TSharedPtr<FJsonObject> MakeReadinessInfoObject(const FReadinessResult& Readiness);

    // Attaches `shadersCompiling` and `readbackFlushed` to a `viewport` object, when a gate ran.
    void AddReadinessFields(const FReadinessResult& Readiness, const TSharedPtr<FJsonObject>& Viewport);

    // The message body of the refusal, naming what was still pending and for how long it drained.
    FString MakeRefusalMessage(const FReadinessResult& Readiness);

#if WITH_DEV_AUTOMATION_TESTS
    // Injected pending-work predicate. Returning true replaces the live survey for this call, so a
    // test can hold the gate against work no host is guaranteed to have in flight. Consulted only
    // by SurveyPendingWork, so the drain loop, the block and the refusal decision all run their
    // production code paths against it.
    using FPendingWorkProbe = TFunction<bool(FPendingWork&)>;
    FPendingWorkProbe& PendingWorkProbe();

    class FScopedPendingWorkProbe
    {
    public:
        explicit FScopedPendingWorkProbe(FPendingWorkProbe InProbe)
            : Previous(PendingWorkProbe())
        {
            PendingWorkProbe() = MoveTemp(InProbe);
        }

        ~FScopedPendingWorkProbe() { PendingWorkProbe() = MoveTemp(Previous); }

        FScopedPendingWorkProbe(const FScopedPendingWorkProbe&) = delete;
        FScopedPendingWorkProbe& operator=(const FScopedPendingWorkProbe&) = delete;

    private:
        FPendingWorkProbe Previous;
    };
#endif
}
