// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/CaptureReadinessGate.h"

#include "Utils/AssetCompilePump.h"

#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ShaderCompiler.h"

namespace PinWrightCaptureReadiness
{
#if WITH_DEV_AUTOMATION_TESTS
    FPendingWorkProbe& PendingWorkProbe()
    {
        static FPendingWorkProbe Probe;
        return Probe;
    }
#endif

    FPendingWork SurveyPendingWork()
    {
        FPendingWork Work;

#if WITH_DEV_AUTOMATION_TESTS
        if (const FPendingWorkProbe& Probe = PendingWorkProbe())
        {
            if (Probe(Work))
            {
                return Work;
            }
        }
#endif

        if (GShaderCompilingManager)
        {
            Work.bShaderCompilerAvailable = true;
            Work.ShaderJobs = GShaderCompilingManager->GetNumRemainingJobs();
            if (Work.ShaderJobs <= 0 && GShaderCompilingManager->IsCompiling())
            {
                // GetNumRemainingJobs counts outstanding + external jobs only. IsCompiling also
                // covers shader maps sitting in ShaderMapJobs / PendingFinalizeShaderMaps, awaiting
                // the game-thread finalize pass -- a material whose map is in that state still
                // renders as the default material, so it has to hold the gate too.
                Work.ShaderJobs = 1;
            }
        }
        Work.AssetCompilations = FAssetCompilingManager::Get().GetNumRemainingAssets();
        return Work;
    }

    FReadinessResult DrainBeforeReadback(const double BudgetSeconds)
    {
        FReadinessResult Result;
        Result.bMeasured = true;
        Result.BudgetMs = BudgetSeconds * 1000.0;
        Result.AtEntry = SurveyPendingWork();
        Result.AtExit = Result.AtEntry;

        const double StartSeconds = FPlatformTime::Seconds();
        // SHADER work only. A pending asset queue is the normal steady state after any map load,
        // so waiting on it spent the whole budget on the game thread on every single capture and
        // then took the frame regardless. Its counts are carried out below, unwaited.
        while (Result.AtExit.HasShaderWork())
        {
            if (FPlatformTime::Seconds() - StartSeconds >= BudgetSeconds)
            {
                Result.bTimedOut = true;
                break;
            }
            // Pump BEFORE polling, so the poll sees this iteration's finalised results.
            PinWright::AssetCompile::AdvanceOnGameThread();
            FPlatformProcess::Sleep(DrainPollIntervalSeconds);
            ++Result.PumpRounds;
            Result.AtExit = SurveyPendingWork();
        }

        Result.DrainMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
        Result.bReady = !Result.AtExit.HasShaderWork();
        return Result;
    }

    TSharedPtr<FJsonObject> MakeReadinessInfoObject(const FReadinessResult& Readiness)
    {
        if (!Readiness.bMeasured)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetBoolField(TEXT("measured"), true);
        Block->SetBoolField(TEXT("shaderCompilerAvailable"), Readiness.AtEntry.bShaderCompilerAvailable);
        Block->SetBoolField(TEXT("pendingAtEntry"), Readiness.AtEntry.HasShaderWork());
        Block->SetBoolField(TEXT("ready"), Readiness.bReady);
        Block->SetBoolField(TEXT("timedOut"), Readiness.bTimedOut);
        Block->SetNumberField(TEXT("shaderJobsAtEntry"), Readiness.AtEntry.ShaderJobs);
        Block->SetNumberField(TEXT("shaderJobsRemaining"), Readiness.AtExit.ShaderJobs);
        Block->SetNumberField(TEXT("assetCompilationsAtEntry"), Readiness.AtEntry.AssetCompilations);
        Block->SetNumberField(TEXT("assetCompilationsRemaining"), Readiness.AtExit.AssetCompilations);
        Block->SetNumberField(TEXT("pumpRounds"), Readiness.PumpRounds);
        Block->SetNumberField(TEXT("drainMs"), Readiness.DrainMs);
        Block->SetNumberField(TEXT("budgetMs"), Readiness.BudgetMs);

        // Reported, never waited on: a non-zero asset queue is the normal steady state after a map
        // load, so it is a caveat on the frame rather than a reason to spend the caller's budget.
        if (Readiness.AtExit.AssetCompilations > 0)
        {
            Block->SetStringField(TEXT("assetCompilationWarning"), FString::Printf(
                TEXT("%d asset compilation(s) were still queued when these pixels were read back "
                     "(textures, static meshes, sound waves). The gate does not wait on them -- a "
                     "pending asset queue is normal after a map load -- so meshes and textures in "
                     "this frame can be drawn at a placeholder or lower-detail state. Re-shoot once "
                     "`assetCompilationsRemaining` reads 0 if the frame has to be comparable with "
                     "one taken from a settled editor."),
                Readiness.AtExit.AssetCompilations));
        }
        if (!Readiness.bReady)
        {
            Block->SetStringField(TEXT("readinessWarning"), FString::Printf(
                TEXT("Shader compilation was still in flight when these pixels were read back: %d "
                     "job(s) at entry, %d still queued after draining for %.1f ms against a %.1f ms "
                     "budget. Materials whose shader map has not landed render as the DEFAULT "
                     "material, so this frame can show grey or WorldGridMaterial stand-ins where "
                     "authored materials belong -- and `warmup` reads settled on it, because a "
                     "stand-in is stable. Re-shoot once `shaderJobsRemaining` reads 0."),
                Readiness.AtEntry.ShaderJobs, Readiness.AtExit.ShaderJobs,
                Readiness.DrainMs, Readiness.BudgetMs));
        }
        return Block;
    }

    void AddReadinessFields(const FReadinessResult& Readiness, const TSharedPtr<FJsonObject>& Viewport)
    {
        if (!Viewport.IsValid())
        {
            return;
        }
        if (const TSharedPtr<FJsonObject> Block = MakeReadinessInfoObject(Readiness))
        {
            Viewport->SetObjectField(TEXT("shadersCompiling"), Block);
            // Published beside the compile state because the two together are the whole readback
            // preamble: what was still compiling, and whether the render/RHI queues were flushed
            // before the copy. See PinWrightScreenshotUtils::FlushBeforeReadback for what the
            // flush does and does not establish.
            Viewport->SetBoolField(TEXT("readbackFlushed"), Readiness.bReadbackFlushed);
        }
    }

    FString MakeRefusalMessage(const FReadinessResult& Readiness)
    {
        return FString::Printf(
            TEXT("Refusing the pixel readback: shader compilation was still in flight after "
                 "draining for %.1f ms against a %.1f ms budget (%d shader job(s) at entry, %d "
                 "still queued). A material whose shader map has not landed renders as the DEFAULT "
                 "material, so this frame would show grey or WorldGridMaterial stand-ins where "
                 "authored materials belong -- and it would read settled, non-blank and clean on "
                 "every other field, which is why it is refused rather than shipped with a warning. "
                 "Wait for the compile to finish -- the editor's own compile progress, or "
                 "material.authoring.compile_material on the material under review -- and re-issue "
                 "the capture. (%d asset compilation(s) were also queued; the gate does not wait on "
                 "those and they are not why this was refused.)"),
            Readiness.DrainMs, Readiness.BudgetMs,
            Readiness.AtEntry.ShaderJobs, Readiness.AtExit.ShaderJobs,
            Readiness.AtExit.AssetCompilations);
    }
}
