// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Compat/EngineVersionCompat.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "MaterialShaderPrecompileMode.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "RHIFeatureLevel.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
#include "RHIShaderPlatform.h"
#endif
#include "ShaderCompiler.h"
#include "Utils/AssetCompilePump.h"

namespace MaterialCompileErrorCollector
{
    // Forces a synchronous compile of every shader permutation, then gathers the
    // failed-permutation HLSL errors for the material's max-RHI feature-level
    // resource.
    //
    // PostEditChange caches the shader map with EMaterialShaderPrecompileMode::None,
    // which only translates the material to HLSL; it does not submit the
    // permutation compile jobs (those are deferred and submitted lazily on the
    // render thread when the material is first drawn). In a headless automation
    // context the material is never rendered, so the platform shader compiler
    // (DXC/FXC) never runs on the HLSL and GShaderCompilingManager has nothing to
    // finish - GetCompileErrors() stays empty even for materials with broken HLSL.
    //
    // CacheShaders(Synchronous) submits the jobs at High priority and blocks on
    // FShaderCompilingManager::FinishCompilation() inline (see
    // FMaterialShaderMap::Compile). On a failed permutation, ProcessCompiledShaderMaps
    // writes the HLSL errors back into FMaterial::CompileErrors (see
    // ShaderCompiler.cpp, "Default Material will be used in game"), so reading
    // GetCompileErrors() afterwards surfaces the same text the engine logs.
    //
    // An empty error list is NOT the same claim as a clean compile, which is why WaitAndCollect
    // returns FCompileWaitOutcome rather than void. A compile that was skipped, that produced no
    // resource, or that was still in flight when the wait expired also reports zero errors, and
    // callers that read only the list published those as successes.

    // Wall-clock ceiling on the drain loop below: the transport's response timeout is 120 s and
    // is response-only - when it fires the handler keeps running and its save commits with nobody
    // watching - so the wait has to expire first, leaving room for the rest of the handler. In
    // practice the loop is a no-op: FMaterialShaderMap::Compile already blocked
    // inline for the Synchronous submission above, so the ceiling only bounds the residue
    // (a shader map still being finalised because PoolSpecialMaterialsCompileJobs deferred the
    // synchronous compile, or one shared with another FMaterial that is still compiling).
    inline constexpr double CompileWaitTimeoutSeconds = 90.0;

    // Poll cadence between pumps: short enough not to dominate the fast case, long enough to keep
    // the loop off a hot spin.
    inline constexpr float CompilePollIntervalSeconds = 0.01f;

    // What the wait actually OBSERVED, as opposed to what it asked for. Every field is
    // measured. The struct exists because the verb used to publish `compileSucceeded: true`
    // whenever the error list came back empty - which it also does when no compile ran at all,
    // when the shader map was never finalised, and when the wait gave up early.
    struct FCompileWaitOutcome
    {
        // The drain loop blocked at least once, i.e. there was still in-flight work after the
        // synchronous submission returned.
        bool bWaited = false;
        // The drain loop hit CompileWaitTimeoutSeconds with work still in flight. The compile
        // is NOT abandoned - it keeps running - but nothing downstream may assume it landed.
        bool bTimedOut = false;
        // FMaterial::IsCompilationFinished(), measured after the wait returned.
        bool bOutstanding = false;
        // FMaterial::IsGameThreadShaderMapComplete(), measured after the wait returned. This is
        // the bit that separates "the engine installed a usable shader map for this material"
        // from "there is an FMaterialResource object". It is FALSE for a skipped compile:
        // FMaterial::BeginCompileShaderMap forces PrecompileMode to None when
        // GShaderCompilingManager->IsShaderCompilationSkipped(), then still calls
        // SetGameThreadShaderMap with a map holding zero shaders (MaterialShared.cpp, the
        // "We didn't compile any shaders but still assign the result" branch) - so a non-null
        // shader map is NOT evidence of a compile, and FMaterial::SetGameThreadShaderMap's own
        // IsComplete(this, true) call is.
        bool bShaderMapComplete = false;
        // Wall-clock seconds spent inside the drain loop.
        double WaitedSeconds = 0.0;
        // Number of distinct compile errors collected into OutErrors.
        int32 NumCompileErrors = 0;
    };

    // Did the compile reach a conclusion the caller may act on? A FAILED compile counts - the
    // errors are its conclusion - which is why this is not simply bShaderMapComplete. False
    // means the verb learned nothing about the material: the wait expired, work is still in
    // flight, or no compile ran (skipped compilation, or a translation that submitted no jobs).
    inline bool DidCompileLand(const FCompileWaitOutcome& Outcome)
    {
        return !Outcome.bTimedOut
            && !Outcome.bOutstanding
            && (Outcome.bShaderMapComplete || Outcome.NumCompileErrors > 0);
    }

    // Stable wire spelling of the five distinguishable outcomes, so one field is the single
    // truth a caller can branch on:
    //   "timedOut"    - the drain loop expired with work still in flight
    //   "outstanding" - work is still in flight and was not waited on
    //   "failed"      - a compile ran and at least one permutation failed
    //   "completed"   - a compile ran and the engine installed a complete shader map
    //   "notCompiled" - no compile ran; there is nothing to report about this material
    inline const TCHAR* DescribeCompileOutcome(const FCompileWaitOutcome& Outcome)
    {
        if (Outcome.bTimedOut)
        {
            return TEXT("timedOut");
        }
        if (Outcome.bOutstanding)
        {
            return TEXT("outstanding");
        }
        if (Outcome.NumCompileErrors > 0)
        {
            return TEXT("failed");
        }
        return Outcome.bShaderMapComplete ? TEXT("completed") : TEXT("notCompiled");
    }

    // The max-RHI resource this collector reads its verdict off. Shared so a caller measuring
    // the same material (a test asserting the reported outcome against the real one) uses the
    // same selector and the same 5.7 compat split rather than a second copy of it.
    inline const FMaterialResource* GetMaxRhiResource(const UMaterialInterface* Material)
    {
        if (!Material)
        {
            return nullptr;
        }
#if UE_VERSION_OLDER_THAN(5, 7, 0)
        return Material->GetMaterialResource(GMaxRHIFeatureLevel);
#else
        // UE 5.7 changed GetMaterialResource's selector from feature level to shader
        // platform; GMaxRHIShaderPlatform is the platform paired with GMaxRHIFeatureLevel.
        return Material->GetMaterialResource(GMaxRHIShaderPlatform);
#endif
    }

    // True when the engine has a COMPLETE game-thread shader map for this material, i.e. when
    // a compile actually produced something renderable. See FCompileWaitOutcome::bShaderMapComplete
    // for why a non-null map is not enough.
    inline bool HasCompleteShaderMap(const UMaterialInterface* Material)
    {
        const FMaterialResource* Resource = GetMaxRhiResource(Material);
        return Resource != nullptr && Resource->IsGameThreadShaderMapComplete();
    }

    // The object whose CacheShaders() actually owns the resource GetMaxRhiResource hands back.
    //
    // A material instance owns shaders only while bHasStaticPermutationResource is set - static
    // switch overrides or overridden base properties. Without it UMaterialInstance::GetMaterialResource
    // forwards to the parent (MaterialInstance.cpp) and UMaterialInstance::CacheShaders reaches
    // InitStaticPermutation, whose CacheResourceShadersForRendering is gated on the same bit and so
    // submits nothing. Calling it on such an instance therefore measures the parent's shader map
    // after compiling nothing, which is the shape of a permanent `notCompiled`.
    inline UMaterialInterface* ResolveCompileSubject(UMaterialInterface* MaterialInterface)
    {
        if (UMaterialInstance* Instance = Cast<UMaterialInstance>(MaterialInterface))
        {
            if (Instance->bHasStaticPermutationResource)
            {
                return MaterialInterface;
            }
            // A PARENTLESS instance resolves GetMaterial() to the engine Default Material, which
            // is neither what the caller named nor an asset a verb may force a compile on.
            return Instance->Parent
                ? static_cast<UMaterialInterface*>(Instance->GetMaterial())
                : nullptr;
        }
        return MaterialInterface;
    }

    inline FCompileWaitOutcome WaitAndCollect(UMaterialInterface* Material, TArray<FString>& OutErrors,
        double TimeoutSeconds = CompileWaitTimeoutSeconds)
    {
        FCompileWaitOutcome Outcome;
        OutErrors.Reset();
        UMaterialInterface* CompileSubject = ResolveCompileSubject(Material);
        if (!CompileSubject)
        {
            return Outcome;
        }

        // Force every permutation through the platform compiler synchronously so
        // broken HLSL actually produces errors instead of being deferred.
        CompileSubject->CacheShaders(EMaterialShaderPrecompileMode::Synchronous);

        // Measured on what the caller asked about, compiled on what owns it: for an instance with
        // no permutation of its own the two resolve to the same parent resource anyway.
        const FMaterialResource* Resource = GetMaxRhiResource(Material);
        if (!Resource)
        {
            // No resource for the running platform means no compile and nothing to read. Say so
            // through the outcome rather than returning an empty error list, which the caller
            // used to publish as a clean compile.
            return Outcome;
        }

        // Drain the residue of the submission above, BOUNDED and PUMPING.
        //
        // This replaces GShaderCompilingManager->FinishAllCompilation(), which was wrong twice
        // over. It has no ceiling and no cancel, and it blocks on EVERY pending shader map in
        // the editor plus the whole of FTextureCompilingManager (ShaderCompiler.cpp, the first
        // thing FinishAllCompilation does) - work this caller never requested and which, in an
        // editor shared with other agents, other callers keep adding to. None of it contributes
        // to this verb's answer: the errors are read off THIS material's resource below.
        //
        // FMaterial::IsCompilationFinished() is the precise predicate - it tests this material's
        // GameThreadCompilingShaderMapId against GShaderCompilingManager - and the pump is what
        // lets it ever become true: ProcessAsyncResults, reached through
        // PinWright::AssetCompile::AdvanceOnGameThread, is the game-thread pass that finalises a
        // finished shader map and writes its errors into FMaterial::CompileErrors. A loop that
        // only slept would starve the single thread that work needs. See Utils/AssetCompilePump.h.
        const double StartSeconds = FPlatformTime::Seconds();
        while (!Resource->IsCompilationFinished())
        {
            Outcome.bWaited = true;
            if (FPlatformTime::Seconds() - StartSeconds >= TimeoutSeconds)
            {
                Outcome.bTimedOut = true;
                break;
            }

            // Pump BEFORE polling, so the poll sees this iteration's finalised results.
            PinWright::AssetCompile::AdvanceOnGameThread();
            FPlatformProcess::Sleep(CompilePollIntervalSeconds);
        }

        Outcome.WaitedSeconds = FPlatformTime::Seconds() - StartSeconds;
        Outcome.bOutstanding = !Resource->IsCompilationFinished();
        Outcome.bShaderMapComplete = Resource->IsGameThreadShaderMapComplete();

        for (const FString& Error : Resource->GetCompileErrors())
        {
            OutErrors.AddUnique(Error);
        }
        Outcome.NumCompileErrors = OutErrors.Num();
        return Outcome;
    }
}
